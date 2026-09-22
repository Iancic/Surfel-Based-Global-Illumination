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
	0.25f,
	TEXT("Strength of the example effect (0-1)."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarSurfelDepthScale(
	TEXT("r.Surfel.DepthScale"),
	10000.0f,
	TEXT("Far distance in world units mapped to white in depth visualization."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int> CVarSurfelBudget(
	TEXT("r.Surfel.Budget"),
	500,
	TEXT("Surfel Budget."),
	ECVF_RenderThreadSafe
	);

// Float, not int: the ImGui slider and every shader that reads it treat it as a float,
// and an int CVar silently truncated whatever the slider set.
TAutoConsoleVariable<float> CVarSurfelRadius(
	TEXT("r.Surfel.Radius"),
	10.0f,
	TEXT("Surfel radius in screen pixels at spawn time, also used as the minimum world radius (cm)."),
	ECVF_RenderThreadSafe
	);

TAutoConsoleVariable<int32> CVarSurfelSpawnsPerFrame(
	TEXT("r.Surfel.SpawnsPerFrame"),
	256,
	TEXT("Roughly how many surfels Gather may spawn per frame across the whole screen, independent of resolution."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarSurfelSpawnCoverageThreshold(
	TEXT("r.Surfel.SpawnCoverageThreshold"),
	0.1f,
	TEXT("Pixels with coverage at or above this are considered covered and never spawn a surfel."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarSurfelGridCellResolution(
	TEXT("r.Surfel.Grid.CellResolution"),
	32,
	TEXT("Cells along each axis of the camera-centred uniform grid (8-64). Memory grows with the cube of this."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarSurfelGridCellSize(
	TEXT("r.Surfel.Grid.CellSize"),
	64,
	TEXT("Edge length of one grid cell, in world units (cm)."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarSurfelGridCellCapacity(
	TEXT("r.Surfel.Grid.CellCapacity"),
	128,
	TEXT("Max surfels one cell can reference. Anything past this is dropped from the cell."),
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
	TEXT("6: Direct Light Only - no overlay, no indirect lighting\n"),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarSurfelCoverageScale(
	TEXT("r.Surfel.CoverageScale"),
	1.0f,
	TEXT("Coverage value mapped to the top of the heat ramp in the Coverage visualization."),
	ECVF_RenderThreadSafe);

int32 GSurfelRefreshRequestId = 0;

static FAutoConsoleCommand CVarSurfelRefreshCmd(
	TEXT("r.Surfel.Refresh"),
	TEXT("Clears all spawned surfels so they respawn from scratch on the current grid."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		++GSurfelRefreshRequestId;
	}));
