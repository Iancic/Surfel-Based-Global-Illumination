# Guide: adding direct light to the irradiance pass

This explains how to get the scene's lights into `SurfelIrradiance.usf`, test whether each light
is visible with a shadow ray through the global SDF, and add the light into the irradiance.
Each section says what to change and why. The snippets are references to check your own work against.

Files involved:
- `Source/Surfel/Public/ComputePasses/SurfelIrradiancePass.h`: light struct and new shader parameters
- `Source/Surfel/Private/SurfelSceneViewExtension.cpp`: collects the lights and uploads them each frame
- `Shaders/Private/Surfels/SurfelIrradiance.usf`: light evaluation, shadow ray, the empty "Step 2"

Engine version checked against: UE 5.8.

---

## Part 0: where the direct light goes

A surfel should store **incoming indirect irradiance only**. Each hemisphere ray hits a surface,
and that hit point sends light back toward the surfel:

```
L_out(hit) = albedo(hit) / PI * ( E_direct(hit) + E_surfels(hit) )
                                  ^ new: lights + shadow ray   ^ what you already have
```

So direct light is evaluated **at the ray hit point**, inside `SampleSurfelRadianceAt`, where you
already left the "Step 2: Direct lights" comment. Not on the surfel itself.

Why this way:
- **No double counting.** UE's deferred lighting already puts direct light on screen. When the
  composite adds `albedo * E_surfel` on top, the surfel must not contain direct light too, or
  it shows up twice.
- **Multiple bounces come for free.** `E_surfels(hit)` holds last frame's bounced light, so every
  frame adds one more bounce through the temporal feedback you already have.

---

## Part 1 (C++): collect the lights on the render thread

You don't need to collect lights on the game thread. `SurfelSceneViewExtension.cpp` already
includes `Runtime/Renderer/Private/SceneRendering.h`, so the private renderer headers are reachable.
`FScene::Lights` is the same list the deferred renderer loops over, and it's safe to read in
`PostRenderBasePassDeferred_RenderThread` because the render thread owns `FScene`.

### 1a. Shared struct

In `SurfelIrradiancePass.h`. Use 16-byte-aligned members so the HLSL struct has the same layout:

```cpp
// Matches FSurfelLight in SurfelIrradiance.usf
struct FSurfelLight
{
	FVector4f TranslatedPositionAndInvRadius;   // xyz: translated world pos, w: 1/AttenuationRadius
	FVector4f ColorAndType;                     // rgb: color * intensity * IndirectLightingIntensity, a: LIGHT_TYPE_*
	FVector4f DirectionAndFalloffExponent;      // xyz: direction TOWARD the light, w: 0 = inverse squared
	FVector4f SpotAngles;                       // xy: UE spot angles (cos outer, 1/(cos inner - cos outer))
};
```

### 1b. New shader parameters

Add to `FSurfelIrradiancePass::FParameters`:

```cpp
SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FSurfelLight>, SurfelLights)
SHADER_PARAMETER(uint32, NumSurfelLights)
```

### 1c. Build the buffer

In `SurfelSceneViewExtension.cpp`, just before you fill `IrradiancePassParameters`. Add
`#include "Runtime/Renderer/Private/ScenePrivate.h"` next to the existing `SceneRendering.h` include.

```cpp
TArray<FSurfelLight> SurfelLights;
if (const FScene* Scene = SceneView.Family->Scene ? SceneView.Family->Scene->GetRenderScene() : nullptr)
{
	for (const FLightSceneInfoCompact& Compact : Scene->Lights)
	{
		const FLightSceneProxy* Proxy = Compact.LightSceneInfo->Proxy;
		const uint8 Type = Proxy->GetLightType();
		if (!Proxy->AffectGlobalIllumination() || Type == LightType_Rect) // rect: treat as point later, or skip
		{
			continue;
		}

		FLightRenderParameters RenderParams;
		Proxy->GetLightShaderParameters(RenderParams);

		// Converts the world position to translated world (LWC-safe). Exposure 1 = no exposure compensation.
		FLightShaderParameters P;
		RenderParams.MakeShaderParameters(SceneView.ViewMatrices, 1.0f, P);

		FSurfelLight& L = SurfelLights.AddDefaulted_GetRef();
		L.TranslatedPositionAndInvRadius = FVector4f(P.TranslatedWorldPosition, P.InvRadius);
		L.ColorAndType = FVector4f(P.Color * Proxy->GetIndirectLightingScale(), (float)Type);
		L.DirectionAndFalloffExponent = FVector4f(P.Direction, P.FalloffExponent);
		L.SpotAngles = FVector4f(P.SpotAngles.X, P.SpotAngles.Y, 0, 0);
	}
}

const uint32 NumLights = SurfelLights.Num();
if (SurfelLights.IsEmpty())
{
	SurfelLights.AddZeroed(); // RDG needs a non-empty buffer; the shader loops 0 times anyway
}

FRDGBufferRef SurfelLightBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("Surfel.Lights"), SurfelLights);
IrradiancePassParameters->SurfelLights = GraphBuilder.CreateSRV(SurfelLightBuffer);
IrradiancePassParameters->NumSurfelLights = NumLights;
```

Why each part is there:
- `MakeShaderParameters()` handles large world coordinates for you. It returns a
  **translated-world** position, the same space the global SDF trace uses.
- `GetIndirectLightingScale()` is the per-light "Indirect Lighting Intensity" slider. Using it
  keeps that artist control working for your GI.
- `P.Direction` already points **toward** the light; UE negates it for directional lights.
  Don't flip it again.
- The dummy element is there because RDG won't create an empty buffer. `NumSurfelLights` stays 0,
  so the shader never reads it.

---

## Part 2 (HLSL): match the struct and add attenuation

At the top of `SurfelIrradiance.usf`:

```hlsl
#include "/Engine/Shared/LightDefinitions.h"   // LIGHT_TYPE_DIRECTIONAL / POINT / SPOT

struct FSurfelLight
{
	float4 TranslatedPositionAndInvRadius;
	float4 ColorAndType;
	float4 DirectionAndFalloffExponent;
	float4 SpotAngles;
};
StructuredBuffer<FSurfelLight> SurfelLights;
uint NumSurfelLights;
```

The falloff below is the same as `DeferredLightingCommon.ush` (around line 480), so your bounce
brightness matches UE's direct lighting:

```hlsl
// Returns the light's irradiance contribution (before N·L and visibility).
// Outputs L (unit vector toward light) and the max shadow-ray distance.
float3 EvaluateLight(FSurfelLight Light, float3 TranslatedPos, out float3 L, out float ShadowDistance)
{
	uint Type = (uint)Light.ColorAndType.a;
	if (Type == LIGHT_TYPE_DIRECTIONAL)
	{
		L = Light.DirectionAndFalloffExponent.xyz;
		ShadowDistance = MaxTraceDistance;
		return Light.ColorAndType.rgb;
	}

	float3 ToLight = Light.TranslatedPositionAndInvRadius.xyz - TranslatedPos;
	float DistSqr = dot(ToLight, ToLight);
	L = ToLight * rsqrt(DistSqr);
	ShadowDistance = sqrt(DistSqr);

	float InvRadius = Light.TranslatedPositionAndInvRadius.w;
	float Atten;
	if (Light.DirectionAndFalloffExponent.w == 0) // inverse squared
	{
		Atten = 1.0f / (DistSqr + 1.0f);
		Atten *= Square(saturate(1.0f - Square(DistSqr * Square(InvRadius))));
	}
	else
	{
		Atten = RadialAttenuation(ToLight * InvRadius, Light.DirectionAndFalloffExponent.w);
	}

	if (Type == LIGHT_TYPE_SPOT)
	{
		Atten *= Square(saturate((dot(L, Light.DirectionAndFalloffExponent.xyz) - Light.SpotAngles.x) * Light.SpotAngles.y));
	}
	return Light.ColorAndType.rgb * Atten;
}
```

`RadialAttenuation` lives in `/Engine/Private/DeferredLightingCommon.ush`. If including that file
causes trouble, copy the function over; it's 5 lines.

---

## Part 3 (HLSL): shadow ray through the global SDF

You can reuse the trace you already have; only the direction and max distance change:

```hlsl
bool IsLightVisible(float3 TranslatedPos, float3 Normal, float3 L, float ShadowDistance)
{
	const float VoxelExtent = GlobalVolumeTranslatedCenterAndExtent[0].w * GlobalVolumeTexelSize;
	float3 Origin = TranslatedPos + Normal * VoxelExtent;   // same self-hit offset as your main ray

	FGlobalSDFTraceInput In = SetupGlobalSDFTraceInput(Origin, L, MinTraceDistance,
	                                                   ShadowDistance - VoxelExtent,  // stop before the light
	                                                   StepFactor, MinStepFactor);
	return !GlobalSDFTraceResultIsHit(RayTraceGlobalDistanceField(In));
}

float3 ComputeDirectLight(float3 TranslatedPos, float3 Normal)
{
	float3 Irradiance = 0;
	for (uint i = 0; i < NumSurfelLights; ++i)
	{
		float3 L; float ShadowDistance;
		float3 Li = EvaluateLight(SurfelLights[i], TranslatedPos, L, ShadowDistance);
		float NoL = saturate(dot(Normal, L));

		BRANCH
		if (NoL > 0 && any(Li > 0) && IsLightVisible(TranslatedPos, Normal, L, ShadowDistance))
		{
			Irradiance += Li * NoL;
		}
	}
	return Irradiance;
}
```

This replaces the current placeholder `ComputeDirectLight()` that returns `1`.

The order inside `BRANCH` matters. The shadow ray is the expensive part, so the cheap N·L and
attenuation checks run first, and back-facing or out-of-range lights never trace. MegaLights and
Lumen order their light loops the same way.

---

## Part 4: put it into `SampleSurfelRadianceAt`

The function only receives the world-space hit position, so also pass the translated one (you
already compute `TranslatedHitPosition` in `MainCS`). Then change the loop to gather **irradiance
and albedo separately** and combine them at the end:

```hlsl
float3 SampleSurfelRadianceAt(float3 WorldPosition, float3 TranslatedPosition, float3 HitNormal)
{
	float3 IrradianceSum = 0, AlbedoSum = 0;
	float WeightSum = 0;
	// ... same grid lookup / weights as now, but:
	//     IrradianceSum += SurfelIrradiance[Other].xyz * Weight;
	//     AlbedoSum     += GetSurfelAlbedo(Other)      * Weight;

	float3 SurfelIrradianceAtHit = WeightSum > 0 ? IrradianceSum / WeightSum : 0;
	float3 HitAlbedo             = WeightSum > 0 ? AlbedoSum / WeightSum : FALLBACK_ALBEDO;

	float3 DirectIrradiance = ComputeDirectLight(TranslatedPosition, HitNormal);

	return HitAlbedo / PI * (DirectIrradiance + SurfelIrradianceAtHit);
}
```

Things to watch:
- **Out-of-grid early return.** Right now an out-of-grid cell does `return 0` at the top. Change
  it so that only the surfel lookup is skipped, not the direct light. Otherwise a ray that hits a
  lit wall outside the grid adds no light at all.
- **Fallback albedo is your call.** Use a constant (0.18–0.5) or make it a CVar. Without it, rays
  that hit unseen, surfel-less geometry return black, which darkens GI near the screen edges. With
  it, those areas get plausible but made-up bounce light. Try both in a room where half the walls
  are off screen.
- **Leave the `*= PI` in `MainCS` where it is.** `SampleSurfelRadianceAt` still returns
  **radiance** along the ray, so the cosine-sampling estimator stays correct.

---

## Part 5: testing

1. **Isolate the direct term.** For one run, return `HitAlbedo / PI * DirectIrradiance` only and
   set the sky-miss value to 0. Use the Irradiance visualize mode with a single directional light:
   surfels facing a sunlit wall should glow, and surfels facing a shadowed wall should stay dark.
2. **Check shadows.** Put a blocker between the light and a wall. If the wall's bounce light doesn't
   drop, check two things: the shadow ray is in translated space, and the SDF is built (use the
   force-SDF-build button).
3. **Check light leaks.** If thin walls leak light, the SDF is too coarse for them. Raise
   `MinStepFactor` or increase the origin offset.
4. **Compare with Lumen.** Toggle between the `LumenGI` and `SurfelGI` visualize modes in a
   Cornell-box style room. The color bleeding should roughly match.
5. **Check the cost** with `stat GPU`. The `SurfelIrradiance` stat grows with
   (lights × surfels) shadow rays.

Expect a delay with moving lights. Temporal accumulation means a change takes up to `MaxSamples`
frames to settle. A lower `MaxSamples` updates faster but is noisier.

---

## Side question: why not use the direct light Unreal already calculated?

It's tempting. Unreal's lit scene color already has full-quality shadows (virtual shadow maps,
contact shadows), IES profiles, light functions and every light type, at no extra cost.
It works for whatever is on screen, and that's exactly where it stops working:

1. **It only covers what's on screen.** Most hemisphere rays hit surfaces outside the view, behind
   the camera, or hidden behind other objects. There's no scene color for those points. A sun
   hitting the floor behind you would give no bounce light. This is the main problem.
2. **Timing.** The passes run in `PostRenderBasePassDeferred_RenderThread`, which is before
   lighting, so scene color isn't lit yet. You'd need a later hook (`PrePostProcessPass_RenderThread`)
   or last frame's scene color, reprojected.
3. **Scene color isn't "direct diffuse".** It also contains specular (which changes with the camera
   angle), emissive, sky light, and any GI already on screen. Once surfel GI is composited into
   scene color, reading it back makes the GI feed into itself through a path you don't control:
   light gets counted twice and the result drifts.
4. **Pre-exposure.** The values change with auto-exposure, so you'd have to divide by
   `View.PreExposure` every frame.

Lumen is the evidence. It runs inside the same renderer that has the full deferred lighting
result, and it still **recomputes direct lighting itself** for its surface cache. It reads screen
data only for screen traces, meaning rays that hit something visible.

**The middle ground** is to use both:
- **Ray hit is on screen:** project the hit point into (previous frame's) scene color and use it
  if the depth matches. This is a screen trace.
- **Ray hit is off screen:** fall back to `ComputeDirectLight()` with SDF shadows.

Or apply it per surfel instead of per ray. When a surfel is visible during Gather, take its direct
light from the lit scene color and cache it on the surfel. Rays that hit that surfel's area reuse
the cached value. This is the same idea as Lumen's surface cache, and it fits surfels well because
they stay alive when off screen.

Recommended order: build the SDF version first, because it works everywhere and gives a baseline.
Add the on-screen scene-color path afterwards as a quality upgrade.

---

## Later improvements

- **Many lights:** pick **one** light at random per ray (`i = floor(Rand * NumLights)`) and multiply
  its contribution by `NumLights`. That's an unbiased Monte Carlo estimate, and the temporal
  accumulation removes the noise. It's the "integrate the lights with Monte Carlo" note in the code.
- **Exposure:** UE's scene color is pre-exposed. When filling in `Composite.usf`, multiply the surfel
  lighting by `View.PreExposure` before adding it to scene color.
- **Sky miss:** the hard-coded `1.0` sky won't match real light units (a default directional light is
  around 10 lux; a physically based sun can be 100k). Once direct light works, sample the skylight
  there as well.
