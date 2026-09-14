#pragma once
#include "SurfelSceneViewExtension.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "SceneViewExtension.h"
#include "ScreenPass.h"
#include "SystemTextures.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "CVarCommands.h"
#include "ComputePasses/GatherPass.h"
#include "ComputePasses/VisualizePass.h"

FSurfelSceneViewExtension::FSurfelSceneViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
	UE_LOG(LogTemp, Log, TEXT("Surfel: SceneViewExtension registered"));
}

// Where should my defined passes hook into the pipeline
void FSurfelSceneViewExtension::SubscribeToPostProcessingPass(
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

	if (CVarSurfelMode.GetValueOnRenderThread() == 1)
	{
		InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(
			this, &FSurfelSceneViewExtension::RunFullscreenPass));
	}
}

void FSurfelSceneViewExtension::PostRenderBasePassDeferred_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView,
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
	
	// Grid allocation after gather pass
	
	UE_LOG(LogTemp, Log, TEXT("Surfel: grid allocation ran, budget %u"), CVarBudget);
	
	// Convert from RDG to the SurfelState struct from the map so surfels are persistent per frame
	// Otherwise RDG resources get freed here
	SurfelState.SurfelPositionAndRadius = GraphBuilder.ConvertToExternalBuffer(SurfelPositionAndRadiusBuffer);
	SurfelState.SurfelNormalAndFlags = GraphBuilder.ConvertToExternalBuffer(SurfelNormalAndFlagsBuffer);
	SurfelState.SurfelCounter = GraphBuilder.ConvertToExternalBuffer(SurfelCounterBuffer);
}

// Pass for the visualize effect
FScreenPassTexture FSurfelSceneViewExtension::RunFullscreenPass(
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