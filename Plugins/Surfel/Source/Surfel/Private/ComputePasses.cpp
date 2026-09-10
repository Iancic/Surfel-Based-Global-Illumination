#include "ComputePasses.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "SceneViewExtension.h"
#include "ScreenPass.h"
#include "SystemTextures.h"
#include "PostProcess/PostProcessMaterialInputs.h"

// CVar Utilities
static TAutoConsoleVariable<int32> CVarSurfelMode(
	TEXT("r.Surfel.Mode"),
	0,
	TEXT("Surfel compute pass mode.\n")
	TEXT(" 0: off\n")
	TEXT(" 1: fullscreen procedural example\n")
	TEXT(" 2: G-Buffer visualization"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarSurfelIntensity(
	TEXT("r.Surfel.Intensity"),
	1.0f,
	TEXT("Blend strength of the fullscreen example effect (0-1)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSurfelGBufferChannel(
	TEXT("r.Surfel.GBufferChannel"),
	2,
	TEXT("Which G-Buffer channel to visualize.\n")
	TEXT(" 0: SceneColor  1: BaseColor  2: WorldNormal  3: Metallic\n")
	TEXT(" 4: Roughness   5: Specular   6: Depth        7: ShadingModel"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarSurfelDepthScale(
	TEXT("r.Surfel.DepthScale"),
	10000.0f,
	TEXT("Far distance in world units mapped to white in depth visualization."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int> CVarSurfelEnable(
	TEXT("r.Surfel.Enable"),
	0,
	TEXT("Is surfel system enabled or not."),
	ECVF_RenderThreadSafe
	);

static TAutoConsoleVariable<int> CVarSurfelBudget(
	TEXT("r.Surfel.Budget"),
	50000,
	TEXT("Surfel Budget."),
	ECVF_RenderThreadSafe
	);

static TAutoConsoleVariable<int> CVarSurfelRadius(
	TEXT("r.Surfel.Radius"),
	15,
	TEXT("Surfel Radius, in world units (cm)."),
	ECVF_RenderThreadSafe
	);

static TAutoConsoleVariable<float> CVarSurfelDebugRadiusScale(
	TEXT("r.Surfel.DebugRadiusScale"),
	0.25f,
	TEXT("Fraction of the true surfel radius drawn by the fullscreen debug visualization (Mode 1).\n")
	TEXT("Keeps r.Surfel.Radius meaningful for spawning/coverage while keeping individual surfels visually distinguishable."),
	ECVF_RenderThreadSafe
	);

static TAutoConsoleVariable<int> CVarSurfelGridSize(
	TEXT("r.Surfel.GridSize"),
	32,
	TEXT("Gather dispatches one thread per NxN pixel block instead of per pixel, spacing spawned surfels out on a coarse screen-space grid.\n")
	TEXT("Stopgap until the Scatter coverage pass is implemented."),
	ECVF_RenderThreadSafe
	);

// TEMP (this session): stand-in for Scatter's coverage texture. See CoverageRadius
// comment in Gather.usf for why this exists and when to remove it.
static TAutoConsoleVariable<float> CVarSurfelCoverageRadius(
	TEXT("r.Surfel.CoverageRadius"),
	40.0f,
	TEXT("TEMP stand-in for Scatter coverage: world-space distance (cm) under which Gather considers a spot already covered by an existing surfel and skips spawning.\n")
	TEXT("Lets old surfels persist and budget only get spent on newly-revealed geometry (e.g. after moving the camera). Remove once Scatter provides real coverage."),
	ECVF_RenderThreadSafe
	);

// Bumped by r.Surfel.Refresh; Gather clears and respawns from scratch when this changes.
static int32 GSurfelRefreshRequestId = 0;

static FAutoConsoleCommand CVarSurfelRefreshCmd(
	TEXT("r.Surfel.Refresh"),
	TEXT("Clears all spawned surfels so they respawn from scratch on the current grid."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		++GSurfelRefreshRequestId;
	}));

/**
 * note: explaining nomenclature
 * called scatter because one surfel writes to many pixels
 * gather because one pixel from many in a tile finds the worst value
 *
 * Scatter answers this question:
 * Where on the screen are no surfels? So I know where to spawn more
 * To store this I use a coverage texture used in the gather step (where surfels get spawned)
 * Scatter is a screen space pass even though surfels are spawned and exist in world-space
 *
 * Get every surfel we can see that exists, make the coverage map to detect gaps where we can spawn more
 * It's an iterative hole filler because every frame I found holes to fill until budget is done of we filled it.
 *
 * When a frame has everywhere 0 coverage (first ever frame)
 * Make sure first time it runs I use blue noise to make the first surfel un-uniform. 
 *
 * One thread per surfel and see where it lands on screen
 */
class FScatterSurfelPass : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FScatterSurfelPass);
	SHADER_USE_PARAMETER_STRUCT(FScatterSurfelPass, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		/**
		 * Write to texture with pixel coverage for gather step
		 * Read surfel buffer for every alive surfel
		 */
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	
		/** Research what's the best dispatch size */
		OutEnvironment.SetDefine(TEXT("THREADS_X"), 16);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 1);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FScatterSurfelPass, "/Surfel/Surfels/Scatter.usf", "MainCS", SF_Compute);


/**
 * 2D dispatch of 16x16
 * Gather Pass
 *
 * Find the tile minimum and it's pixel coordinate (tile min reduction)
 * Using that pixel coordinate of the worst pixel, sample the GBuffer for depth and normal.
 * Depth: GBuffer is not enough as a position, I need to reconstruct to world position + depth + inverse VP
 * Normal: is good as is just read
 *
 * With this gathered data I can spawn (not really spawn since it's allocated already, more like modify) a surfel from the pool buffer
 * e.g.: {pos, normal, radius, radiance = 0) in Pool[index]
 * index can be InterlockedAdd
 */
class FGatherSurfelPass : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGatherSurfelPass);
	SHADER_USE_PARAMETER_STRUCT(FGatherSurfelPass, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		/**
		 * Read depth GBuffer
		 * Read normal GBuffer
		 * Read and write surfel buffers
		 * Write surfel structure
		 *
		 * TODO: read texture with pixel coverage once Scatter writes one; for now
		 * every dispatched pixel is treated as uncovered and always spawns.
		 */
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SurfelCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelPositionAndRadius)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelNormalAndFlags)
	
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferATexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferBTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferCTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferDTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferETexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferFTexture)
	
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
	
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER(FUintVector2, ViewRectMin)
		SHADER_PARAMETER(FUintVector2, ViewRectMax)

		SHADER_PARAMETER(uint32, SurfelBudget)
		SHADER_PARAMETER(float, SurfelRadius) // Temporarily from the CVar

		// Stopgap until Scatter coverage exists: one thread per GridSize x GridSize
		// pixel block (sampling the block's center pixel) instead of one thread per pixel,
		// so spawned surfels land spread out instead of piling up on every visible pixel.
		SHADER_PARAMETER(uint32, GridSize)

		// TEMP (this session): stand-in for Scatter coverage. See CoverageRadius
		// comment in Gather.usf.
		SHADER_PARAMETER(float, CoverageRadius)

	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("THREADS_X"), 16);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 16);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FGatherSurfelPass, "/Surfel/Surfels/Gather.usf", "MainCS", SF_Compute);

/**
 * Note: after one scatter and one gather, doing scatter again will be done with the newly spawned surfel
 * meaning the scatter step won't detect that place as unoccupied so it finds the next worse covered spot
 *
 * 1D Dispatch: runs per surfel
 *
 * Because my solution is for low-end
 * Must be implemented with ray marching because hardware RT is not on most devices
 *
 * What happens per surfel
 * Trace visibility ray. Is this shadowed? If not store irradiance.
 * RECURSIVE
 *     Trace from a hemisphere (still researching what's the best algorithm for mitigating light leaking)
 *     and using the global SDF and ray marching (origin + direction * distance)
 *     I can find a position which I query to find a surfel (which stores more irradiance)
 *     from that surfel I can keep doing it
 *
 * This pass updates all the radiance values from the surfels
 */
class FSurfelIrradiancePass : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSurfelIrradiancePass);
	SHADER_USE_PARAMETER_STRUCT(FSurfelIrradiancePass, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		/**
		 * Read and write surfel buffers
		 * Read structure with surfels
		 */
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("THREADS_X"), 16);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 1);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSurfelIrradiancePass, "/Surfel/Surfels/SurfelIrradiance.usf", "MainCS", SF_Compute);

/**
 * 2D Dispatch
 * Lookup in the grid for surfel in the buffer.
 * From depth buffer (using inverse vp) I can do a lookup in the structure that holds surfels to find what surfels are there.
 * Using this I write to a irradiance texture I can use in a composite pass.
 */
class FTextureIrradiancePass : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FTextureIrradiancePass);
	SHADER_USE_PARAMETER_STRUCT(FTextureIrradiancePass, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		/**
		 * Write irradiance texture
		 * Read depth
		 * Read surfel buffers
		 * Read and Write surfel structure
		 */
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("THREADS_X"), 16);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 16);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FTextureIrradiancePass, "/Surfel/Surfels/TextureIrradiance.usf", "MainCS", SF_Compute);

/** Composites the Irradiance texture */
class FCompositePass : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCompositePass);
	SHADER_USE_PARAMETER_STRUCT(FCompositePass, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		/**
		 * Read irradiance texture
		 * Read the framebuffer without post process
		 * Write new framebuffer image to be displayed
		 */
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("THREADS_X"), 16);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 16);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCompositePass, "/Surfel/Surfels/Composite.usf", "MainCS", SF_Compute);

/** Visualize surfels */
class FSurfelVisualizeCoverage : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSurfelVisualizeCoverage);
	SHADER_USE_PARAMETER_STRUCT(FSurfelVisualizeCoverage, FGlobalShader);

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

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("THREADS_X"), FComputeShaderUtils::kGolden2DGroupSize);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), FComputeShaderUtils::kGolden2DGroupSize);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSurfelVisualizeCoverage, "/Surfel/Surfels/Visualize.usf", "MainCS", SF_Compute);

/** Color Filter Compute Shader */
class FSurfelFullscreenCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSurfelFullscreenCS);
	SHADER_USE_PARAMETER_STRUCT(FSurfelFullscreenCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		
		// Surfels to visualize
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SurfelCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelPositionAndRadius)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelNormalAndFlags)
	
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputSceneColor)
		SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, InputViewport)
		SHADER_PARAMETER(float, Intensity)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutRenderTarget)

		// Needed to project each surfel's world position back to screen space.
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		// Fraction of the true surfel radius drawn, so individual surfels stay distinguishable.
		SHADER_PARAMETER(float, DebugRadiusScale)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("THREADS_X"), FComputeShaderUtils::kGolden2DGroupSize);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), FComputeShaderUtils::kGolden2DGroupSize);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSurfelFullscreenCS, "/Surfel/FullscreenCS.usf", "MainCS", SF_Compute);

/** GBuffer Visualization Compute Shader */
class FSurfelGBufferVisualizeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSurfelGBufferVisualizeCS);
	SHADER_USE_PARAMETER_STRUCT(FSurfelGBufferVisualizeCS, FGlobalShader);

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

		/** Needed by ConvertFromDeviceZ() inside Common.ush. */
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		SHADER_PARAMETER(int32, VisualizeMode)
		SHADER_PARAMETER(float, DepthScale)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutRenderTarget)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("THREADS_X"), FComputeShaderUtils::kGolden2DGroupSize);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), FComputeShaderUtils::kGolden2DGroupSize);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSurfelGBufferVisualizeCS, "/Surfel/GBufferVisualizeCS.usf", "MainCS", SF_Compute);

FComputePasses::FComputePasses(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
	UE_LOG(LogTemp, Log, TEXT("Surfel: SceneViewExtension registered"));
}

/** Where should my defined passes hook into the pipeline */
void FComputePasses::SubscribeToPostProcessingPass(
	EPostProcessingPass PassId,
	const FSceneView& View,
	FAfterPassCallbackDelegateArray& InOutPassCallbacks,
	bool bIsPassEnabled)
{	
	/** Runs only before the ... pass (MotionBlur now) */
	if (PassId != EPostProcessingPass::MotionBlur)
	{
		return;
	}
	
	const int32 Mode = CVarSurfelMode.GetValueOnRenderThread();
	
	// The procedural effect
	if (Mode == 1)
	{
		InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(
			this, &FComputePasses::RunFullscreenPass));
	}
	// The GBuffer visualization
	else if (Mode == 2)
	{
		InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(
			this, &FComputePasses::RunGBufferPass));
	}
}

// Create a writtable texture (UAV) for the scene
static FRDGTextureRef CreateOutputLike(FRDGBuilder& GraphBuilder, FRDGTextureRef SceneColorTexture, const TCHAR* Name)
{
	FRDGTextureDesc Desc = SceneColorTexture->Desc;

	// Strip state we do not want, add the UAV flag so a compute shader can write it.
	Desc.Reset();
	Desc.Flags |= TexCreate_UAV;
	Desc.Flags &= ~(TexCreate_RenderTargetable | TexCreate_FastVRAM);
	Desc.ClearValue = FClearValueBinding(FLinearColor::Transparent);

	return GraphBuilder.CreateTexture(Desc, Name);
}

void FComputePasses::PostRenderBasePassDeferred_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView,
	const FRenderTargetBindingSlots& RenderTargets, TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
	// For RenderDoc
	RDG_EVENT_SCOPE(GraphBuilder, "Surfel Gather Pass");

	// Are surfel enabled? Is view valid? Are the GBuffers?
	if (!CVarSurfelEnable.GetValueOnRenderThread() || !InView.State || !SceneTextures)
	{
		return;
	}

	// Get GBuffers
	const FSceneTextureUniformParameters GBufferTextures = *SceneTextures->GetContents();

	// Check if normal and depth are available
	if (!GBufferTextures.GBufferATexture || !GBufferTextures.SceneDepthTexture)
	{
		return;
	}

	uint32 ViewKey = InView.State->GetViewKey();

	// Get all the data for the surfels from omap
	FSurfelViewState& SurfelState = ViewStates.FindOrAdd(ViewKey);

	uint32 CVarBudget = FMath::Max(1024, CVarSurfelBudget.GetValueOnRenderThread());
	const bool bRefreshRequested = SurfelState.LastRefreshRequestId != GSurfelRefreshRequestId;
	if (SurfelState.Budget != CVarBudget || bRefreshRequested)
	{
		// Budget changed through CVar, or r.Surfel.Refresh was run.
		// Recreated the SurfelState below at the new size / from scratch.
		SurfelState.SurfelPositionAndRadius.SafeRelease();
		SurfelState.SurfelNormalAndFlags.SafeRelease();
		SurfelState.SurfelCounter.SafeRelease();
		SurfelState.Budget = CVarBudget;
		SurfelState.LastRefreshRequestId = GSurfelRefreshRequestId;
	}

	// Create buffers (SurfelState) if there are none, otherwise pull the cached ones from last frame into these RDG buffers
	FRDGBufferRef SurfelPositionAndRadiusBuffer;
	FRDGBufferRef SurfelNormalAndFlagsBuffer;
	FRDGBufferRef SurfelCounterBuffer;

	if (SurfelState.SurfelPositionAndRadius.IsValid())
	{
		SurfelPositionAndRadiusBuffer = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelPositionAndRadius);
		SurfelNormalAndFlagsBuffer    = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelNormalAndFlags);
		SurfelCounterBuffer           = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelCounter);
	}
	else // Create them if they are not there
	{
		SurfelPositionAndRadiusBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), CVarBudget),
			TEXT("Surfel.PositionAndRadius"));

		SurfelNormalAndFlagsBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), CVarBudget),
			TEXT("Surfel.NormalAndFlags"));

		SurfelCounterBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
			TEXT("Surfel.Counter"));

		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SurfelCounterBuffer), 0u);
	}

	// Allocate memory for GatherPass parameters
	FGatherSurfelPass::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FGatherSurfelPass::FParameters>();
	
	FRDGTextureRef BlackDummy = GSystemTextures.GetBlackDummy(GraphBuilder);
	PassParameters->GBufferATexture   = GBufferTextures.GBufferATexture;
	PassParameters->GBufferBTexture   = GBufferTextures.GBufferBTexture ? GBufferTextures.GBufferBTexture : BlackDummy;
	PassParameters->GBufferCTexture   = GBufferTextures.GBufferCTexture ? GBufferTextures.GBufferCTexture : BlackDummy;
	PassParameters->GBufferDTexture   = GBufferTextures.GBufferDTexture ? GBufferTextures.GBufferDTexture : BlackDummy;
	PassParameters->GBufferETexture   = GBufferTextures.GBufferETexture ? GBufferTextures.GBufferETexture : BlackDummy;
	PassParameters->GBufferFTexture   = GBufferTextures.GBufferFTexture ? GBufferTextures.GBufferFTexture : BlackDummy;
	PassParameters->SceneDepthTexture = GBufferTextures.SceneDepthTexture;

	PassParameters->View = InView.ViewUniformBuffer;
	// InView.UnscaledViewRect is important otherwise the imagine will be smaller because Unreal has upscaling somewhere
	PassParameters->ViewRectMin = FUintVector2(InView.UnscaledViewRect.Min.X, InView.UnscaledViewRect.Min.Y);
	PassParameters->ViewRectMax = FUintVector2(InView.UnscaledViewRect.Max.X, InView.UnscaledViewRect.Max.Y);

	PassParameters->SurfelBudget =  CVarBudget;
	PassParameters->SurfelRadius = CVarSurfelRadius.GetValueOnRenderThread();

	const uint32 GridSize = FMath::Max(1, CVarSurfelGridSize.GetValueOnRenderThread());
	PassParameters->GridSize = GridSize;
	PassParameters->CoverageRadius = FMath::Max(0.0f, CVarSurfelCoverageRadius.GetValueOnRenderThread());

	PassParameters->SurfelCount              = GraphBuilder.CreateUAV(SurfelCounterBuffer);
	PassParameters->SurfelPositionAndRadius  = GraphBuilder.CreateUAV(SurfelPositionAndRadiusBuffer);
	PassParameters->SurfelNormalAndFlags     = GraphBuilder.CreateUAV(SurfelNormalAndFlagsBuffer);

	// Add compute shader pass
	TShaderMapRef<FGatherSurfelPass> ComputeShader(GetGlobalShaderMap(InView.GetFeatureLevel()));
	// InView.UnscaledViewRect is important otherwise the imagine will be smaller because Unreal has upscaling somewhere
	// One thread per grid cell rather than per pixel, so dispatch is sized in cells (ViewSize / GridSize), not pixels.
	const FIntPoint ViewSize = InView.UnscaledViewRect.Size();
	const FIntPoint GridDispatchSize(
		FMath::DivideAndRoundUp(ViewSize.X, (int32)GridSize),
		FMath::DivideAndRoundUp(ViewSize.Y, (int32)GridSize));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Surfel Gather"), ComputeShader, PassParameters, FComputeShaderUtils::GetGroupCount(GridDispatchSize, FIntPoint(16, 16)));

	UE_LOG(LogTemp, Log, TEXT("Surfel: gather ran, budget %u"), CVarBudget);
	
	// Convert from RDG to the SurfelState struct from the map so surfels are persistent per frame
	// Otherwise RDG resources get freed here
	SurfelState.SurfelPositionAndRadius = GraphBuilder.ConvertToExternalBuffer(SurfelPositionAndRadiusBuffer);
	SurfelState.SurfelNormalAndFlags = GraphBuilder.ConvertToExternalBuffer(SurfelNormalAndFlagsBuffer);
	SurfelState.SurfelCounter = GraphBuilder.ConvertToExternalBuffer(SurfelCounterBuffer);
}

// Pass for the procedural effect
FScreenPassTexture FComputePasses::RunFullscreenPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	// Scene color can arrive as a slice of a texture array (e.g. instanced stereo),
	// so normalize it to a plain 2D texture first.
	const FScreenPassTexture SceneColor =
		FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));

	if (!SceneColor.IsValid())
	{
		return SceneColor;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "Surfel Fullscreen CS");

	// Views without persistent state (e.g. some scene captures/thumbnails) have no
	// ViewKey to look up a surfel pool with, so there is nothing to visualize.
	if (!View.State)
	{
		return SceneColor;
	}

	const FScreenPassTextureViewport SceneColorViewport(SceneColor);
	const FIntPoint PassSize = SceneColor.ViewRect.Size();

	FRDGTextureRef OutputTexture = CreateOutputLike(GraphBuilder, SceneColor.Texture, TEXT("Surfel.FullscreenOutput"));

	FSurfelFullscreenCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FSurfelFullscreenCS::FParameters>();

	PassParameters->InputSceneColor = SceneColor.Texture;
	PassParameters->InputViewport   = GetScreenPassTextureViewportParameters(SceneColorViewport);
	PassParameters->Intensity       = CVarSurfelIntensity.GetValueOnRenderThread();
	PassParameters->OutRenderTarget = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(OutputTexture));
	PassParameters->View            = View.ViewUniformBuffer;
	PassParameters->DebugRadiusScale = FMath::Max(0.0f, CVarSurfelDebugRadiusScale.GetValueOnRenderThread());

	// Send surfels to visualize
	//---------------------------------------------------------------------------------------------------------------------------------------------

	uint32 ViewKey = View.State->GetViewKey();

	// Get all the data for the surfels from omap
	FSurfelViewState& SurfelState = ViewStates.FindOrAdd(ViewKey);

	uint32 CVarBudget = FMath::Max(1024, CVarSurfelBudget.GetValueOnRenderThread());
	if (SurfelState.Budget != CVarBudget)
	{
		// Budget changed through CVar
		// Recreated the SurfelState below at the new size.
		SurfelState.SurfelPositionAndRadius.SafeRelease();
		SurfelState.SurfelNormalAndFlags.SafeRelease();
		SurfelState.SurfelCounter.SafeRelease();
		SurfelState.Budget = CVarBudget;
	}

	// Create buffers (SurfelState) if there are none, otherwise pull the cached ones from last frame into these RDG buffers
	FRDGBufferRef SurfelPositionAndRadiusBuffer;
	FRDGBufferRef SurfelNormalAndFlagsBuffer;
	FRDGBufferRef SurfelCounterBuffer;

	if (SurfelState.SurfelPositionAndRadius.IsValid())
	{
		SurfelPositionAndRadiusBuffer = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelPositionAndRadius);
		SurfelNormalAndFlagsBuffer    = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelNormalAndFlags);
		SurfelCounterBuffer           = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelCounter);
	}
	else // Create them if they are not there
	{
		SurfelPositionAndRadiusBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), CVarBudget),
			TEXT("Surfel.PositionAndRadius"));

		SurfelNormalAndFlagsBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), CVarBudget),
			TEXT("Surfel.NormalAndFlags"));

		SurfelCounterBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
			TEXT("Surfel.Counter"));

		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SurfelCounterBuffer), 0u);
	}

	PassParameters->SurfelCount              = GraphBuilder.CreateUAV(SurfelCounterBuffer);
	PassParameters->SurfelPositionAndRadius  = GraphBuilder.CreateUAV(SurfelPositionAndRadiusBuffer);
	PassParameters->SurfelNormalAndFlags     = GraphBuilder.CreateUAV(SurfelNormalAndFlagsBuffer);

	//---------------------------------------------------------------------------------------------------------------------------------------------
	
	TShaderMapRef<FSurfelFullscreenCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("Surfel Fullscreen %dx%d", PassSize.X, PassSize.Y),
		ComputeShader,
		PassParameters,
		FComputeShaderUtils::GetGroupCount(PassSize, FComputeShaderUtils::kGolden2DGroupSize));

	// The post-process chain expects the result back in the texture it handed us.
	AddCopyTexturePass(GraphBuilder, OutputTexture, SceneColor.Texture);

	return SceneColor;
}

// Pass for the GBuffer visualization
FScreenPassTexture FComputePasses::RunGBufferPass(
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

	// Pull the G-Buffer out of the scene textures uniform buffer we were handed.
	// SceneTextures is a binding wrapper, so unwrap it to the actual RDG uniform buffer.
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUB =
		Inputs.SceneTextures.SceneTextures.GetUniformBuffer();

	if (!SceneTexturesUB)
	{
		// Mobile / forward path: no deferred G-Buffer to read.
		return SceneColor;
	}

	// GetContents() gives us the parameter struct the renderer filled in, which
	// holds the individual G-Buffer texture references.
	const FSceneTextureUniformParameters& SceneTextures = *SceneTexturesUB->GetContents();

	if (!SceneTextures.GBufferATexture || !SceneTextures.SceneDepthTexture)
	{
		// Deferred G-Buffer is unavailable (forward shading, mobile, etc.).
		return SceneColor;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "Surfel GBuffer Visualize");

	const FScreenPassTextureViewport SceneColorViewport(SceneColor);
	const FIntPoint PassSize = SceneColor.ViewRect.Size();

	FRDGTextureRef OutputTexture = CreateOutputLike(GraphBuilder, SceneColor.Texture, TEXT("Surfel.GBufferVisOutput"));

	// Some G-Buffer slots are only allocated for certain project settings; fall
	// back to a black system texture so the binding is always valid.
	FRDGTextureRef BlackDummy = GSystemTextures.GetBlackDummy(GraphBuilder);

	FSurfelGBufferVisualizeCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FSurfelGBufferVisualizeCS::FParameters>();

	PassParameters->InputSceneColor = SceneColor.Texture;
	PassParameters->InputViewport   = GetScreenPassTextureViewportParameters(SceneColorViewport);

	PassParameters->GBufferATexture   = SceneTextures.GBufferATexture;
	PassParameters->GBufferBTexture   = SceneTextures.GBufferBTexture ? SceneTextures.GBufferBTexture : BlackDummy;
	PassParameters->GBufferCTexture   = SceneTextures.GBufferCTexture ? SceneTextures.GBufferCTexture : BlackDummy;
	PassParameters->GBufferDTexture   = SceneTextures.GBufferDTexture ? SceneTextures.GBufferDTexture : BlackDummy;
	PassParameters->GBufferETexture   = SceneTextures.GBufferETexture ? SceneTextures.GBufferETexture : BlackDummy;
	PassParameters->GBufferFTexture   = SceneTextures.GBufferFTexture ? SceneTextures.GBufferFTexture : BlackDummy;
	PassParameters->SceneDepthTexture = SceneTextures.SceneDepthTexture;

	PassParameters->View = View.ViewUniformBuffer;

	PassParameters->VisualizeMode   = CVarSurfelGBufferChannel.GetValueOnRenderThread();
	PassParameters->DepthScale      = CVarSurfelDepthScale.GetValueOnRenderThread();
	PassParameters->OutRenderTarget = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(OutputTexture));

	TShaderMapRef<FSurfelGBufferVisualizeCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("Surfel GBuffer Visualize %dx%d", PassSize.X, PassSize.Y),
		ComputeShader,
		PassParameters,
		FComputeShaderUtils::GetGroupCount(PassSize, FComputeShaderUtils::kGolden2DGroupSize));

	AddCopyTexturePass(GraphBuilder, OutputTexture, SceneColor.Texture);

	return SceneColor;
}