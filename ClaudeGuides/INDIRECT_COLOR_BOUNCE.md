# Why indirect color bounces are not visible

Findings from reading the shaders, not from a profiled run. Each cause says where it lives, why it
hides the color bleed, and how to confirm it. Fix order is at the bottom.

Files involved:
- `Shaders/Private/Surfels/SurfelIrradiance.usf`: the per-surfel ray, `SampleRadianceAt`, sky miss
- `Shaders/Private/Surfels/Spawn.usf`: where `SurfelAlbedo` is written
- `Shaders/Private/Surfels/Utilities/SurfelCommon.ush`: `SurfelIndirectRadiance`
- `Shaders/Private/Surfels/Composite.usf`: adds the result to scene color

---

## What is already correct

The unit chain is consistent, so the bug is not a missing `PI`:

```
ray hit radiance   L  = HitAlbedo / PI * (E_indirect + E_direct)      SampleRadianceAt
surfel stores      E  = PI * L  (cosine-weighted, 1 sample)           TotalIncomingIrradiance *= PI
composite adds     DiffuseColor / PI * E                              SurfelIndirectRadiance
```

Color bleed needs `HitAlbedo` to be the color of the surface the ray hit. Every cause below breaks
that, or buries it under uncolored light.

---

## Cause 1: albedo at the hit is zero wherever no surfel covers it

`SampleRadianceAt` gets the albedo of the hit surface only from surfels near the hit:

```hlsl
float3 HitAlbedo = WeightSum > 0.0f ? (AlbedoSum) / WeightSum : 0;
return HitAlbedo / PI * (IndirectIrradiance + DirectIrradiance);
```

When no surfel reaches the hit, `WeightSum == 0`, so `HitAlbedo == 0` and the whole return value is 0.
That includes the direct sun on that surface. The ray contributes nothing instead of a colored bounce.

Why it is common:
- Surfels spawn from **screen-space pixels** only (`Spawn.usf`). A red curtain that is off-screen,
  behind the camera, or hidden has no surfels, so it can never bleed red onto anything.
- The lookup reads **one grid cell** (the cell containing the hit). A hit near a cell border misses
  surfels one cell over that are well within radius.
- A fresh surfel has irradiance alpha 0 and has not accumulated anything yet.

How to confirm: in the Surfels visualization, look at the surface that should be tinting its
neighbours. If it is not covered by discs (black in the new Albedo/Indirect surfel color), this is the cause.

Fix direction: read the albedo from something that exists for every surface. Options, cheapest first:
1. If `WeightSum == 0`, fall back to a neutral albedo (for example 0.5) so unseen surfaces at least
   return lit radiance instead of 0. Not colored, but no longer black.
2. Search the 3x3x3 neighbour cells when the hit cell gives no weight.
3. Use the mesh card or material color of the hit (what Lumen does). Larger change.

---

## Cause 2: the outside-the-grid return skips albedo

```hlsl
if (any(PixelCell < 0.) || any(PixelCell >= (float)CellResolution))
{
    return DirectIrradiance;   // irradiance, not radiance
}
```

This returns raw irradiance, with no albedo and no `/ PI`, then the caller treats it as radiance.
It is uncolored and the wrong scale. It should be `Albedo / PI * DirectIrradiance`, and since there
is no surfel to read the albedo from, it needs the same fallback as Cause 1.

---

## Cause 3: the uncolored sky swamps the bounce

```hlsl
else // Ray miss
{
    TotalIncomingIrradiance += float3(1.f, 1.f, 1.f);
}
```

Every escaping ray adds a flat white. After `*= PI` that is about 3.14 per channel. A bounced ray from a
mid-albedo colored surface is `albedo / PI * E`, which is a fraction of that. A surfel on open ground
averages many white misses and a few weak colored hits, so the tint is small on top of a white floor.

How to confirm: temporarily set the miss to `float3(0, 0, 0)` and raise `MaxSamples`. If color bleed
shows up, the sky is hiding it. The real fix is a skylight term (the `TODO` already there) that is
dimmer or colored, and it should be on the same scale as the direct lights.

---

## Cause 4: one ray per surfel per frame converges slowly

Each surfel traces one cosine-weighted ray per frame and averages up to `MaxSamples`. The chance that
one ray hits the specific colored surface is small. And a colored hit is only colored once the surfel
at the hit has valid albedo and irradiance, which is a second-order effect that starts at zero.
Right after a refresh or camera move the bleed is genuinely not there yet.

How to confirm: stand still after `r.Surfel.Refresh` and watch the Indirect surfel color settle over
a few hundred frames.

---

## Smaller issues

- **Albedo vs diffuse color.** `SurfelAlbedo` stores raw GBufferC base color, the composite uses
  `GBuffer.DiffuseColor`. They differ for metals. Metals should bounce little diffuse light.
- **Direct light units.** `Light.ColorAndType.rgb` is used with no `PI` or exposure handling while the
  sky is a fixed 1.0. If the light units differ from the sky scale, bounce and sky are mis-balanced
  even when everything else is right.
- **Normal weighting at the hit.** `SampleRadianceAt` weights by `dot(SurfelNormal, HitNormal)` where
  `HitNormal` comes from the coarse global SDF. On thin or curved geometry it can reject valid surfels.

---

## Fix order

1. Cause 1 and 2: give the hit a non-zero albedo when no surfel is near. This is the real fix for
   missing bleed.
2. Cause 3: make the miss color dimmer or a real skylight so bounce is visible against it.
3. Cause 4: more samples per frame or a higher `MaxSamples` once 1 to 3 are fixed.

## Debugging with the surfel colors

Pick **Surfels** in the visualization combo, then **Surfel Color** (`r.Surfel.ColorMode`):

| Mode     | Shows                                         | Use it to check                                    |
|----------|-----------------------------------------------|----------------------------------------------------|
| Hash     | random color per surfel index                 | where surfels exist and how big they are           |
| Albedo   | `SurfelAlbedo` captured at spawn              | whether the colored surface has surfels at all (Cause 1) |
| Indirect | `SurfelIrradiance / PI * GIIntensity`         | whether any color reaches a surfel, and how fast   |

Black on a surface in Albedo mode means no surfel is there, so rays hitting it get zero albedo.
Indirect mode on a floor next to a colored wall should tint toward the wall color. If it is white or grey, work through
Causes 1 to 3.



THis one is still a weird point. Like aren't I adding to much surfel togeher and i looks to blended?

The last step is putting everything togheter. From a certain pixel, using the GBuffer and the camera matrices you can retrieve a world position, using that you can retrieve a grid cell. Using the grid cell index you can retrieve the surfels and sum their radiance to find out how much indirect light is reaching that point.