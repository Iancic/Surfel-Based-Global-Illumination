#include "ComputePasses/GridAllocationPass.h"

IMPLEMENT_GLOBAL_SHADER(FGridAllocationPass, "/Surfel/Surfels/GridAllocation.usf", "MainCS", SF_Compute);