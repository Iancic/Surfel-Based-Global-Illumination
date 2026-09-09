#include "Surfel.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FSurfelModule"

void FSurfelModule::StartupModule()
{
	// Map the virtual adress of the shaders in this plugin
	AddShaderSourceDirectoryMapping(
		TEXT("/Surfel"), 
		IPluginManager::Get().FindPlugin("Surfel")->GetBaseDir() + "/Shaders/Private");
}

void FSurfelModule::ShutdownModule()
{
	
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FSurfelModule, Surfel)