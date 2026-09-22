#pragma once
#include "CoreMinimal.h"
#include "Runtime/Engine/Public/SceneViewExtension.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "ComputePasses/GatherPass.h"
#include "ComputePasses/VisualizePass.h"

class FSurfelSceneViewExtension : public FSceneViewExtensionBase
{
public:
	FSurfelSceneViewExtension(const FAutoRegister& AutoRegister);

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	// Applies the lighting-reference visualize modes (Lumen / no Lumen / direct only).
	// Runs after the view's post-process settings are final, so it wins over both the
	// project's GI method and any Post Process Volume in the level.
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

	// Called once per post-process stage so I can register a callback for a dispatch
	virtual void SubscribeToPostProcessingPass(
		EPostProcessingPass PassId,
		const FSceneView& View,
		FAfterPassCallbackDelegateArray& InOutPassCallbacks,
		bool bIsPassEnabled) override;

	// Called once after the deferred pass has been completed so I can register a callback for a dispatch
	virtual void PostRenderBasePassDeferred_RenderThread(
		FRDGBuilder& GraphBuilder,
		FSceneView& InView,
		const FRenderTargetBindingSlots& RenderTargets,
		TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;
	
	// Data we need to cache because RDG is not persistent per frame
	struct FSurfelViewState
	{
		TRefCountPtr<FRDGPooledBuffer> SurfelCounter;
		TRefCountPtr<FRDGPooledBuffer> SurfelPositionAndRadius;
		TRefCountPtr<FRDGPooledBuffer> SurfelNormalAndFlags;
	
		FVector PreviousPreViewTranslation = FVector::ZeroVector;
		uint32 Budget = 0;
		uint32 LastFrameSeen = 0;
		int32 LastRefreshRequestId = 0;
	};

	// Transient per-frame handles to what PostRenderBasePassDeferred_RenderThread built
	// (the grid from GridAllocation, the coverage map from Scatter), so RunFullscreenPass
	// - a separate callback that runs later in the same frame's render graph - can read them.
	// Not persisted across frames: both are rebuilt from scratch every frame,
	// then read here before that same FRDGBuilder executes.
	struct FSurfelFrameData
	{
		FRDGBufferRef GridCellEntries = nullptr;
		FRDGBufferRef GridCounter = nullptr;
		FVector3f GridPosition = FVector3f::ZeroVector;
		FRDGTextureRef CoverageTexture = nullptr;
	};
	TMap<uint32, FSurfelFrameData> SurfelFrameDataByViewKey;

	// The SVE is owned by an UEngineSubsystem, so it outlives individual worlds and PIE sessions
	// state must therefore be keyed, not global, or PIE restarts will inherit stale surfels from the previous run.

	// keyed by view key so PIE restarts and multiple viewports get distinct pools
	TMap<uint32, FSurfelViewState> ViewStates;
	
	// Utility to create UAV for the scene
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

private:
	FScreenPassTexture RunFullscreenPass(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);
};
