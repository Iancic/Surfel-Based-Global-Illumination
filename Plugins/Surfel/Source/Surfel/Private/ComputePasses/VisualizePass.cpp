#pragma once
#include "ComputePasses/VisualizePass.h"

IMPLEMENT_GLOBAL_SHADER(FVisualizePassCS, "/Surfel/Surfels/Visualize.usf", "MainCS", SF_Compute);