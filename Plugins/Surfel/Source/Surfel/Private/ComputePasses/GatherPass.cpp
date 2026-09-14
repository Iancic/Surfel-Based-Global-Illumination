#include "ComputePasses/GatherPass.h"

IMPLEMENT_GLOBAL_SHADER(FGatherSurfelPass, "/Surfel/Surfels/Gather.usf", "MainCS", SF_Compute);