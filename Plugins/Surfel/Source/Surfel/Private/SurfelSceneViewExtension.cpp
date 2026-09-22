#pragma once
#include "SurfelSceneViewExtension.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "SceneViewExtension.h"
#include "SceneInterface.h"
#include "Engine/World.h"
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

void FSurfelSceneViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
	// Game worlds only (PIE / standalone). The mode CVar is global and survives the end of
	// PIE, so without this, stopping PIE in "Direct Light Only" leaves the editor viewport
	// unlit as well.
	const UWorld* World = InViewFamily.Scene ? InViewFamily.Scene->GetWorld() : nullptr;
	if (!World || !World->IsGameWorld())
	{
		return;
	}

	// Lumen GI only runs when the view's resolved GI method is Lumen AND nothing vetoes it.
	// The project ships with r.DynamicGlobalIlluminationMethod=0 (None), and
	// r.Lumen.DiffuseIndirect.Allow can only veto, never enable. So the way to switch it
	// both ways is the view's FinalPostProcessSettings: SetupView runs right after
	// EndFinalPostprocessSettings, so what is set here beats the project default and any
	// Post Process Volume.
	FFinalPostProcessSettings& Settings = InView.FinalPostProcessSettings;

	switch ((ESurfelVisualizeMode)CVarSurfelVisualizeMode.GetValueOnGameThread())
	{
	case ESurfelVisualizeMode::LumenGI:
		Settings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::Lumen;
		Settings.ReflectionMethod = EReflectionMethod::Lumen;
		break;

	case ESurfelVisualizeMode::SurfelGI:
		// No Lumen anywhere, including its reflections, which would carry Lumen's bounce.
		Settings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::None;
		Settings.ReflectionMethod = EReflectionMethod::ScreenSpace;
		break;

	case ESurfelVisualizeMode::DirectLightOnly:
		// Every indirect term off: GI, reflections, and the skylight's ambient.
		Settings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::None;
		Settings.ReflectionMethod = EReflectionMethod::None;
		InViewFamily.EngineShowFlags.SetSkyLighting(false);
		InViewFamily.EngineShowFlags.SetReflectionEnvironment(false);
		break;

	default:
		// Debug overlays draw on top of whatever the project / level has configured.
		break;
	}
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

	// Nothing to draw unless the visualize pass is on AND the selected mode is one the
	// shader actually renders. The lighting-reference modes only reconfigure the
	// renderer, so skip the fullscreen dispatch for them entirely.
	if (CVarSurfelMode.GetValueOnRenderThread() == 1
		&& IsSurfelOverlayMode(CVarSurfelVisualizeMode.GetValueOnRenderThread()))
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
	
	// Drop last frame's grid handle before any early return: those RDG buffers belong to a
	// finished graph, and RunFullscreenPass must never pick them up if this frame skips the grid.
	if (InView.State)
	{
		SurfelFrameDataByViewKey.Remove(InView.State->GetViewKey());
	}

	// Before any early return, so RunFullscreenPass later this frame sees the same
	// grid layout whether or not the grid got built.
	FUniformGridViewState::UpdateFromCVars();

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
	
	const float SpawnsPerFrame = (float)FMath::Max(CVarSurfelSpawnsPerFrame.GetValueOnRenderThread(), 0);
	float SpawnChance = SpawnsPerFrame / float(CoverageExtent.X * CoverageExtent.Y) ; // Dividing by the pixel count makes SpawnChance the probability per uncovered pixel. Across the whole screen, that gives about SpawnsPerFrame new surfels per frame at most, regardless of resolution.
	GatherPassParameters->SpawnChance = SpawnChance;

	GatherPassParameters->SpawnCoverageThreshold = CVarSurfelSpawnCoverageThreshold.GetValueOnRenderThread();
	
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

	// Hand this frame's grid buffers and coverage map off to RunFullscreenPass, which runs
	// later in the same frame's render graph (same GraphBuilder) but is a separate callback
	// with no access to these locals. Not persisted across frames - overwritten here
	// every frame before it's read.
	SurfelFrameDataByViewKey.Add(ViewKey, FSurfelFrameData{ GridCellEntriesBuffer, GridCounterBuffer, GridPassParameters->GridPosition, CoverageTexture });

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

	// Grid and coverage: pull in whatever the passes built for this view earlier this same
	// frame. If they're not there yet (first frame, or the grid feature is off), fall
	// back to the brute-force loop with small dummy buffers just to keep the
	// shader parameters valid.
	// Consume (copy + remove) so a handle can never outlive the graph it was created in.
	FSurfelFrameData SurfelFrameData;
	const bool bHasFrameData = SurfelFrameDataByViewKey.RemoveAndCopyValue(ViewKey, SurfelFrameData);

	// The Grid visualization needs the grid itself, so read it there whatever r.Surfel.UseGrid says.
	const int32 VisualizeMode = CVarSurfelVisualizeMode.GetValueOnRenderThread();
	const bool bUseGrid = bHasFrameData
		&& (CVarSurfelUseGrid.GetValueOnRenderThread() != 0 || VisualizeMode == (int32)ESurfelVisualizeMode::Grid);

	if (bUseGrid)
	{
		PassParameters->GridCellEntries = GraphBuilder.CreateUAV(SurfelFrameData.GridCellEntries);
		PassParameters->GridCounter     = GraphBuilder.CreateUAV(SurfelFrameData.GridCounter);
		PassParameters->GridPosition    = SurfelFrameData.GridPosition;
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

	// Coverage map, for the Coverage visualization. Scatter writes it and Gather reads it,
	// so by the time this pass runs it holds this frame's finished values.
	const bool bHasCoverage = bHasFrameData && SurfelFrameData.CoverageTexture != nullptr;
	PassParameters->CoverageTexture     = bHasCoverage ? SurfelFrameData.CoverageTexture : BlackDummy;
	PassParameters->bHasCoverageTexture = bHasCoverage ? 1u : 0u;
	PassParameters->CoverageScale       = CVarSurfelCoverageScale.GetValueOnRenderThread();

	PassParameters->VisualizeMode = (uint32)VisualizeMode;

	PassParameters->GridVisFlags         = (uint32)CVarSurfelGridVisFlags.GetValueOnRenderThread();
	PassParameters->GridVisEdgeThickness = FMath::Clamp(CVarSurfelGridVisEdgeThickness.GetValueOnRenderThread(), 0.0f, 0.5f);
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