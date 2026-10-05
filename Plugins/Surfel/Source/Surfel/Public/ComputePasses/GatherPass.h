#pragma once
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "Runtime/Engine/Public/SceneView.h"
#include "SceneTexturesConfig.h"
/**
 * 2D dispatch of sizeXsize groups
 * 
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
		
		// Surfel related parameters
		SHADER_PARAMETER(uint32, SurfelBudget)
		SHADER_PARAMETER(float, SurfelRadius)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SurfelCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelPositionAndRadius)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelNormalAndFlags)
	
		// Coverage
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, CoverageTexture)
		SHADER_PARAMETER(float, SpawnChance)
		SHADER_PARAMETER(float, SpawnCoverageThreshold)
		
		// View
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER(FUintVector2, ViewRectMin)
		SHADER_PARAMETER(FUintVector2, ViewRectMax)
		SHADER_PARAMETER(uint32, SpawnTileSize)

		// GBuffers
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureShaderParameters, GBufferTextures)

	END_SHADER_PARAMETER_STRUCT()
	
	// Credit Claude:
	/**
	 * Side in pixels of the square that gets one spawn per frame: the diameter a new surfel
	 * covers on screen. A surfel only blocks spawning where its weight, 1 - smoothstep(0, R, d),
	 * stays at or above the threshold, which is out to t * R with t = inverse smoothstep(1 - threshold).
	 * Smaller tiles let neighbours spawn inside each other's disc in the same frame.
	 */
	static uint32 ComputeSpawnTileSize(float SurfelRadiusPixels, float SpawnCoverageThreshold)
	{
		const float Y = FMath::Clamp(1.0f - SpawnCoverageThreshold, 0.0f, 1.0f);
		const float CoveredFraction = 0.5f - FMath::Sin(FMath::Asin(1.0f - 2.0f * Y) / 3.0f);
		const float CoveredDiameter = 2.0f * CoveredFraction * SurfelRadiusPixels;

		// At least one group's worth of pixels; capped so a huge radius can't stall a group
		return (uint32)FMath::Clamp(FMath::CeilToInt(CoveredDiameter), 16, 256);
	}

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