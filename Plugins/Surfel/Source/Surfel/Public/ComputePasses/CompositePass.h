#pragma once
#include "SceneTexturesConfig.h"
#include "ShaderParameterStruct.h"

/**
 * Adds the surfel indirect light (DiffuseColor / PI * Irradiance) into scene color in place.
 * direct lights, fog, translucency and antialiasing all run after it, same as with its own GI.
 */
class FCompositePass : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCompositePass);
	SHADER_USE_PARAMETER_STRUCT(FCompositePass, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Scene color after the base pass, read and written in place
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SceneColor)

		// TextureIrradiance output: rgb = surfel irradiance, a = summed surfel weight (0 = no surfel reaches it)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, IrradianceTexture)

		// TextureIrradiance output: screen space AO * material AO, multiplied onto the indirect light only
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, AmbientOcclusionTexture)

		SHADER_PARAMETER(float, GIIntensity)
		SHADER_PARAMETER(uint32, bUseAO) // 0 = skip AO, composite the raw indirect light

		// View
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER(FUintVector2, ViewRectMin)
		SHADER_PARAMETER(FUintVector2, ViewRectMax)

		// GBuffers, for the diffuse color
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureShaderParameters, GBufferTextures)
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
