#include "CVarCommands.h"

TAutoConsoleVariable<float> CVarMinTraceDistance(
	TEXT("r.Surfel.MinTraceDistance"),
	0.f,
	TEXT("MinTraceDistance.\n")
	TEXT("MinTraceDistance."),
	ECVF_RenderThreadSafe
	);

TAutoConsoleVariable<float> CVarMaxTraceDistance(
	TEXT("r.Surfel.MaxTraceDistance"),
	100000.f, // 100 meters, 10.000 cm
	TEXT("MaxTraceDistance.\n")
	TEXT("MaxTraceDistance."),
	ECVF_RenderThreadSafe
	);

TAutoConsoleVariable<float> CVarStepFactor(
	TEXT("r.Surfel.StepFactor"),
	1.f,
	TEXT("StepFactor.\n")
	TEXT("StepFactor."),
	ECVF_RenderThreadSafe
	);

TAutoConsoleVariable<float> CVarMinStepFactor(
	TEXT("r.Surfel.MinStepFactor"),
	1.f,
	TEXT("MinStepFactor.\n")
	TEXT("MinStepFactor."),
	ECVF_RenderThreadSafe
	);

TAutoConsoleVariable<float> CVarMaxSamples(
	TEXT("r.Surfel.MaxSamples"),
	128.f,
	TEXT("MaxSamples.\n")
	TEXT("MaxSamples."),
	ECVF_RenderThreadSafe
	);

TAutoConsoleVariable<int32> CVarMaxBounces(
	TEXT("r.Surfel.MaxBounces"),
	4,
	TEXT("Bounces each surfel ray path follows per frame (1-8).\n")
	TEXT("1: one hop, the surfel cache supplies the rest over time.\n")
	TEXT("More: every extra hop adds light explicitly, at the cost of one more ray march per surfel."),
	ECVF_RenderThreadSafe
	);

TAutoConsoleVariable<int32> CVarRaysPerSurfel(
	TEXT("r.Surfel.RaysPerSurfel"),
	4,
	TEXT("Independent ray paths each surfel traces per frame and averages (1-16).\n")
	TEXT("More rays = less noise and splotchiness per frame, cost scales linearly."),
	ECVF_RenderThreadSafe
	);

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

TAutoConsoleVariable<int> CVarSurfelBudget(
	TEXT("r.Surfel.Budget"),
	20000,
	TEXT("Surfel Budget."),
	ECVF_RenderThreadSafe
	);

TAutoConsoleVariable<float> CVarSurfelRadius(
	TEXT("r.Surfel.Radius"),
	46.0f,
	TEXT("Surfel radius in screen pixels at spawn time, also used as the minimum world radius (cm)."),
	ECVF_RenderThreadSafe
	);

// Important to have a lower value
// The surfels clump up together because first frame on a new wall everything is uncovered. 
// I should delay spawning some so the algorithm can go through the gather with new surfels. 
// Not spawning all surfels from one. Thomas talk on PICA PICA
TAutoConsoleVariable<int32> CVarSurfelSpawnsPerFrame(
	TEXT("r.Surfel.SpawnsPerFrame"),
	1,
	TEXT("Roughly how many surfels the Spawn pass may create per frame across the screen."),
	ECVF_RenderThreadSafe);

// This avoids over-crowding, sitting next to each other
// 1 means spaced so for a surfel to spawn that has to be uncovererd fully
// closer to 0 means touching each other, so covered is considered something in between like a bit covered by another surfel
// Used by spawn after reading the coverage texture
TAutoConsoleVariable<float> CVarSurfelSpawnCoverageThreshold(
	TEXT("r.Surfel.SpawnCoverageThreshold"),
	0.6f,
	TEXT("Pixels with coverage at or above this are considered covered and never spawn a surfel."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarSurfelGridCellResolution(
	TEXT("r.Surfel.Grid.CellResolution"),
	128,
	TEXT("Cells along each axis of the camera."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarSurfelGridCellSize(
	TEXT("r.Surfel.Grid.CellSize"),
	128,
	TEXT("Edge length of one grid cell, in world units (cm)."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarSurfelGridCellCapacity(
	TEXT("r.Surfel.Grid.CellCapacity"),
	1024,
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
	TEXT("5: Surfel GI - Lumen off, surfel indirect light composited under the direct lights\n")
	TEXT("6: Direct Light Only - no overlay, no indirect lighting\n")
	TEXT("7: Irradiance - surfel irradiance resolved per pixel\n")
	TEXT("8: Indirect Light - only the surfel bounce light, as the composite would add it\n")
	TEXT("9: SSAO - the ambient occlusion buffer on its own\n"),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarSurfelColorMode(
	TEXT("r.Surfel.ColorMode"),
	0,
	TEXT("What each surfel disc shows in the Surfels visualization.\n")
	TEXT("0: Hash - random color per surfel\n")
	TEXT("1: Albedo - surface color the surfel captured at spawn\n")
	TEXT("2: Indirect - incoming indirect light the surfel accumulated\n"),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarSurfelGIIntensity(
	TEXT("r.Surfel.GIIntensity"),
	1.0f,
	TEXT("Multiplier on the surfel indirect light the composite pass adds to scene color."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarSurfelUseAO(
	TEXT("r.Surfel.UseAO"),
	0,
	TEXT("1: multiply ambient occlusion onto the composited surfel indirect light.\n")
	TEXT("0: skip it, so the composite shows the raw indirect light (for comparison)."),
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
void ForceGlobalSDFBuild()
{
	// Owned by the Renderer module, so look it up by name. It only exists once the renderer has loaded.
	IConsoleVariable* NecessityCheck = IConsoleManager::Get().FindConsoleVariable(TEXT("r.AOGlobalDistanceField.DetailedNecessityCheck"));
	if (!NecessityCheck)
	{
		UE_LOG(LogTemp, Warning, TEXT("Surfel: r.AOGlobalDistanceField.DetailedNecessityCheck not found, global SDF may not be built"));
		return;
	}

	NecessityCheck->Set(0, ECVF_SetByConsole);
}
