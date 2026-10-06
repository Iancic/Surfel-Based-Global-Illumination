#include "DevGuiSubsystem.generated.h"
#pragma once

UCLASS()
class UDevGuiSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()
	
public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	virtual UWorld* GetTickableGameObjectWorld() const override { return GetWorld(); }
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override { return IsTemplate() ? ETickableTickType::Never : ETickableTickType::Always; }
	virtual bool IsTickableWhenPaused() const override { return true; }
	
	UFUNCTION(BlueprintCallable)
	static void ToggleImGuiInput();

private:
	void DrawScenesSection();
	void DrawCamerasSection();
	void DrawSurfelSettings();
	void DrawBudgetMeter();
	void DrawVisualizationSettings();
	void DrawGridSettings();


	int32 QualityPresetIndex = 0;
	float CameraBlendTime = 0.5f;
	float WindowFontScale = 1.0f;
	bool bInitializedFromCVars = false;
};
