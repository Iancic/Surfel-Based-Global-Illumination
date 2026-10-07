#include "SurfelModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "CVarCommands.h"

#define LOCTEXT_NAMESPACE "FSurfelModule"

void FSurfelModule::StartupModule()
{
	// Map the virtual address of the shaders in this plugin
	AddShaderSourceDirectoryMapping(
		TEXT("/Surfel"), 
		IPluginManager::Get().FindPlugin("Surfel")->GetBaseDir() + "/Shaders/Private");

	// The module loads at PostConfigInit, before the renderer registers its CVars, so wait for engine init
	FCoreDelegates::OnPostEngineInit.AddStatic(&ForceGlobalSDFBuild);
}

void FSurfelModule::ShutdownModule() { }

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FSurfelModule, Surfel)