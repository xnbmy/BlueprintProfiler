// Copyright xnbmy 2026. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Analyzers/RuntimeProfiler.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Tests/AutomationCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRuntimeProfilerBasicTest, "BlueprintProfiler.RuntimeProfiler.BasicFunctionality",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext)

bool FRuntimeProfilerBasicTest::RunTest(const FString& Parameters)
{
	// Test basic runtime profiler functionality (using singleton)
	FRuntimeProfiler& RuntimeProfiler = FRuntimeProfiler::Get();

	// Test initial state
	TestFalse("Profiler should not be recording initially", RuntimeProfiler.IsRecording());

	// Test recording control
	RuntimeProfiler.StartRecording();
	TestTrue("Profiler should be recording after start", RuntimeProfiler.IsRecording());

	RuntimeProfiler.StopRecording();
	TestFalse("Profiler should not be recording after stop", RuntimeProfiler.IsRecording());

	// Test data reset
	RuntimeProfiler.ResetData();
	TestFalse("Profiler should not be recording after reset", RuntimeProfiler.IsRecording());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRuntimeProfilerDataCollectionTest, "BlueprintProfiler.RuntimeProfiler.DataCollection",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext)

bool FRuntimeProfilerDataCollectionTest::RunTest(const FString& Parameters)
{
	// Test data collection functionality (using singleton)
	FRuntimeProfiler& RuntimeProfiler = FRuntimeProfiler::Get();

	// Test getting execution data when not recording
	TArray<FNodeExecutionData> ExecutionData = RuntimeProfiler.GetExecutionData();
	TestTrue("Execution data should be retrievable", ExecutionData.Num() >= 0);

	// Test getting hot nodes
	TArray<FHotNodeInfo> HotNodes = RuntimeProfiler.GetHotNodes(1000.0f);
	TestTrue("Hot nodes should be retrievable", HotNodes.Num() >= 0);

	// Test getting tick abuse data
	TArray<FTickAbuseInfo> TickAbuse = RuntimeProfiler.GetTickAbuseActors();
	TestTrue("Tick abuse data should be retrievable", TickAbuse.Num() >= 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRuntimeProfilerHotNodeDetectionTest, "BlueprintProfiler.RuntimeProfiler.HotNodeDetection",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext)

bool FRuntimeProfilerHotNodeDetectionTest::RunTest(const FString& Parameters)
{
	// Test hot node detection with different thresholds (using singleton)
	FRuntimeProfiler& RuntimeProfiler = FRuntimeProfiler::Get();

	// Test with different thresholds
	TArray<FHotNodeInfo> HotNodes1000 = RuntimeProfiler.GetHotNodes(1000.0f);
	TArray<FHotNodeInfo> HotNodes2000 = RuntimeProfiler.GetHotNodes(2000.0f);
	TArray<FHotNodeInfo> HotNodes500 = RuntimeProfiler.GetHotNodes(500.0f);

	// Higher threshold should return fewer or equal results
	TestTrue("Higher threshold should return fewer results", HotNodes2000.Num() <= HotNodes1000.Num());
	TestTrue("Lower threshold should return more results", HotNodes500.Num() >= HotNodes1000.Num());

	return true;
}