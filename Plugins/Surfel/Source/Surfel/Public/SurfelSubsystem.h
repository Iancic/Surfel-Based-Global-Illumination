#pragma once
#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "SurfelSubsystem.generated.h"

class FSurfelSceneViewExtension;

UCLASS()
class SURFEL_API USurfelSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	
	TSharedPtr<FSurfelSceneViewExtension, ESPMode::ThreadSafe> SurfelSceneViewExtension;
};
