# Guide: implementing the gather pass

This is a guide, not code to paste. Each section says what a function needs to do and why, in the
order you should write them. You write the actual lines.

Reference while you work: your existing `RunGBufferPass` in `ComputePasses.cpp` already does steps 1-5
below correctly for a different pass. Every new function follows the same shape.

---

## The shape every RDG pass follows

1. Get your resources — textures/buffers you're handed, or ones you create.
2. Allocate a parameter struct: `GraphBuilder.AllocParameters<T>()`.
3. Fill in every field on it. A field left null is a crash, not a warning.
4. Get the shader: `TShaderMapRef<T> ComputeShader(GetGlobalShaderMap(FeatureLevel))`.
5. `FComputeShaderUtils::AddPass(...)`.

Keep this list next to you. Everything below is this list applied five times, plus one new idea:
buffers that need to survive between frames.

---

## Part 1: two new CVars

Next to your four existing ones at the top of `ComputePasses.cpp`, add:

- `r.Surfel.Enable` — int, default `0`. Master switch so surfel work stays off until you turn it on.
- `r.Surfel.Budget` — int, default `65536`. Max number of surfels the pool can hold.

Copy the shape of `CVarSurfelIntensity` exactly — same `TAutoConsoleVariable` pattern, same
`ECVF_RenderThreadSafe` flag.

---

## Part 2: the gather shader class (C++ side)

Add a new `FGlobalShader` class next to `FSurfelGBufferVisualizeCS`. Call it `FSurfelGatherCS`.

**What its parameter struct needs**, and why each one is there:

| Field | Macro | Why |
|---|---|---|
| `SurfelPositionAndRadius` | `SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, ...)` | We write new surfels here. `xyz` = position, `w` = radius. |
| `SurfelNormalAndFlags` | same, `RWStructuredBuffer<float4>` | `xyz` = normal, `w` = flags (bit 0 = alive). |
| `SurfelCounter` | `SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, ...)` | One `uint`: how many surfels exist. `InterlockedAdd` claims a slot. |
| `GBufferATexture` .. `GBufferFTexture` | `SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ...)` | Needed by `DecodeGBufferData`. Bind all six even though you only read normals — the decode function reads all of them. |
| `SceneDepthTexture` | `SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ...)` | To find where a surfel should spawn. |
| `View` | `SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)` | Needed by `ConvertFromDeviceZ` and `SvPositionToTranslatedWorld` in the shader. |
| `ViewRectMin`, `ViewRectSize` | `SHADER_PARAMETER(FUintVector2, ...)` | See "the ViewRect bug" below. |
| `SurfelBudget` | `SHADER_PARAMETER(uint32, ...)` | So the shader can refuse to write past the buffer's end. |
| `SurfelRadius` | `SHADER_PARAMETER(float, ...)` | From the CVar you're about to add. |

`ShouldCompilePermutation` and `ModifyCompilationEnvironment`: copy them verbatim from
`FSurfelGBufferVisualizeCS`, but set `THREADS_X = 16`, `THREADS_Y = 16`, `THREADS_Z = 1` — this is a 2D
per-pixel-tile dispatch.

**Then add the line every one of your six stub shader classes is currently missing:**

```
IMPLEMENT_GLOBAL_SHADER(FSurfelGatherCS, "/Surfel/Surfels/Gather.usf", "MainCS", SF_Compute);
```

Without this exact macro call the shader is never compiled, and the pass silently does nothing — no
error, no warning, just zero effect. This is the single most likely thing to trip you up.

---

## Part 3: `PostRenderBasePassDeferred_RenderThread` — what goes in it

You already declared this override and `FSurfelViewState` in the header. Here is what the body needs to
do, in order. Each bullet is one job; write it as its own block so you can test incrementally.

### 3.1 — Guard clauses

Return early if:
- `CVarSurfelEnable.GetValueOnRenderThread() == 0`
- `!InView.State` (true for thumbnail renders, reflection captures, etc. — you have no per-view key
  without it)
- `!SceneTextures` (the uniform buffer wasn't built this frame)

### 3.2 — Unwrap scene textures

`SceneTextures` is a `TRDGUniformBufferRef`, not the struct itself. Get the real data with
`*SceneTextures->GetContents()`. This gives you an `FSceneTextureUniformParameters` — the same type
`RunGBufferPass` already unwraps, just handed to you directly instead of via
`Inputs.SceneTextures.SceneTextures.GetUniformBuffer()`.

Check `GBufferATexture` and `SceneDepthTexture` are non-null before continuing; they're null on
forward-shading or mobile paths where there's no deferred G-Buffer.

### 3.3 — Find or create this view's state

- Get the key: `InView.State->GetViewKey()`.
- `FSurfelViewState& State = ViewStates.FindOrAdd(ViewKey);`
- Read the budget from the CVar (clamp to something sane, e.g. `FMath::Max(1024, ...)`).
- If `State.Budget != Budget`, the CVar changed at runtime — reset `State` to a fresh
  `FSurfelViewState()` and set the new budget. Assigning a fresh struct drops the old ref-counted
  pointers, which frees the old GPU memory automatically.

### 3.4 — Import or create the three buffers

**Why this step exists at all:** RDG buffers from `CreateBuffer` only live for the current frame's
graph — the memory gets recycled once the graph executes. To keep surfel data across frames, you hold
onto a `TRefCountPtr<FRDGPooledBuffer>` yourself (that's what's already sitting in `FSurfelViewState`)
and re-register it with each new frame's graph.

Test whether this is the first frame by checking if `State.SurfelPositionAndRadius.IsValid()` — a null
pointer means nothing was ever allocated, so this is frame one for this view. No separate boolean flag
needed.

**If first frame — create:**
- `SurfelPositionAndRadius` and `SurfelNormalAndFlags`: `GraphBuilder.CreateBuffer` with
  `FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), Budget)`.
- `SurfelCounter`: `GraphBuilder.CreateBuffer` with `FRDGBufferDesc::CreateBufferDesc(sizeof(uint32),
  1)`. **Important:** this one uses `CreateBufferDesc`, not `CreateStructuredDesc` — the counter is
  bound as a typed `RWBuffer<uint>` in the shader, not a structured buffer, and those two buffer kinds
  are not interchangeable. Mixing them up is a common early crash.
- Then clear `SurfelNormalAndFlags` and `SurfelCounter` to zero with `AddClearUAVPass` (counter needs
  the UAV created with `PF_R32_UINT` as the format argument). **Do not skip this.** Pooled GPU memory is
  recycled memory — it holds whatever the previous owner left in it. An uncleared flags buffer reads as
  thousands of surfels already marked alive at garbage positions. If your very first frame shows a
  storm of surfels, this clear is what's missing.

**If not first frame — import:**
- `GraphBuilder.RegisterExternalBuffer(State.SurfelPositionAndRadius, TEXT("..."))` for each of the
  three, reusing last frame's allocation.

### 3.5 — Run the gather pass

This is the five-step shape from the top of this guide:
- `AllocParameters<FSurfelGatherCS::FParameters>()`.
- Fill the two surfel buffers and the counter with `GraphBuilder.CreateUAV(...)` (counter needs
  `PF_R32_UINT` passed as the second argument, same as the clear above).
- Fill the six G-Buffer texture fields. For any that are null on `SceneTextures`, fall back to
  `GSystemTextures.GetBlackDummy(GraphBuilder)` — copy this pattern straight from `RunGBufferPass`.
- `PassParameters->View = InView.ViewUniformBuffer;`
- `ViewRectMin` / `ViewRectSize` from `InView.ViewRect` (see the bug note below — get this right).
- `SurfelBudget` and `SurfelRadius` from the CVars.
- `TShaderMapRef<FSurfelGatherCS> ComputeShader(GetGlobalShaderMap(InView.GetFeatureLevel()));`
- `FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Surfel Gather"), ComputeShader,
  PassParameters, FComputeShaderUtils::GetGroupCount(InView.ViewRect.Size(), FIntPoint(16, 16)));`

Wrap the whole pass in `RDG_EVENT_SCOPE(GraphBuilder, "Surfel")` so it's grouped under one label in
RenderDoc / the GPU profiler, matching what your existing passes already do.

### 3.6 — Export the buffers back out

`ConvertToExternalBuffer` tells the pool "don't recycle this," and returns the pointer you need to keep:

```
State.SurfelPositionAndRadius = GraphBuilder.ConvertToExternalBuffer(PositionAndRadius);
```

Do this for all three buffers. **This line is what actually keeps surfels alive between frames** — if
you forget it, the pool recycles the memory the moment this graph finishes and your surfels vanish
every frame without any error telling you why.

Also stamp `State.LastFrameSeen = InView.Family ? InView.Family->FrameNumber : 0;` — you need this for
the next step.

### 3.7 — Evict stale view states

Your `FComputePasses` is owned by a `UEngineSubsystem`, so it outlives individual worlds and PIE
sessions. Without cleanup, every PIE run and every closed editor viewport leaves its surfel buffers
alive forever, held by the ref count in `ViewStates`.

Loop over `ViewStates` and remove any entry whose `LastFrameSeen` is more than ~60 frames behind the
current one. `TMap::CreateIterator()` + `It.RemoveCurrent()` is the standard way to erase while
iterating.

---

## Part 4: the gather shader (HLSL side) — `Shaders/Private/Surfels/Gather.usf`

Currently 0 bytes. What it needs to do, one thread group at a time (milestone version — no coverage
input yet, just spawn blindly so you can see *something*):

1. **Includes:** `/Engine/Public/Platform.ush`, `/Engine/Private/Common.ush`,
   `/Engine/Private/DeferredShadingCommon.ush`. Do **not** declare the G-Buffer textures yourself —
   `DeferredShadingCommon.ush` already declares them, using the exact same names your C++ parameter
   struct binds to. Redeclaring is a compile error. Your existing `GBufferVisualizeCS.usf` has a comment
   explaining this same thing.

2. **Declare the loose parameters** matching your C++ struct one-for-one: the two
   `RWStructuredBuffer<float4>`, the `RWBuffer<uint>` counter, `ViewRectMin`/`ViewRectSize` as `uint2`,
   `SurfelBudget` as `uint`, `SurfelRadius` as `float`.

3. **`MainCS` entry point**, `[numthreads(THREADS_X, THREADS_Y, THREADS_Z)]`:
   - For this milestone, only let one thread per group (`SV_GroupIndex == 0`) do anything, so you get
     roughly one surfel per 16x16 tile instead of one per pixel.
   - Compute `SamplePos = DispatchThreadId.xy + ViewRectMin`. Bounds-check against `ViewRectSize` and
     bail if out of range — `GetGroupCount` rounds up, so some threads are past the real view.
   - Load `DeviceZ` from `SceneDepthTexture` at `SamplePos`. If it's `<= 0`, that's sky — return without
     spawning.
   - Reserve a slot: `InterlockedAdd(SurfelCounter[0], 1, Index)`. Check `Index >= SurfelBudget` and
     bail if so — the counter can overshoot the budget when many tiles spawn in the same frame, so
     never trust it without clamping.
   - Reconstruct position with `SvPositionToTranslatedWorld(float4(SamplePos + 0.5f, DeviceZ, 1.0f))`.
     This gives you **translated world space** — position relative to the camera, not absolute world
     space. See the note below on why that matters.
   - Get the normal via `DecodeGBufferData(...)` with all six G-Buffer loads, exactly as
     `GBufferVisualizeCS.usf` already does it, then read `.WorldNormal` off the result. Don't hand-decode
     `GBufferA` yourself — the encoding differs between octahedral and non-octahedral project settings,
     and `DecodeGBufferData` already handles both.
   - Write `SurfelPositionAndRadius[Index] = float4(TranslatedPos, SurfelRadius)` and
     `SurfelNormalAndFlags[Index] = float4(WorldNormal, asfloat(1u))` (the `1u` in the `w` slot is your
     "alive" flag, bit 0).

---

## Two things that will bite you if you skip them

**Why translated world space, not absolute world space.** In UE 5.8 there's no plain `float3`
`PreViewTranslation` on the view uniform — it's split into `PreViewTranslationHigh` /
`PreViewTranslationLow` and reassembled as a double-precision value. Storing raw absolute world
positions in a `float3` loses precision at world scale, not just inconvenient. Translated world space
(camera-relative) sidesteps this completely, and it's exactly what `SvPositionToTranslatedWorld`
already returns. The cost you take on: stored positions must be shifted whenever the camera's
translation changes, which is a separate small pass you'll add later (not needed for this milestone
since nothing persists meaningfully yet, but the buffer layout is already correct for it).

**The `ViewRectMin` bug to avoid repeating.** Look at line 58 of your own
`Shaders/Private/GBufferVisualizeCS.usf` — it indexes the G-Buffer using the raw dispatch thread ID with
no offset. That's only correct when the viewport starts at (0,0), which breaks under split-screen,
non-full screen percentage, or TSR upscaling. Passing `ViewRectMin` explicitly and adding it to every
G-Buffer sample in `Gather.usf`, as described above, is what avoids inheriting that same bug.

---

## Verifying it before moving on

Don't write the visualize pass yet — prove gather works first with logging.

1. Add a temporary `UE_LOG(LogTemp, Log, TEXT("Surfel: gather ran, budget %u"), Budget);` right after
   `AddPass` in the C++ function.
2. In the editor console: `r.ShaderDevelopmentMode 1`, then `r.Surfel.Enable 1`.
3. Confirm the log line prints. If it doesn't, the hook isn't firing — check the plugin loaded and the
   CVar is actually 1.
4. If it prints but nothing else seems to happen, check the output log for shader compile errors on
   first dispatch. A **missing** `IMPLEMENT_GLOBAL_SHADER` produces no error at all — the pass just
   silently does nothing — so if there's no error and no visible effect, that macro is the first thing
   to check.
5. Run with `r.RDG.Debug 1`. Any complaint about reading an uninitialized buffer means the first-frame
   clear (step 3.4) isn't happening.
6. You still can't *see* surfels yet — that's the next pass (visualize), which reads the counter and
   buffers back and draws something on screen.

If you edit `Gather.usf` after this, run `recompileshaders changed` in the editor console. Live Coding
only picks up C++ changes, never `.usf` edits — a very common source of "I changed the shader and
nothing happened."

---

## What comes after gather works

1. **Visualize** — read `SurfelCounter` and draw discs for each surfel in `SurfelPositionAndRadius`, so
   you can finally see something on screen. Start with just printing the count via
   `Renderer/Public/ShaderPrintParameters.h` before drawing geometry — it's the cheapest way to confirm
   gather is spawning anything at all.
2. **Rebase** — shift stored positions when the camera's translation changes, so surfels don't slide
   as you fly around. One pass, one line of math per surfel.
3. **Scatter** — project each surfel to screen and write a coverage texture, so gather can stop spawning
   blindly and start filling actual gaps.
