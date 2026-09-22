#pragma once
#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

// Declared here, defined once in CVarCommands.cpp. Defining them `static` in the header
// gave every including .cpp its own copy (own CVar registration, own refresh counter).
extern TAutoConsoleVariable<int> CVarSurfelEnable;
extern TAutoConsoleVariable<int32> CVarSurfelMode;
extern TAutoConsoleVariable<float> CVarSurfelIntensity;
extern TAutoConsoleVariable<float> CVarSurfelDepthScale;
extern TAutoConsoleVariable<int> CVarSurfelBudget;
extern TAutoConsoleVariable<float> CVarSurfelRadius;
extern TAutoConsoleVariable<int32> CVarSurfelUseGrid;
extern TAutoConsoleVariable<int32> CVarSurfelVisualizeMode;
extern TAutoConsoleVariable<float> CVarSurfelCoverageScale;
extern TAutoConsoleVariable<int32> CVarSurfelSpawnsPerFrame;
extern TAutoConsoleVariable<float> CVarSurfelSpawnCoverageThreshold;

// Grid layout. Read once per frame on the render thread (FUniformGridViewState::UpdateFromCVars)
// so every pass in a frame agrees on the same layout.
extern TAutoConsoleVariable<int32> CVarSurfelGridCellResolution;
extern TAutoConsoleVariable<int32> CVarSurfelGridCellSize;
extern TAutoConsoleVariable<int32> CVarSurfelGridCellCapacity;

// Grid visualization options
extern TAutoConsoleVariable<int32> CVarSurfelGridVisFlags;
extern TAutoConsoleVariable<float> CVarSurfelGridVisEdgeThickness;

// Bits of r.Surfel.Grid.VisFlags. Baked into Visualize.usf as GRID_VIS_* defines.
enum class ESurfelGridVisFlags : uint32
{
	None      = 0,
	Wireframe = 1 << 0, // White lines on cell boundaries
	Overflow  = 1 << 1, // Cells past CellCapacity drawn solid red
	HideEmpty = 1 << 2, // Cells with no surfels left as scene color
	Heatmap   = 1 << 3, // Color by occupancy instead of a hashed color per cell
};
ENUM_CLASS_FLAGS(ESurfelGridVisFlags);

// What the visualize pass draws on top of scene color.
// The first four are debug overlays the shader handles; the last three are lighting
// references that only reconfigure the renderer, so the shader treats them as None.
// The numbers are baked into Visualize.usf as VISUALIZE_MODE_* defines by
// FSurfelFullscreenCS::ModifyCompilationEnvironment, so keep the two in sync.
enum class ESurfelVisualizeMode : int32
{
	None            = 0, // Scene color, untouched
	Surfels         = 1, // Every surfel disc in its own hashed color
	Grid            = 2, // Uniform grid cells, tinted by how full each cell is
	Coverage        = 3, // The coverage texture Scatter writes, as a heat map
	LumenGI         = 4, // No overlay, engine Lumen GI left on (reference image)
	SurfelGI        = 5, // No overlay, Lumen off so only our own GI contributes
	DirectLightOnly = 6, // No overlay, no indirect lighting at all
	MAX
};

// True for the modes the visualize compute shader actually draws something for.
// The rest need no fullscreen dispatch at all.
inline bool IsSurfelOverlayMode(int32 Mode)
{
	return Mode >= (int32)ESurfelVisualizeMode::Surfels && Mode <= (int32)ESurfelVisualizeMode::Coverage;
}

// Bumped by r.Surfel.Refresh; Gather clears and respawns from scratch when this changes.
extern int32 GSurfelRefreshRequestId;
