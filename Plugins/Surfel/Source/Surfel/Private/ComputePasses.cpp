#include "ComputePasses.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "SceneViewExtension.h"
#include "ScreenPass.h"
#include "SystemTextures.h"
#include "PostProcess/PostProcessMaterialInputs.h"

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

// note: explaining nomenclature
// called scatter because one surfel writes to many pixels
// gather because one pixel from many in a tile finds the worst value

// Scatter answers this question:
// Where on the screen are no surfels? So I know where to spawn more
// To store this I use a coverage texture used in the gather step (where surfels get spawned)
// Scatter is a screen space pass even though surfels are spawned and exist in world-space

// Get every surfel we can see that exists, make the coverage map to detect gaps where we can spawn more
// It's an iterative hole filler because every frame I found holes to fill until budget is done of we filled it.

// When a frame has everywhere 0 coverage (first ever frame)
// Make sure first time it runs I use blue noise to make the first surfel un-uniform. 

// One thread per surfel and see where it lands on screen
class FScatterSurfelPass : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FScatterSurfelPass);
	SHADER_USE_PARAMETER_STRUCT(FScatterSurfelPass, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Write to texture with pixel coverage for gather step
		// Read surfel buffer for every alive surfel
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	
		OutEnvironment.SetDefine(TEXT("THREADS_X"), 16); // Research what's the best dispatch size
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 1);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};

// 2D dispatch of 16x16
// Gather Pass

// Find the tile minimum and it's pixel coordinate (tile min reduction)
// Using that pixel coordinate of the worst pixel, sample the GBuffer for depth and normal.
// Depth: GBuffer is not enough as a position, I need to reconstruct to world position + depth + inverse VP
// Normal: is good as is just read

// With this gathered data I can spawn (not really spawn since it's allocated already, more like modify) a surfel from the pool buffer
// e.g.: {pos, normal, radius, radiance = 0) in Pool[index]
// index can be InterlockedAdd
class FGatherSurfelPass : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGatherSurfelPass);
	SHADER_USE_PARAMETER_STRUCT(FGatherSurfelPass, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Read texture with texture coverage
		// Read depth GBuffer
		// Read normal GBuffer
		// Read and write surfel buffers
		// Write surfel structure
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

// Note: after one scatter and one gather, doing scatter again will be done with the newly spawned surfel
// meaning the scatter step won't detect that place as unoccupied so it finds the next worse covered spot

// 1D Dispatch: runs per surfel

// Because my solution is for low-end
// Must be implemented with ray marching because hardware RT is not on most devices

// What happens per surfel
// Trace visibility ray. Is this shadowed? If not store irradiance.
// RECURSIVE 
	// Trace from a hemisphere (still researching what's the best algorithm for mitigating light leaking)
	// and using the global SDF and ray marching (origin + direction * distance) 
	// I can find a position which I query to find a surfel (which stores more irradiance)
	// from that surfel I can keep doing it

// This pass updates all the radiance values from the surfels
class FSurfelIrradiancePass : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSurfelIrradiancePass);
	SHADER_USE_PARAMETER_STRUCT(FSurfelIrradiancePass, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Read and write surfel buffers
		// Read structure with surfels
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

// 2D Dispatch
// Lookup in the grid for surfel in the buffer. 
// From depth buffer (using inverse vp) I can do a lookup in the structure that holds surfels to find what surfels are there.
// Using this I write to a irradiance texture I can use in a composite pass.
class FTextureIrradiancePass : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FTextureIrradiancePass);
	SHADER_USE_PARAMETER_STRUCT(FTextureIrradiancePass, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Write irradiance texture
		// Read depth 
		// Read surfel buffers
		// Read and Write surfel structure
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

// Composites the Irradiance texture
class FCompositePass : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCompositePass);
	SHADER_USE_PARAMETER_STRUCT(FCompositePass, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Read irradiance texture
		// Read the framebuffer without post process
		// Write new framebuffer image to be displayed
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

// Visualize surfels
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

// Color Filter Compute Shader
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

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("THREADS_X"), FComputeShaderUtils::kGolden2DGroupSize);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), FComputeShaderUtils::kGolden2DGroupSize);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSurfelFullscreenCS, "/Surfel/FullscreenCS.usf", "MainCS", SF_Compute);

// GBuffer Visualization Compute Shader
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

		// Needed by ConvertFromDeviceZ() inside Common.ush.
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

// Where should my defined passes hook into the pipeline
void FComputePasses::SubscribeToPostProcessingPass(
	EPostProcessingPass PassId,
	const FSceneView& View,
	FAfterPassCallbackDelegateArray& InOutPassCallbacks,
	bool bIsPassEnabled)
{	
	// Runs only before the ... pass (MotionBlur now)
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

}

// Pass for the procedural effect
FScreenPassTexture FComputePasses::RunFullscreenPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	// Scene color can arrive as a slice of a texture array (e.g. instanced stereo),
	// so normalise it to a plain 2D texture first.
	const FScreenPassTexture SceneColor =
		FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));

	if (!SceneColor.IsValid())
	{
		return SceneColor;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "Surfel Fullscreen CS");

	const FScreenPassTextureViewport SceneColorViewport(SceneColor);
	const FIntPoint PassSize = SceneColor.ViewRect.Size();

	FRDGTextureRef OutputTexture = CreateOutputLike(GraphBuilder, SceneColor.Texture, TEXT("Surfel.FullscreenOutput"));

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