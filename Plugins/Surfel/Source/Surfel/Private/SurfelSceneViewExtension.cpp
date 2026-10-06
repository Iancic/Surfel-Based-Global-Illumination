#pragma once
#include "SurfelSceneViewExtension.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "PostProcess/PostProcessMaterialInputs.h"

#include "CVarCommands.h"
#include "ComputePasses/GatherPass.h"
#include "ComputePasses/GridAllocationPass.h"
#include "ComputePasses/ScatterPass.h"
#include "ComputePasses/SurfelIrradiancePass.h"
#include "ComputePasses/VisualizePass.h"
#include "Runtime/Renderer/Private/SceneRendering.h"
#include "Runtime/Renderer/Private/ScenePrivate.h"

// GPU stats, shown with `stat GPU0_Graphics0`. Must be at file scope.
DECLARE_GPU_STAT(SurfelGrid);
DECLARE_GPU_STAT(SurfelScatter);
DECLARE_GPU_STAT(SurfelGather);
DECLARE_GPU_STAT(SurfelIrradiance);
DECLARE_GPU_STAT(SurfelVisualize);

FSurfelSceneViewExtension::FSurfelSceneViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

void FSurfelSceneViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
	// Applies the lighting-reference visualize modes (Lumen / no Lumen / direct only).
	// Runs after the view's post-process settings are final, so it wins over both the
	// project's GI method and any Post Process Volume in the level.
	const UWorld* World = InViewFamily.Scene ? InViewFamily.Scene->GetWorld() : nullptr;
	if (!World || !World->IsGameWorld())
	{
		return;
	}

	FFinalPostProcessSettings& Settings = InView.FinalPostProcessSettings;

	switch ((ESurfelVisualizeMode)CVarSurfelVisualizeMode.GetValueOnGameThread())
	{
	case ESurfelVisualizeMode::LumenGI:
		Settings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::Lumen;
		Settings.ReflectionMethod = EReflectionMethod::Lumen;
		break;

	case ESurfelVisualizeMode::SurfelGI:
		Settings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::None;
		Settings.ReflectionMethod = EReflectionMethod::None;
		break;

	case ESurfelVisualizeMode::DirectLightOnly:
		Settings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::None;
		Settings.ReflectionMethod = EReflectionMethod::None;
		InViewFamily.EngineShowFlags.SetSkyLighting(false);
		InViewFamily.EngineShowFlags.SetReflectionEnvironment(false);
		break;
	default:
		break;
	}
}

void FSurfelSceneViewExtension::SubscribeToPostProcessingPass(
	EPostProcessingPass PassId,
	const FSceneView& View,
	FAfterPassCallbackDelegateArray& InOutPassCallbacks,
	bool bIsPassEnabled)
{
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
			this, &FSurfelSceneViewExtension::RunVisualizePass));
	}
}

void FSurfelSceneViewExtension::PostRenderBasePassDeferred_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& SceneView,
	const FRenderTargetBindingSlots& RenderTargets, TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
	// Drop last frame's grid handle before any early return: those RDG buffers belong to a
	// finished graph, and RunFullscreenPass must never pick them up if this frame skips the grid.
	if (SceneView.State)
	{
		SurfelFrameDataByViewKey.Remove(SceneView.State->GetViewKey());
	}
	
	// Update grid if changed by CVar
	FUniformGridViewState::UpdateFromCVars();

	// Are surfel enabled? Is view valid? Are the GBuffers?
	if (!CVarSurfelEnable.GetValueOnRenderThread() || !SceneView.State || !SceneTextures)
	{
		return;
	}

	// Get all the data for the surfels from this map	
	uint32 ViewKey = SceneView.State->GetViewKey();
	FSurfelViewState& SurfelState = ViewStates.FindOrAdd(ViewKey);

	uint32 CVarBudget = FMath::Max(1024, CVarSurfelBudget.GetValueOnRenderThread());
	const bool bRefreshRequested = SurfelState.LastRefreshRequestId != GSurfelRefreshRequestId;
	
	if (SurfelState.Budget != CVarBudget || bRefreshRequested)
	{
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
	FRDGBufferRef SurfelIrradianceBuffer;

	if (SurfelState.SurfelPositionAndRadius.IsValid())
	{
		SurfelPositionAndRadiusBuffer = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelPositionAndRadius);
		SurfelNormalAndFlagsBuffer = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelNormalAndFlags);
		SurfelCounterBuffer = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelCounter);
		SurfelIrradianceBuffer = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelIrradiance); 
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
		
		SurfelIrradianceBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), CVarBudget),
			TEXT("Surfel.Irradiance")
			);

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
	
	// Parameters for shaders related to the scene texture (the GBuffers)
	auto SceneTextureShaderParameters = CreateSceneTextureShaderParameters(GraphBuilder, SceneView, ESceneTextureSetupMode::GBuffers | ESceneTextureSetupMode::SceneDepth);
	
	// Snapped to whole cells so cell boundaries stay fixed in world space as the camera moves
	const double CellSize = FUniformGridViewState::CellSize;
	const FVector CameraCell(
		FMath::FloorToDouble(SceneView.ViewLocation.X / CellSize),
		FMath::FloorToDouble(SceneView.ViewLocation.Y / CellSize),
		FMath::FloorToDouble(SceneView.ViewLocation.Z / CellSize));
	FVector GridPosition = (CameraCell - FVector((double)(FUniformGridViewState::CellResolution / 2))) * CellSize;
	
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
	TShaderMapRef<FGridAllocationPass> ComputeShaderGrid(GetGlobalShaderMap(SceneView.GetFeatureLevel()));
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, SurfelGrid, "-------------------------------------------------------------- SurfelGrid");
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Surfel Grid"), ComputeShaderGrid, GridPassParameters, FComputeShaderUtils::GetGroupCount(FIntPoint(int(CVarBudget), 1), FIntPoint(64, 1)));
	}
		
	UE_LOG(LogTemp, Log, TEXT("Surfel: grid allocation ran, budget %u"), CVarBudget);

	// Scatter pass: reads the grid built above, writes the coverage map Gather then reads.
	// For RenderDoc
	RDG_EVENT_SCOPE(GraphBuilder, "Surfel Scatter Pass");
	
	FScatterSurfelPass::FParameters* ScatterPassParameters =
		GraphBuilder.AllocParameters<FScatterSurfelPass::FParameters>();
	
	const FIntPoint CoverageExtent(SceneView.UnscaledViewRect.Max.X, SceneView.UnscaledViewRect.Max.Y);
	
	FRDGTextureDesc CoverageDesc = FRDGTextureDesc::Create2D(
		CoverageExtent, PF_R32_FLOAT, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
	
	FRDGTextureRef CoverageTexture = GraphBuilder.CreateTexture(CoverageDesc, TEXT("CoverageTexture"));
	
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(CoverageTexture), FLinearColor::Black);
	
	// Parameters for Scatter
	ScatterPassParameters->CoverageTexture = GraphBuilder.CreateUAV(CoverageTexture);

	ScatterPassParameters->GridCellEntries = GraphBuilder.CreateUAV(GridCellEntriesBuffer);
	ScatterPassParameters->GridCounter = GraphBuilder.CreateUAV(GridCounterBuffer);
	
	ScatterPassParameters->GridPosition = FVector3f(GridPosition);
	ScatterPassParameters->GridCellCount = FUniformGridViewState::GridCellCount;
	ScatterPassParameters->CellResolution = FUniformGridViewState::CellResolution;
	ScatterPassParameters->CellCapacity = FUniformGridViewState::CellCapacity;
	ScatterPassParameters->CellSize = FUniformGridViewState::CellSize;
	
	ScatterPassParameters->View = SceneView.ViewUniformBuffer;
	ScatterPassParameters->ViewRectMin = FUintVector2(SceneView.UnscaledViewRect.Min.X, SceneView.UnscaledViewRect.Min.Y);
	ScatterPassParameters->ViewRectMax = FUintVector2(SceneView.UnscaledViewRect.Max.X, SceneView.UnscaledViewRect.Max.Y);

	ScatterPassParameters->SurfelBudget = CVarBudget;
	ScatterPassParameters->SurfelRadius = CVarSurfelRadius.GetValueOnRenderThread();
	ScatterPassParameters->SurfelCount              = GraphBuilder.CreateUAV(SurfelCounterBuffer);
	ScatterPassParameters->SurfelPositionAndRadius  = GraphBuilder.CreateUAV(SurfelPositionAndRadiusBuffer);
	ScatterPassParameters->SurfelNormalAndFlags     = GraphBuilder.CreateUAV(SurfelNormalAndFlagsBuffer);
	
	ScatterPassParameters->GBufferTextures = SceneTextureShaderParameters;
	
	// Add compute shader pass
	TShaderMapRef<FScatterSurfelPass> ComputeShaderScatter(GetGlobalShaderMap(SceneView.GetFeatureLevel()));
	
	const FIntPoint ViewSizeScatter = SceneView.UnscaledViewRect.Size();
	const FIntPoint GridDispatchSizeScatter(ViewSizeScatter.X, ViewSizeScatter.Y);
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, SurfelScatter, "-------------------------------------------------------------- SurfelScatter");
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Surfel Scatter"), ComputeShaderScatter, ScatterPassParameters, FComputeShaderUtils::GetGroupCount(GridDispatchSizeScatter, FIntPoint(16, 16)));
	}

	UE_LOG(LogTemp, Log, TEXT("Surfel: scatter pass ran, budget %u"), CVarBudget);
	
	// For RenderDoc
	RDG_EVENT_SCOPE(GraphBuilder, "Surfel Gather Pass");

	// Allocate memory for GatherPass parameters
	FGatherSurfelPass::FParameters* GatherPassParameters =
		GraphBuilder.AllocParameters<FGatherSurfelPass::FParameters>();
	
	const float SpawnsPerFrame = (float)FMath::Max(CVarSurfelSpawnsPerFrame.GetValueOnRenderThread(), 0);
	
	float SpawnChance = SpawnsPerFrame;
	GatherPassParameters->SpawnChance = SpawnChance;

	const float SpawnCoverageThreshold = CVarSurfelSpawnCoverageThreshold.GetValueOnRenderThread();
	GatherPassParameters->SpawnCoverageThreshold = SpawnCoverageThreshold;

	// Each group reduces a tile as wide as a new surfel's covered disc, not a fixed 16x16
	const uint32 SpawnTileSize = FGatherSurfelPass::ComputeSpawnTileSize(CVarSurfelRadius.GetValueOnRenderThread(), SpawnCoverageThreshold);
	GatherPassParameters->SpawnTileSize = SpawnTileSize;
	
	GatherPassParameters->CoverageTexture = CoverageTexture;

	GatherPassParameters->View = SceneView.ViewUniformBuffer;
	GatherPassParameters->ViewRectMin = FUintVector2(SceneView.UnscaledViewRect.Min.X, SceneView.UnscaledViewRect.Min.Y);
	GatherPassParameters->ViewRectMax = FUintVector2(SceneView.UnscaledViewRect.Max.X, SceneView.UnscaledViewRect.Max.Y);

	GatherPassParameters->SurfelBudget =  CVarBudget;
	GatherPassParameters->SurfelRadius = CVarSurfelRadius.GetValueOnRenderThread();

	GatherPassParameters->SurfelCount = GraphBuilder.CreateUAV(SurfelCounterBuffer);
	GatherPassParameters->SurfelPositionAndRadius = GraphBuilder.CreateUAV(SurfelPositionAndRadiusBuffer);
	GatherPassParameters->SurfelNormalAndFlags = GraphBuilder.CreateUAV(SurfelNormalAndFlagsBuffer);
	
	GatherPassParameters->GBufferTextures = SceneTextureShaderParameters;
	
	// Add compute shader pass
	TShaderMapRef<FGatherSurfelPass> ComputeShader(GetGlobalShaderMap(SceneView.GetFeatureLevel()));
	
	const FIntPoint ViewSize = SceneView.UnscaledViewRect.Size();
	const FIntPoint GridDispatchSize(ViewSize.X, ViewSize.Y);
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, SurfelGather, "-------------------------------------------------------------- SurfelGather");
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Surfel Gather"), ComputeShader, GatherPassParameters, FComputeShaderUtils::GetGroupCount(GridDispatchSize, FIntPoint(SpawnTileSize, SpawnTileSize)));
	}

	UE_LOG(LogTemp, Log, TEXT("Surfel: gather ran, budget %u"), CVarBudget);

	// For RenderDoc
	RDG_EVENT_SCOPE(GraphBuilder, "Surfel Irradiance Pass");
	
	// Allocate memory for IrradiancePass parameters
	FSurfelIrradiancePass::FParameters* IrradiancePassParameters =
		GraphBuilder.AllocParameters<FSurfelIrradiancePass::FParameters>();
	
	IrradiancePassParameters->View = SceneView.ViewUniformBuffer;
	IrradiancePassParameters->ViewRectMin = FUintVector2(SceneView.UnscaledViewRect.Min.X, SceneView.UnscaledViewRect.Min.Y);
	IrradiancePassParameters->ViewRectMax = FUintVector2(SceneView.UnscaledViewRect.Max.X, SceneView.UnscaledViewRect.Max.Y);

	IrradiancePassParameters->SurfelBudget =  CVarBudget;
	IrradiancePassParameters->SurfelRadius = CVarSurfelRadius.GetValueOnRenderThread();

	IrradiancePassParameters->SurfelCount = GraphBuilder.CreateUAV(SurfelCounterBuffer);
	IrradiancePassParameters->SurfelPositionAndRadius = GraphBuilder.CreateUAV(SurfelPositionAndRadiusBuffer);
	IrradiancePassParameters->SurfelNormalAndFlags = GraphBuilder.CreateUAV(SurfelNormalAndFlagsBuffer);
	IrradiancePassParameters->SurfelIrradiance = GraphBuilder.CreateUAV(SurfelIrradianceBuffer);
	
	IrradiancePassParameters->GBufferTextures = SceneTextureShaderParameters;
	
	IrradiancePassParameters->GridCellEntries = GraphBuilder.CreateUAV(GridCellEntriesBuffer);
	IrradiancePassParameters->GridCounter = GraphBuilder.CreateUAV(GridCounterBuffer);
	
	IrradiancePassParameters->GridPosition = FVector3f(GridPosition);
	IrradiancePassParameters->GridCellCount = FUniformGridViewState::GridCellCount;
	IrradiancePassParameters->CellResolution = FUniformGridViewState::CellResolution;
	IrradiancePassParameters->CellCapacity = FUniformGridViewState::CellCapacity;
	IrradiancePassParameters->CellSize = FUniformGridViewState::CellSize;
	
	// Add compute shader pass
	TShaderMapRef<FSurfelIrradiancePass> IrradianceComputeShader(GetGlobalShaderMap(SceneView.GetFeatureLevel()));
	
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, SurfelIrradiance, "-------------------------------------------------------------- SurfelIrradiance");
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Surfel Irradiance"), IrradianceComputeShader, IrradiancePassParameters, FComputeShaderUtils::GetGroupCount(FIntPoint(int(CVarBudget), 1), FIntPoint(64, 1)));
	}

	UE_LOG(LogTemp, Log, TEXT("Surfel: irradiance pass"));
		
	// A way to hand over the surfel data to the other hook which is the visualization one
	SurfelFrameDataByViewKey.Add(ViewKey, FSurfelFrameData{ GridCellEntriesBuffer, GridCounterBuffer, GridPassParameters->GridPosition, CoverageTexture });

	// Surfel counter readback for the ImGui budget meter
	if (!SurfelState.CounterReadback)
	{
		SurfelState.CounterReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Surfel.CounterReadback"));
		SurfelState.bCounterReadbackPending = false;
	}

	if (SurfelState.bCounterReadbackPending && SurfelState.CounterReadback->IsReady())
	{
		if (const uint32* Counter = static_cast<const uint32*>(SurfelState.CounterReadback->Lock(sizeof(uint32))))
		{
			// Gather can overshoot the budget momentarily before it decrements back
			GSurfelAllocatedCount.store(FMath::Min<int32>(*Counter, CVarBudget), std::memory_order_relaxed);
		}
		SurfelState.CounterReadback->Unlock();
		SurfelState.bCounterReadbackPending = false;
	}

	if (!SurfelState.bCounterReadbackPending)
	{
		AddEnqueueCopyPass(GraphBuilder, SurfelState.CounterReadback.Get(), SurfelCounterBuffer, sizeof(uint32));
		SurfelState.bCounterReadbackPending = true;
	}

	// Convert from RDG to the SurfelState struct from the map so surfels are persistent per frame
	// Otherwise RDG resources get freed here
	SurfelState.SurfelPositionAndRadius = GraphBuilder.ConvertToExternalBuffer(SurfelPositionAndRadiusBuffer);
	SurfelState.SurfelNormalAndFlags = GraphBuilder.ConvertToExternalBuffer(SurfelNormalAndFlagsBuffer);
	SurfelState.SurfelCounter = GraphBuilder.ConvertToExternalBuffer(SurfelCounterBuffer);
	SurfelState.SurfelIrradiance = GraphBuilder.ConvertToExternalBuffer(SurfelIrradianceBuffer);
}

FScreenPassTexture FSurfelSceneViewExtension::RunVisualizePass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& SceneView,
	const FPostProcessMaterialInputs& Inputs)
{
	const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
	if (!SceneColor.IsValid())
	{
		return SceneColor;
	}
	
	RDG_EVENT_SCOPE(GraphBuilder, "Surfel Fullscreen CS");

	// Views without persistent state (e.g. some scene captures/thumbnails) have no
	// ViewKey to look up a surfel pool with, so there is nothing to visualize.
	if (!SceneView.State)
	{
		return SceneColor;
	}

	const FScreenPassTextureViewport SceneColorViewport(SceneColor);
	const FIntPoint PassSize = SceneColor.ViewRect.Size();

	FRDGTextureRef OutputTexture = CreateOutputLike(GraphBuilder, SceneColor.Texture, TEXT("Surfel.FullscreenOutput"));

	FVisualizePassCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FVisualizePassCS::FParameters>();

	PassParameters->InputSceneColor = SceneColor.Texture;
	PassParameters->InputViewport   = GetScreenPassTextureViewportParameters(SceneColorViewport);
	PassParameters->Intensity       = CVarSurfelIntensity.GetValueOnRenderThread();
	PassParameters->OutRenderTarget = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(OutputTexture));
	PassParameters->View            = SceneView.ViewUniformBuffer;
	
	uint32 ViewKey = SceneView.State->GetViewKey();
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

	PassParameters->SurfelCount = GraphBuilder.CreateUAV(SurfelCounterBuffer);
	PassParameters->SurfelPositionAndRadius = GraphBuilder.CreateUAV(SurfelPositionAndRadiusBuffer);
	PassParameters->SurfelNormalAndFlags = GraphBuilder.CreateUAV(SurfelNormalAndFlagsBuffer);

	PassParameters->GBufferTextures = Inputs.SceneTextures;

	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUB =
		Inputs.SceneTextures.SceneTextures.GetUniformBuffer();
	
	FSurfelFrameData SurfelFrameData;
	const bool bHasFrameData = SurfelFrameDataByViewKey.RemoveAndCopyValue(ViewKey, SurfelFrameData);
	
	const int32 VisualizeMode = CVarSurfelVisualizeMode.GetValueOnRenderThread();
	const bool bUseGrid = bHasFrameData && (CVarSurfelUseGrid.GetValueOnRenderThread() != 0 || VisualizeMode == (int32)ESurfelVisualizeMode::Grid);

	if (bUseGrid)
	{
		PassParameters->GridCellEntries = GraphBuilder.CreateUAV(SurfelFrameData.GridCellEntries);
		PassParameters->GridCounter     = GraphBuilder.CreateUAV(SurfelFrameData.GridCounter);
		PassParameters->GridPosition    = SurfelFrameData.GridPosition;
	}
	else
	{
		// If brute forced was selected by user make a dummy grid
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
	PassParameters->SpawnCoverageThreshold = CVarSurfelSpawnCoverageThreshold.GetValueOnRenderThread();

	// Coverage map, for the Coverage visualization. Scatter writes it and Gather reads it
	const bool bHasCoverage = bHasFrameData && SurfelFrameData.CoverageTexture != nullptr;
	PassParameters->CoverageTexture = SurfelFrameData.CoverageTexture;
	PassParameters->bHasCoverageTexture = bHasCoverage ? 1u : 0u;
	PassParameters->CoverageScale = CVarSurfelCoverageScale.GetValueOnRenderThread();

	PassParameters->VisualizeMode = static_cast<uint32>(VisualizeMode);

	PassParameters->GridVisFlags = static_cast<uint32>(CVarSurfelGridVisFlags.GetValueOnRenderThread());
	PassParameters->GridVisEdgeThickness = FMath::Clamp(CVarSurfelGridVisEdgeThickness.GetValueOnRenderThread(), 0.0f, 0.5f);
	
	TShaderMapRef<FVisualizePassCS> ComputeShader(GetGlobalShaderMap(SceneView.GetFeatureLevel()));

	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, SurfelVisualize, "SurfelVisualize");
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Surfel Fullscreen %dx%d", PassSize.X, PassSize.Y),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(PassSize, FComputeShaderUtils::kGolden2DGroupSize));
	}

	// The post-process chain expects the result back in the texture handed initially
	AddCopyTexturePass(GraphBuilder, OutputTexture, SceneColor.Texture);

	return SceneColor;
}