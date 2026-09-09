# The `PostRenderBasePassDeferred_RenderThread` body — what to actually write

You already declared `FSurfelViewState` and the hook in `ComputePasses.h`. The gap is the function
body. This document is that body, written the way Unreal expects, with each block explained.

Read it top to bottom once, then paste it in sections. Everything follows the same idioms your working
`RunGBufferPass` already uses, so nothing here is a new pattern except the persistent buffers.

---

## The shape of every UE render pass

Every RDG pass you will ever write is the same five moves. Your `RunGBufferPass` does exactly this, and
so will these:

1. **Get your resources** — textures and buffers, either handed to you or created.
2. **Allocate a parameter struct** with `GraphBuilder.AllocParameters<T>()`.
3. **Fill every field** on that struct. Leaving one null is a crash, not a warning.
4. **Grab the shader** with `TShaderMapRef<T>`.
5. **Add the pass** with `FComputeShaderUtils::AddPass`, passing a group count.

Once that clicks, the rest is bookkeeping. The only genuinely new idea below is persistent buffers,
which is step 0: importing memory that survived from last frame.

---

## Part 1: CVars

Put these at the top of `ComputePasses.cpp`, next to your four existing ones.

```cpp
static TAutoConsoleVariable<int32> CVarSurfelEnable(
    TEXT("r.Surfel.Enable"),
    0,  // default off so you opt in while developing
    TEXT("Enable the surfel scatter/gather passes."),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSurfelBudget(
    TEXT("r.Surfel.Budget"),
    65536,
    TEXT("Maximum number of surfels."),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarSurfelRadius(
    TEXT("r.Surfel.Radius"),
    25.0f,
    TEXT("Surfel radius in world units."),
    ECVF_RenderThreadSafe);
```

---

## Part 2: The gather shader class

Add this next to your existing shader classes. Compare it to `FSurfelGBufferVisualizeCS` — the shape is
identical, only the parameters differ.

```cpp
class FSurfelGatherCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FSurfelGatherCS);
    SHADER_USE_PARAMETER_STRUCT(FSurfelGatherCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // RWStructuredBuffer -> we write surfels into these.
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelPositionAndRadius)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelNormalAndFlags)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, SurfelCounter)

        // These names MUST match what DeferredShadingCommon.ush declares internally.
        // That is why your GBufferVisualizeCS.usf does not declare them itself.
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferATexture)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferBTexture)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferCTexture)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferDTexture)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferETexture)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferFTexture)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)

        // Needed by ConvertFromDeviceZ and SvPositionToTranslatedWorld.
        SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

        SHADER_PARAMETER(FUintVector2, ViewRectMin)
        SHADER_PARAMETER(FUintVector2, ViewRectSize)
        SHADER_PARAMETER(uint32, SurfelBudget)
        SHADER_PARAMETER(float, SurfelRadius)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("THREADS_X"), 16);
        OutEnvironment.SetDefine(TEXT("THREADS_Y"), 16);
        OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
    }
};

// WITHOUT THIS LINE the shader is never compiled and the pass silently does nothing.
// This is the single easiest mistake to make here - all six of your stub classes are
// currently missing it.
IMPLEMENT_GLOBAL_SHADER(FSurfelGatherCS, "/Surfel/Surfels/Gather.usf", "MainCS", SF_Compute);
```

---

## Part 3: The function body

This is the part you asked for. Written out in full, in order.

```cpp
void FComputePasses::PostRenderBasePassDeferred_RenderThread(
    FRDGBuilder& GraphBuilder,
    FSceneView& InView,
    const FRenderTargetBindingSlots& RenderTargets,
    TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
    // ---- Guard clauses -------------------------------------------------------
    // Bail out early and often. InView.State is null for thumbnail renders,
    // reflection captures and similar, and it is our per-view state key, so we
    // cannot do anything useful without it.
    if (CVarSurfelEnable.GetValueOnRenderThread() == 0)
    {
        return;
    }
    if (!InView.State || !SceneTextures)
    {
        return;
    }

    // Unwrap the scene textures. GetContents() gives the struct the renderer filled
    // in - same call your RunGBufferPass makes, just reached differently because
    // this hook hands us the uniform buffer directly.
    const FSceneTextureUniformParameters& Textures = *SceneTextures->GetContents();
    if (!Textures.GBufferATexture || !Textures.SceneDepthTexture)
    {
        return;  // forward shading or mobile: no deferred G-Buffer to read
    }

    // ---- Find or create this view's persistent state -------------------------
    const uint32 ViewKey = InView.State->GetViewKey();
    const uint32 Budget  = (uint32)FMath::Max(1024, CVarSurfelBudget.GetValueOnRenderThread());

    FSurfelViewState& State = ViewStates.FindOrAdd(ViewKey);

    // If the budget changed at runtime, throw the old buffers away. Assigning a
    // fresh struct drops the ref counts, which frees the old allocations.
    if (State.Budget != Budget)
    {
        State = FSurfelViewState();
        State.Budget = Budget;
    }

    // ---- Import persistent buffers into this frame's graph --------------------
    // RDG buffers are graph-scoped: CreateBuffer gives you memory for THIS FRAME
    // ONLY, recycled once the graph runs. To keep surfels across frames we hold a
    // ref-counted pointer ourselves and re-register it every frame.
    //
    // A null pointer is our "first frame" signal, so no separate bool is needed.
    const bool bFirstFrame = !State.SurfelPositionAndRadius.IsValid();

    FRDGBufferRef PositionAndRadius;
    FRDGBufferRef NormalAndFlags;
    FRDGBufferRef Counter;

    if (bFirstFrame)
    {
        PositionAndRadius = GraphBuilder.CreateBuffer(
            FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), Budget),
            TEXT("Surfel.PositionAndRadius"));

        NormalAndFlags = GraphBuilder.CreateBuffer(
            FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), Budget),
            TEXT("Surfel.NormalAndFlags"));

        // NOTE: CreateBufferDesc, not CreateStructuredDesc. The counter is bound as
        // RWBuffer<uint> (a typed buffer), which needs a plain vertex-buffer style
        // desc plus a pixel format on the UAV. Structured and typed buffers are not
        // interchangeable - mismatching them here is a common early crash.
        Counter = GraphBuilder.CreateBuffer(
            FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1),
            TEXT("Surfel.Counter"));

        // Pooled memory is RECYCLED memory - it contains whatever the previous
        // owner left behind. Without this clear the flags field reads as thousands
        // of live surfels at random positions. If your first frame shows a storm
        // of surfels, this clear is what is missing.
        AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(NormalAndFlags), 0u);
        AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Counter, PF_R32_UINT), 0u);
    }
    else
    {
        // Re-register last frame's allocations with this frame's graph.
        PositionAndRadius = GraphBuilder.RegisterExternalBuffer(
            State.SurfelPositionAndRadius, TEXT("Surfel.PositionAndRadius"));
        NormalAndFlags = GraphBuilder.RegisterExternalBuffer(
            State.SurfelNormalAndFlags, TEXT("Surfel.NormalAndFlags"));
        Counter = GraphBuilder.RegisterExternalBuffer(
            State.SurfelCounter, TEXT("Surfel.Counter"));
    }

    // Groups all our passes under one label in RenderDoc and Unreal's GPU profiler.
    RDG_EVENT_SCOPE(GraphBuilder, "Surfel");

    // ---- The gather pass -----------------------------------------------------
    {
        FSurfelGatherCS::FParameters* PassParameters =
            GraphBuilder.AllocParameters<FSurfelGatherCS::FParameters>();

        // CreateUAV on a buffer we intend to write; RDG uses these declarations to
        // work out barriers and pass ordering for us.
        PassParameters->SurfelPositionAndRadius = GraphBuilder.CreateUAV(PositionAndRadius);
        PassParameters->SurfelNormalAndFlags    = GraphBuilder.CreateUAV(NormalAndFlags);
        PassParameters->SurfelCounter =
            GraphBuilder.CreateUAV(Counter, PF_R32_UINT);

        // Some G-Buffer slots are null depending on project settings. Bind a black
        // dummy rather than leaving them null - a null binding is a crash.
        FRDGTextureRef BlackDummy = GSystemTextures.GetBlackDummy(GraphBuilder);

        PassParameters->GBufferATexture = Textures.GBufferATexture;
        PassParameters->GBufferBTexture = Textures.GBufferBTexture ? Textures.GBufferBTexture : BlackDummy;
        PassParameters->GBufferCTexture = Textures.GBufferCTexture ? Textures.GBufferCTexture : BlackDummy;
        PassParameters->GBufferDTexture = Textures.GBufferDTexture ? Textures.GBufferDTexture : BlackDummy;
        PassParameters->GBufferETexture = Textures.GBufferETexture ? Textures.GBufferETexture : BlackDummy;
        PassParameters->GBufferFTexture = Textures.GBufferFTexture ? Textures.GBufferFTexture : BlackDummy;
        PassParameters->SceneDepthTexture = Textures.SceneDepthTexture;

        PassParameters->View = InView.ViewUniformBuffer;

        // ViewRect is where THIS view lives inside a possibly larger render target
        // (split screen, editor viewports). Passing it explicitly and offsetting by
        // ViewRectMin in the shader is what keeps this correct under screen
        // percentage and TSR - the bug your GBufferVisualizeCS.usf:58 currently has.
        PassParameters->ViewRectMin  = FUintVector2(InView.ViewRect.Min.X, InView.ViewRect.Min.Y);
        PassParameters->ViewRectSize = FUintVector2(InView.ViewRect.Width(), InView.ViewRect.Height());
        PassParameters->SurfelBudget = Budget;
        PassParameters->SurfelRadius = CVarSurfelRadius.GetValueOnRenderThread();

        TShaderMapRef<FSurfelGatherCS> ComputeShader(GetGlobalShaderMap(InView.GetFeatureLevel()));

        // One thread per pixel, 16x16 per group. GetGroupCount rounds up, which is
        // why the shader still bounds-checks against ViewRectSize.
        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("Surfel Gather"),
            ComputeShader,
            PassParameters,
            FComputeShaderUtils::GetGroupCount(InView.ViewRect.Size(), FIntPoint(16, 16)));
    }

    // ---- Export buffers so they survive to next frame ------------------------
    // ConvertToExternalBuffer tells the pool not to recycle this memory, and
    // storing the returned pointer is what actually keeps it alive. Forget this
    // and your surfels vanish every frame.
    State.SurfelPositionAndRadius = GraphBuilder.ConvertToExternalBuffer(PositionAndRadius);
    State.SurfelNormalAndFlags    = GraphBuilder.ConvertToExternalBuffer(NormalAndFlags);
    State.SurfelCounter           = GraphBuilder.ConvertToExternalBuffer(Counter);

    // Stamp for eviction below. FrameNumber is the render-thread-safe copy of
    // GFrameNumber, already on the view family.
    State.LastFrameSeen = InView.Family ? InView.Family->FrameNumber : 0;

    // ---- Evict dead views ----------------------------------------------------
    // Our SceneViewExtension is owned by an UEngineSubsystem, so it outlives worlds
    // and PIE sessions. Without this, every PIE run and every closed viewport
    // leaves a multi-megabyte pool alive by ref count for the rest of the session.
    const uint32 CurrentFrame = State.LastFrameSeen;
    for (auto It = ViewStates.CreateIterator(); It; ++It)
    {
        if (CurrentFrame - It.Value().LastFrameSeen > 60)
        {
            It.RemoveCurrent();
        }
    }
}
```

Add these includes at the top of `ComputePasses.cpp`:

```cpp
#include "RenderGraphBuilder.h"
#include "SceneRendering.h"      // not needed if it already compiles without it
#include "GlobalShader.h"
```

You already include `RenderGraphUtils.h` (for `AddClearUAVPass`) and `SystemTextures.h` (for
`GSystemTextures`), so those are covered.

---

## Part 4: The gather shader

`Shaders/Private/Surfels/Gather.usf`. Currently 0 bytes.

```hlsl
#include "/Engine/Public/Platform.ush"
#include "/Engine/Private/Common.ush"
#include "/Engine/Private/DeferredShadingCommon.ush"

RWStructuredBuffer<float4> SurfelPositionAndRadius;
RWStructuredBuffer<float4> SurfelNormalAndFlags;
RWBuffer<uint>             SurfelCounter;

uint2  ViewRectMin;
uint2  ViewRectSize;
uint   SurfelBudget;
float  SurfelRadius;

[numthreads(THREADS_X, THREADS_Y, THREADS_Z)]
void MainCS(uint3 DispatchThreadId : SV_DispatchThreadID,
            uint  GroupIndex       : SV_GroupIndex)
{
    // Milestone 1: spawn one surfel per 16x16 tile, at the tile's top-left pixel.
    // No coverage input yet - that arrives with the scatter pass, and turns this
    // from "spawn everywhere" into "spawn only where there are gaps".
    if (GroupIndex != 0)
    {
        return;
    }

    const uint2 LocalPos = DispatchThreadId.xy;
    if (any(LocalPos >= ViewRectSize))
    {
        return;  // GetGroupCount rounded up past the view, so bounds-check
    }

    // Offset into the G-Buffer. ViewRectMin is why this survives split screen and
    // screen percentage.
    const uint2 SamplePos = LocalPos + ViewRectMin;

    const float DeviceZ = SceneDepthTexture.Load(int3(SamplePos, 0)).r;
    if (DeviceZ <= 0.0f)
    {
        return;  // sky - nothing to attach a surfel to
    }

    // Reserve a slot. InterlockedAdd hands each thread a unique index even though
    // hundreds of tiles run at once. The counter CAN exceed the budget when many
    // tiles spawn in one frame, so always clamp when reading it back.
    uint Index;
    InterlockedAdd(SurfelCounter[0], 1, Index);
    if (Index >= SurfelBudget)
    {
        return;
    }

    // Translated world space: positions relative to the camera. This avoids UE's
    // large-world-coordinate machinery entirely, because in 5.8 the absolute
    // PreViewTranslation is a split high/low double and does not fit a float3.
    const float3 TranslatedPos =
        SvPositionToTranslatedWorld(float4(SamplePos + 0.5f, DeviceZ, 1.0f));

    // Decode via DecodeGBufferData rather than reading GBufferA by hand: normal
    // encoding differs between octahedral and non-octahedral builds and this
    // handles both. Your GBufferVisualizeCS.usf makes the identical call.
    const float SceneDepth = ConvertFromDeviceZ(DeviceZ);
    FGBufferData GBuffer = DecodeGBufferData(
        GBufferATexture.Load(int3(SamplePos, 0)),
        GBufferBTexture.Load(int3(SamplePos, 0)),
        GBufferCTexture.Load(int3(SamplePos, 0)),
        GBufferDTexture.Load(int3(SamplePos, 0)),
        GBufferETexture.Load(int3(SamplePos, 0)),
        GBufferFTexture.Load(int3(SamplePos, 0)),
        (float4)0, 0.0f, 0u, SceneDepth, true, false);

    SurfelPositionAndRadius[Index] = float4(TranslatedPos, SurfelRadius);
    SurfelNormalAndFlags[Index]    = float4(GBuffer.WorldNormal, asfloat(1u));  // bit 0 = alive
}
```

Do not declare the G-Buffer textures in this file. `DeferredShadingCommon.ush` already declares them,
and redeclaring is a compile error. Your own visualizer has a comment saying exactly this.

---

## Part 5: Prove it works before writing anything else

Nothing renders yet, so verify with logging first.

```cpp
// Temporary, right after AddPass. Confirms the pass is reached at all.
UE_LOG(LogTemp, Log, TEXT("Surfel: gather dispatched, budget %u"), Budget);
```

Then in the editor:

```
r.ShaderDevelopmentMode 1
r.Surfel.Enable 1
```

If you edit the `.usf` afterwards, run `recompileshaders changed`. Live Coding handles C++ only and
never picks up shader edits, which catches people out constantly.

What to check, in order:

1. **It compiles and the log line fires.** If not, the hook is not being called: confirm the plugin
   loaded and `r.Surfel.Enable` is 1.
2. **Shader compiles.** Errors appear in the output log on first dispatch. A missing
   `IMPLEMENT_GLOBAL_SHADER` produces no error at all — just a pass that does nothing.
3. **Buffers survive frames.** Run `r.RDG.Debug 1`. Complaints about uninitialized buffers mean the
   first-frame clear is not happening.
4. **The counter climbs.** You cannot see it yet, which is why the visualize pass comes next.

---

## What comes next, in order

1. **Visualize** — read the counter back and draw surfel discs, so you can finally see something. Build
   the count readout first using `ShaderPrintParameters.h` from `Renderer/Public`.
2. **Rebase** — a one-line-per-surfel pass that shifts positions when the camera's translation changes.
   Without it surfels slide against geometry as you fly, which looks like broken projection.
3. **Scatter** — project surfels into a coverage texture and feed it to gather, turning blind
   per-tile spawning into real hole-filling.

Then irradiance and composite, which is where the actual global illumination starts.
