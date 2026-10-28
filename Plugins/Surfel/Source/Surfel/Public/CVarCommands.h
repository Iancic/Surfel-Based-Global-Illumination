#pragma once
#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include <atomic>

extern TAutoConsoleVariable<int> CVarSurfelEnable;
extern TAutoConsoleVariable<int32> CVarSurfelMode;
extern TAutoConsoleVariable<float> CVarSurfelIntensity;
extern TAutoConsoleVariable<float> CVarSurfelDepthScale;
extern TAutoConsoleVariable<int> CVarSurfelBudget;
extern TAutoConsoleVariable<float> CVarSurfelRadius;
extern TAutoConsoleVariable<int32> CVarSurfelUseGrid;
extern TAutoConsoleVariable<int32> CVarSurfelVisualizeMode;
extern TAutoConsoleVariable<int32> CVarSurfelColorMode;
extern TAutoConsoleVariable<float> CVarSurfelCoverageScale;
extern TAutoConsoleVariable<float> CVarSurfelGIIntensity;
extern TAutoConsoleVariable<int32> CVarSurfelUseAO;
extern TAutoConsoleVariable<int32> CVarSurfelSpawnsPerFrame;
extern TAutoConsoleVariable<float> CVarSurfelSpawnCoverageThreshold;
extern TAutoConsoleVariable<float> CVarMinTraceDistance;
extern TAutoConsoleVariable<float> CVarMaxTraceDistance;
extern TAutoConsoleVariable<float> CVarStepFactor;
extern TAutoConsoleVariable<float> CVarMinStepFactor;
extern TAutoConsoleVariable<float> CVarMaxSamples;
extern TAutoConsoleVariable<int32> CVarMaxBounces;
extern TAutoConsoleVariable<int32> CVarRaysPerSurfel;
extern TAutoConsoleVariable<int32> CVarSurfelGridCellResolution;
extern TAutoConsoleVariable<int32> CVarSurfelGridCellSize;
extern TAutoConsoleVariable<int32> CVarSurfelGridCellCapacity;
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

enum class ESurfelVisualizeMode : int32
{
	None            = 0,
	Surfels         = 1,
	Grid            = 2,
	Coverage        = 3,
	LumenGI         = 4,
	SurfelGI        = 5,
	DirectLightOnly = 6,
	Irradiance      = 7,
	IndirectLight   = 8,
	SSAO            = 9,
	MAX
};

// What each surfel disc is colored with in the Surfels visualization. Baked into Visualize.usf as SURFEL_COLOR_* defines.
enum class ESurfelColorMode : int32
{
	Hash     = 0, // Random color per surfel index
	Albedo   = 1, // Surface color the surfel captured when it spawned
	Indirect = 2, // Incoming indirect light the surfel has accumulated
	MAX
};

// Modes the post-process visualize pass draws something for
inline bool IsSurfelOverlayMode(int32 Mode)
{
	return (Mode >= (int32)ESurfelVisualizeMode::Surfels && Mode <= (int32)ESurfelVisualizeMode::Coverage)
		|| Mode == (int32)ESurfelVisualizeMode::Irradiance
		|| Mode == (int32)ESurfelVisualizeMode::IndirectLight
		|| Mode == (int32)ESurfelVisualizeMode::SSAO;
}

// Modes where the composite pass adds surfel GI into scene color (before UE's direct lights)
inline bool ShouldCompositeSurfelGI(int32 Mode)
{
	return Mode == (int32)ESurfelVisualizeMode::SurfelGI;
}

// Spawn clears and respawns from scratch when this changes.
extern int32 GSurfelRefreshRequestId;

extern std::atomic<int32> GSurfelAllocatedCount;

// Sets r.AOGlobalDistanceField.DetailedNecessityCheck 0 so the engine always builds the global SDF
void ForceGlobalSDFBuild();
