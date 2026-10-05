#include "ComputePasses/TextureIrradiancePass.h"

IMPLEMENT_GLOBAL_SHADER(FTextureIrradiancePass, "/Surfel/Surfels/TextureIrradiance.usf", "MainCS", SF_Compute);