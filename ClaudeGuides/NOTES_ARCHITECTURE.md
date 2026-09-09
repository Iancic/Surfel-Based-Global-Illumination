# Surfel Plugin — Architecture Primer

This is a from-scratch explanation of *why* the plugin is shaped the way it is,
written against the actual files in this repo. Read top to bottom once, then
use it as a reference.

---

## 1. The three threads you need to know about

Unreal has (at least) three threads that matter here:

- **Game thread** — ticks actors, components, gameplay logic. Blueprints run here.
- **Render thread** — takes a snapshot of "what the game thread wants drawn" and
  issues the actual draw/dispatch commands. This is where almost all of your
  Surfel code runs.
- **RHI thread / GPU** — the render thread doesn't talk to the GPU directly, it
  builds a command list that gets submitted further down. You don't touch this
  layer directly when using RDG (see §4).

The render thread runs **one frame behind** the game thread and is filled by a
queue of commands. Anything you write in `ComputePasses.cpp` that has
`_RenderThread` in the name, or is called from inside an RDG pass, executes on
the render thread — never assume game-thread state (UObjects, actors) is safe
to touch there without an explicit thread-safe handoff.

This is *why* `USurfelSubsystem::Deinitialize()` doesn't just delete the
extension — it first tells it to report itself as inactive, because the render
thread might still have frames queued that reference it.

---

## 2. What a Scene View Extension (SVE) actually is

Normal Unreal content (materials, Niagara, post-process volumes) is a
*data-driven* way to influence rendering — you fill in assets and the renderer
decides how to use them. An **SVE is a code-driven escape hatch**: a C++
object that the renderer itself calls, by name, at fixed points during frame
construction, so you can splice in arbitrary GPU work.

Think of the renderer as running a fixed sequence of stages every frame
(shadow depth, base pass, lighting, post-process chain, etc). At specific
points in that sequence, the renderer loops over **every currently registered
SVE** and calls a specific virtual function on each, if that SVE overrode it.
Your `FComputePasses` class is one such SVE. It currently overrides:

- `SubscribeToPostProcessingPass` — "which post-process stage do you want to
  attach work to, and here's a callback array to add yourself to."
- `PostRenderBasePassDeferred_RenderThread` — "the base pass (GBuffer) just
  finished, here's the scene textures if you want to read/write around it."

There are many more hooks available (see `SceneViewExtension.h` in engine
source) — `PrePostProcessPass_RenderThread`, `PreRenderView_RenderThread`,
`SetupView`, etc. Each corresponds to a different moment in the frame. You
only override the ones you need; the rest are no-ops (that's why most of
`FComputePasses`'s overrides in the header are `{}`).

**Key mental model:** an SVE doesn't run continuously. It's *dormant* until
the renderer reaches a stage it cares about, gets called once (per view, per
frame, per relevant stage), does its work by building onto the frame's
`FRDGBuilder`, and returns.

---

## 3. How the SVE gets registered and kept alive

An SVE is just a C++ object — nothing makes it "active" by existing. Two
things are required:

1. **Registration**: `FSceneViewExtensions::NewExtension<FComputePasses>()`
   (called in `USurfelSubsystem::Initialize`) constructs the object *and*
   registers it with the global `FSceneViewExtensions` registry, which is what
   the renderer actually iterates over each frame. This returns a
   `TSharedPtr`.

2. **Lifetime ownership**: nothing else holds a strong reference to that
   `TSharedPtr` unless you keep one. If it goes out of scope, the object is
   destroyed and (via a weak-pointer mechanism internally) drops out of the
   renderer's iteration automatically. That's why `USurfelSubsystem` exists at
   all — **its entire job is to be a long-lived owner for that `TSharedPtr`.**

This is why the subsystem is a `UEngineSubsystem` and not, say, a
`UWorldSubsystem`: engine subsystems live for the process lifetime (from
`PostConfigInit` until engine shutdown), so your rendering hook survives
across level loads and PIE sessions rather than getting destroyed and
needing re-registration every time a world changes.

`Deinitialize()`'s dance (force `IsActiveThisFrameFunctions` to return
`false`, *then* `Reset()`) exists because the render thread runs a frame
behind — at the moment the game thread decides to shut the subsystem down,
the render thread may already be mid-flight processing a frame that expects
this SVE to be there. Marking it explicitly inactive first is a clean way to
make it a no-op before the pointer actually goes away.

**`SceneViewExtensionUtilities` (the other plugin in this project) is not a
dependency of `Surfel` — it's a reference/template plugin** by a third party
(Ossi Luoto) showing this exact registration/lifetime pattern in isolation,
without any of the surfel-specific logic. Compare
`UTemplateSubsystem`/`FCustomSceneViewExtension` side-by-side with
`USurfelSubsystem`/`FComputePasses` — they're structurally the same thing.
Use it to see the *minimal* version of the pattern when the real plugin's
GI-specific code is too much noise, or to find hooks you haven't used yet
(e.g. it demonstrates `PrePostProcessPass_RenderThread`, which `Surfel`
doesn't currently use).

---

## 4. RDG — the thing every pass is built on

`FRDGBuilder` (Render Dependency Graph) is Unreal's modern rendering
abstraction. Instead of issuing GPU commands immediately and manually
tracking resource state/barriers yourself, you **declare** a graph:

- "I want a texture/buffer with these properties" → `GraphBuilder.CreateTexture` / `CreateBuffer`
- "I want a compute pass that reads X and writes Y" → `FComputeShaderUtils::AddPass(...)`
- Resources are `FRDGTextureRef` / `FRDGBufferRef` — lightweight handles valid
  only within that frame's graph, not persistent GPU objects.

The builder figures out execution order, resource transitions (e.g. "this
texture was just written as a UAV, now it needs to transition to be readable
as an SRV"), and culls unused passes, then compiles it all into actual RHI
commands at the end of the frame. You never issue "draw"/"dispatch" calls
directly — you describe a pass and hand it a callback; RDG invokes the
callback at the right time with the right barriers already in place.

**Persistent data across frames** (like your surfel buffers, which must
survive frame-to-frame, unlike normal RDG resources which are transient) uses
`TRefCountPtr<FRDGPooledBuffer>` stored outside the graph (in your
`FSurfelViewState`), and gets pulled into the current frame's graph via
`GraphBuilder.RegisterExternalBuffer(...)`. This is exactly the pattern your
(currently broken) `PostRenderBasePassDeferred_RenderThread` is reaching for.

---

## 5. How a shader gets defined and registered — step by step

A working shader needs three pieces that must all agree with each other.
Using your **already-working** `FSurfelFullscreenCS` as the reference:

### Step 1 — Virtual shader path mapping (done once, plugin-wide)

```cpp
// Surfel.cpp, FSurfelModule::StartupModule()
AddShaderSourceDirectoryMapping(
    TEXT("/Surfel"),
    IPluginManager::Get().FindPlugin("Surfel")->GetBaseDir() + "/Shaders/Private");
```

This tells the shader compiler "the virtual path `/Surfel/...` means
`<PluginDir>/Shaders/Private/...` on disk." You already have this — you don't
need to touch it again unless you add a second shader root folder.

### Step 2 — Write the `.usf` file (the actual GPU code)

`Shaders/Private/FullscreenCS.usf` declares loose global variables that are
the shader's inputs/outputs:

```hlsl
Texture2D InputSceneColor;
RWTexture2D<float4> OutRenderTarget;
float Intensity;

[numthreads(THREADS_X, THREADS_Y, THREADS_Z)]
void MainCS(uint3 DispatchThreadId : SV_DispatchThreadID) { ... }
```

`THREADS_X/Y/Z` aren't defined in the file — they get injected as
preprocessor `#define`s from C++ (Step 3).

### Step 3 — Declare a matching C++ shader class

```cpp
class FSurfelFullscreenCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FSurfelFullscreenCS);
    SHADER_USE_PARAMETER_STRUCT(FSurfelFullscreenCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputSceneColor)
        SHADER_PARAMETER(float, Intensity)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutRenderTarget)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(...) { ... }
    static void ModifyCompilationEnvironment(...)
    {
        OutEnvironment.SetDefine(TEXT("THREADS_X"), FComputeShaderUtils::kGolden2DGroupSize);
        ...
    }
};
```

Every entry in `BEGIN_SHADER_PARAMETER_STRUCT` must have a **matching global
variable of a compatible type** in the `.usf` file, with the same name. This
struct is the strongly-typed C++ mirror of the shader's bindings — the
compiler cross-checks them. `SHADER_PARAMETER_RDG_TEXTURE_UAV` ↔
`RWTexture2D<float4>`, `SHADER_PARAMETER` (float) ↔ `float`, etc.

### Step 4 — Register the class against the `.usf` file and entry point

```cpp
IMPLEMENT_GLOBAL_SHADER(FSurfelFullscreenCS, "/Surfel/FullscreenCS.usf", "MainCS", SF_Compute);
```

This one line is what actually links everything: "the C++ class
`FSurfelFullscreenCS` compiles from the virtual path
`/Surfel/FullscreenCS.usf`, entry function `MainCS`, and it's a compute
shader." **Without this line the shader class exists in C++ but is never
compiled or usable — this is the step your five stub passes
(`FScatterSurfelPass`, `FGatherSurfelPass`, etc.) are all missing.**

### Step 5 — Dispatch it from render-thread code

```cpp
FSurfelFullscreenCS::FParameters* PassParameters =
    GraphBuilder.AllocParameters<FSurfelFullscreenCS::FParameters>();
PassParameters->InputSceneColor = SceneColor.Texture;
PassParameters->Intensity = CVarSurfelIntensity.GetValueOnRenderThread();
PassParameters->OutRenderTarget = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(OutputTexture));

TShaderMapRef<FSurfelFullscreenCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));

FComputeShaderUtils::AddPass(
    GraphBuilder,
    RDG_EVENT_NAME("Surfel Fullscreen %dx%d", PassSize.X, PassSize.Y),
    ComputeShader,
    PassParameters,
    FComputeShaderUtils::GetGroupCount(PassSize, FComputeShaderUtils::kGolden2DGroupSize));
```

`AllocParameters` gives you a frame-lifetime-scoped struct to fill in.
`TShaderMapRef` looks up the compiled shader for the current feature level.
`FComputeShaderUtils::AddPass` is what actually registers the RDG pass node
— it reads the `FParameters` struct via reflection to know what
resources/barriers this pass needs, and schedules your dispatch.

**Summary of the five things a "real" shader needs, all currently missing for
Scatter/Gather/Irradiance/Composite/Visualize:**
1. Non-empty `.usf` body
2. Non-empty `FParameters` struct matching it
3. `IMPLEMENT_GLOBAL_SHADER(...)` line
4. A dispatch function (`RunXPass`) that fills params and calls `AddPass`
5. A call site that invokes that dispatch function from an SVE hook

---

## 6. How your two working demo passes actually get triggered end-to-end

Full call chain for `r.Surfel.Mode 1`:

1. Every frame, renderer reaches the post-process chain, iterates registered
   SVEs, calls `FComputePasses::SubscribeToPostProcessingPass` once per
   `EPostProcessingPass` stage.
2. Your code checks `PassId == EPostProcessingPass::MotionBlur` — only true
   once per view per frame — then reads `CVarSurfelMode`. If `1`, appends
   `RunFullscreenPass` as a callback into `InOutPassCallbacks`.
3. Later, when the renderer actually executes the MotionBlur stage, it
   invokes every registered callback, including yours, passing the live
   `FRDGBuilder` and `FPostProcessMaterialInputs` for that frame.
4. `RunFullscreenPass` builds an RDG pass as described in §5, mutates the
   scene color texture in place, returns it.
5. Post-process chain continues, unaware anything unusual happened — it just
   sees a `SceneColor` texture, possibly modified.

`r.Surfel.Mode`, `r.Surfel.Intensity`, `r.Surfel.GBufferChannel`,
`r.Surfel.DepthScale` are `TAutoConsoleVariable`s — you can flip these live in
the in-editor console (`` ` `` key) to switch modes without recompiling
anything.

---

## 7. Fast iteration loop

- **Editing `.usf` only** (HLSL logic, no new bindings): with the editor
  running, save the file, then in the in-editor console run
  `recompileshaders changed`. No engine restart. Put
  `r.ShaderDevelopmentMode=1` in your project's `DefaultEngine.ini` under
  `[ConsoleVariables]` once so error messages are verbose and shaders aren't
  aggressively cached.
- **Editing `FParameters`, adding a new shader class, or C++ dispatch logic**:
  needs a C++ recompile. Live Coding (Ctrl+Alt+F11 in most setups, or your
  IDE's hot reload) usually suffices; if you add/remove a `UCLASS`/`USTRUCT`
  (e.g. touch `SurfelSubsystem.h`), you need a full editor restart because UHT
  (Unreal Header Tool) has to regenerate `.generated.h`.
- **Toggle modes without recompiling anything**: console variables
  (`r.Surfel.Mode 2`, etc).
- **Debugging what a pass actually produced**: RenderDoc capture (UE has
  built-in RenderDoc integration under the editor's "Tools" or via
  `r.RenderDoc.CaptureFrame`), or add a debug visualize mode like your
  existing `FSurfelGBufferVisualizeCS` and view it directly on screen.

---

## 8. Suggested reading order in this repo, to build intuition

1. `SceneViewExtensionUtilities/.../CustomSceneViewExtension.h` — minimal SVE
   shape, no domain noise.
2. `Surfel/Source/Surfel/Public/SurfelSubsystem.h` +
   `Private/SurfelSubsystem.cpp` — the lifetime-owner pattern, real version.
3. `Surfel/Source/Surfel/Public/ComputePasses.h` — your SVE's declared
   surface area (what hooks you're using, what state you keep per view).
4. `Surfel/Source/Surfel/Private/ComputePasses.cpp`, top to bottom — shader
   classes first (currently mostly stubs + comments describing the intended
   algorithm), then `SubscribeToPostProcessingPass`, then the two working
   `RunXPass` functions as your template for writing the next one.
5. `Surfel/Shaders/Private/FullscreenCS.usf` and `GBufferVisualizeCS.usf` —
   the only two `.usf` files with real content; read them to see the
   `SCREEN_PASS_TEXTURE_VIEWPORT` / `GlobalPointClampedSampler` idioms used
   for screen-space sampling.
6. `Surfel/Shaders/Private/Surfels/SurfelCommon.usf` — the shared data layout
   (`FSurfel` struct, buffer declarations) intended to be `#include`d by the
   still-empty Scatter/Gather/Irradiance/Composite shaders once you write
   them.
