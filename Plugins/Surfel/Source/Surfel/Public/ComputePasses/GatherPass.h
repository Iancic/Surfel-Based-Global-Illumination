#pragma once
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "Runtime/Engine/Public/SceneView.h"

/**
 * 2D dispatch of 16x16
 * Gather Pass
 *
 * Find the tile minimum and it's pixel coordinate (tile min reduction)
 * Using that pixel coordinate of the worst pixel, sample the GBuffer for depth and normal.
 * Depth: GBuffer is not enough as a position, I need to reconstruct to world position + depth + inverse VP
 * Normal: is good as is just read
 *
 * With this gathered data I can spawn (not really spawn since it's allocated already, more like modify) a surfel from the pool buffer
 * e.g.: {pos, normal, radius, radiance = 0) in Pool[index]
 * index can be InterlockedAdd
 */
class FGatherSurfelPass : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGatherSurfelPass);
	SHADER_USE_PARAMETER_STRUCT(FGatherSurfelPass, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		/**
		 * Read depth GBuffer
		 * Read normal GBuffer
		 * Read and write surfel buffers
		 * Write surfel structure
		 *
		 * TODO: read texture with pixel coverage once Scatter writes one; for now
		 * every dispatched pixel is treated as uncovered and always spawns.
		 */
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SurfelCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelPositionAndRadius)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelNormalAndFlags)
	
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferATexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferBTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferCTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferDTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferETexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferFTexture)
	
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
	
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER(FUintVector2, ViewRectMin)
		SHADER_PARAMETER(FUintVector2, ViewRectMax)

		SHADER_PARAMETER(uint32, SurfelBudget)
		SHADER_PARAMETER(float, SurfelRadius) // Temporarily from the CVar

		// Stopgap until Scatter coverage exists: one thread per GridSize x GridSize
		// pixel block (sampling the block's center pixel) instead of one thread per pixel,
		// so spawned surfels land spread out instead of piling up on every visible pixel.
		SHADER_PARAMETER(uint32, GridSize)

		// TEMP (this session): stand-in for Scatter coverage. See CoverageRadius
		// comment in Gather.usf.
		SHADER_PARAMETER(float, CoverageRadius)

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