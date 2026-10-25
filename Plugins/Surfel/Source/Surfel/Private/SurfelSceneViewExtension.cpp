#pragma once
#include "SurfelSceneViewExtension.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "PostProcess/PostProcessMaterialInputs.h"

#include "CVarCommands.h"
#include "ComputePasses/CompositePass.h"
#include "ComputePasses/SpawnPass.h"
#include "ComputePasses/ScatterPass.h"
#include "ComputePasses/GatherPass.h"
#include "ComputePasses/SurfelIrradiancePass.h"
#include "ComputePasses/TextureIrradiancePass.h"
#include "ComputePasses/VisualizePass.h"
#include "Runtime/Renderer/Private/SceneRendering.h"
#include "SystemTextures.h"
#include "Runtime/Renderer/Private/ScenePrivate.h"

FSurfelSceneViewExtension::FSurfelSceneViewExtension(const FAutoRegister& AutoRegister) : FSceneViewExtensionBase(AutoRegister) {}

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

	switch (static_cast<ESurfelVisualizeMode>(CVarSurfelVisualizeMode.GetValueOnGameThread()))
	{
	case ESurfelVisualizeMode::LumenGI:
		Settings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::Lumen;
		Settings.ReflectionMethod = EReflectionMethod::Lumen;
		break;

	case ESurfelVisualizeMode::SurfelGI:
	case ESurfelVisualizeMode::IndirectLight: // Overwritten by the visualize pass anyway, so don't pay for Lumen
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
	FRDGBufferRef SurfelAlbedoBuffer;

	if (SurfelState.SurfelPositionAndRadius.IsValid())
	{
		SurfelPositionAndRadiusBuffer = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelPositionAndRadius);
		SurfelNormalAndFlagsBuffer = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelNormalAndFlags);
		SurfelCounterBuffer = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelCounter);
		SurfelIrradianceBuffer = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelIrradiance); 
		SurfelAlbedoBuffer = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelAlbedo);
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
		
		SurfelIrradianceBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), CVarBudget),
			TEXT("Surfel.Irradiance")
			);
		
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SurfelIrradianceBuffer), 0u);
		
		SurfelAlbedoBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), CVarBudget),
			TEXT("Surfel.Albedo"));
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
	RDG_EVENT_SCOPE(GraphBuilder, "Surfel Scatter Pass");
	
	// Allocate memory for ScatterPass parameters
	FScatterPass::FParameters* ScatterPassParameters =
		GraphBuilder.AllocParameters<FScatterPass::FParameters>();
	
	// Set Parameters For Grid
	ScatterPassParameters->GridCellEntries = GraphBuilder.CreateUAV(GridCellEntriesBuffer);
	ScatterPassParameters->GridCounter = GraphBuilder.CreateUAV(GridCounterBuffer);
	ScatterPassParameters->GridPosition = FVector3f(GridPosition);
	ScatterPassParameters->GridCellCount = FUniformGridViewState::GridCellCount;
	ScatterPassParameters->CellResolution = FUniformGridViewState::CellResolution;
	ScatterPassParameters->CellCapacity = FUniformGridViewState::CellCapacity;
	ScatterPassParameters->CellSize = FUniformGridViewState::CellSize;
	
	// Set Parameters For Surfels
	// Already above just create a new UAV
	ScatterPassParameters->SurfelCount = GraphBuilder.CreateUAV(SurfelCounterBuffer);
	ScatterPassParameters->SurfelPositionAndRadius = GraphBuilder.CreateUAV(SurfelPositionAndRadiusBuffer);
	ScatterPassParameters->SurfelNormalAndFlags = GraphBuilder.CreateUAV(SurfelNormalAndFlagsBuffer);
	ScatterPassParameters->SurfelBudget = CVarBudget;
	ScatterPassParameters->SurfelRadius = CVarSurfelRadius.GetValueOnRenderThread();

	// Dispatch Compute
	TShaderMapRef<FScatterPass> ComputeShaderScatter(GetGlobalShaderMap(SceneView.GetFeatureLevel()));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Surfel Scatter"), ComputeShaderScatter, ScatterPassParameters, FComputeShaderUtils::GetGroupCount(FIntPoint(static_cast<int>(CVarBudget), 1), FIntPoint(64, 1)));
	
	// Gather pass: reads the grid built above, writes the coverage map Spawn then reads.
	// For RenderDoc
	RDG_EVENT_SCOPE(GraphBuilder, "Surfel Gather Pass");
	
	FGatherSurfelPass::FParameters* GatherPassParameters =
		GraphBuilder.AllocParameters<FGatherSurfelPass::FParameters>();
	
	const FIntPoint CoverageExtent(SceneView.UnscaledViewRect.Max.X, SceneView.UnscaledViewRect.Max.Y);
	
	FRDGTextureDesc CoverageDesc = FRDGTextureDesc::Create2D(
		CoverageExtent, PF_R32_FLOAT, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
	
	FRDGTextureRef CoverageTexture = GraphBuilder.CreateTexture(CoverageDesc, TEXT("CoverageTexture"));
	
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(CoverageTexture), FLinearColor::Black);
	
	// Parameters for Gather
	GatherPassParameters->CoverageTexture = GraphBuilder.CreateUAV(CoverageTexture);

	GatherPassParameters->GridCellEntries = GraphBuilder.CreateUAV(GridCellEntriesBuffer);
	GatherPassParameters->GridCounter = GraphBuilder.CreateUAV(GridCounterBuffer);
	
	GatherPassParameters->GridPosition = FVector3f(GridPosition);
	GatherPassParameters->GridCellCount = FUniformGridViewState::GridCellCount;
	GatherPassParameters->CellResolution = FUniformGridViewState::CellResolution;
	GatherPassParameters->CellCapacity = FUniformGridViewState::CellCapacity;
	GatherPassParameters->CellSize = FUniformGridViewState::CellSize;
	
	GatherPassParameters->View = SceneView.ViewUniformBuffer;
	GatherPassParameters->ViewRectMin = FUintVector2(SceneView.UnscaledViewRect.Min.X, SceneView.UnscaledViewRect.Min.Y);
	GatherPassParameters->ViewRectMax = FUintVector2(SceneView.UnscaledViewRect.Max.X, SceneView.UnscaledViewRect.Max.Y);

	GatherPassParameters->SurfelBudget = CVarBudget;
	GatherPassParameters->SurfelRadius = CVarSurfelRadius.GetValueOnRenderThread();
	GatherPassParameters->SurfelCount = GraphBuilder.CreateUAV(SurfelCounterBuffer);
	GatherPassParameters->SurfelPositionAndRadius = GraphBuilder.CreateUAV(SurfelPositionAndRadiusBuffer);
	GatherPassParameters->SurfelNormalAndFlags = GraphBuilder.CreateUAV(SurfelNormalAndFlagsBuffer);
	
	GatherPassParameters->GBufferTextures = SceneTextureShaderParameters;
	
	// Add compute shader pass
	TShaderMapRef<FGatherSurfelPass> ComputeShaderGather(GetGlobalShaderMap(SceneView.GetFeatureLevel()));
	
	const FIntPoint ViewSizeGather = SceneView.UnscaledViewRect.Size();
	const FIntPoint GridDispatchSizeGather(ViewSizeGather.X, ViewSizeGather.Y);
	
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Surfel Gather"), ComputeShaderGather, GatherPassParameters, FComputeShaderUtils::GetGroupCount(GridDispatchSizeGather, FIntPoint(16, 16)));

	// For RenderDoc
	RDG_EVENT_SCOPE(GraphBuilder, "Surfel Spawn Pass");

	// Allocate memory for SpawnPass parameters
	FSpawnSurfelPass::FParameters* SpawnPassParameters =
		GraphBuilder.AllocParameters<FSpawnSurfelPass::FParameters>();
	
	const float SpawnsPerFrame = static_cast<float>(FMath::Max(CVarSurfelSpawnsPerFrame.GetValueOnRenderThread(), 0));
	
	float SpawnChance = SpawnsPerFrame;
	SpawnPassParameters->SpawnChance = SpawnChance;

	const float SpawnCoverageThreshold = CVarSurfelSpawnCoverageThreshold.GetValueOnRenderThread();
	SpawnPassParameters->SpawnCoverageThreshold = SpawnCoverageThreshold;

	// Each group reduces a tile as wide as a new Surfels covered disc, not a fixed 16x16
	// I did this cause otherwise if a Surfel is bigger than the grid size it would cover more grid cells than necessary
	const uint32 SpawnTileSize = FSpawnSurfelPass::ComputeSpawnTileSize(CVarSurfelRadius.GetValueOnRenderThread(), SpawnCoverageThreshold);
	SpawnPassParameters->SpawnTileSize = SpawnTileSize;
	
	SpawnPassParameters->CoverageTexture = CoverageTexture;

	SpawnPassParameters->View = SceneView.ViewUniformBuffer;
	SpawnPassParameters->ViewRectMin = FUintVector2(SceneView.UnscaledViewRect.Min.X, SceneView.UnscaledViewRect.Min.Y);
	SpawnPassParameters->ViewRectMax = FUintVector2(SceneView.UnscaledViewRect.Max.X, SceneView.UnscaledViewRect.Max.Y);

	SpawnPassParameters->SurfelBudget =  CVarBudget;
	SpawnPassParameters->SurfelRadius = CVarSurfelRadius.GetValueOnRenderThread();

	SpawnPassParameters->SurfelCount = GraphBuilder.CreateUAV(SurfelCounterBuffer);
	SpawnPassParameters->SurfelPositionAndRadius = GraphBuilder.CreateUAV(SurfelPositionAndRadiusBuffer);
	SpawnPassParameters->SurfelNormalAndFlags = GraphBuilder.CreateUAV(SurfelNormalAndFlagsBuffer);
	SpawnPassParameters->SurfelAlbedo = GraphBuilder.CreateUAV(SurfelAlbedoBuffer);
	
	SpawnPassParameters->GBufferTextures = SceneTextureShaderParameters;
	
	// Add compute shader pass
	TShaderMapRef<FSpawnSurfelPass> ComputeShader(GetGlobalShaderMap(SceneView.GetFeatureLevel()));
	
	const FIntPoint ViewSize = SceneView.UnscaledViewRect.Size();
	const FIntPoint GridDispatchSize(ViewSize.X, ViewSize.Y);
	
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Surfel Spawn"), ComputeShader, SpawnPassParameters, FComputeShaderUtils::GetGroupCount(GridDispatchSize, FIntPoint(SpawnTileSize, SpawnTileSize)));

	// For RenderDoc
	RDG_EVENT_SCOPE(GraphBuilder, "Surfel Irradiance Pass");

	// Get lights from scene and add to buffer for irradiance
	TArray<FSurfelLightStruct> SceneLights;
	if (SceneView.Family->Scene)
	{
		const FScene* Scene = SceneView.Family->Scene->GetRenderScene();
		if (Scene)
		{
			for (const FLightSceneInfoCompact& SceneLightsInfo: Scene->Lights)
			{
				const FLightSceneProxy* LightProxy = SceneLightsInfo.LightSceneInfo->Proxy;
				const uint8_t LightType = LightProxy->GetLightType();
				if (!LightProxy->AffectGlobalIllumination() || LightType == LightType_Rect)
				{
					continue;
				}
				
				FLightRenderParameters RenderParameters;
				LightProxy->GetLightShaderParameters(RenderParameters);
				
				// Translated world position
				// Same space needed for the global SDF
				FLightShaderParameters ShaderParameters;
				RenderParameters.MakeShaderParameters(SceneView.ViewMatrices, 1.0f, ShaderParameters);
				
				// Populate array element
				FSurfelLightStruct& Light = SceneLights.AddDefaulted_GetRef();
				Light.PositionAndAttenuation = FVector4f(ShaderParameters.TranslatedWorldPosition, ShaderParameters.InvRadius);
				Light.ColorAndType = FVector4f(ShaderParameters.Color * LightProxy->GetIndirectLightingScale(), static_cast<float>(LightType));
				Light.DirectionAndInverseFalloff = FVector4f(ShaderParameters.Direction, ShaderParameters.FalloffExponent);
				Light.Angle = FVector4f(ShaderParameters.SpotAngles.X, ShaderParameters.SpotAngles.Y, 0, 0);
			}
		}
	}
	
	// Make sure array is not zero for RDG
	const uint32 LightsNumber = SceneLights.Num();
	if (SceneLights.IsEmpty())
	{
		SceneLights.AddZeroed();
	}
	
	// Create RDG buffer ref for scene lights
	FRDGBufferRef LightsBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("Surfel Lights"), SceneLights);
	
	// Allocate memory for IrradiancePass parameters
	FSurfelIrradiancePass::FParameters* IrradiancePassParameters =
		GraphBuilder.AllocParameters<FSurfelIrradiancePass::FParameters>();
	
	IrradiancePassParameters->SurfelLights = GraphBuilder.CreateSRV(LightsBuffer);
	IrradiancePassParameters->NumSurfelLights = LightsNumber;
	
	IrradiancePassParameters->View = SceneView.ViewUniformBuffer;
	IrradiancePassParameters->ViewRectMin = FUintVector2(SceneView.UnscaledViewRect.Min.X, SceneView.UnscaledViewRect.Min.Y);
	IrradiancePassParameters->ViewRectMax = FUintVector2(SceneView.UnscaledViewRect.Max.X, SceneView.UnscaledViewRect.Max.Y);

	IrradiancePassParameters->SurfelBudget =  CVarBudget;
	IrradiancePassParameters->SurfelRadius = CVarSurfelRadius.GetValueOnRenderThread();

	IrradiancePassParameters->SurfelCount = GraphBuilder.CreateUAV(SurfelCounterBuffer);
	IrradiancePassParameters->SurfelPositionAndRadius = GraphBuilder.CreateUAV(SurfelPositionAndRadiusBuffer);
	IrradiancePassParameters->SurfelNormalAndFlags = GraphBuilder.CreateUAV(SurfelNormalAndFlagsBuffer);
	IrradiancePassParameters->SurfelIrradiance = GraphBuilder.CreateUAV(SurfelIrradianceBuffer);
	IrradiancePassParameters->SurfelAlbedo = GraphBuilder.CreateUAV(SurfelAlbedoBuffer);
	
	IrradiancePassParameters->GBufferTextures = SceneTextureShaderParameters;
	
	IrradiancePassParameters->GridCellEntries = GraphBuilder.CreateUAV(GridCellEntriesBuffer);
	IrradiancePassParameters->GridCounter = GraphBuilder.CreateUAV(GridCounterBuffer);
	
	IrradiancePassParameters->GridPosition = FVector3f(GridPosition);
	IrradiancePassParameters->GridCellCount = FUniformGridViewState::GridCellCount;
	IrradiancePassParameters->CellResolution = FUniformGridViewState::CellResolution;
	IrradiancePassParameters->CellCapacity = FUniformGridViewState::CellCapacity;
	IrradiancePassParameters->CellSize = FUniformGridViewState::CellSize;
		
	IrradiancePassParameters->MinTraceDistance = CVarMinTraceDistance.GetValueOnRenderThread();
	IrradiancePassParameters->MaxTraceDistance = CVarMaxTraceDistance.GetValueOnRenderThread();
	IrradiancePassParameters->StepFactor = CVarStepFactor.GetValueOnRenderThread();
	IrradiancePassParameters->MinStepFactor = CVarMinStepFactor.GetValueOnRenderThread();
	IrradiancePassParameters->MaxSamples = CVarMaxSamples.GetValueOnRenderThread();
	
	// Add Irradiance pass
	TShaderMapRef<FSurfelIrradiancePass> IrradianceComputeShader(GetGlobalShaderMap(SceneView.GetFeatureLevel()));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Surfel Irradiance"), IrradianceComputeShader, IrradiancePassParameters, FComputeShaderUtils::GetGroupCount(FIntPoint(static_cast<int>(CVarBudget), 1), FIntPoint(64, 1)));

	// Texture irradiance pass store screen irradiance into a texture
	FRDGTextureRef IrradianceTexture = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(CoverageExtent, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV),
		TEXT("Surfel.IrradianceTexture"));

	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(IrradianceTexture), FLinearColor::Black);

	// Ambient occlusion (screen space * material), kept out of the irradiance so that stays pure surfel light
	// Cleared to white, 1 = unoccluded
	FRDGTextureRef AmbientOcclusionTexture = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(CoverageExtent, PF_R16F, FClearValueBinding::White, TexCreate_ShaderResource | TexCreate_UAV),
		TEXT("Surfel.AmbientOcclusionTexture"));

	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(AmbientOcclusionTexture), FLinearColor::White);

	FTextureIrradiancePass::FParameters* TextureIrradiancePassParameters =
		GraphBuilder.AllocParameters<FTextureIrradiancePass::FParameters>();

	TextureIrradiancePassParameters->IrradianceTexture = GraphBuilder.CreateUAV(IrradianceTexture);
	TextureIrradiancePassParameters->AmbientOcclusionTexture = GraphBuilder.CreateUAV(AmbientOcclusionTexture);

	TextureIrradiancePassParameters->SurfelPositionAndRadius = GraphBuilder.CreateUAV(SurfelPositionAndRadiusBuffer);
	TextureIrradiancePassParameters->SurfelNormalAndFlags = GraphBuilder.CreateUAV(SurfelNormalAndFlagsBuffer);
	TextureIrradiancePassParameters->SurfelIrradiance = GraphBuilder.CreateUAV(SurfelIrradianceBuffer);

	TextureIrradiancePassParameters->View = SceneView.ViewUniformBuffer;
	TextureIrradiancePassParameters->ViewRectMin = FUintVector2(SceneView.UnscaledViewRect.Min.X, SceneView.UnscaledViewRect.Min.Y);
	TextureIrradiancePassParameters->ViewRectMax = FUintVector2(SceneView.UnscaledViewRect.Max.X, SceneView.UnscaledViewRect.Max.Y);

	TextureIrradiancePassParameters->GridCellEntries = GraphBuilder.CreateUAV(GridCellEntriesBuffer);
	TextureIrradiancePassParameters->GridCounter = GraphBuilder.CreateUAV(GridCounterBuffer);
	TextureIrradiancePassParameters->GridPosition = FVector3f(GridPosition);
	TextureIrradiancePassParameters->CellResolution = FUniformGridViewState::CellResolution;
	TextureIrradiancePassParameters->CellCapacity = FUniformGridViewState::CellCapacity;
	TextureIrradiancePassParameters->CellSize = FUniformGridViewState::CellSize;

	TextureIrradiancePassParameters->GBufferTextures = SceneTextureShaderParameters;

	TShaderMapRef<FTextureIrradiancePass> TextureIrradianceComputeShader(GetGlobalShaderMap(SceneView.GetFeatureLevel()));

	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Surfel Texture Irradiance"), TextureIrradianceComputeShader, TextureIrradiancePassParameters, FComputeShaderUtils::GetGroupCount(SceneView.UnscaledViewRect.Size(), FIntPoint(16, 16)));

	// Composite: add the surfel indirect light into scene color
	// Scene color only holds the base pass here, UE's direct lights get added on top of it afterwards
	FRDGTextureRef SceneColorTexture = RenderTargets[0].GetTexture();
	if (ShouldCompositeSurfelGI(CVarSurfelVisualizeMode.GetValueOnRenderThread()) && SceneColorTexture)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "Surfel Composite Pass");

		FCompositePass::FParameters* CompositePassParameters =
			GraphBuilder.AllocParameters<FCompositePass::FParameters>();

		CompositePassParameters->SceneColor = GraphBuilder.CreateUAV(SceneColorTexture);
		CompositePassParameters->IrradianceTexture = IrradianceTexture;
		CompositePassParameters->AmbientOcclusionTexture = AmbientOcclusionTexture;
		CompositePassParameters->GIIntensity = CVarSurfelGIIntensity.GetValueOnRenderThread();
		CompositePassParameters->bUseAO = CVarSurfelUseAO.GetValueOnRenderThread() != 0 ? 1u : 0u;

		CompositePassParameters->View = SceneView.ViewUniformBuffer;
		CompositePassParameters->ViewRectMin = FUintVector2(SceneView.UnscaledViewRect.Min.X, SceneView.UnscaledViewRect.Min.Y);
		CompositePassParameters->ViewRectMax = FUintVector2(SceneView.UnscaledViewRect.Max.X, SceneView.UnscaledViewRect.Max.Y);

		CompositePassParameters->GBufferTextures = SceneTextureShaderParameters;

		TShaderMapRef<FCompositePass> CompositeComputeShader(GetGlobalShaderMap(SceneView.GetFeatureLevel()));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Surfel Composite"), CompositeComputeShader, CompositePassParameters, FComputeShaderUtils::GetGroupCount(SceneView.UnscaledViewRect.Size(), FIntPoint(16, 16)));
	}

	// A way to hand over the surfel data to the other hook which is the visualization one
	SurfelFrameDataByViewKey.Add(ViewKey, FSurfelFrameData{ GridCellEntriesBuffer, GridCounterBuffer, ScatterPassParameters->GridPosition, CoverageTexture, IrradianceTexture, AmbientOcclusionTexture });

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
			// Spawn can overshoot the budget momentarily before it decrements back
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
	SurfelState.SurfelAlbedo = GraphBuilder.ConvertToExternalBuffer(SurfelAlbedoBuffer);
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
	FRDGBufferRef SurfelIrradianceBuffer;
	FRDGBufferRef SurfelAlbedoBuffer;

	if (SurfelState.SurfelPositionAndRadius.IsValid() && SurfelState.SurfelIrradiance.IsValid() && SurfelState.SurfelAlbedo.IsValid())
	{
		SurfelPositionAndRadiusBuffer = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelPositionAndRadius);
		SurfelNormalAndFlagsBuffer    = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelNormalAndFlags);
		SurfelCounterBuffer           = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelCounter);
		SurfelIrradianceBuffer        = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelIrradiance);
		SurfelAlbedoBuffer            = GraphBuilder.RegisterExternalBuffer(SurfelState.SurfelAlbedo);
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

		// Counter is 0 here so the shader reads none of these, they only have to exist for binding
		SurfelIrradianceBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), CVarBudget),
			TEXT("Surfel.Irradiance"));

		SurfelAlbedoBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), CVarBudget),
			TEXT("Surfel.Albedo"));
	}

	PassParameters->SurfelCount = GraphBuilder.CreateUAV(SurfelCounterBuffer);
	PassParameters->SurfelPositionAndRadius = GraphBuilder.CreateUAV(SurfelPositionAndRadiusBuffer);
	PassParameters->SurfelNormalAndFlags = GraphBuilder.CreateUAV(SurfelNormalAndFlagsBuffer);
	PassParameters->SurfelIrradiance = GraphBuilder.CreateUAV(SurfelIrradianceBuffer);
	PassParameters->SurfelAlbedo = GraphBuilder.CreateUAV(SurfelAlbedoBuffer);
	PassParameters->SurfelColorMode = static_cast<uint32>(FMath::Clamp(CVarSurfelColorMode.GetValueOnRenderThread(), 0, (int32)ESurfelColorMode::MAX - 1));

	PassParameters->GBufferTextures = Inputs.SceneTextures;
	
	FSurfelFrameData SurfelFrameData;
	const bool bHasFrameData = SurfelFrameDataByViewKey.RemoveAndCopyValue(ViewKey, SurfelFrameData);
	
	const int32 VisualizeMode = CVarSurfelVisualizeMode.GetValueOnRenderThread();
	const bool bUseGrid = bHasFrameData && (CVarSurfelUseGrid.GetValueOnRenderThread() != 0 || VisualizeMode == static_cast<int32>(ESurfelVisualizeMode::Grid));

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

	// Coverage map, for the Coverage visualization. Gather writes it and Spawn reads it
	const bool bHasCoverage = bHasFrameData && SurfelFrameData.CoverageTexture != nullptr;
	PassParameters->CoverageTexture = SurfelFrameData.CoverageTexture;
	PassParameters->bHasCoverageTexture = bHasCoverage ? 1u : 0u;
	PassParameters->CoverageScale = CVarSurfelCoverageScale.GetValueOnRenderThread();

	// Per-pixel irradiance, for the Irradiance visualization. A black dummy keeps RDG happy when it is missing
	const bool bHasIrradiance = bHasFrameData && SurfelFrameData.IrradianceTexture != nullptr;
	PassParameters->IrradianceTexture = bHasIrradiance ? SurfelFrameData.IrradianceTexture : GSystemTextures.GetBlackDummy(GraphBuilder);
	PassParameters->bHasIrradianceTexture = bHasIrradiance ? 1u : 0u;

	// AO for the Indirect Light view. White dummy = no occlusion when it is missing
	const bool bHasAmbientOcclusion = bHasFrameData && SurfelFrameData.AmbientOcclusionTexture != nullptr;
	PassParameters->AmbientOcclusionTexture = bHasAmbientOcclusion ? SurfelFrameData.AmbientOcclusionTexture : GSystemTextures.GetWhiteDummy(GraphBuilder);
	PassParameters->GIIntensity = CVarSurfelGIIntensity.GetValueOnRenderThread();

	PassParameters->VisualizeMode = static_cast<uint32>(VisualizeMode);

	PassParameters->GridVisFlags = static_cast<uint32>(CVarSurfelGridVisFlags.GetValueOnRenderThread());
	PassParameters->GridVisEdgeThickness = FMath::Clamp(CVarSurfelGridVisEdgeThickness.GetValueOnRenderThread(), 0.0f, 0.5f);
	
	TShaderMapRef<FVisualizePassCS> ComputeShader(GetGlobalShaderMap(SceneView.GetFeatureLevel()));

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("Surfel Fullscreen %dx%d", PassSize.X, PassSize.Y),
		ComputeShader,
		PassParameters,
		FComputeShaderUtils::GetGroupCount(PassSize, FComputeShaderUtils::kGolden2DGroupSize));

	// The post-process chain expects the result back in the texture handed initially
	AddCopyTexturePass(GraphBuilder, OutputTexture, SceneColor.Texture);

	return SceneColor;
}