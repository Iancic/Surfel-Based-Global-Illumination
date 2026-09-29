#include "DevGuiSubsystem.h"
#include "CVarCommands.h"
#include "ImGuiModule.h"
#include "EngineUtils.h"
#include "Camera/CameraActor.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/PackageName.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Styling/StarshipCoreStyle.h"

// - David
// Credit AI Usage: After I laid out some general scaffolding and to do ideas, I used Claude Code to generate most of the visualization code
// Prompts used: 
	// @Plugins/Surfel/Source/Surfel/Private/DevGuiSubsystem.cpp Inside this can you change ImGui editor window size. 
	// Secondly I have 2 combo options that should have for the first, changing budget of surfels, second combo should display all those things so also make change to the visualize shader to support those when I click the different combos. 
	// Like the coverage texture. I am also curious in the end maybe tell me how to setup a camera system so I can change from all those scenes I said above.
// Another prompt: 
	// @Plugins/Surfel/Source/Surfel/Private/DevGuiSubsystem.cpp  I left out a few to do items around the file for the visualization of the grid and its options. 
	// Enabling or disabling manual settings for surfels when custom is selected. And investigate more on how to disable and enable lumen from that ImGui button. For that I say you can modify source outside of this file also. 
	// Last thing, investigate how to enable ImGui Input to true by default without me going in and typing the CVar command every time.

UE_DISABLE_OPTIMIZATION

namespace
{

	struct FDevScene
	{
		const char* Name;
		const TCHAR* PackagePath;
	};

	const FDevScene GDevScenes[] =
	{
		{ "Static Sponza",  TEXT("/Game/StaticSponza") },
		{ "Dynamic Sponza", TEXT("/Game/DynamicSponza") },
		{ "Cornell Box",    TEXT("/Game/CornellBox") },
		{ "Dragon",        TEXT("/Game/Dragon") },
	};

	struct FSurfelQualityPreset
	{
		const char* Name;
		int32 Budget;
		float Radius;
		int32 SpawnsPerFrame;
	};
	
	// Smaller radius - high cost but sharper detail
	// Larger radius - fewer surfel to cover but approximated detail
	// Quality Settings Should also change grid settings
	const FSurfelQualityPreset GSurfelQualityPresets[] =
	{
		{ "Low (8k)",       8 * 1024, 16.0f,  128 },
		{ "Medium (32k)",  32 * 1024, 12.0f,  256 },
		{ "High (128k)",  128 * 1024,  8.0f,  512 },
		{ "Ultra (512k)", 512 * 1024,  6.0f, 1024 },
		{ "Custom",               0,  0.0f,    0 },
	};

	const int32 GNumScenes = (int32)UE_ARRAY_COUNT(GDevScenes);
	const int32 GNumQualityPresets = (int32)UE_ARRAY_COUNT(GSurfelQualityPresets);
	const int32 GCustomPresetIndex = GNumQualityPresets - 1;

	const char* const GVisualizeModeNames[] =
	{
		"None",
		"Surfels",
		"Grid",
		"Coverage Map",
		"Lumen GI (reference)",
		"Surfel GI (no Lumen)",
		"Direct Light Only",
	};

	static_assert((int32)UE_ARRAY_COUNT(GVisualizeModeNames) == (int32)ESurfelVisualizeMode::MAX,
		"GVisualizeModeNames must have one entry per ESurfelVisualizeMode");

	const char* const GVisualizeModeHelp[] =
	{
		"Scene color",
		"Surfel discs",
		"Uniform grid cells, brighter where more surfels are binned",
		"Coverage map Scatter writes",
		"Lumen GI",
		"Surfel GI",
		"All indirect lighting off",
	};

	// Which preset the current CVars correspond to, or Custom if they match none of them.
	int32 FindPresetForCurrentSettings()
	{
		const int32 Budget = CVarSurfelBudget.GetValueOnGameThread();
		const float Radius = CVarSurfelRadius.GetValueOnGameThread();
		const int32 SpawnsPerFrame = CVarSurfelSpawnsPerFrame.GetValueOnGameThread();

		for (int32 Index = 0; Index < GCustomPresetIndex; ++Index)
		{
			const FSurfelQualityPreset& Preset = GSurfelQualityPresets[Index];
			if (Preset.Budget == Budget
				&& FMath::IsNearlyEqual(Preset.Radius, Radius)
				&& Preset.SpawnsPerFrame == SpawnsPerFrame)
			{
				return Index;
			}
		}
		return GCustomPresetIndex;
	}

	void ApplyQualityPreset(const FSurfelQualityPreset& Preset)
	{
		CVarSurfelBudget->Set(Preset.Budget, ECVF_SetByConsole);
		CVarSurfelRadius->Set(Preset.Radius, ECVF_SetByConsole);
		CVarSurfelSpawnsPerFrame->Set(Preset.SpawnsPerFrame, ECVF_SetByConsole);

		++GSurfelRefreshRequestId;
	}

	// Explain a greyed-out slider, since a disabled widget alone doesn't say why.
	void LockedByPresetTooltip()
	{
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			ImGui::SetTooltip("Set by the quality preset. Choose \"Sliders\" to edit.");
		}
	}

	// Key the font is registered under. The plugin also copies it into ImFontConfig::Name,
	// which is how FindSlateFont picks it back out of the atlas.
	const char* const GSlateFontName = "Roboto (Slate)";

	// Give ImGui the same font the editor's Slate UI uses: Roboto Regular from the engine's
	// Slate content, at Starship's regular text size.
	void RegisterSlateFont()
	{
		FImGuiModuleProperties& Properties = FImGuiModule::Get().GetProperties();
		const FName FontKey(GSlateFontName);

		// Module properties outlive PIE sessions, so only the first session registers it.
		if (Properties.GetCustomFonts().Contains(FontKey))
		{
			return;
		}

		// The plugin keeps this config and re-reads it every time it rebuilds the atlas (e.g. on
		// a DPI change), so the TTF bytes have to live as long as the editor does.
		static TArray<uint8> FontData;
		const FString FontPath = FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf");
		if (!FFileHelper::LoadFileToArray(FontData, *FontPath))
		{
			UE_LOG(LogTemp, Warning, TEXT("DevGui: couldn't load %s, keeping ImGui's default font"), *FontPath);
			return;
		}

		TSharedPtr<ImFontConfig> Config = MakeShared<ImFontConfig>();
		Config->FontData = FontData.GetData();
		Config->FontDataSize = FontData.Num();
		// Not owned: the atlas copies the bytes on each build instead of freeing our buffer,
		// which would leave the next rebuild reading freed memory.
		Config->FontDataOwnedByAtlas = false;
		// Slate sizes are points at 96 DPI, ImGui's are pixels.
		Config->SizePixels = FMath::RoundToFloat(FStarshipCoreStyle::RegularTextSize * 96.0f / 72.0f);

		Properties.AddCustomFont(FontKey, Config);

		// The plugin built its atlas at startup, before this font existed.
		FImGuiModule::Get().RebuildFontAtlas();
	}

	// Looked up by name each frame rather than cached: a rebuild replaces every ImFont.
	ImFont* FindSlateFont()
	{
		for (ImFont* Font : ImGui::GetIO().Fonts->Fonts)
		{
			if (FCStringAnsi::Strcmp(Font->GetDebugName(), GSlateFontName) == 0)
			{
				return Font;
			}
		}
		return nullptr;
	}
}

void UDevGuiSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// So I don't type the CVar command of enabling input manually 
	FImGuiModule::Get().GetProperties().SetInputEnabled(true);

	RegisterSlateFont();
}

void UDevGuiSubsystem::Tick(float DeltaTime)
{
	ImFont* SlateFont = FindSlateFont();
	if (SlateFont)
	{
		ImGui::PushFont(SlateFont);
	}
	ON_SCOPE_EXIT
	{
		if (SlateFont)
		{
			ImGui::PopFont();
		}
	};

	ImGui::SetNextWindowSize(ImVec2(1200.0f, 720.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);

	if (!ImGui::Begin("Surfel Settings"))
	{
		ImGui::End();
		return;
	}

	ImGui::SetWindowFontScale(WindowFontScale);

	if (!bInitializedFromCVars)
	{
		QualityPresetIndex = FindPresetForCurrentSettings();
		bInitializedFromCVars = true;
	}
	
	if (CVarSurfelEnable.GetValueOnGameThread() != 0)
	{
		DrawGridSettings(); 
	}
	
	DrawSurfelSettings();
	DrawScenesSection();
	DrawCamerasSection();

	ImGui::End();
}

void UDevGuiSubsystem::DrawScenesSection()
{
	ImGui::SeparatorText("Scenes");

	for (int32 Index = 0; Index < GNumScenes; ++Index)
	{
		const FDevScene& Scene = GDevScenes[Index];

		if (Index > 0)
		{
			ImGui::SameLine();
		}

		// Don't expose the button at all until the map exists.
		const bool bExists = FPackageName::DoesPackageExist(Scene.PackagePath);

		ImGui::BeginDisabled(!bExists);
		if (ImGui::Button(Scene.Name))
		{
			UGameplayStatics::OpenLevel(this, FName(Scene.PackagePath));
		}
		ImGui::EndDisabled();

		if (!bExists && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			ImGui::SetTooltip("Map not created yet: %s", TCHAR_TO_ANSI(Scene.PackagePath));
		}
	}
}

// Credit Claude Code: detect existing cameras and switch between them for easy visualization.
void UDevGuiSubsystem::DrawCamerasSection()
{
	ImGui::SeparatorText("Cameras");

	UWorld* World = GetWorld();
	APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;

	if (!PlayerController)
	{
		ImGui::TextDisabled("No player controller (not in PIE?)");
		return;
	}

	const AActor* CurrentViewTarget = PlayerController->GetViewTarget();

	int32 CameraIndex = 0;
	for (TActorIterator<ACameraActor> It(World); It; ++It)
	{
		ACameraActor* Camera = *It;
		
		// Avoid camera names conflicting
		const FString Label = FString::Printf(TEXT("%s##Cam%d"), *Camera->GetName(), CameraIndex);

		if (ImGui::RadioButton(TCHAR_TO_ANSI(*Label), CurrentViewTarget == Camera))
		{
			PlayerController->SetViewTargetWithBlend(Camera, CameraBlendTime);
		}

		++CameraIndex;
	}

	if (CameraIndex == 0)
	{
		ImGui::TextDisabled("No CameraActors in this level");
		return;
	}

	if (ImGui::Button("Reset To Player Pawn"))
	{
		PlayerController->SetViewTargetWithBlend(PlayerController->GetPawn(), CameraBlendTime);
	}
}

void UDevGuiSubsystem::DrawSurfelSettings()
{
	ImGui::SeparatorText("Surfel Settings");

	if (ImGui::BeginCombo("Quality Preset", GSurfelQualityPresets[QualityPresetIndex].Name))
	{
		for (int32 Index = 0; Index < GNumQualityPresets; ++Index)
		{
			const bool bSelected = QualityPresetIndex == Index;
			if (ImGui::Selectable(GSurfelQualityPresets[Index].Name, bSelected))
			{
				QualityPresetIndex = Index;

				if (Index != GCustomPresetIndex)
				{
					ApplyQualityPreset(GSurfelQualityPresets[Index]);
				}
			}
			if (bSelected)
			{
				ImGui::SetItemDefaultFocus();
			}
		}
		ImGui::EndCombo();
	}

	const bool bManualSettings = QualityPresetIndex == GCustomPresetIndex;

	ImGui::BeginDisabled(!bManualSettings);
	{
		int Budget = CVarSurfelBudget.GetValueOnGameThread();
		if (ImGui::InputInt("Budget", &Budget, 1024, 512 * 1024))
		{
			CVarSurfelBudget->Set(Budget, ECVF_SetByConsole);
			++GSurfelRefreshRequestId;
		}
		LockedByPresetTooltip();

		float Radius = CVarSurfelRadius.GetValueOnGameThread();
		if (ImGui::SliderFloat("Radius (px)", &Radius, 1.0f, 256.0f, "%.1f"))
		{
			CVarSurfelRadius->Set(Radius, ECVF_SetByConsole);
			++GSurfelRefreshRequestId;
		}
		LockedByPresetTooltip();

		int SpawnsPerFrame = CVarSurfelSpawnsPerFrame.GetValueOnGameThread();
		if (ImGui::SliderInt("Spawn Rate", &SpawnsPerFrame, 0, 4096))
		{
			CVarSurfelSpawnsPerFrame->Set(SpawnsPerFrame, ECVF_SetByConsole);
		}
		LockedByPresetTooltip();
	}
	ImGui::EndDisabled();

	DrawBudgetMeter();
	
	DrawVisualizationSettings();
}

void UDevGuiSubsystem::DrawBudgetMeter()
{
	// GSurfelAllocatedCount is filled in by a GPU readback on the render thread, so it trails
	// the GPU by a frame or two and sits at 0 until the first copy lands. That is fine for a
	// gauge, but it means a fresh PIE session shows 0/N for a moment rather than being broken.
	const int32 Used = GSurfelAllocatedCount.load(std::memory_order_relaxed);
	const int32 Budget = FMath::Max(CVarSurfelBudget.GetValueOnGameThread(), 1);
	const float Fraction = FMath::Clamp((float)Used / (float)Budget, 0.0f, 1.0f);

	// The meter earns its place at the top of the range: once the pool is full, Gather silently
	// stops spawning and coverage gaps never fill, which looks like a spawning bug rather than
	// an exhausted budget. Colour makes that state obvious before you go hunting.
	ImVec4 BarColor(0.20f, 0.70f, 0.30f, 1.0f);            // healthy
	if (Fraction >= 0.95f)      BarColor = ImVec4(0.85f, 0.20f, 0.20f, 1.0f);  // effectively full
	else if (Fraction >= 0.75f) BarColor = ImVec4(0.85f, 0.65f, 0.15f, 1.0f);  // filling up

	const FString Overlay = FString::Printf(TEXT("%d / %d  (%.1f%%)"), Used, Budget, Fraction * 100.0f);

	ImGui::PushStyleColor(ImGuiCol_PlotHistogram, BarColor);
	ImGui::ProgressBar(Fraction, ImVec2(-FLT_MIN, 0.0f), TCHAR_TO_UTF8(*Overlay));
	ImGui::PopStyleColor();

	ImGui::SameLine();
	ImGui::Text("Budget Used");

	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Surfels currently allocated out of the budget.");
	}

	ImGui::BeginDisabled(Used == 0);
	if (ImGui::Button("Clear Surfels"))
	{
		// Same path as r.Surfel.Refresh: the render thread notices the id changed and
		// reallocates the pool from scratch.
		++GSurfelRefreshRequestId;
	}
	ImGui::EndDisabled();
}

void UDevGuiSubsystem::DrawVisualizationSettings()
{
	ImGui::SeparatorText("Visualization Settings");

	int VisualizeMode = CVarSurfelVisualizeMode.GetValueOnGameThread();
	VisualizeMode = FMath::Clamp(VisualizeMode, 0, (int32)ESurfelVisualizeMode::MAX - 1);

	if (ImGui::BeginCombo("Visualization Mode", GVisualizeModeNames[VisualizeMode]))
	{
		for (int32 Index = 0; Index < (int32)ESurfelVisualizeMode::MAX; ++Index)
		{
			const bool bSelected = VisualizeMode == Index;
			if (ImGui::Selectable(GVisualizeModeNames[Index], bSelected))
			{
				// The lighting modes are applied per view by FSurfelSceneViewExtension::SetupView,
				// so setting the CVar is all it takes.
				CVarSurfelVisualizeMode->Set(Index, ECVF_SetByConsole);
				VisualizeMode = Index;
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s", GVisualizeModeHelp[Index]);
			}
			if (bSelected)
			{
				ImGui::SetItemDefaultFocus();
			}
		}
		ImGui::EndCombo();
	}

	ImGui::TextWrapped("%s", GVisualizeModeHelp[VisualizeMode]);

	// The Lumen mode forces the GI method on, but scalability can still veto it through
	// r.Lumen.DiffuseIndirect.Allow (e.g. sg.GlobalIlluminationQuality 0 / Low settings).
	// Say so, rather than leaving you to wonder why the image didn't change.
	if (VisualizeMode == (int32)ESurfelVisualizeMode::LumenGI)
	{
		static const IConsoleVariable* LumenAllowCVar =
			IConsoleManager::Get().FindConsoleVariable(TEXT("r.Lumen.DiffuseIndirect.Allow"));
		if (LumenAllowCVar && LumenAllowCVar->GetInt() == 0)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
				"r.Lumen.DiffuseIndirect.Allow is 0 (scalability too low?), so Lumen stays off.");
		}
	}

	/*
	int VisualizeSurfels = CVarSurfelMode.GetValueOnGameThread();
	if (ImGui::SliderInt("Visualize Pass Enabled", &VisualizeSurfels, 0, 1))
	{
		CVarSurfelMode->Set(VisualizeSurfels, ECVF_SetByConsole);
	}
	*/
	
	if (VisualizeMode == (int32)ESurfelVisualizeMode::Surfels
		|| VisualizeMode == (int32)ESurfelVisualizeMode::Grid)
	{
		float SurfelIntensity = CVarSurfelIntensity.GetValueOnGameThread();
		if (ImGui::SliderFloat("Color Strenght", &SurfelIntensity, 0.0f, 1.0f))
		{
			CVarSurfelIntensity->Set(SurfelIntensity, ECVF_SetByConsole);
		}
	}
}

// Credit Claude Code: Grid settings for visualization and the shaders are prototyped with AI.
void UDevGuiSubsystem::DrawGridSettings()
{
	ImGui::SeparatorText("Grid Settings");

	bool bUseGrid = CVarSurfelUseGrid.GetValueOnGameThread() != 0;
	if (ImGui::Checkbox("Use Grid For Surfel Lookup", &bUseGrid))
	{
		CVarSurfelUseGrid->Set(bUseGrid ? 1 : 0, ECVF_SetByConsole);
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Off = the visualize pass brute-forces every surfel per pixel.");
	}

	int CellResolution = CVarSurfelGridCellResolution.GetValueOnGameThread();
	if (ImGui::SliderInt("Cell Resolution", &CellResolution, 8, 128))
	{
		CVarSurfelGridCellResolution->Set(CellResolution, ECVF_SetByConsole);
	}

	int CellSize = CVarSurfelGridCellSize.GetValueOnGameThread();
	if (ImGui::SliderInt("Cell Size (cm)", &CellSize, 8, 1024))
	{
		CVarSurfelGridCellSize->Set(CellSize, ECVF_SetByConsole);
	}
	
	// TODO: Should scale with the quality levels
	int CellCapacity = CVarSurfelGridCellCapacity.GetValueOnGameThread();
	if (ImGui::SliderInt("Cell Capacity", &CellCapacity, 8, 2048))
	{
		CVarSurfelGridCellCapacity->Set(CellCapacity, ECVF_SetByConsole);
	}

	//const double ExtentMeters = (double)CellResolution * CellSize / 100.0;
	//const double EntriesMB = (double)CellResolution * CellResolution * CellResolution * CellCapacity * sizeof(uint32) / (1024.0 * 1024.0);
	//ImGui::TextDisabled("Covers %.1f m around the camera, entries buffer %.1f MB", ExtentMeters, EntriesMB);

	ImGui::SeparatorText("Grid Visualization");

	int VisFlags = CVarSurfelGridVisFlags.GetValueOnGameThread();
	bool bFlagsChanged = false;
	bFlagsChanged |= ImGui::CheckboxFlags("Wireframe", &VisFlags, (int)ESurfelGridVisFlags::Wireframe);
	ImGui::SameLine();
	bFlagsChanged |= ImGui::CheckboxFlags("Flag Overflow", &VisFlags, (int)ESurfelGridVisFlags::Overflow);
	ImGui::SameLine();
	bFlagsChanged |= ImGui::CheckboxFlags("Hide Empty", &VisFlags, (int)ESurfelGridVisFlags::HideEmpty);
	ImGui::SameLine();
	bFlagsChanged |= ImGui::CheckboxFlags("Heat Map", &VisFlags, (int)ESurfelGridVisFlags::Heatmap);
	if (bFlagsChanged)
	{
		CVarSurfelGridVisFlags->Set(VisFlags, ECVF_SetByConsole);
	}

	ImGui::BeginDisabled((VisFlags & (int)ESurfelGridVisFlags::Wireframe) == 0);
	float EdgeThickness = CVarSurfelGridVisEdgeThickness.GetValueOnGameThread();
	if (ImGui::SliderFloat("Wireframe Thickness", &EdgeThickness, 0.005f, 0.2f, "%.3f"))
	{
		CVarSurfelGridVisEdgeThickness->Set(EdgeThickness, ECVF_SetByConsole);
	}
	ImGui::EndDisabled();
}

TStatId UDevGuiSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UDevGuiSubsystem, STATGROUP_Tickables);
}

void UDevGuiSubsystem::ToggleImGuiInput()
{
	FImGuiModule::Get().GetProperties().ToggleInput();
}

UE_ENABLE_OPTIMIZATION
