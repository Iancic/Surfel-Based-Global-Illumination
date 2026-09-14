#pragma once
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "Runtime/Engine/Public/SceneView.h"


class FUniformGridViewState
{
public:
	TRefCountPtr<FRDGPooledBuffer> GridCellEntries;
	TRefCountPtr<FRDGPooledBuffer> GridCounter;
		
	FVector3f GridPosition;
	inline static uint32 CellResolution = 32;
	inline static uint32 GridCellCount = CellResolution * CellResolution * CellResolution;
	inline static uint32 CellCapacity = 32; 
	inline static uint32 CellSize = 32; // Measured in World Units per cell
};
class FGridAllocationPass : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGridAllocationPass);
	SHADER_USE_PARAMETER_STRUCT(FGridAllocationPass, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		
		// For surfels
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SurfelCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelPositionAndRadius)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelNormalAndFlags)
		SHADER_PARAMETER(uint32, SurfelBudget)
		SHADER_PARAMETER(float, SurfelRadius)
	
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
	
		OutEnvironment.SetDefine(TEXT("THREADS_X"), 64);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 1);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};