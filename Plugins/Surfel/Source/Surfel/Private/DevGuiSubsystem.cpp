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
#include "Engine/Engine.h"
#include "HAL/FileManager.h"
#include "SceneUtils.h"
#include "UnrealClient.h"
#include "DynamicRHI.h"
#include "SurfelGPUStats.h"

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
		{ "Sponza",          TEXT("/Game/Sponza") },
		{ "Cornell Box",     TEXT("/Game/CornellBox") },
		{ "Dragon",          TEXT("/Game/Dragon") },
		{ "Conference Room", TEXT("/Game/ConferenceRoom") },
		{ "Living Room",     TEXT("/Game/LivingRoom") },
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

	// Same order as ESurfelVisualizeMode, the combo index is written straight into the CVar
	const char* const GVisualizeModeNames[] =
	{
		"None",
		"Surfels",
		"Uniform Grid",
		"Coverage Texture",
		"Lumen Global Illumination",
		"Surfel Global Illumination",
		"Direct Light Only",
		"Irradiance Texture",
		"Surfel Indirect Light + AO",
		"SSAO Only"
	};

	static_assert((int32)UE_ARRAY_COUNT(GVisualizeModeNames) == (int32)ESurfelVisualizeMode::MAX,
		"GVisualizeModeNames must have one entry per ESurfelVisualizeMode");

	const char* const GVisualizeModeHelp[] =
	{
		"Scene color",
		"Surfel discs",
		"Uniform grid cells, brighter where more surfels are binned",
		"Coverage map Gather writes",
		"Lumen GI",
		"Direct light + surfel indirect light composited under it, Lumen off",
		"All indirect lighting off",
		"Raw surfel irradiance resolved per pixel, no AO, dark where no surfel reaches",
		"Surfel bounce light with screen space + material AO, exactly what the composite adds",
		"Ambient occlusion buffer, white = open, black = occluded"
	};

	static_assert((int32)UE_ARRAY_COUNT(GVisualizeModeHelp) == (int32)ESurfelVisualizeMode::MAX,
		"GVisualizeModeHelp must have one entry per ESurfelVisualizeMode");

	// Same order as ESurfelColorMode
	const char* const GSurfelColorModeNames[] =
	{
		"Hash",
		"Albedo",
		"Indirect"
	};

	static_assert((int32)UE_ARRAY_COUNT(GSurfelColorModeNames) == (int32)ESurfelColorMode::MAX,
		"GSurfelColorModeNames must have one entry per ESurfelColorMode");

	// Where the screenshot utility drops its PNGs, next to the project rather than in Saved/ so the
	// shots are easy to pick up for the report.
	FString GetScreenshotDirectory()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Assets"));
	}

	// Mode name straight from the combo, trimmed down to something safe to put in a filename.
	FString MakeScreenshotFilename()
	{
		int32 VisualizeMode = CVarSurfelVisualizeMode.GetValueOnGameThread();
		VisualizeMode = FMath::Clamp(VisualizeMode, 0, (int32)ESurfelVisualizeMode::MAX - 1);

		FString ModeName = ANSI_TO_TCHAR(GVisualizeModeNames[VisualizeMode]);
		for (TCHAR& Character : ModeName)
		{
			if (!FChar::IsAlnum(Character))
			{
				Character = TEXT('_');
			}
		}

		return FString::Printf(TEXT("Surfel_%s_%s.png"), *ModeName, *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	}

	// Same order as EAntiAliasingMethod in SceneUtils.h, the combo index is written straight into the CVar
	const char* const GAntiAliasingMethodNames[] =
	{
		"None",
		"FXAA",
		"TAA",
		"MSAA",
		"TSR",
		"SMAA",
	};

	static_assert((int32)UE_ARRAY_COUNT(GAntiAliasingMethodNames) == (int32)AAM_MAX,
		"GAntiAliasingMethodNames must have one entry per EAntiAliasingMethod");

	const char* const GAntiAliasingMethodHelp[] =
	{
		"No anti-aliasing, raw aliased edges and no temporal smearing over the surfel noise",
		"Post-process edge blur, cheap, no temporal accumulation",
		"Temporal Anti-Aliasing, jitters the view and accumulates over frames",
		"Forward shading only, the deferred renderer this project uses falls back to None",
		"Temporal Super-Resolution, the UE5 default",
		"Subpixel Morphological Anti-Aliasing, post-process, no temporal accumulation",
	};

	static_assert((int32)UE_ARRAY_COUNT(GAntiAliasingMethodHelp) == (int32)AAM_MAX,
		"GAntiAliasingMethodHelp must have one entry per EAntiAliasingMethod");
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

	// GPU times for the surfel passes, next to the whole GPU frame so switching Lumen / Surfel visualize modes
	// shows what each costs. The per pass numbers come from the stats in SurfelGPUStats.h and arrive a few frames late.
	// In Lumen GI mode the surfel passes don't run, so it shows Lumen's total instead (see SurfelLumenGPUTime).
	void DrawGPUTimings()
	{
#if HAS_GPU_STATS
		ImGui::SeparatorText("GPU Timings");

		static float SmoothedFrameMs = 0.0f;
		const float FrameMs = static_cast<float>(FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles()));
		SmoothedFrameMs = SmoothedFrameMs <= 0.0f ? FrameMs : FMath::Lerp(SmoothedFrameMs, FrameMs, 0.1f);
		ImGui::Text("GPU frame: %.2f ms", SmoothedFrameMs);

		// Surfel passes are skipped in Lumen GI mode, show Lumen's cost instead
		const bool bLumenMode = CVarSurfelVisualizeMode.GetValueOnGameThread() == (int32)ESurfelVisualizeMode::LumenGI;
#if STATS
		if (bLumenMode)
		{
			SurfelLumenGPUTime::Start();
		}
		else
		{
			SurfelLumenGPUTime::Stop();
		}
#endif
		if (bLumenMode)
		{
#if STATS
			const float LumenMs = SurfelLumenGPUTime::SmoothedMs.load(std::memory_order_relaxed);
			ImGui::Text("Lumen GI: %.3f ms (%.0f%% of frame)", LumenMs, SmoothedFrameMs > 0.0f ? 100.0f * LumenMs / SmoothedFrameMs : 0.0f);
#else
			ImGui::TextDisabled("Lumen GI: needs a build with stats");
#endif
			return;
		}

		if (CVarSurfelEnable.GetValueOnGameThread() == 0)
		{
			ImGui::TextDisabled("Surfels disabled");
			return;
		}

		const float TotalMs = GPUStat_SurfelTotal.SmoothedMs.load(std::memory_order_relaxed);
		ImGui::Text("Surfel GI: %.3f ms (%.0f%% of frame)", TotalMs, SmoothedFrameMs > 0.0f ? 100.0f * TotalMs / SmoothedFrameMs : 0.0f);

		if (ImGui::TreeNode("Per pass"))
		{
			// Skip index 0, that is the total shown above
			for (int32 Index = 1; Index < UE_ARRAY_COUNT(GSurfelGPUStats); ++Index)
			{
				const FSurfelGPUStat* Stat = GSurfelGPUStats[Index];
				ImGui::Text("%s: %.3f ms", TCHAR_TO_UTF8(Stat->DisplayName), Stat->SmoothedMs.load(std::memory_order_relaxed));
			}
			ImGui::TreePop();
		}
#endif
	}
}

void UDevGuiSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// So I don't type the CVar command of enabling input manually 
	FImGuiModule::Get().GetProperties().SetInputEnabled(true);

	RegisterSlateFont();
}

void UDevGuiSubsystem::Deinitialize()
{
#if STATS
	// Drop the stats collection request from the Lumen GPU timing readout
	SurfelLumenGPUTime::Stop();
#endif

	Super::Deinitialize();
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
	DrawGPUTimings();
	DrawAntiAliasingSettings();
	DrawScenesSection();
	DrawCamerasSection();
	DrawFreeMovementSection();
	DrawScreenshotSection();

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

// Credit Claude Code: the anti-aliasing dropdown is AI written, it is just a utility for switching
// r.AntiAliasingMethod without the console and nothing in the renderer depends on it.
// Prompt used:
	// Can you make a dropdown option for all aliasing methods, I want to turn on TAA or others from the ImGui dropdown.
void UDevGuiSubsystem::DrawAntiAliasingSettings()
{
	ImGui::SeparatorText("Anti-Aliasing");

	// Owned by the Engine module rather than this plugin, so it is looked up by name the same way
	// ForceGlobalSDFBuild reaches the global SDF CVar.
	static IConsoleVariable* AntiAliasingMethod = IConsoleManager::Get().FindConsoleVariable(TEXT("r.AntiAliasingMethod"));
	if (!AntiAliasingMethod)
	{
		ImGui::TextDisabled("r.AntiAliasingMethod not found");
		return;
	}

	int32 Method = FMath::Clamp(AntiAliasingMethod->GetInt(), 0, (int32)AAM_MAX - 1);

	if (ImGui::BeginCombo("Anti-Aliasing Method", GAntiAliasingMethodNames[Method]))
	{
		for (int32 Index = 0; Index < (int32)AAM_MAX; ++Index)
		{
			const bool bSelected = Method == Index;
			if (ImGui::Selectable(GAntiAliasingMethodNames[Index], bSelected))
			{
				// Views read the CVar when they are set up, so this lands on the next frame.
				AntiAliasingMethod->Set(Index, ECVF_SetByConsole);
				Method = Index;
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s", GAntiAliasingMethodHelp[Index]);
			}
			if (bSelected)
			{
				ImGui::SetItemDefaultFocus();
			}
		}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("r.AntiAliasingMethod\nA Post Process Volume or camera in the level can still override this per view.");
	}

	// The renderer quietly substitutes a method it cannot run, so show what actually ends up being
	// used rather than letting the combo claim MSAA while the image is aliased.
	const UWorld* World = GetWorld();
	const EAntiAliasingMethod Effective = GetDefaultAntiAliasingMethod(World ? World->GetFeatureLevel() : GMaxRHIFeatureLevel);
	if ((int32)Effective != Method)
	{
		ImGui::TextColored(ImVec4(0.85f, 0.65f, 0.15f, 1.0f), "Not supported here, the renderer falls back to %s",
			GAntiAliasingMethodNames[(int32)Effective]);
	}
}

// Credit Claude Code: the free movement button is AI written, it is just a utility for flying around a
// scene while comparing visualization modes and nothing in the renderer depends on it.
// Prompt used:
	// Add a button that will allow me to move freely in the scene.
void UDevGuiSubsystem::DrawFreeMovementSection()
{
	ImGui::SeparatorText("Free Movement");

	UWorld* World = GetWorld();
	APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;

	if (!PlayerController)
	{
		ImGui::TextDisabled("No player controller (not in PIE?)");
		return;
	}

	if (ImGui::Button("Fly The Scene"))
	{
		// Two separate things pin you in place. Looking through a CameraActor locks the view, so the
		// pawn gets it back first.
		PlayerController->SetViewTargetWithBlend(PlayerController->GetPawn(), CameraBlendTime);

		// And ImGui holds the mouse and keyboard the whole time its input is on, which is what stops
		// WASD and mouse look from ever reaching the pawn. Releasing it hands input back to the game.
		FImGuiModule::Get().GetProperties().SetInputEnabled(false);
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Hands input back to the game so the default pawn can fly:\n"
			"WASD to move, E and Q for up and down, mouse to look.\n"
			"F1 (or the ImGui.ToggleInput command) brings this window back.");
	}

	ImGui::SameLine();
	ImGui::TextDisabled("F1 returns to the GUI");
}

// Credit Claude Code: the screenshot section below is AI written, it is only a utility for grabbing
// comparison shots of the visualization modes and nothing in the renderer depends on it.
// Prompt used:
	// I want a dev button that screenshots the framebuffer and saves the result as png in assets.
	// Screenshot without the ImGui window somehow.
void UDevGuiSubsystem::DrawScreenshotSection()
{
	ImGui::SeparatorText("Screenshot");

	// The engine only services the request on the next viewport draw, so the previous one is
	// resolved here rather than at the call site.
	if (!PendingScreenshotPath.IsEmpty() && !FScreenshotRequest::IsScreenshotRequested())
	{
		const bool bSaved = IFileManager::Get().FileExists(*PendingScreenshotPath);
		ScreenshotStatus = bSaved
			? FString::Printf(TEXT("Saved %s"), *PendingScreenshotPath)
			: FString::Printf(TEXT("Failed to write %s"), *PendingScreenshotPath);

		UE_LOG(LogTemp, Log, TEXT("DevGui: %s"), *ScreenshotStatus);
		PendingScreenshotPath.Reset();
	}

	const bool bHasViewport = GEngine != nullptr && GEngine->GameViewport != nullptr;

	ImGui::BeginDisabled(!bHasViewport || !PendingScreenshotPath.IsEmpty());
	if (ImGui::Button("Capture PNG"))
	{
		const FString Directory = GetScreenshotDirectory();
		IFileManager::Get().MakeDirectory(*Directory, true);

		PendingScreenshotPath = Directory / MakeScreenshotFilename();

		// bShowUI false is what keeps this window out of the shot: the viewport render target is
		// read back right after the scene is drawn, before Slate composites any widget on top of
		// it, so ImGui never makes it into the pixels. The path contains a slash, so the engine
		// writes exactly there instead of redirecting into the screenshot directory.
		FScreenshotRequest::RequestScreenshot(PendingScreenshotPath, /*bShowUI*/ false, /*bAddFilenameSuffix*/ false);
	}
	ImGui::EndDisabled();

	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
	{
		ImGui::SetTooltip("Writes the viewport without any ImGui or Slate UI to:\n%s", TCHAR_TO_UTF8(*GetScreenshotDirectory()));
	}

	if (!PendingScreenshotPath.IsEmpty())
	{
		ImGui::SameLine();
		ImGui::TextDisabled("Capturing...");
	}
	else if (!ScreenshotStatus.IsEmpty())
	{
		ImGui::SameLine();
		ImGui::TextDisabled("%s", TCHAR_TO_UTF8(*ScreenshotStatus));
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
		if (ImGui::SliderFloat("Radius (px on screen)", &Radius, 1.0f, 256.0f, "%.1f"))
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
		
		float CoverageThreshold = CVarSurfelSpawnCoverageThreshold.GetValueOnGameThread();
		if (ImGui::SliderFloat("Spacing Between Surfels", &CoverageThreshold, 0, 1))
		{
			CVarSurfelSpawnCoverageThreshold->Set(CoverageThreshold, ECVF_SetByConsole);
		}
		LockedByPresetTooltip();
	}
	ImGui::EndDisabled();

	int MaxSamples = FMath::RoundToInt(CVarMaxSamples.GetValueOnGameThread());
	if (ImGui::SliderInt("Max Samples", &MaxSamples, 1, 256))
	{
		CVarMaxSamples->Set(static_cast<float>(MaxSamples), ECVF_SetByConsole);
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("r.Surfel.MaxSamples\nHow many frames of rays each surfel averages over.\nHigher = less noise, slower to react to moving lights.");
	}

	int MaxBounces = CVarMaxBounces.GetValueOnGameThread();
	if (ImGui::SliderInt("Bounces", &MaxBounces, 1, 8))
	{
		CVarMaxBounces->Set(MaxBounces, ECVF_SetByConsole);
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("r.Surfel.MaxBounces\nSegments each surfel ray path follows per frame, adding the light of every hit.\n1 = single hop, the surfel cache fills in later bounces over time.\nEach extra bounce costs another ray march per surfel.");
	}

	int RaysPerSurfel = CVarRaysPerSurfel.GetValueOnGameThread();
	if (ImGui::SliderInt("Rays Per Surfel", &RaysPerSurfel, 1, 16))
	{
		CVarRaysPerSurfel->Set(RaysPerSurfel, ECVF_SetByConsole);
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("r.Surfel.RaysPerSurfel\nIndependent ray paths per surfel each frame, averaged together.\nLess noise and splotchiness, cost scales linearly.");
	}

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

	// The meter earns its place at the top of the range: once the pool is full, Spawn silently
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

	ImGui::SameLine();
	if (ImGui::Button("Force SDF Build"))
	{
		ForceGlobalSDFBuild();
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("r.AOGlobalDistanceField.DetailedNecessityCheck 0\nAlways build the global SDF the irradiance rays march through.");
	}
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

	if (VisualizeMode == (int32)ESurfelVisualizeMode::Surfels)
	{
		int ColorMode = FMath::Clamp(CVarSurfelColorMode.GetValueOnGameThread(), 0, (int32)ESurfelColorMode::MAX - 1);
		if (ImGui::Combo("Surfel Color", &ColorMode, GSurfelColorModeNames, (int)ESurfelColorMode::MAX))
		{
			CVarSurfelColorMode->Set(ColorMode, ECVF_SetByConsole);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("r.Surfel.ColorMode\nHash: random color per surfel\nAlbedo: surface color captured at spawn\nIndirect: incoming indirect light accumulated by the surfel");
		}

		// Indirect values are raw light units, so they get the same multiplier the composite uses
		if (ColorMode == (int32)ESurfelColorMode::Indirect)
		{
			float GIIntensity = CVarSurfelGIIntensity.GetValueOnGameThread();
			if (ImGui::SliderFloat("GI Intensity", &GIIntensity, 0.0f, 4.0f))
			{
				CVarSurfelGIIntensity->Set(GIIntensity, ECVF_SetByConsole);
			}
		}
	}

	if (VisualizeMode == (int32)ESurfelVisualizeMode::Surfels
		|| VisualizeMode == (int32)ESurfelVisualizeMode::Grid)
	{
		float SurfelIntensity = CVarSurfelIntensity.GetValueOnGameThread();
		if (ImGui::SliderFloat("Color Strenght", &SurfelIntensity, 0.0f, 1.0f))
		{
			CVarSurfelIntensity->Set(SurfelIntensity, ECVF_SetByConsole);
		}
	}

	// Irradiance view brightness reuses the overlay intensity, it's a raw debug view with no exposure
	if (VisualizeMode == (int32)ESurfelVisualizeMode::Irradiance)
	{
		float IrradianceScale = CVarSurfelIntensity.GetValueOnGameThread();
		if (ImGui::SliderFloat("Irradiance Scale", &IrradianceScale, 0.0f, 10.0f))
		{
			CVarSurfelIntensity->Set(IrradianceScale, ECVF_SetByConsole);
		}
	}

	if (VisualizeMode == (int32)ESurfelVisualizeMode::SurfelGI
		|| VisualizeMode == (int32)ESurfelVisualizeMode::IndirectLight)
	{
		float GIIntensity = CVarSurfelGIIntensity.GetValueOnGameThread();
		if (ImGui::SliderFloat("GI Intensity", &GIIntensity, 0.0f, 4.0f))
		{
			CVarSurfelGIIntensity->Set(GIIntensity, ECVF_SetByConsole);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("r.Surfel.GIIntensity\nMultiplier on the surfel indirect light added to scene color.\n1 is physically correct.");
		}
	}

	if (VisualizeMode == (int32)ESurfelVisualizeMode::SurfelGI)
	{
		bool bUseAO = CVarSurfelUseAO.GetValueOnGameThread() != 0;
		if (ImGui::Checkbox("Apply Ambient Occlusion", &bUseAO))
		{
			CVarSurfelUseAO->Set(bUseAO ? 1 : 0, ECVF_SetByConsole);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("r.Surfel.UseAO\nOff composites the raw surfel indirect light with no occlusion,\nfor comparing against AO on.");
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
