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
#include "ComputePasses/GridAllocationPass.h"
#include "ComputePasses/ScatterPass.h"
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
	// TODO:
	// 1. Get gbuffers as jack said
	// 2. Separate passes into functions that can be defined inside their own headers/cpp not everything in sceneview just the dispatchng and parameters
	
	// Are surfel enabled? Is view valid? Are the GBuffers?
	if (!CVarSurfelEnable.GetValueOnRenderThread() || !InView.State || !SceneTextures)
	{
		return;
	}
	
	// Get GBuffers
	FSceneTextureUniformParameters GBufferTextures = *SceneTextures->GetContents();
	const int32 GBufferAIndex = FSceneTexturesConfig::Get().GBufferBindings[GBL_Default].GBufferA.Index;
	FRDGTextureRef GBufferA = GBufferAIndex >= 0 ? RenderTargets.Output[GBufferAIndex].GetTexture() : nullptr;

	// Check if normal and depth are available
	if (!GBufferA || !GBufferTextures.SceneDepthTexture)
	{
		return;
	}
	GBufferTextures.GBufferATexture = GBufferA;

	// Get all the data for the surfels from this map	
	uint32 ViewKey = InView.State->GetViewKey();
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
	
	// Create RDG Buffers for the grid
	FRDGBufferRef GridCellEntriesBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FUniformGridViewState::GridCellCount * FUniformGridViewState::CellCapacity),
			TEXT("Grid.GridCellEntries"));

	FRDGBufferRef GridCounterBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FUniformGridViewState::GridCellCount),
		TEXT("Grid.GridCounter"));
	
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(GridCounterBuffer), 0u);
	
	
	// Scatter pass
	// For RenderDoc
	RDG_EVENT_SCOPE(GraphBuilder, "Surfel Scatter Pass");
	
	FScatterSurfelPass::FParameters* ScatterPassParameters =
		GraphBuilder.AllocParameters<FScatterSurfelPass::FParameters>();
	
	// size should be big enough for the shader to read
	const FIntPoint CoverageExtent(InView.UnscaledViewRect.Max.X, InView.UnscaledViewRect.Max.Y);
	
	FRDGTextureDesc CoverageDesc = FRDGTextureDesc::Create2D(
		CoverageExtent, PF_R32_FLOAT, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
	
	FRDGTextureRef CoverageTexture = GraphBuilder.CreateTexture(CoverageDesc, TEXT("CoverageTexture"));
	
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(CoverageTexture), FLinearColor::Black);

	ScatterPassParameters->CoverageTexture = GraphBuilder.CreateUAV(CoverageTexture);
	
	// set parameters for scatter
	ScatterPassParameters->GridCellEntries = GraphBuilder.CreateUAV(GridCellEntriesBuffer);
	ScatterPassParameters->GridCounter = GraphBuilder.CreateUAV(GridCounterBuffer);
	
	// Position of the grid is always where the camera is
	FVector CameraPosition = InView.ViewLocation;
	const float GridExtent = static_cast<float>(FUniformGridViewState::CellResolution * FUniformGridViewState::CellSize);
	FVector GridPosition = CameraPosition - FVector(GridExtent * 0.5f);

	
	ScatterPassParameters->GridPosition = FVector3f(GridPosition);
	ScatterPassParameters->GridCellCount = FUniformGridViewState::GridCellCount;
	ScatterPassParameters->CellResolution = FUniformGridViewState::CellResolution;
	ScatterPassParameters->CellCapacity = FUniformGridViewState::CellCapacity;
	ScatterPassParameters->CellSize = FUniformGridViewState::CellSize;
	
	
	FRDGTextureRef BlackDummy = GSystemTextures.GetBlackDummy(GraphBuilder);
	ScatterPassParameters->GBufferATexture   = GBufferTextures.GBufferATexture;
	ScatterPassParameters->GBufferBTexture   = GBufferTextures.GBufferBTexture ? GBufferTextures.GBufferBTexture : BlackDummy;
	ScatterPassParameters->GBufferCTexture   = GBufferTextures.GBufferCTexture ? GBufferTextures.GBufferCTexture : BlackDummy;
	ScatterPassParameters->GBufferDTexture   = GBufferTextures.GBufferDTexture ? GBufferTextures.GBufferDTexture : BlackDummy;
	ScatterPassParameters->GBufferETexture   = GBufferTextures.GBufferETexture ? GBufferTextures.GBufferETexture : BlackDummy;
	ScatterPassParameters->GBufferFTexture   = GBufferTextures.GBufferFTexture ? GBufferTextures.GBufferFTexture : BlackDummy;
	ScatterPassParameters->SceneDepthTexture = GBufferTextures.SceneDepthTexture;
	
	ScatterPassParameters->View = InView.ViewUniformBuffer;
	// InView.UnscaledViewRect is important otherwise the imagine will be smaller because Unreal has upscaling somewhere
	ScatterPassParameters->ViewRectMin = FUintVector2(InView.UnscaledViewRect.Min.X, InView.UnscaledViewRect.Min.Y);
	ScatterPassParameters->ViewRectMax = FUintVector2(InView.UnscaledViewRect.Max.X, InView.UnscaledViewRect.Max.Y);

	ScatterPassParameters->SurfelBudget = CVarBudget;
	ScatterPassParameters->SurfelRadius = CVarSurfelRadius.GetValueOnRenderThread();

	ScatterPassParameters->SurfelCount              = GraphBuilder.CreateUAV(SurfelCounterBuffer);
	ScatterPassParameters->SurfelPositionAndRadius  = GraphBuilder.CreateUAV(SurfelPositionAndRadiusBuffer);
	ScatterPassParameters->SurfelNormalAndFlags     = GraphBuilder.CreateUAV(SurfelNormalAndFlagsBuffer);
	
	// Add compute shader pass
	TShaderMapRef<FScatterSurfelPass> ComputeShaderScatter(GetGlobalShaderMap(InView.GetFeatureLevel()));
	
	const FIntPoint ViewSizeScatter = InView.UnscaledViewRect.Size();
	const FIntPoint GridDispatchSizeScatter(ViewSizeScatter.X, ViewSizeScatter.Y);
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Surfel Gather"), ComputeShaderScatter, ScatterPassParameters, FComputeShaderUtils::GetGroupCount(GridDispatchSizeScatter, FIntPoint(16, 16)));

	UE_LOG(LogTemp, Log, TEXT("Surfel: scatter pass ran, budget %u"), CVarBudget);
	
	// TODO: get this for the gbuffers as Jack said in Teams using FVIewInfo
	
	// For RenderDoc
	RDG_EVENT_SCOPE(GraphBuilder, "Surfel Gather Pass");

	// Allocate memory for GatherPass parameters
	FGatherSurfelPass::FParameters* GatherPassParameters =
		GraphBuilder.AllocParameters<FGatherSurfelPass::FParameters>();
	
	// TODO: 256 is hardcoded should be exposed to ImGui
	float SpawnChance = 256.f / float(CoverageExtent.X * CoverageExtent.Y) ; // Dividing by the pixel count makes SpawnChance the probability per uncovered pixel. Across the whole screen, that gives about TargetSpawnsPerFrame new surfels per frame at most, regardless of resolution.
	GatherPassParameters->SpawnChance = SpawnChance;
	
	// TODO: to make global for ImGui
	static float SpawnCoverageThreshold = 0.1f;
	GatherPassParameters->SpawnCoverageThreshold = SpawnCoverageThreshold;
	
	GatherPassParameters->CoverageTexture = CoverageTexture;
	
	GatherPassParameters->GBufferATexture   = GBufferTextures.GBufferATexture;
	GatherPassParameters->GBufferBTexture   = GBufferTextures.GBufferBTexture ? GBufferTextures.GBufferBTexture : BlackDummy;
	GatherPassParameters->GBufferCTexture   = GBufferTextures.GBufferCTexture ? GBufferTextures.GBufferCTexture : BlackDummy;
	GatherPassParameters->GBufferDTexture   = GBufferTextures.GBufferDTexture ? GBufferTextures.GBufferDTexture : BlackDummy;
	GatherPassParameters->GBufferETexture   = GBufferTextures.GBufferETexture ? GBufferTextures.GBufferETexture : BlackDummy;
	GatherPassParameters->GBufferFTexture   = GBufferTextures.GBufferFTexture ? GBufferTextures.GBufferFTexture : BlackDummy;
	GatherPassParameters->SceneDepthTexture = GBufferTextures.SceneDepthTexture;

	GatherPassParameters->View = InView.ViewUniformBuffer;
	// InView.UnscaledViewRect is important otherwise the imagine will be smaller because Unreal has upscaling somewhere
	GatherPassParameters->ViewRectMin = FUintVector2(InView.UnscaledViewRect.Min.X, InView.UnscaledViewRect.Min.Y);
	GatherPassParameters->ViewRectMax = FUintVector2(InView.UnscaledViewRect.Max.X, InView.UnscaledViewRect.Max.Y);

	GatherPassParameters->SurfelBudget =  CVarBudget;
	GatherPassParameters->SurfelRadius = CVarSurfelRadius.GetValueOnRenderThread();

	GatherPassParameters->SurfelCount              = GraphBuilder.CreateUAV(SurfelCounterBuffer);
	GatherPassParameters->SurfelPositionAndRadius  = GraphBuilder.CreateUAV(SurfelPositionAndRadiusBuffer);
	GatherPassParameters->SurfelNormalAndFlags     = GraphBuilder.CreateUAV(SurfelNormalAndFlagsBuffer);

	// Add compute shader pass
	TShaderMapRef<FGatherSurfelPass> ComputeShader(GetGlobalShaderMap(InView.GetFeatureLevel()));
	
	const FIntPoint ViewSize = InView.UnscaledViewRect.Size();
	const FIntPoint GridDispatchSize(ViewSize.X, ViewSize.Y);
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Surfel Gather"), ComputeShader, GatherPassParameters, FComputeShaderUtils::GetGroupCount(GridDispatchSize, FIntPoint(16, 16)));

	UE_LOG(LogTemp, Log, TEXT("Surfel: gather ran, budget %u"), CVarBudget);
	
	// Grid allocation after gather pass
	// For RenderDoc
	RDG_EVENT_SCOPE(GraphBuilder, "Grid Allocation Pass");
	
	
	// Allocate memory for GridPass parameters
	FGridAllocationPass::FParameters* GridPassParameters =
		GraphBuilder.AllocParameters<FGridAllocationPass::FParameters>();
	
	// Set Parameters For Grid
	GridPassParameters->GridCellEntries = GraphBuilder.CreateUAV(GridCellEntriesBuffer);
	GridPassParameters->GridCounter = GraphBuilder.CreateUAV(GridCounterBuffer);
	GridPassParameters->GridPosition = FVector3f(GridPosition);
	GridPassParameters->GridCellCount = FUniformGridViewState::GridCellCount;
	GridPassParameters->CellResolution = FUniformGridViewState::CellResolution;
	GridPassParameters->CellCapacity = FUniformGridViewState::CellCapacity;
	GridPassParameters->CellSize = FUniformGridViewState::CellSize;
	
	// Set Parameters For Surfels
	// Already above just create a new UAV
	GridPassParameters->SurfelCount              = GraphBuilder.CreateUAV(SurfelCounterBuffer);
	GridPassParameters->SurfelPositionAndRadius  = GraphBuilder.CreateUAV(SurfelPositionAndRadiusBuffer);
	GridPassParameters->SurfelNormalAndFlags     = GraphBuilder.CreateUAV(SurfelNormalAndFlagsBuffer);
	GridPassParameters->SurfelBudget = CVarBudget;
	GridPassParameters->SurfelRadius = CVarSurfelRadius.GetValueOnRenderThread();

	// Dispatch Compute
	TShaderMapRef<FGridAllocationPass> ComputeShaderGrid(GetGlobalShaderMap(InView.GetFeatureLevel()));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Surfel Grid"), ComputeShaderGrid, GridPassParameters, FComputeShaderUtils::GetGroupCount(FIntPoint(int(CVarBudget), 1), FIntPoint(64, 1)));
		
	UE_LOG(LogTemp, Log, TEXT("Surfel: grid allocation ran, budget %u"), CVarBudget);

	// Hand the grid this frame's buffers off to RunFullscreenPass, which runs later
	// in the same frame's render graph (same GraphBuilder) but is a separate callback
	// with no access to these locals. Not persisted across frames - overwritten here
	// every frame before it's read.
	GridFrameDataByViewKey.Add(ViewKey, FGridFrameData{ GridCellEntriesBuffer, GridCounterBuffer, GridPassParameters->GridPosition });

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

	// Send surfels to visualize

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

	// Depth is needed to reconstruct each pixel's world position, which is what
	// the grid query and the disc test need.
	// Normal keeps discs on surfaces facing the same way as the surfel.
	FRDGTextureRef BlackDummy = GSystemTextures.GetBlackDummy(GraphBuilder);
	PassParameters->SceneDepthTexture = BlackDummy;
	PassParameters->GBufferATexture   = BlackDummy;
	PassParameters->bHasGBufferNormal = 0u;

	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUB =
		Inputs.SceneTextures.SceneTextures.GetUniformBuffer();

	if (SceneTexturesUB)
	{
		const FSceneTextureUniformParameters& SceneTextures = *SceneTexturesUB->GetContents();
		if (SceneTextures.SceneDepthTexture)
		{
			PassParameters->SceneDepthTexture = SceneTextures.SceneDepthTexture;
		}
		if (SceneTextures.GBufferATexture)
		{
			PassParameters->GBufferATexture   = SceneTextures.GBufferATexture;
			PassParameters->bHasGBufferNormal = 1u;
		}
	}

	// Grid: pull in whatever GridAllocation built for this view earlier this same
	// frame. If it's not there yet (first frame, or the grid feature is off), fall
	// back to the brute-force loop with small dummy buffers just to keep the
	// shader parameters valid.
	const FGridFrameData* GridFrameData = GridFrameDataByViewKey.Find(ViewKey);
	const bool bUseGrid = GridFrameData != nullptr && CVarSurfelUseGrid.GetValueOnRenderThread() != 0;

	if (bUseGrid)
	{
		PassParameters->GridCellEntries = GraphBuilder.CreateUAV(GridFrameData->GridCellEntries);
		PassParameters->GridCounter     = GraphBuilder.CreateUAV(GridFrameData->GridCounter);
		PassParameters->GridPosition    = GridFrameData->GridPosition;
	}
	else
	{
		FRDGBufferRef DummyGridBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
			TEXT("Grid.Dummy"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyGridBuffer), 0u);

		PassParameters->GridCellEntries = GraphBuilder.CreateUAV(DummyGridBuffer);
		PassParameters->GridCounter     = GraphBuilder.CreateUAV(DummyGridBuffer);
		PassParameters->GridPosition    = FVector3f::ZeroVector;
	}

	PassParameters->GridCellCount  = FUniformGridViewState::GridCellCount;
	PassParameters->CellResolution = FUniformGridViewState::CellResolution;
	PassParameters->CellCapacity   = FUniformGridViewState::CellCapacity;
	PassParameters->CellSize       = FUniformGridViewState::CellSize;
	PassParameters->bUseGrid       = bUseGrid ? 1u : 0u;
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