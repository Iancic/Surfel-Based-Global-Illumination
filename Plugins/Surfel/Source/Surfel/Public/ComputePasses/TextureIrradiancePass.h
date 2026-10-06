#pragma once
#include "SceneTexturesConfig.h"
#include "ShaderParameterStruct.h"

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
		// Output: rgb = surfel irradiance at this pixel, a = summed surfel weight (0 = no surfel reaches it)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, IrradianceTexture)

		// Surfel related parameters
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelPositionAndRadius)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelNormalAndFlags)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, SurfelIrradiance)

		// View
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER(FUintVector2, ViewRectMin)
		SHADER_PARAMETER(FUintVector2, ViewRectMax)

		// Uniform Grid
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, GridCellEntries)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, GridCounter)
		SHADER_PARAMETER(FVector3f, GridPosition)
		SHADER_PARAMETER(uint32, CellResolution)
		SHADER_PARAMETER(uint32, CellCapacity)
		SHADER_PARAMETER(uint32, CellSize)

		// GBuffers
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

