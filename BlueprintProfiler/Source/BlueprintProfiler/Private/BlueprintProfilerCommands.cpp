// Copyright xnbmy 2026. All Rights Reserved.

#include "BlueprintProfilerCommands.h"

#define LOCTEXT_NAMESPACE "BlueprintProfiler"

void FBlueprintProfilerCommands::RegisterCommands()
{
	UI_COMMAND(OpenPluginWindow, "Blueprint Profiler", "Open Blueprint Profiler window", EUserInterfaceActionType::Button, FInputChord());
}

#undef LOCTEXT_NAMESPACE