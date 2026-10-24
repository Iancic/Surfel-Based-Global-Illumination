#include "ComputePasses/ScatterPass.h"

IMPLEMENT_GLOBAL_SHADER(FScatterPass, "/Surfel/Surfels/Scatter.usf", "MainCS", SF_Compute);