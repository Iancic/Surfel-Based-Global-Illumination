#pragma once
#include "ShaderParameterStruct.h"

/** Composites the Irradiance texture */
class FCompositePass : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCompositePass);
	SHADER_USE_PARAMETER_STRUCT(FCompositePass, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		/**
		 * Read irradiance texture
		 * Read the framebuffer without post process
		 * Write new framebuffer image to be displayed
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