#include "BlueprintProfilerCommands.h"

#define LOCTEXT_NAMESPACE "FBlueprintProfilerModule"

void FBlueprintProfilerCommands::RegisterCommands()
{
	UI_COMMAND(OpenPluginWindow, "Blueprint Profiler", "Bring up Blueprint Profiler window", EUserInterfaceActionType::Button, FInputChord());
}

#undef LOCTEXT_NAMESPACE