#pragma once
#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "SurfelSubsystem.generated.h"

class FComputePasses;

UCLASS()
class SURFEL_API USurfelSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	
	// Surfel plugin - keeps the SceneViewExtension alive for the engine's lifetime.
	TSharedPtr<FComputePasses, ESPMode::ThreadSafe> SurfelSceneViewExtension;
};
