// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class Surfel : ModuleRules
{
    public Surfel(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "RenderCore",
                "RHI",
                "Renderer",   // FSceneViewExtensionBase, post-process hooks
                "Projects",   // IPluginManager, for the shader directory mapping
            }
            );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "Slate",
                "SlateCore"
            }
            );

        // ImGui    	
        PrivateDependencyModuleNames.Add("ImGui");
        
        // Headers shared between C++ and shaders (LightStruct.h)
        PublicIncludePaths.Add(Path.Combine(PluginDirectory, "Shaders", "Shared"));

        // For the Translucent header error
        PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory("Renderer"), "Internal"));
    }
}
