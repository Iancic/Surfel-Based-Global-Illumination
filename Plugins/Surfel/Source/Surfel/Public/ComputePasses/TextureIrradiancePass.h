#pragma once


/**
 * 2D Dispatch
 * Lookup in the grid for surfel in the buffer.
 * From depth buffer (using inverse vp) I can do a lookup in the structure that holds surfels to find what surfels are there.
 * Using this I write to a irradiance texture I can use in a composite pass.
 */
class FTextureIrradiancePass : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FTextureIrradiancePass);
	SHADER_USE_PARAMETER_STRUCT(FTextureIrradiancePass, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		/**
		 * Write irradiance texture
		 * Read depth
		 * Read surfel buffers
		 * Read and Write surfel structure
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

IMPLEMENT_GLOBAL_SHADER(FTextureIrradiancePass, "/Surfel/Surfels/TextureIrradiance.usf", "MainCS", SF_Compute);

