#pragma once
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

/**
 * note: explaining nomenclature
 * called scatter because one surfel writes to many pixels
 * gather because one pixel from many in a tile finds the worst value
 *
 * Scatter answers this question:
 * Where on the screen are no surfels? So I know where to spawn more
 * To store this I use a coverage texture used in the gather step (where surfels get spawned)
 * Scatter is a screen space pass even though surfels are spawned and exist in world-space
 *
 * Get every surfel we can see that exists, make the coverage map to detect gaps where we can spawn more
 * It's an iterative hole filler because every frame I found holes to fill until budget is done of we filled it.
 *
 * When a frame has everywhere 0 coverage (first ever frame)
 * Make sure first time it runs I use blue noise to make the first surfel un-uniform. 
 *
 * One thread per surfel and see where it lands on screen
 */
class FScatterSurfelPass : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FScatterSurfelPass);
	SHADER_USE_PARAMETER_STRUCT(FScatterSurfelPass, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		/**
		 * Write to texture with pixel coverage for gather step
		 * Read surfel buffer for every alive surfel
		 */
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	
		OutEnvironment.SetDefine(TEXT("THREADS_X"), 16);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 16);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};