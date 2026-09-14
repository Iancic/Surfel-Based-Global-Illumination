#include "SurfelSubsystem.h"
#include "SurfelSceneViewExtension.h"
#include "SceneViewExtension.h"

void USurfelSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	SurfelSceneViewExtension = FSceneViewExtensions::NewExtension<FSurfelSceneViewExtension>();
	UE_LOG(LogTemp, Log, TEXT("Surfel: subsystem initialized"));
}

void USurfelSubsystem::Deinitialize()
{
	// Force the extension inactive before dropping it, so the render thread
	// stops calling into an object that is about to be destroyed.
	if (SurfelSceneViewExtension.IsValid())
	{
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
