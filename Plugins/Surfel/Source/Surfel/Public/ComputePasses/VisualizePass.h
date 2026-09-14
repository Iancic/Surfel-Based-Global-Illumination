#pragma once
#include "GlobalShader.h"
#include "RenderGraphUtils.h"
#include "ScreenPass.h"
#include "ShaderParameterStruct.h"
#include "Runtime/Renderer/Public/ScreenPass.h"

/** Visualize surfels */
class FSurfelFullscreenCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSurfelFullscreenCS);
	SHADER_USE_PARAMETER_STRUCT(FSurfelFullscreenCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		
		// Surfels to visualize
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SurfelCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelPositionAndRadius)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelNormalAndFlags)
	
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputSceneColor)
		SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, InputViewport)
		SHADER_PARAMETER(float, Intensity)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutRenderTarget)

		// Needed to project each surfel's world position back to screen space.
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		// Fraction of the true surfel radius drawn, so individual surfels stay distinguishable.
		SHADER_PARAMETER(float, DebugRadiusScale)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("THREADS_X"), FComputeShaderUtils::kGolden2DGroupSize);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), FComputeShaderUtils::kGolden2DGroupSize);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};