#pragma once
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "Runtime/Engine/Public/SceneView.h"
#include "CVarCommands.h"


class FUniformGridViewState
{
public:
	TRefCountPtr<FRDGPooledBuffer> GridCellEntries;
	TRefCountPtr<FRDGPooledBuffer> GridCounter;

	// Driven by r.Surfel.Grid.* (exposed in the ImGui panel), see UpdateFromCVars
	inline static uint32 CellResolution = 32;
	inline static uint32 GridCellCount = CellResolution * CellResolution * CellResolution;
	// Surfels are now inserted into every cell they overlap, so cells fill up faster
	inline static uint32 CellCapacity = 128; // Cells near the camera hold many small surfels; overflow gets dropped
	inline static uint32 CellSize = 64; // Measured in World Units per cell

	// Pull the grid layout from the CVars. Render thread only, once at the top of the frame:
	// the grid buffers are recreated every frame, so a new layout is safe to pick up here,
	// and every pass after this point in the frame sees the same numbers.
	static void UpdateFromCVars()
	{
		check(IsInRenderingThread());

		// Clamped because the entries buffer is Resolution^3 * Capacity * 4 bytes:
		// 64^3 cells * 256 entries is already ~268 MB.
		CellResolution = (uint32)FMath::Clamp(CVarSurfelGridCellResolution.GetValueOnRenderThread(), 8, 64);
		CellCapacity   = (uint32)FMath::Clamp(CVarSurfelGridCellCapacity.GetValueOnRenderThread(), 8, 256);
		CellSize       = (uint32)FMath::Max(CVarSurfelGridCellSize.GetValueOnRenderThread(), 1);
		GridCellCount  = CellResolution * CellResolution * CellResolution;
	}
};
class FGridAllocationPass : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FGridAllocationPass);
	SHADER_USE_PARAMETER_STRUCT(FGridAllocationPass, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		// Surfels
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SurfelCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelPositionAndRadius)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfelNormalAndFlags)
		SHADER_PARAMETER(uint32, SurfelBudget)
		SHADER_PARAMETER(float, SurfelRadius)
	
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
	
		OutEnvironment.SetDefine(TEXT("THREADS_X"), 64);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 1);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};