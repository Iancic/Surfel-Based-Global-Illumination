#pragma once
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "SceneTexturesConfig.h"

/**
 * Gather Pass (coverage)
 *
 * One thread per pixel. Each pixel pulls every surfel binned in its grid cell and sums
 * their coverage weight (distance + normal alignment). A gather: many surfels read, one
 * coverage value written per pixel.
 *
 * The result is the coverage map: where surfels already sit on screen, and where gaps
 * remain. The Spawn pass reads it next to place new surfels in the emptiest spots, one
 * per tile per frame - an iterative hole filler across frames until the budget fills.
 *
 * Screen-space pass, even though the surfels it reads live in world space.
 */
class FGatherSurfelPass : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGatherSurfelPass);
	SHADER_USE_PARAMETER_STRUCT(FGatherSurfelPass, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// View
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER(FUintVector2, ViewRectMin)
		SHADER_PARAMETER(FUintVector2, ViewRectMax)
	
		// Coverage Texture
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, CoverageTexture)
		
		// Surfels buffers
		SHADER_PARAMETER(uint32, SurfelBudget)
		SHADER_PARAMETER(float, SurfelRadius) // Temporarily from the CVar
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SurfelCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelPositionAndRadius)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelNormalAndFlags)
	
		// GBuffers
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureShaderParameters, GBufferTextures)
	
		// Uniform Grid
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, GridCellEntries)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, GridCounter)
		SHADER_PARAMETER(FVector3f, GridPosition)
		SHADER_PARAMETER(uint32, GridCellCount)
		SHADER_PARAMETER(uint32, CellResolution)
		SHADER_PARAMETER(uint32, CellCapacity)
		SHADER_PARAMETER(uint32, CellSize)
	
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