#pragma once
#include "GlobalShader.h"
#include "RenderGraphUtils.h"
#include "ScreenPass.h"
#include "ShaderParameterStruct.h"
#include "Runtime/Renderer/Public/ScreenPass.h"
#include "CVarCommands.h"

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
	
		// For grid
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, GridCellEntries)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint32>, GridCounter)
		SHADER_PARAMETER(FVector3f, GridPosition)
		SHADER_PARAMETER(uint32, GridCellCount)
		SHADER_PARAMETER(uint32, CellResolution)
		SHADER_PARAMETER(uint32, CellCapacity)
		SHADER_PARAMETER(uint32, CellSize)

		// 0 = brute-force loop over every surfel (fallback), 1 = query the grid above.
		SHADER_PARAMETER(uint32, bUseGrid)

		// Which overlay to draw: one of the VISUALIZE_MODE_* values below.
		SHADER_PARAMETER(uint32, VisualizeMode)

		// The coverage map Scatter wrote earlier this frame, for VISUALIZE_MODE_COVERAGE.
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CoverageTexture)
		SHADER_PARAMETER(uint32, bHasCoverageTexture) // 0 = CoverageTexture is a dummy
		SHADER_PARAMETER(float, CoverageScale)        // Coverage value that maps to the top of the ramp

		// Grid visualization options: ESurfelGridVisFlags bits, and the wireframe width.
		SHADER_PARAMETER(uint32, GridVisFlags)
		SHADER_PARAMETER(float, GridVisEdgeThickness)

		// For depth and normals
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferATexture)
		SHADER_PARAMETER(uint32, bHasGBufferNormal) // 0 = GBufferATexture is a dummy, skip the normal test
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferBTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferCTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferDTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferETexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferFTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)

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

		// So the shader branches on the same numbers ESurfelVisualizeMode does.
		OutEnvironment.SetDefine(TEXT("VISUALIZE_MODE_NONE"),     (uint32)ESurfelVisualizeMode::None);
		OutEnvironment.SetDefine(TEXT("VISUALIZE_MODE_SURFELS"),  (uint32)ESurfelVisualizeMode::Surfels);
		OutEnvironment.SetDefine(TEXT("VISUALIZE_MODE_GRID"),     (uint32)ESurfelVisualizeMode::Grid);
		OutEnvironment.SetDefine(TEXT("VISUALIZE_MODE_COVERAGE"), (uint32)ESurfelVisualizeMode::Coverage);

		OutEnvironment.SetDefine(TEXT("GRID_VIS_WIREFRAME"),  (uint32)ESurfelGridVisFlags::Wireframe);
		OutEnvironment.SetDefine(TEXT("GRID_VIS_OVERFLOW"),   (uint32)ESurfelGridVisFlags::Overflow);
		OutEnvironment.SetDefine(TEXT("GRID_VIS_HIDE_EMPTY"), (uint32)ESurfelGridVisFlags::HideEmpty);
		OutEnvironment.SetDefine(TEXT("GRID_VIS_HEATMAP"),    (uint32)ESurfelGridVisFlags::Heatmap);
	}
};