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
	
		// To know the dispatch size
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER(FUintVector2, ViewRectMin)
		SHADER_PARAMETER(FUintVector2, ViewRectMax)
	
		// The important output of this shader
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CoverageTexture)
	
		// Surfels buffers
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SurfelCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelPositionAndRadius)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelNormalAndFlags)
	
		// Normal for coverage
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferATexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferBTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferCTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferDTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferETexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferFTexture)
		
		// Depth Buffer for position reconstruction
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
	
		// For grid
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