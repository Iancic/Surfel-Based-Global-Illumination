#include "SurfelSubsystem.h"
#include "ComputePasses.h"
#include "SceneViewExtension.h"

// Surfel plugin - keeps the SceneViewExtension alive for the engine's lifetime.

void USurfelSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	SurfelSceneViewExtension = FSceneViewExtensions::NewExtension<FComputePasses>();
	UE_LOG(LogTemp, Log, TEXT("Surfel: subsystem initialized"));
}

void USurfelSubsystem::Deinitialize()
{
	if (SurfelSceneViewExtension.IsValid())
	{
		// Force the extension inactive before dropping it, so the render thread
		// stops calling into an object that is about to be destroyed.
		SurfelSceneViewExtension->IsActiveThisFrameFunctions.Empty();

		FSceneViewExtensionIsActiveFunctor IsActiveFunctor;
		IsActiveFunctor.IsActiveFunction =
			[](const ISceneViewExtension* SceneViewExtension, const FSceneViewExtensionContext& Context)
			{
				return TOptional<bool>(false);
			};

		SurfelSceneViewExtension->IsActiveThisFrameFunctions.Add(IsActiveFunctor);
	}

	SurfelSceneViewExtension.Reset();
}
