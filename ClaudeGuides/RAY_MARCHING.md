# Guide: why the irradiance splotches don't accumulate (and how to make them)

This explains what you see now, why it happens, and the changes needed to get a stable,
converging result. Each section says what to change and why. You write the actual lines.

Files involved:
- `Shaders/Private/Surfels/SurfelIrradiance.usf` — the ray-march pass (one thread per surfel)
- `Shaders/Private/Surfels/TextureIrradiance.usf` — resolves surfels to a screen texture
- `Shaders/Private/Surfels/Gather.usf` — spawns surfels
- `Source/Surfel/Private/SurfelSceneViewExtension.cpp` — buffer lifetime

---

## Short answer

The buffer persists between frames, but the shader **overwrites** it every frame with a
single sample instead of **blending** into it:

```hlsl
// SurfelIrradiance.usf, end of MainCS
SurfelIrradiance[SurfelIdx] = TotalIncomingRadiance;
```

Each surfel fires one random ray per frame. So what you see is a one-ray estimate that is
thrown away next frame and replaced by another one-ray estimate. That's why the splotches
flicker and never settle. The C++ side already does the hard part. `SurfelState.SurfelIrradiance`
is registered with `RegisterExternalBuffer` and saved with `ConvertToExternalBuffer`, so last
frame's value is right there in the buffer. The shader just never reads its own previous value.

---

## What "accumulate" means here

Monte Carlo GI with one ray per surfel per frame is only correct **on average**. You need the
mean of many frames:

```
E_N = (1/N) * sum(sample_i)
```

You can do this incrementally without storing all the samples:

```
E_new = lerp(E_old, sample, 1 / N)        // N = samples taken so far, including this one
```

- At `N = 1` the weight is 1, so the first sample fully replaces whatever was there.
- At `N = 100` the new sample only moves the estimate 1%.

If you let `N` grow forever, the result never reacts to lighting changes. The usual fix
(EA SEED's GIBS surfel GI, and Lumen-style radiance caches do the same thing) is to clamp
the weight:

```
alpha = max(1 / N, 1 / MaxSamples)        // e.g. MaxSamples = 32..128
```

Early on it converges fast (true average). After that it becomes an exponential moving average
that can still follow a moving light.

---

## Reason 1 (the main one): no history blend

**Change:** in `MainCS`, read `SurfelIrradiance[SurfelIdx]` before writing, and blend the new
sample into it with the alpha above.

**Why you need a sample count:** `alpha` depends on `N`, and right now there is nowhere to
store it. `SurfelIrradiance` is `RWStructuredBuffer<float3>`. Options:

| Option | How | Trade-off |
|---|---|---|
| Widen to `float4` | rgb = irradiance, a = sample count | Simplest. Change `sizeof(FVector3f)` → `FVector4f` in the C++ buffer desc and the type in **all three** shaders that bind it (`SurfelIrradiance.usf`, `TextureIrradiance.usf`, plus the pass header macros). |
| Use `SurfelNormalAndFlags.w` | Pack count into the flags float | No new memory, but you'll want those bits for real flags (alive, etc.) later. |
| Separate `RWStructuredBuffer<uint>` | Dedicated count buffer | Cleanest separation, one more persistent buffer to manage. |

I'd go with `float4`. It also fixes a quiet problem: `float3` structured buffers have a 12-byte
stride, which some drivers and platforms handle badly. 16 bytes is the safe stride.

---

## Reason 2: new surfels must start with an empty history

Once you blend, a surfel's starting value matters. Two places produce bad starting values:

1. **Fresh buffer is uninitialised.** In `SurfelSceneViewExtension.cpp`, the `else` branch creates
   `Surfel.Irradiance` but only clears `SurfelCounterBuffer`. That's harmless today because
   everything gets overwritten. With blending, garbage memory (or a NaN) gets mixed into every
   later frame and never washes out. **Add an `AddClearUAVPass` for the irradiance buffer**
   next to the counter clear.

2. **Spawned surfels.** `Gather.usf` writes position/radius and normal for the new slot
   (`SurfelPositionAndRadius[SurfelIndex] = ...`) but doesn't touch irradiance. Right now slots
   are only handed out once per refresh, so (1) covers it. But as soon as you recycle surfels
   (despawn and respawn into a freed slot), the new surfel would inherit the old one's history.
   **Either bind the irradiance buffer in the gather pass and write count = 0 at spawn**, or let
   the irradiance pass treat `count == 0` as "replace, don't blend". The `1 / N` formula
   already does that at `N = 1`.

Side note: the refresh/budget-change block at the top of the view extension releases
position, normal and counter but **not** `SurfelState.SurfelIrradiance`. Release it there too,
so a budget change can't pair a new surfel set with the old irradiance buffer.

---

## Reason 3: the signal is mostly constant, so there's little to see converge

Even with accumulation working, the current placeholders limit what you'll see:

- `ComputeDirectLight()` always returns `(0.5, 0.7, 0.2)`. Every surfel gets the same direct
  term no matter where it is.
- A miss adds `float3(1,1,1)` (the comment says red, but it's white).
- The return value of `ComputeDirectLight()` inside `SampleSurfelRadianceAt` is thrown away.

So every frame each surfel lands on one of roughly two values: "ray escaped" ≈ `(1.5,1.7,1.2)`
or "ray hit" ≈ `(0.5,0.7,0.2) + bounce`. Without accumulation that's a binary flicker, which is
the splotchy look. With accumulation it settles to "constant + sky visibility × 1". That is
effectively **ambient occlusion**, and it's a good first test: corners and creases should
darken smoothly. Use it to check the accumulation before plugging in real lights.

---

## Reason 4: energy lost at hit points

`SampleSurfelRadianceAt` returns `0` in two common cases:

- The hit point is **outside the grid**, or
- The hit cell has **no surfel with a matching normal**. Surfels only exist where the camera
  has looked, so off-screen or back-facing geometry has none.

It also only checks **one** cell. A hit near a cell border misses surfels whose radius reaches
across from the neighbouring cell.

These samples count as "black bounce". With accumulation they pull the average darker. This
isn't a bug in accumulation, but it will make the converged result look too dark in places.
Later fixes: fall back to the direct-light term at the hit (you'll need it anyway), or check
the 2×2×2 neighbouring cells.

---

## Reason 5 (subtle): reading and writing the same buffer in one dispatch

`SampleSurfelRadianceAt` reads `SurfelIrradiance[Other]` while other threads in the same
dispatch write `SurfelIrradiance[...]`. Some neighbours give you last frame's value and some
give you this frame's. Today that only adds noise. With accumulation, it gives you
**multi-bounce for free**: each frame adds one more bounce, because hits read surfels that
already contain earlier bounces. To make it deterministic, ping-pong two buffers (read
`Prev`, write `Curr`, swap in `SurfelState`). It's fine to leave this for later, but know it's
there if you see order-dependent shimmer.

---

## Spatial "accumulation" in `TextureIrradiance.usf`

If by "accumulate" you meant that overlapping splotches should **add up** on screen: they don't,
on purpose. The resolve divides by `WeightSum`:

```hlsl
float3 Irradiance = WeightSum > 0.f ? IrradianceSum / WeightSum : 0.f;
```

That's correct. Irradiance is a property of the surface, so two surfels covering the same pixel
should average, not double. Removing the divide would make areas with dense surfels brighter,
which is wrong. Once temporal accumulation is in, neighbouring surfels converge to similar
values and the visible splotch edges fade. That's the real fix for the "splotchy" look.

---

## Order to do it in

1. Clear the irradiance buffer on creation, and release it in the refresh block.
2. Widen `SurfelIrradiance` to `float4` (rgb + sample count) in C++ and every shader that binds it.
3. In `SurfelIrradiance.usf`, read history → `N = count + 1` → `alpha = max(1/N, 1/MaxSamples)` →
   `lerp` → write `float4(result, min(N, MaxSamples))`.
4. Add a CVar for `MaxSamples` so you can watch the effect live (1 = current behaviour).
5. Check: with the placeholder lighting you should get smooth, stable AO-like darkening in
   corners after about a second.
6. Then replace the placeholders with real direct light and sky, and handle surfel recycling (Reason 2.2).
