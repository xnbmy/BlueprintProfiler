// Copyright xnbmy 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "BlueprintProfilerStyle.h"

/**
 * Command definitions for Blueprint Profiler plugin
 */
class FBlueprintProfilerCommands : public TCommands<FBlueprintProfilerCommands>
{
public:
	FBlueprintProfilerCommands()
		: TCommands<FBlueprintProfilerCommands>(TEXT("BlueprintProfiler"), NSLOCTEXT("Contexts", "BlueprintProfiler", "Blueprint Profiler Plugin"), NAME_None, FBlueprintProfilerStyle::GetStyleSetName())
	{
	}

	// TCommands<> interface
	virtual void RegisterCommands() override;

public:
	TSharedPtr<FUICommandInfo> OpenPluginWindow;
};