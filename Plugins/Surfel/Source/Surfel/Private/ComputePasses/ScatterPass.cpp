#include "ComputePasses/ScatterPass.h"

IMPLEMENT_GLOBAL_SHADER(FScatterSurfelPass, "/Surfel/Surfels/Scatter.usf", "MainCS", SF_Compute);