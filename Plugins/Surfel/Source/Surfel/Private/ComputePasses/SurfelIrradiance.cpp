#include "ComputePasses/SurfelIrradiancePass.h"

IMPLEMENT_GLOBAL_SHADER(FSurfelIrradiancePass, "/Surfel/Surfels/SurfelIrradiance.usf", "MainCS", SF_Compute);