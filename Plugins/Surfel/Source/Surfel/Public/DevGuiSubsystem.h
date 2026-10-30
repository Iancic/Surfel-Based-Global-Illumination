#pragma once
#include "DevGuiSubsystem.generated.h"

UCLASS()
class UDevGuiSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()
	
public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

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
	void DrawAntiAliasingSettings();
	void DrawGridSettings();
	void DrawFreeMovementSection();
	void DrawScreenshotSection();


	// Screenshot the viewport is handed to the engine and only lands on disk a frame later,
	// so the path is held until then to report where it ended up.
	FString PendingScreenshotPath;
	FString ScreenshotStatus;

	int32 QualityPresetIndex = 0;
	float CameraBlendTime = 0.5f;
	float WindowFontScale = 1.0f;
	bool bInitializedFromCVars = false;
};
