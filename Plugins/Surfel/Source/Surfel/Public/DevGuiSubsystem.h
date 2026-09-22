#include "DevGuiSubsystem.generated.h"
#pragma once

UCLASS()
class UDevGuiSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()
	
public:
	// USubsystem implementation Begin
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	// USubsystem implementation End

	// FTickableGameObject implementation Begin
	virtual UWorld* GetTickableGameObjectWorld() const override { return GetWorld(); }
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override { return IsTemplate() ? ETickableTickType::Never : ETickableTickType::Always; }
	virtual bool IsTickableWhenPaused() const override { return true; }
	// FTickableGameObject implementation End

	UFUNCTION(BlueprintCallable)
	static void ToggleImGuiInput();

private:
	void DrawScenesSection();
	void DrawCamerasSection();
	void DrawSurfelSettings();
	void DrawVisualizationSettings();
	void DrawGridSettings();

	// Index into GSurfelQualityPresets; the last entry is Custom ("Sliders"), the only one
	// where the manual sliders are editable. Mirrored here so the combo keeps its selection
	// between frames.
	int32 QualityPresetIndex = 0;

	// How long SetViewTargetWithBlend takes when switching cameras, in seconds.
	float CameraBlendTime = 0.5f;

	// ImGui text is tiny on high-DPI displays, so make it scalable.
	float WindowFontScale = 1.0f;

	// The quality preset has to be seeded from the budget CVar once at startup,
	// otherwise the combo claims "Low" no matter what the budget actually is.
	bool bInitializedFromCVars = false;
};
