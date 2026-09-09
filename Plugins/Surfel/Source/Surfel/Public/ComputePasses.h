#pragma once
#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "ScreenPass.h"

enum class EGBufferVisualizeMode : int32
{
	SceneColor   = 0,
	BaseColor    = 1,
	WorldNormal  = 2,
	Metallic     = 3,
	Roughness    = 4,
	Specular     = 5,
	Depth        = 6,
	ShadingModel = 7,
};

class FComputePasses : public FSceneViewExtensionBase
{
public:
	FComputePasses(const FAutoRegister& AutoRegister);

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

	// Called once per post-process stage so we can register a callback when we want our dispatch
	virtual void SubscribeToPostProcessingPass(
		EPostProcessingPass PassId,
		const FSceneView& View,
		FAfterPassCallbackDelegateArray& InOutPassCallbacks,
		bool bIsPassEnabled) override;
	
	struct FSurfelViewState
	{
		TRefCountPtr<FRDGPooledBuffer> SurfelCounter;
		TRefCountPtr<FRDGPooledBuffer> SurfelPositionAndRadius;
		TRefCountPtr<FRDGPooledBuffer> SurfelNormalAndFlags;
	
		FVector PreviousPreViewTranslation = FVector::ZeroVector;
		uint32 Budget = 0;
		uint32 LastFrameSeen = 0;
	};

	// The SVE is owned by an UEngineSubsystem, so it outlives individual worlds and PIE sessions — state must therefore be keyed, not global, or PIE restarts will inherit stale surfels from the previous run.
	// keyed by view key so PIE restarts and multiple viewports get distinct pools
	TMap<uint32, FSurfelViewState> ViewStates;
	
	virtual void PostRenderBasePassDeferred_RenderThread(
		FRDGBuilder& GraphBuilder,
		FSceneView& InView,
		const FRenderTargetBindingSlots& RenderTargets,
		TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;

private:
	FScreenPassTexture RunFullscreenPass(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);

	FScreenPassTexture RunGBufferPass(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);
};
