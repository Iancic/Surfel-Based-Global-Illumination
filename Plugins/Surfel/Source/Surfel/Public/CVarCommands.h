#pragma once
#include "CVarCommands.h"

static TAutoConsoleVariable<int32> CVarSurfelMode(
	TEXT("r.Surfel.Mode"),
	1,
	TEXT("Surfel compute pass mode.\n")
	TEXT(" 0: off\n")
	TEXT(" 1: fullscreen procedural example\n")
	TEXT(" 2: G-Buffer visualization"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarSurfelIntensity(
	TEXT("r.Surfel.Intensity"),
	0.25f,
	TEXT("Blend strength of the fullscreen example effect (0-1)."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarSurfelDepthScale(
	TEXT("r.Surfel.DepthScale"),
	10000.0f,
	TEXT("Far distance in world units mapped to white in depth visualization."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int> CVarSurfelEnable(
	TEXT("r.Surfel.Enable"),
	1,
	TEXT("Is surfel system enabled or not."),
	ECVF_RenderThreadSafe
	);

static TAutoConsoleVariable<int> CVarSurfelBudget(
	TEXT("r.Surfel.Budget"),
	500,
	TEXT("Surfel Budget."),
	ECVF_RenderThreadSafe
	);

static TAutoConsoleVariable<int> CVarSurfelRadius(
	TEXT("r.Surfel.Radius"),
	10, // THESE VALUES AND THE GRIDS SHOLD BE CONNECTED
	TEXT("Surfel Radius, in world units (cm)."),
	ECVF_RenderThreadSafe
	);

static TAutoConsoleVariable<int> CVarSurfelGridSize(
	TEXT("r.Surfel.GridSize"),
	32,
	TEXT("Gather dispatches one thread per NxN pixel block instead of per pixel, spacing spawned surfels out on a coarse screen-space grid.\n")
	TEXT("Stopgap until the Scatter coverage pass is implemented."),
	ECVF_RenderThreadSafe
	);

static TAutoConsoleVariable<float> CVarSurfelCoverageRadius(
	TEXT("r.Surfel.CoverageRadius"),
	40.0f,
	TEXT("TEMP stand-in for Scatter coverage: world-space distance (cm) under which Gather considers a spot already covered by an existing surfel and skips spawning.\n")
	TEXT("Lets old surfels persist and budget only get spent on newly-revealed geometry (e.g. after moving the camera). Remove once Scatter provides real coverage."),
	ECVF_RenderThreadSafe
	);

static TAutoConsoleVariable<int32> CVarSurfelUseGrid(
	TEXT("r.Surfel.UseGrid"),
	1,
	TEXT("1: fullscreen visualize queries the uniform grid instead of brute-force looping every surfel.\n")
	TEXT("0: brute-force fallback (loop every surfel per pixel) - useful for A/B verifying the grid query is correct."),
	ECVF_RenderThreadSafe
	);

// Bumped by r.Surfel.Refresh; Gather clears and respawns from scratch when this changes.
static int32 GSurfelRefreshRequestId = 0;

static FAutoConsoleCommand CVarSurfelRefreshCmd(
	TEXT("r.Surfel.Refresh"),
	TEXT("Clears all spawned surfels so they respawn from scratch on the current grid."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		++GSurfelRefreshRequestId;
	}));