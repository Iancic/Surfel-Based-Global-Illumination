#include "ComputePasses/SpawnPass.h"

IMPLEMENT_GLOBAL_SHADER(FSpawnSurfelPass, "/Surfel/Surfels/Spawn.usf", "MainCS", SF_Compute);