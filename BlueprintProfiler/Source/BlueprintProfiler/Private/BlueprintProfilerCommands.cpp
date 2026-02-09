#include "BlueprintProfilerCommands.h"

#define LOCTEXT_NAMESPACE "FBlueprintProfilerModule"

void FBlueprintProfilerCommands::RegisterCommands()
{
	UI_COMMAND(OpenPluginWindow, "蓝图分析器", "打开蓝图分析器窗口", EUserInterfaceActionType::Button, FInputChord());
}

#undef LOCTEXT_NAMESPACE