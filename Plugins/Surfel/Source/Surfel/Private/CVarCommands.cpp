#include "CVarCommands.h"

TAutoConsoleVariable<int> CVarSurfelEnable(
	TEXT("r.Surfel.Enable"),
	1,
	TEXT("Is surfel system enabled or not."),
	ECVF_RenderThreadSafe
	);

TAutoConsoleVariable<int32> CVarSurfelMode(
	TEXT("r.Surfel.Mode"),
	1,
	TEXT("Surfel compute pass visualize mode.\n")
	TEXT("0: Visualize Off\n")
	TEXT("1: Visualize Compute Pass\n"),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarSurfelIntensity(
	TEXT("r.Surfel.Intensity"),
	1.00f,
	TEXT("Strength of the example effect (0-1)."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarSurfelDepthScale(
	TEXT("r.Surfel.DepthScale"),
	10000.0f,
	TEXT("Far distance in world units mapped to white in depth visualization."),
	ECVF_RenderThreadSafe);

// 12k is a base default I found that works well enough with the rest of the settings
TAutoConsoleVariable<int> CVarSurfelBudget(
	TEXT("r.Surfel.Budget"),
	12000,
	TEXT("Surfel Budget."),
	ECVF_RenderThreadSafe
	);

TAutoConsoleVariable<float> CVarSurfelRadius(
	TEXT("r.Surfel.Radius"),
	25.0f,
	TEXT("Surfel radius in screen pixels at spawn time, also used as the minimum world radius (cm)."),
	ECVF_RenderThreadSafe
	);

// Important to have a lower value
// The surfels clump up together because first frame on a new wall everything is uncovered. 
// I should delay spawning some so the algorithm can go through the scatter with new surfels. 
// Not spawning all surfels from one. Thomas talk on PICA PICA
TAutoConsoleVariable<int32> CVarSurfelSpawnsPerFrame(
	TEXT("r.Surfel.SpawnsPerFrame"),
	1,
	TEXT("Roughly how many surfels Gather may spawn per frame across the screen."),
	ECVF_RenderThreadSafe);

// This avoids over-crowding, sitting next to each other
// 1 means spaced so for a surfel to spawn that has to be uncovererd fully
// closer to 0 means touching each other, so covered is considered something in between like a bit covered by another surfel
// Used by gather after reading the coverage texture
TAutoConsoleVariable<float> CVarSurfelSpawnCoverageThreshold(
	TEXT("r.Surfel.SpawnCoverageThreshold"),
	0.1f,
	TEXT("Pixels with coverage at or above this are considered covered and never spawn a surfel."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarSurfelGridCellResolution(
	TEXT("r.Surfel.Grid.CellResolution"),
	64,
	TEXT("Cells along each axis of the camera."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarSurfelGridCellSize(
	TEXT("r.Surfel.Grid.CellSize"),
	64,
	TEXT("Edge length of one grid cell, in world units (cm)."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarSurfelGridCellCapacity(
	TEXT("r.Surfel.Grid.CellCapacity"),
	512,
	TEXT("Max surfels one cell can reference."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarSurfelGridVisFlags(
	TEXT("r.Surfel.Grid.VisFlags"),
	(int32)(ESurfelGridVisFlags::Wireframe | ESurfelGridVisFlags::Overflow),
	TEXT("Bitmask of what the Grid visualization draws.\n")
	TEXT("1: Cell wireframe\n")
	TEXT("2: Flag overflowed cells red\n")
	TEXT("4: Hide empty cells\n")
	TEXT("8: Heat map by occupancy instead of a color per cell\n"),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarSurfelGridVisEdgeThickness(
	TEXT("r.Surfel.Grid.VisEdgeThickness"),
	0.03f,
	TEXT("Width of the Grid visualization's wireframe, as a fraction of a cell."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarSurfelUseGrid(
	TEXT("r.Surfel.UseGrid"),
	1,
	TEXT("1: Use grid.\n")
	TEXT("0: Brute force."),
	ECVF_RenderThreadSafe
	);

TAutoConsoleVariable<int32> CVarSurfelVisualizeMode(
	TEXT("r.Surfel.VisualizeMode"),
	1,
	TEXT("What the visualize pass draws.\n")
	TEXT("0: None - scene color untouched\n")
	TEXT("1: Surfels - every surfel disc in its own color\n")
	TEXT("2: Grid - uniform grid cells tinted by occupancy\n")
	TEXT("3: Coverage - the coverage texture as a heat map\n")
	TEXT("4: Lumen GI - no overlay, Lumen left on\n")
	TEXT("5: Surfel GI - no overlay, Lumen off\n")
	TEXT("6: Direct Light Only - no overlay, no indirect lighting\n")
	TEXT("7: Irradiance - surfel irradiance resolved per pixel\n"),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarSurfelCoverageScale(
	TEXT("r.Surfel.CoverageScale"),
	1.0f,
	TEXT("Coverage value mapped to the top of the heat ramp in the Coverage visualization."),
	ECVF_RenderThreadSafe);

int32 GSurfelRefreshRequestId = 0;

std::atomic<int32> GSurfelAllocatedCount{0};

static FAutoConsoleCommand CVarSurfelRefreshCmd(
	TEXT("r.Surfel.Refresh"),
	TEXT("Clears all spawned surfels so they respawn from scratch on the current grid."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		++GSurfelRefreshRequestId;
	}));
