# Fullscreen Compute Shaders & G-Buffer Visualization in Unreal Engine

A ground-up tutorial for the Surfel plugin. Written for UE **5.8.1** (source build).

Everything here is in this repo and **compiles**. Start at Part 0, and by Part 6 you
will have a compute shader reading the deferred G-Buffer and drawing it fullscreen.

> **New to this?** Read [WARMUP.md](WARMUP.md) first. It covers the project and plugin
> architecture, the game-thread/render-thread split, and the simplest possible compute
> shader — one that fills a render target with a gradient, with no renderer hooks
> involved. This tutorial assumes that material.

---

## Table of contents

- [Part 0 — The mental model](#part-0--the-mental-model)
- [Part 1 — Anatomy of the plugin](#part-1--anatomy-of-the-plugin)
- [Part 2 — The four things every compute pass needs](#part-2--the-four-things-every-compute-pass-needs)
- [Part 3 — Stage 1: a fullscreen compute shader](#part-3--stage-1-a-fullscreen-compute-shader)
- [Part 4 — Hooking into the frame with a SceneViewExtension](#part-4--hooking-into-the-frame-with-a-sceneviewextension)
- [Part 5 — Running it](#part-5--running-it)
- [Part 6 — Stage 2: reading the G-Buffer](#part-6--stage-2-reading-the-g-buffer)
- [Part 7 — Debugging](#part-7--debugging)
- [Part 8 — Gotchas that cost people days](#part-8--gotchas-that-cost-people-days)
- [Part 9 — Where to go next](#part-9--where-to-go-next)

---

## Part 0 — The mental model

Before any code, five ideas. Almost every beginner bug traces back to one of these.

### 0.1 Two threads, one frame apart

Unreal has a **game thread** (gameplay, actors, ticks) and a **render thread**
(builds GPU commands). The render thread typically runs one frame *behind* the game
thread.

Consequence: you cannot read a `UObject` from render-thread code. It may be
half-destroyed or mid-mutation. Data must be *captured* on the game thread and
handed across. In this tutorial we sidestep this entirely by using **console
variables**, which are safe to read on the render thread via
`GetValueOnRenderThread()`.

### 0.2 Compute shaders are a grid of threads

A pixel shader runs once per rasterized pixel; you do not choose where. A **compute
shader** has no geometry at all. You say "run N threads" and each thread asks *"which
one am I?"* via `SV_DispatchThreadID`, then decides what to read and write.

Threads are organised in a two-level hierarchy:

- A **thread group** is a fixed tile of threads, declared in the shader with
  `[numthreads(X, Y, Z)]`. We use 8×8×1 = 64 threads.
- A **dispatch** is a grid of those groups, sized from C++.

For a 1920×1080 viewport with 8×8 groups:

```
GroupCount.X = ceil(1920 / 8) = 240
GroupCount.Y = ceil(1080 / 8) = 135
Total threads = 240 * 135 * 64 = 2,073,600   (slightly more than 1920*1080)
```

`ceil` means the last row/column of groups runs **past the edge** of the image. Those
extra threads must be told to do nothing — hence the bounds check at the top of every
kernel in this tutorial. Skipping it corrupts memory or crashes the GPU.

### 0.3 UAV: the "writable texture"

A shader reading a texture uses an **SRV** (Shader Resource View). A shader *writing*
a texture needs a **UAV** (Unordered Access View) — "unordered" because thousands of
threads write in a nondeterministic order.

In HLSL an SRV is `Texture2D`; a UAV is `RWTexture2D` (RW = read/write). A texture can
only get a UAV if it was created with the `TexCreate_UAV` flag. Forgetting that flag
is a very common first bug.

### 0.4 RDG: you describe the frame, then it runs

Modern Unreal rendering goes through the **Render Dependency Graph (RDG)**. You do not
issue GPU commands directly. Instead you *declare* passes with their inputs and
outputs, and RDG then:

- works out which passes depend on which,
- inserts resource barriers (transition texture from "being written" to "being read"),
- allocates transient memory, reusing it across passes,
- culls passes whose output nobody reads.

Two rules follow, and both surprise beginners:

1. **`FRDGTextureRef` is a promise, not a texture.** During graph *setup* it is just a
   handle; no GPU memory exists yet.
2. **Parameters must be allocated by the graph**, via
   `GraphBuilder.AllocParameters<...>()`, because they must outlive your function and
   survive until execution. A stack local would be long gone by then.

### 0.5 Shaders live at virtual paths

Unreal shaders are not referenced by disk path but by a **virtual path** starting with
`/`. `/Engine/...` maps into the engine's shader folder. Plugins register their own
mapping at startup — ours maps `/Surfel` to `Plugins/Surfel/Shaders/Private`.

This is why the mapping must be registered *before* shaders compile, which is why the
plugin's loading phase matters (see 1.3).

---

## Part 1 — Anatomy of the plugin

### 1.1 Files

```
Plugins/Surfel/
├── Surfel.uplugin                      Plugin descriptor
├── Shaders/Private/
│   ├── FullscreenCS.usf                Stage 1 shader
│   └── GBufferVisualizeCS.usf          Stage 2 shader
└── Source/Surfel/
    ├── Surfel.Build.cs                 Module dependencies
    ├── Public/
    │   ├── Surfel.h                    Module interface
    │   └── SurfelSubsystem.h           Owns the SceneViewExtension
    └── Private/
        ├── Surfel.cpp                  Registers the /Surfel shader path
        ├── SurfelSubsystem.cpp
        ├── SurfelSceneViewExtension.h   Shader classes + pass declarations
        └── SurfelSceneViewExtension.cpp Shader classes + pass implementations
```

- `.usf` = **U**nreal **S**hader **F**ile — an HLSL file that gets compiled.
- `.ush` = **U**nreal **S**hader **H**eader — meant to be `#include`d, never compiled alone.

### 1.2 Module dependencies

`Surfel.Build.cs`:

```csharp
PublicDependencyModuleNames.AddRange(
    new string[]
    {
        "Core",
        "CoreUObject",
        "Engine",
        "RenderCore",   // FGlobalShader, RDG, shader parameter macros
        "RHI",          // Render Hardware Interface: pixel formats, texture flags
        "Renderer",     // FSceneViewExtensionBase, post-process hook types
        "Projects",     // IPluginManager, needed for the shader path mapping
    }
    );
```

`Renderer` is the one people forget; without it `FSceneViewExtensionBase` will not
resolve.

> **On reaching into `Renderer/Private`.** Many tutorials add
> `Source/Runtime/Renderer/Private` to `PrivateIncludePaths` to get at internal
> headers. That works for *compiling*, but many functions declared there are not
> DLL-exported, so your plugin will **link-fail** with `unresolved external symbol`.
> This actually happened while writing this tutorial: `GetSceneTextureParameters()`
> compiles fine from a plugin but does not link. Part 6 shows the public alternative.
> `Surfel.Build.cs` keeps the include paths commented out as a reference.

### 1.3 Registering the shader directory

`Private/Surfel.cpp`:

```cpp
void FSurfelModule::StartupModule()
{
	AddShaderSourceDirectoryMapping(
		TEXT("/Surfel"),
		IPluginManager::Get().FindPlugin("Surfel")->GetBaseDir() + "/Shaders/Private");
}
```

This makes `/Surfel/FullscreenCS.usf` resolve to the real file on disk.

**Critical:** this must run before the shader compiler starts. In `Surfel.uplugin`:

```json
"LoadingPhase": "PostConfigInit"
```

The default `"Default"` phase is **too late** and you will get
`Shader file not found: /Surfel/FullscreenCS.usf`. This tutorial changed the
descriptor to `PostConfigInit` for exactly this reason.

---

## Part 2 — The four things every compute pass needs

Every compute pass in Unreal is the same four pieces. Learn these and the rest is detail.

| # | Piece | Where | Purpose |
|---|-------|-------|---------|
| 1 | Parameter struct | C++ | Declares what the shader receives; must match the `.usf` by name |
| 2 | Shader class | C++ | Binds a C++ type to a `.usf` file and entry point |
| 3 | The `.usf` kernel | HLSL | The code each thread runs |
| 4 | The dispatch | C++ | Fills parameters, computes group count, adds the RDG pass |

**The single most important rule:** parameter names must match *exactly* between the
C++ struct and the `.usf`. `SHADER_PARAMETER(float, Intensity)` in C++ requires
`float Intensity;` in the shader. A mismatch usually fails silently — the value is
just zero — which is a miserable bug to track down.

---

## Part 3 — Stage 1: a fullscreen compute shader

### 3.1 The parameter struct and shader class

From `SurfelSceneViewExtension.cpp`:

```cpp
class FSurfelFullscreenCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSurfelFullscreenCS);
	SHADER_USE_PARAMETER_STRUCT(FSurfelFullscreenCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputSceneColor)
		SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, InputViewport)
		SHADER_PARAMETER(float, Intensity)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutRenderTarget)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("THREADS_X"), FComputeShaderUtils::kGolden2DGroupSize);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), FComputeShaderUtils::kGolden2DGroupSize);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSurfelFullscreenCS, "/Surfel/FullscreenCS.usf", "MainCS", SF_Compute);
```

Line by line:

- **`FGlobalShader`** — a shader not tied to any material or vertex factory. Exactly
  what you want for a screen-space effect. (The alternative, `FMaterialShader`, is for
  shaders generated per-material.)
- **`DECLARE_GLOBAL_SHADER`** — boilerplate: type info, shader-map registration.
- **`SHADER_USE_PARAMETER_STRUCT`** — generates the constructor that binds `FParameters`.
- **`SHADER_PARAMETER_RDG_TEXTURE`** — a read-only texture (SRV).
- **`SHADER_PARAMETER_RDG_TEXTURE_UAV`** — a writable texture (UAV).
- **`SHADER_PARAMETER_STRUCT`** — embeds a nested struct. `FScreenPassTextureViewportParameters`
  expands in HLSL to `InputViewport_ViewportMin`, `_ViewportMax`, `_Extent`,
  `_ExtentInverse`, and more.
- **`ShouldCompilePermutation`** — return false and the shader is skipped for that
  platform. Gating on SM5 avoids compiling for mobile, where the deferred G-Buffer
  does not exist anyway.
- **`ModifyCompilationEnvironment`** — injects `#define`s. This is how C++ tells the
  `.usf` its thread group size, keeping the two in sync from a single source.
  `kGolden2DGroupSize` is 8, a good default for 2D work.
- **`IMPLEMENT_GLOBAL_SHADER`** — ties class → virtual path → entry point → shader
  frequency. Put it in a `.cpp`, exactly once.

### 3.2 The shader

`Shaders/Private/FullscreenCS.usf`:

```hlsl
#include "/Engine/Public/Platform.ush"
#include "/Engine/Private/Common.ush"
#include "/Engine/Private/ScreenPass.ush"

Texture2D InputSceneColor;

SCREEN_PASS_TEXTURE_VIEWPORT(InputViewport)

float Intensity;

RWTexture2D<float4> OutRenderTarget;

[numthreads(THREADS_X, THREADS_Y, THREADS_Z)]
void MainCS(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    if (any(DispatchThreadId.xy >= (uint2)InputViewport_ViewportMax))
    {
        return;
    }

    uint2 PixelPos = (uint2)InputViewport_ViewportMin + DispatchThreadId.xy;

    float2 UV = (float2(InputViewport_ViewportMin) + (float2(DispatchThreadId.xy) + 0.5f))
              * InputViewport_ExtentInverse;

    float4 SceneColor = InputSceneColor.SampleLevel(GlobalPointClampedSampler, UV, 0);

    float2 ViewportUV = (float2(DispatchThreadId.xy) + 0.5f)
                      / float2(InputViewport_ViewportMax - InputViewport_ViewportMin);

    float Radius = length(ViewportUV - 0.5f) * 2.0f;
    float3 Tint = lerp(float3(0.1f, 0.4f, 1.0f), float3(1.0f, 0.2f, 0.1f), saturate(Radius));

    float3 Result = lerp(SceneColor.rgb, SceneColor.rgb * Tint, saturate(Intensity));

    OutRenderTarget[PixelPos] = float4(Result, SceneColor.a);
}
```

The pieces worth dwelling on:

**`SCREEN_PASS_TEXTURE_VIEWPORT(InputViewport)`** matches the C++
`SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, InputViewport)` and
declares the `InputViewport_*` variables.

**Viewport vs. texture.** This is the subtlest idea in the whole tutorial. Unreal
allocates scene textures at the *maximum* size needed, then renders into a sub-rectangle
of them. At 50% screen percentage on a 1920×1080 window, the texture may be 1920×1080
while the active region is only 960×540 in one corner. So:

- `InputViewport_ViewportMin` / `_ViewportMax` — the active rect, in pixels.
- `InputViewport_Extent` / `_ExtentInverse` — the full texture size and its reciprocal.

Hardcoding UVs as `DispatchThreadId / ScreenSize` works at 100% screen percentage and
breaks the moment anyone changes `r.ScreenPercentage`, enables TSR upscaling, or uses
dynamic resolution. Always go through the viewport parameters.

**The `+ 0.5f`** samples the *centre* of the pixel. Texel (0,0) spans UV 0.0→1/width,
so its centre is at 0.5/width. Omitting this offsets everything by half a pixel — a
subtle blur or shimmer rather than an obvious failure.

**`GlobalPointClampedSampler`** is a sampler provided by `Common.ush`. Point (nearest)
filtering, clamped at edges. For 1:1 screen-space work this is what you want; bilinear
would blend neighbouring pixels for no reason.

---

## Part 4 — Hooking into the frame with a SceneViewExtension

### 4.1 What it is

`FSceneViewExtensionBase` is Unreal's official hook into the render pipeline **without
modifying the engine**. You subclass it, override the stages you care about, and the
renderer calls you every frame for every view — main viewport, scene captures, editor
previews, each eye in VR.

### 4.2 Choosing an insertion point

`SubscribeToPostProcessingPass` is called once per post-process stage. You check the
`PassId` and register a callback on the stage you want:

```cpp
void FSurfelSceneViewExtension::SubscribeToPostProcessingPass(
	EPostProcessingPass PassId,
	const FSceneView& View,
	FAfterPassCallbackDelegateArray& InOutPassCallbacks,
	bool bIsPassEnabled)
{
	if (PassId != EPostProcessingPass::MotionBlur)
	{
		return;
	}

	const int32 Mode = CVarSurfelMode.GetValueOnRenderThread();

	if (Mode == 1)
	{
		InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(
			this, &FSurfelSceneViewExtension::RunFullscreenPass));
	}
	else if (Mode == 2)
	{
		InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(
			this, &FSurfelSceneViewExtension::RunGBufferPass));
	}
}
```

Your callback fires *after* the named pass. The main choices:

| `EPostProcessingPass` | Scene color state | Notes |
|---|---|---|
| `MotionBlur` | Linear HDR | **Used here.** G-Buffer still valid. Good default. |
| `Tonemap` | Display-referred LDR | After tonemapping and colour grading. |
| `FXAA` | LDR | Very late; near the end of the chain. |

For G-Buffer work, insert **before tonemapping**. `MotionBlur` is the conventional
choice and the one this plugin uses.

### 4.3 The pass itself

```cpp
FScreenPassTexture FSurfelSceneViewExtension::RunFullscreenPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	const FScreenPassTexture SceneColor =
		FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));

	if (!SceneColor.IsValid())
	{
		return SceneColor;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "Surfel Fullscreen CS");

	const FScreenPassTextureViewport SceneColorViewport(SceneColor);
	const FIntPoint PassSize = SceneColor.ViewRect.Size();

	FRDGTextureRef OutputTexture =
		CreateOutputLike(GraphBuilder, SceneColor.Texture, TEXT("Surfel.FullscreenOutput"));

	FSurfelFullscreenCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FSurfelFullscreenCS::FParameters>();

	PassParameters->InputSceneColor = SceneColor.Texture;
	PassParameters->InputViewport   = GetScreenPassTextureViewportParameters(SceneColorViewport);
	PassParameters->Intensity       = CVarSurfelIntensity.GetValueOnRenderThread();
	PassParameters->OutRenderTarget = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(OutputTexture));

	TShaderMapRef<FSurfelFullscreenCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("Surfel Fullscreen %dx%d", PassSize.X, PassSize.Y),
		ComputeShader,
		PassParameters,
		FComputeShaderUtils::GetGroupCount(PassSize, FComputeShaderUtils::kGolden2DGroupSize));

	AddCopyTexturePass(GraphBuilder, OutputTexture, SceneColor.Texture);

	return SceneColor;
}
```

Notes on the non-obvious parts:

**`CopyFromSlice`** — scene color may arrive as one slice of a texture *array* (VR
instanced stereo, or certain mobile paths). This normalises it to a plain 2D texture.

**`RDG_EVENT_SCOPE` / `RDG_EVENT_NAME`** — the labels you will see in RenderDoc. In
shipping builds they compile out. Name them well; you will be staring at them.

**Why a separate output texture and a copy?** A compute shader cannot safely read and
write the same texture — thousands of threads with no ordering guarantee means a thread
could read a texel another thread has already overwritten. So we write to a fresh
texture, then copy back. The copy is cheap; correctness is not negotiable here.

Creating that texture:

```cpp
static FRDGTextureRef CreateOutputLike(FRDGBuilder& GraphBuilder, FRDGTextureRef SceneColorTexture, const TCHAR* Name)
{
	FRDGTextureDesc Desc = SceneColorTexture->Desc;

	Desc.Reset();
	Desc.Flags |= TexCreate_UAV;                                       // must have, to write from compute
	Desc.Flags &= ~(TexCreate_RenderTargetable | TexCreate_FastVRAM);  // not needed here
	Desc.ClearValue = FClearValueBinding(FLinearColor::Transparent);

	return GraphBuilder.CreateTexture(Desc, Name);
}
```

Copying the source descriptor means we automatically inherit the right size and pixel
format, whatever the project's settings are.

**`GetGroupCount`** does the `ceil` division from 0.2. Pass the **viewport size**, not
the texture size — otherwise you dispatch threads for regions that are not being
rendered.

### 4.4 Keeping the extension alive

A `SceneViewExtension` is reference-counted; drop the last reference and it dies. An
`UEngineSubsystem` gives it engine-lifetime ownership:

```cpp
void USurfelSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	SurfelSceneViewExtension = FSceneViewExtensions::NewExtension<FSurfelSceneViewExtension>();
}

void USurfelSubsystem::Deinitialize()
{
	if (SurfelSceneViewExtension.IsValid())
	{
		SurfelSceneViewExtension->IsActiveThisFrameFunctions.Empty();

		FSceneViewExtensionIsActiveFunctor IsActiveFunctor;
		IsActiveFunctor.IsActiveFunction =
			[](const ISceneViewExtension* SceneViewExtension, const FSceneViewExtensionContext& Context)
			{
				return TOptional<bool>(false);
			};

		SurfelSceneViewExtension->IsActiveThisFrameFunctions.Add(IsActiveFunctor);
	}

	SurfelSceneViewExtension.Reset();
}
```

The shutdown dance matters. The render thread runs behind the game thread, so at the
moment `Deinitialize` runs it may still be about to call into this object. Forcing
`IsActiveFunction` to return `false` guarantees it is skipped for any in-flight frame
before the pointer is released. Skip this and you get shutdown crashes that only
reproduce sometimes.

---

## Part 5 — Running it

### 5.1 Build

```powershell
& "C:\GameDevelopment\Unreal\UnrealEngine-release\Engine\Build\BatchFiles\Build.bat" `
    SurfelsEditor Win64 Development `
    -Project="C:\Users\david\Documents\Unreal Projects\Surfels\Surfels.uproject" -WaitMutex
```

Or just build from Visual Studio / Rider.

### 5.2 Console commands

Open the console with **`** (backtick) and:

```
r.Surfel.Mode 1          # fullscreen procedural effect
r.Surfel.Intensity 1.0   # 0 = untouched scene, 1 = full effect
r.Surfel.Mode 0          # off
```

You should see a blue-to-red radial tint over the viewport. If you do, all four
moving parts — path mapping, shader compile, RDG dispatch, copy-back — are working.

### 5.3 Iterating on shaders without restarting

```
r.ShaderDevelopmentMode 1
r.DumpShaderDebugInfo 1
```

Then edit a `.usf` and run `recompileshaders changed`. Turnaround drops from minutes to
seconds. Worth setting up before you write your second shader, not your tenth.

---

## Part 6 — Stage 2: reading the G-Buffer

### 6.1 What the G-Buffer is

Unreal's deferred renderer does not light objects as it draws them. It first writes
their *surface properties* into several render targets — the **G-Buffer** (geometry
buffer) — and lights the result afterwards in screen space. That is what makes many
dynamic lights affordable, and it is what makes techniques like surfel-based GI
possible: after the base pass, every pixel's surface data is available to you.

Layout in UE 5.8 (this varies with project settings; verify in RenderDoc):

| Target | Contents |
|---|---|
| **GBufferA** | World-space normal (`.rgb`), per-object data (`.a`) |
| **GBufferB** | Metallic, Specular, Roughness, ShadingModelID + flags |
| **GBufferC** | BaseColor (`.rgb`), AO / indirect irradiance (`.a`) |
| **GBufferD** | Custom data, meaning depends on shading model (subsurface, clear coat, …) |
| **GBufferE** | Precomputed shadow factors |
| **GBufferF** | World tangent + anisotropy |
| **SceneDepth** | Device depth (non-linear, and **reversed-Z** in UE) |

Encodings are not obvious — normals are compressed, roughness may be non-linear, and
GBufferD changes meaning per shading model. **Never unpack by hand.** Use the engine's
`DecodeGBufferData()`, which is the same code the lighting pass uses.

### 6.2 Getting the textures in C++

This is the part where most tutorials will send you into a link error. The canonical
`GetSceneTextureParameters()` lives in `Renderer/Private/SceneTextureParameters.h` and
is **not DLL-exported**, so calling it from a plugin compiles and then fails to link:

```
error LNK2019: unresolved external symbol "class FSceneTextureParameters __cdecl
GetSceneTextureParameters(class FRDGBuilder &, class TRDGUniformBuffer<...> *)"
```

The public route: `FPostProcessMaterialInputs` already carries the scene textures
uniform buffer, and `FSceneTextureUniformParameters` is declared `ENGINE_API` in the
public header `Engine/Public/SceneTexturesConfig.h`. So unwrap the binding and read
its contents directly:

```cpp
TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUB =
    Inputs.SceneTextures.SceneTextures.GetUniformBuffer();

if (!SceneTexturesUB)
{
    return SceneColor;   // mobile / forward path: no deferred G-Buffer
}

const FSceneTextureUniformParameters& SceneTextures = *SceneTexturesUB->GetContents();

if (!SceneTextures.GBufferATexture || !SceneTextures.SceneDepthTexture)
{
    return SceneColor;   // G-Buffer not available this frame
}
```

`Inputs.SceneTextures.SceneTextures` is a `TRDGUniformBufferBinding`, a thin wrapper;
`GetUniformBuffer()` unwraps it and `GetContents()` returns the filled-in parameter
struct. No private headers, no linker problems.

### 6.3 Binding them

```cpp
BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputSceneColor)
	SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, InputViewport)

	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferATexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferBTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferCTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferDTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferETexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferFTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)

	// Required by ConvertFromDeviceZ() in Common.ush.
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

	SHADER_PARAMETER(int32, VisualizeMode)
	SHADER_PARAMETER(float, DepthScale)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutRenderTarget)
END_SHADER_PARAMETER_STRUCT()
```

**The `View` uniform buffer is essential.** `ConvertFromDeviceZ()` needs projection
constants that live in `View`. Bind it with `PassParameters->View = View.ViewUniformBuffer;`.
Omit it and you get either a compile error about undefined `View`, or silently garbage
depth.

Not every G-Buffer slot is allocated in every project configuration, so guard the
optional ones with a fallback:

```cpp
FRDGTextureRef BlackDummy = GSystemTextures.GetBlackDummy(GraphBuilder);

PassParameters->GBufferATexture = SceneTextures.GBufferATexture;
PassParameters->GBufferBTexture = SceneTextures.GBufferBTexture ? SceneTextures.GBufferBTexture : BlackDummy;
// ... same for C, D, E, F
PassParameters->SceneDepthTexture = SceneTextures.SceneDepthTexture;
```

Binding a null texture is a hard RDG validation failure, so never leave one unset.

### 6.3b The G-Buffer is a different size from scene color

> **Status:** this is a **known open issue** in the plugin, not something the shipped
> code handles. The shader currently indexes the G-Buffer with the output pixel
> coordinate, which is correct only at 100% screen percentage. See
> [KNOWN-ISSUES.md](KNOWN-ISSUES.md) for the two ways to fix it. The explanation below
> is still worth reading — the underlying trap applies to any multi-texture pass.

This is the bug that actually bit this plugin, and it is worth understanding properly
because it is invisible at 100% screen percentage and obvious at any other setting.

**The symptom:** turn on `r.Surfel.Mode 2` with TSR enabled (the UE5 default) and the
G-Buffer content fills only the top-left quadrant of the screen. The rest is garbage or
black.

**The cause:** we hook after `MotionBlur`. Look at where that sits in the pass order
(`PostProcessing.cpp`, `enum class EPass`):

```
... MotionBlur ... Tonemap ... FXAA ... PrimaryUpscale, SecondaryUpscale
```

TSR's *temporal upscale* is not `PrimaryUpscale` — it happens much earlier, as part of
the temporal AA pass, before `MotionBlur`. `PostProcessing.cpp` spells this out:

```cpp
const FIntPoint PostTAAViewSize =
    (View.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::TemporalUpscale
     && TAAConfig != EMainTAAPassConfig::Disabled)
    ? View.GetSecondaryViewRectSize()   // <- already full output size
    : View.ViewRect.Size();
```

So by the time our callback runs:

| Texture | Size at 50% screen percentage |
|---|---|
| Scene color (our output) | **1920×1080** — already upscaled |
| G-Buffer, SceneDepth | **960×540** — still render resolution |

Indexing both with the same `DispatchThreadId` reads only the top-left 960×540 of a
1920×1080 output. Hence "half the screen".

**The fix:** give the G-Buffer its own viewport and map between the two.

The obvious C++ — `View.ViewRect` — does *not* compile:

```
error C2039: 'ViewRect': is not a member of 'FSceneView'
```

`ViewRect` lives on `FViewInfo`, which is renderer-private and not reachable from a
plugin (same `Renderer/Private` problem as 1.2). `FSceneView` exposes only
`UnscaledViewRect` and `UnconstrainedViewRect`, neither of which is the
render-resolution rect.

Instead, carry the ratio across from scene color, whose rect and extent we already
have. The active rect occupies the same *fraction* of each texture:

```cpp
const FIntPoint GBufferExtent    = SceneTextures.GBufferATexture->Desc.Extent;
const FIntPoint SceneColorExtent = SceneColor.Texture->Desc.Extent;

const FIntRect GBufferRect(
    FMath::DivideAndRoundDown(SceneColor.ViewRect.Min.X * GBufferExtent.X, SceneColorExtent.X),
    FMath::DivideAndRoundDown(SceneColor.ViewRect.Min.Y * GBufferExtent.Y, SceneColorExtent.Y),
    FMath::DivideAndRoundUp  (SceneColor.ViewRect.Max.X * GBufferExtent.X, SceneColorExtent.X),
    FMath::DivideAndRoundUp  (SceneColor.ViewRect.Max.Y * GBufferExtent.Y, SceneColorExtent.Y));

const FScreenPassTextureViewport GBufferViewport(GBufferExtent, GBufferRect);

PassParameters->GBufferViewport = GetScreenPassTextureViewportParameters(GBufferViewport);
```

with a matching `SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, GBufferViewport)`
in the struct, and in the shader:

```hlsl
SCREEN_PASS_TEXTURE_VIEWPORT(GBufferViewport)

// Normalise to 0..1 across the OUTPUT viewport ...
float2 ViewportUV = (float2(DispatchThreadId.xy) + 0.5f)
                  / float2(InputViewport_ViewportMax - InputViewport_ViewportMin);

// ... then scale into the G-Buffer's own viewport.
float2 GBufferViewportSize = float2(GBufferViewport_ViewportMax - GBufferViewport_ViewportMin);

uint2 GBufferPos = (uint2)clamp(
    float2(GBufferViewport_ViewportMin) + ViewportUV * GBufferViewportSize,
    float2(GBufferViewport_ViewportMin),
    float2(GBufferViewport_ViewportMax) - 1.0f);

int3 LoadPos = int3(GBufferPos, 0);
```

Going through *both* viewports means this is correct at any screen percentage, and
costs nothing when the two happen to be the same size.

> **The general rule.** Whenever a pass reads texture A and writes texture B, never
> assume one coordinate is valid in both. Give each its own
> `SCREEN_PASS_TEXTURE_VIEWPORT` and convert explicitly. Test at
> `r.ScreenPercentage 50` — a bug like this is invisible at 100.

**Want the G-Buffer at full resolution instead?** Then do not upscale-sample it — hook
*earlier*, before TSR runs, using `PrePostProcessPass_RenderThread` or
`PostRenderBasePassDeferred_RenderThread`. There the G-Buffer and scene color are both
at render resolution and match one another. The trade-off is that your output is then
also at render resolution and gets upscaled afterwards along with everything else.

### 6.4 Decoding in the shader

> **Do not declare the G-Buffer textures in your `.usf`.** This is the one mistake that
> is easy to make here, because the C++ side makes it look like you should.
>
> `DeferredShadingCommon.ush` already declares them, inside `#if SHADING_PATH_DEFERRED`,
> under a comment that reads *"Matches FSceneTextureParameters"*:
>
> ```hlsl
> Texture2D SceneDepthTexture;
> Texture2D GBufferATexture;
> Texture2D GBufferBTexture;
> // ... C, D, E, F, Velocity, SGGX
> ```
>
> Those are exactly the names your C++ parameter struct binds, so **binding works with
> no declaration of your own**. Adding your own produces:
>
> ```
> error: redefinition of 'GBufferATexture'
> ```
>
> Declare only what the engine does not: your own inputs (`InputSceneColor`), your
> viewport struct, your scalars, and your UAV.

`Shaders/Private/GBufferVisualizeCS.usf`, the core:

```hlsl
#include "/Engine/Private/DeferredShadingCommon.ush"   // declares the G-Buffer + DecodeGBufferData

int3 LoadPos = int3(PixelPos, 0);

float4 GBufferA = GBufferATexture.Load(LoadPos);
float4 GBufferB = GBufferBTexture.Load(LoadPos);
float4 GBufferC = GBufferCTexture.Load(LoadPos);
float4 GBufferD = GBufferDTexture.Load(LoadPos);
float4 GBufferE = GBufferETexture.Load(LoadPos);
float4 GBufferF = GBufferFTexture.Load(LoadPos);

float DeviceZ    = SceneDepthTexture.Load(LoadPos).r;
float SceneDepth = ConvertFromDeviceZ(DeviceZ);   // -> linear world units

FGBufferData GBuffer = DecodeGBufferData(
    GBufferA, GBufferB, GBufferC, GBufferD, GBufferE, GBufferF,
    /*InGBufferVelocity*/  (float4)0,
    /*CustomNativeDepth*/  0.0f,
    /*CustomStencil*/      0u,
    SceneDepth,
    /*bGetNormalizedNormal*/ true,
    /*bChecker*/             false);
```

**`Load()` not `Sample()`.** `Load` takes integer pixel coordinates and does no
filtering. G-Buffer contents are *encoded data*, not colour — bilinearly blending two
neighbouring encoded normals gives a value that decodes to nonsense. Always `Load`
the G-Buffer.

`ConvertFromDeviceZ` converts UE's reversed-Z non-linear depth to linear world units
(centimetres by default). Raw `DeviceZ` looks almost entirely white near the camera
and is nearly useless to look at directly.

After decoding, `FGBufferData` gives you clean fields:

```hlsl
GBuffer.WorldNormal     // float3, normalized
GBuffer.BaseColor       // float3
GBuffer.Metallic        // float
GBuffer.Roughness       // float
GBuffer.Specular        // float
GBuffer.ShadingModelID  // uint
GBuffer.CustomData      // float4, per shading model
GBuffer.GBufferAO       // float
```

Then just pick one to display:

```hlsl
switch (VisualizeMode)
{
    case VIS_MODE_BASECOLOR:
        Result = GBuffer.BaseColor;
        break;

    case VIS_MODE_WORLDNORMAL:
        // Remap [-1,1] -> [0,1], otherwise negative components clamp to black.
        Result = GBuffer.WorldNormal * 0.5f + 0.5f;
        break;

    case VIS_MODE_ROUGHNESS:
        Result = GBuffer.Roughness.xxx;
        break;

    case VIS_MODE_DEPTH:
        Result = saturate(SceneDepth / max(DepthScale, 1.0f)).xxx;
        break;

    // ...
}
```

### 6.5 Running it

```
r.Surfel.Mode 2
r.Surfel.GBufferChannel 2     # world normal
```

| Value | Channel | What you should see |
|---|---|---|
| 0 | SceneColor | The unmodified scene (sanity check) |
| 1 | BaseColor | Albedo with no lighting — flat, "unlit" look |
| 2 | WorldNormal | The classic pastel normal map; flat ground is greenish |
| 3 | Metallic | Black except metal surfaces |
| 4 | Roughness | Black = mirror, white = fully rough |
| 5 | Specular | Mostly mid-grey (0.5 is the default) |
| 6 | Depth | Black near camera → white at `r.Surfel.DepthScale` |
| 7 | ShadingModel | Flat colour per shading model |

Tune the depth range with `r.Surfel.DepthScale 5000`.

Compare against the engine's own `Buffer Visualization` viewmode in the editor. If
your normals match Epic's, your decode is right.

---

## Part 7 — Debugging

### 7.1 RenderDoc

The single most valuable tool. Enable the **RenderDoc** plugin, press the capture
button, and you get the whole frame.

What to check, in order:

1. **Is the pass there?** Find `Surfel GBuffer Visualize` in the event list. Missing
   means your `SubscribeToPostProcessingPass` never registered — check the CVar and the
   `PassId`.
2. **Are inputs bound?** Select the dispatch, open the resource bindings. A texture
   showing as black/dummy that you expected to have content is your bug.
3. **Is the output written?** Look at the UAV after the dispatch. All black usually
   means the bounds check rejected everything (wrong viewport values) or the group
   count was zero.
4. **Is the dispatch size right?** RenderDoc shows the group count. For 1920×1080 with
   8×8 groups you want 240×135×1.

### 7.2 Reading shader compile errors

Shader errors appear in the Output Log with the virtual path and line number. The
common ones:

| Message | Cause |
|---|---|
| `Shader file not found: /Surfel/...` | Path mapping not registered, or loading phase too late |
| `undeclared identifier 'THREADS_X'` | `ModifyCompilationEnvironment` missing or not setting the define |
| `undeclared identifier 'View'` | Needed `SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)` |
| `redefinition of 'GBufferATexture'` | Re-declared something `DeferredShadingCommon.ush` already declares (see 6.4) |
| `could not be bound to ...'s shader parameter structure FParameters` | The shader declares a parameter the C++ struct does not — **or the C++ struct does have it but the module was never relinked** (see below) |
| Parameter is silently 0 | Name mismatch between C++ struct and `.usf` |

**The "could not be bound" trap.** This message means the shader asked for a parameter
the loaded `FParameters` layout does not contain. The obvious cause is a genuine
mismatch, and that is worth checking first. But there is a sneakier cause:

> If the editor is running while you build, it holds `UnrealEditor-<Module>.dll` open.
> The compile succeeds, **the link silently fails or is skipped**, and the editor keeps
> using the old DLL. Your `.usf` is then compiled against a stale `FParameters` that
> predates the parameter you just added.

This happened while writing this tutorial: `GBufferViewport_ViewportMin` and
`_ViewportMax` reported as unbindable even though `SHADER_PARAMETER_STRUCT(...,
GBufferViewport)` was plainly there in the C++.

Diagnose it by comparing timestamps before you go hunting for a typo:

```powershell
Get-Item "Plugins\Surfel\Binaries\Win64\UnrealEditor-Surfel.dll" | Select LastWriteTime
Get-Item "Plugins\Surfel\Source\Surfel\Private\SurfelSceneViewExtension.cpp" | Select LastWriteTime
```

If the DLL is older than the source, that is your answer: **close the editor, rebuild,
reopen.** Adding or removing any shader parameter requires a real relink, not just a
shader recompile — `recompileshaders changed` cannot help you here, because the C++ side
is what changed.

### 7.3 Printing values from a shader

`ShaderPrint` lets you print numbers from GPU code:

```hlsl
#include "/Engine/Private/ShaderPrint.ush"
```

Enable with `r.ShaderPrint 1`. Useful when a value is wrong but you cannot tell which
of several inputs is at fault.

### 7.4 Colour-as-printf

Often faster: write the suspect value straight to the output and look at it.

```hlsl
OutRenderTarget[PixelPos] = float4(SuspectValue.xxx, 1);
```

Black means zero, white means ≥1, magenta-ish means NaN. Crude, effective.

---

## Part 8 — Gotchas that cost people days

1. **`LoadingPhase` must be `PostConfigInit`.** `Default` is too late for the shader
   path mapping. Symptom: `Shader file not found`.

2. **Private renderer functions compile but do not link.** Adding
   `Renderer/Private` to include paths is not enough — many symbols are not exported.
   Symptom: `LNK2019: unresolved external symbol`. Prefer public APIs; see 6.2.

3. **Parameter name mismatches fail silently.** `Intensity` in C++ and `intensity` in
   HLSL will not warn — the shader just reads zero. Copy-paste the names.

4. **Do not re-declare the G-Buffer textures in your `.usf`.** Including
   `DeferredShadingCommon.ush` already declares `GBufferATexture`, `SceneDepthTexture`
   and friends under `#if SHADING_PATH_DEFERRED`, with names that match
   `FSceneTextureParameters`. Symptom: `error: redefinition of 'GBufferATexture'`.
   The C++ binding still works — just delete the HLSL declarations.

5. **Forgetting `TexCreate_UAV`.** You cannot create a UAV for a texture without it.
   Symptom: RDG assertion, or a validation failure at pass setup.

6. **Never read and write the same texture in one compute pass.** No ordering
   guarantee between threads. Write to a separate texture and copy back.

7. **Always bounds-check `DispatchThreadID`.** `ceil` division means the last groups
   run past the edge.

8. **Use viewport parameters, never hardcoded screen size.** Screen percentage, TSR,
   and dynamic resolution all make texture size ≠ viewport size.

9. **Close the editor before rebuilding after a parameter change.** A running editor
   holds the module DLL open, so the link is skipped and the editor keeps the old
   `FParameters`. Symptom: `could not be bound to ...'s shader parameter structure
   FParameters` for a parameter that is plainly there in your C++. Compare the DLL and
   `.cpp` timestamps to confirm.

10. **Two textures in one pass means two viewports.** After `MotionBlur`, scene color is
   already TSR-upscaled while the G-Buffer is still at render resolution. Indexing both
   with the same `DispatchThreadId` shows the G-Buffer in a corner of the screen. Give
   each texture its own `SCREEN_PASS_TEXTURE_VIEWPORT` and convert between them.
   **Invisible at 100% screen percentage — always test at `r.ScreenPercentage 50`.**

11. **`Load()` the G-Buffer, do not `Sample()` it.** Filtering encoded data produces
   garbage after decode.

12. **Bind the `View` uniform buffer** whenever you use engine helpers like
   `ConvertFromDeviceZ`.

13. **Sample at pixel centres (`+ 0.5`).** Otherwise everything is offset half a texel.

14. **Deactivate the extension before releasing it.** The render thread lags; skipping
    the `IsActiveFunction` dance gives intermittent shutdown crashes.

15. **Never touch `UObject`s from render-thread code.** Pass plain data across, or use
    CVars as this tutorial does.

---

## Part 9 — Where to go next

Now that you can read the G-Buffer, the natural progressions:

**Reconstruct world position.** From depth plus the view matrices you can recover the
world-space position of every pixel — the foundation of screen-space GI, SSAO, and
surfel placement:

```hlsl
float2 ScreenPos = (UV - View.ScreenPositionScaleBias.wz) / View.ScreenPositionScaleBias.xy;
float4 WorldPos  = mul(float4(ScreenPos * SceneDepth, SceneDepth, 1), View.ScreenToWorld);
```

**Structured buffers.** For surfels you need a *list*, not a texture. Look at
`SHADER_PARAMETER_RDG_BUFFER_UAV` with `GraphBuilder.CreateBuffer(...)`, plus
`RWStructuredBuffer<FSurfel>` in HLSL and an atomic counter for appends.

**Persistent state across frames.** RDG resources are transient by default. To keep a
surfel cache alive between frames, allocate a pooled resource
(`TRefCountPtr<FRDGPooledBuffer>`) held by your extension and register it into the
graph each frame with `GraphBuilder.RegisterExternalBuffer(...)`.

**Earlier hook points.** `PrePostProcessPass_RenderThread` runs before the post chain,
`PostRenderBasePassDeferred_RenderThread` right after the G-Buffer is written — the
natural spot for surfel injection.

**Indirect dispatch.** When the thread count depends on GPU-side data (e.g. how many
surfels exist), use `FComputeShaderUtils::AddPass` with an indirect args buffer instead
of a CPU-computed group count.

### Reference reading in the engine source

Reading engine code is the fastest way to learn this area. Good entry points:

| File | Why |
|---|---|
| `Engine/Shaders/Private/DeferredShadingCommon.ush` | G-Buffer encode/decode, ground truth |
| `Engine/Shaders/Private/Common.ush` | `ConvertFromDeviceZ`, samplers, helpers |
| `Engine/Source/Runtime/Engine/Public/SceneTexturesConfig.h` | The public scene-textures struct |
| `Engine/Source/Runtime/Engine/Public/SceneViewExtension.h` | Every available hook |
| `Engine/Source/Runtime/Renderer/Private/PostProcess/` | Real examples of screen-space passes |
| `Engine/Source/Runtime/RenderCore/Public/RenderGraphUtils.h` | RDG helper functions |

---

## Appendix — Console variable reference

| CVar | Default | Meaning |
|---|---|---|
| `r.Surfel.Mode` | 0 | 0 off, 1 fullscreen example, 2 G-Buffer visualization |
| `r.Surfel.Intensity` | 1.0 | Blend strength of the stage 1 effect |
| `r.Surfel.GBufferChannel` | 2 | Which channel to show (see 6.5) |
| `r.Surfel.DepthScale` | 10000.0 | World units mapped to white in depth mode |
