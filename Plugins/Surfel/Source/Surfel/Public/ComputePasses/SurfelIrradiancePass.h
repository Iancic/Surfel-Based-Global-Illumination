#pragma once
#include "ShaderParameterStruct.h"

/**
 * Note: after one scatter and one gather, doing scatter again will be done with the newly spawned surfel
 * meaning the scatter step won't detect that place as unoccupied so it finds the next worse covered spot
 *
 * 1D Dispatch: runs per surfel
 *
 * Because my solution is for low-end
 * Must be implemented with ray marching because hardware RT is not on most devices
 *
 * What happens per surfel
 * Trace visibility ray. Is this shadowed? If not store irradiance.
 * RECURSIVE
 *     Trace from a hemisphere (still researching what's the best algorithm for mitigating light leaking)
 *     and using the global SDF and ray marching (origin + direction * distance)
 *     I can find a position which I query to find a surfel (which stores more irradiance)
 *     from that surfel I can keep doing it
 *
 * This pass updates all the radiance values from the surfels
 */
class FSurfelIrradiancePass : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSurfelIrradiancePass);
	SHADER_USE_PARAMETER_STRUCT(FSurfelIrradiancePass, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		/**
		 * Read and write surfel buffers
		 * Read structure with surfels
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
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 1);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};