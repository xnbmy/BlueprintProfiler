// Copyright xnbmy 2026. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Analyzers/StaticLinter.h"
#include "Data/ProfilerDataTypes.h"
#include "Engine/Blueprint.h"
#include "AssetRegistry/AssetData.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStaticLinterBatchProcessingTest, "BlueprintProfiler.StaticLinter.BatchProcessing", 
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStaticLinterBatchProcessingTest::RunTest(const FString& Parameters)
{
	// Test batch processing functionality
	FStaticLinter Linter;
	
	// Test 1: Verify initial state
	TestFalse("Linter should not be scanning initially", Linter.IsScanInProgress());
	TestEqual("Initial issues count should be zero", Linter.GetIssues().Num(), 0);
	
	// Test 2: Verify progress tracking structure
	FScanProgress Progress = Linter.GetScanProgress();
	TestEqual("Initial processed assets should be zero", Progress.ProcessedAssets, 0);
	TestEqual("Initial total assets should be zero", Progress.TotalAssets, 0);
	TestEqual("Initial progress percentage should be zero", Progress.ProgressPercentage, 0.0f);
	TestFalse("Initial completion state should be false", Progress.bIsCompleted);
	TestFalse("Initial cancellation state should be false", Progress.bWasCancelled);
	
	// Test 3: Verify scan configuration defaults
	FScanConfiguration Config;
	TestTrue("Multi-threading should be enabled by default", Config.bUseMultiThreading);
	TestEqual("Default max concurrent tasks should be 4", Config.MaxConcurrentTasks, 4);
	TestTrue("Dead node check should be enabled by default", Config.EnabledChecks.Contains(ELintIssueType::DeadNode));
	TestTrue("Orphan node check should be enabled by default", Config.EnabledChecks.Contains(ELintIssueType::OrphanNode));
	TestTrue("Cast abuse check should be enabled by default", Config.EnabledChecks.Contains(ELintIssueType::CastAbuse));
	TestTrue("Tick abuse check should be enabled by default", Config.EnabledChecks.Contains(ELintIssueType::TickAbuse));
	
	// Test 4: Test empty asset array handling
	TArray<FAssetData> EmptyAssets;
	bool bCallbackCalled = false;
	
	Linter.OnScanComplete.AddLambda([&bCallbackCalled](const TArray<FLintIssue>& Issues)
	{
		bCallbackCalled = true;
	});
	
	Linter.ScanBlueprints(EmptyAssets, Config);
	
	// Give some time for async operations
	FPlatformProcess::Sleep(0.1f);
	
	TestTrue("Scan complete callback should be called for empty asset array", bCallbackCalled);
	TestFalse("Linter should not be scanning after empty scan", Linter.IsScanInProgress());
	
	// Test 5: Test cancellation functionality
	Linter.CancelScan(); // Should handle gracefully even when not scanning
	TestFalse("Cancel should not affect non-scanning state", Linter.IsScanInProgress());
	
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStaticLinterProgressFeedbackTest, "BlueprintProfiler.StaticLinter.ProgressFeedback", 
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStaticLinterProgressFeedbackTest::RunTest(const FString& Parameters)
{
	// Test progress feedback functionality
	FStaticLinter Linter;
	
	// Test progress callback mechanism
	int32 ProgressCallbackCount = 0;
	int32 LastProcessedAssets = 0;
	int32 LastTotalAssets = 0;
	
	Linter.OnScanProgress.AddLambda([&](int32 ProcessedAssets, int32 TotalAssets)
	{
		ProgressCallbackCount++;
		LastProcessedAssets = ProcessedAssets;
		LastTotalAssets = TotalAssets;
	});
	
	// Simulate progress updates
	Linter.UpdateScanProgress(5, 10, TEXT("TestAsset"));
	
	FScanProgress Progress = Linter.GetScanProgress();
	TestEqual("Progress processed assets should be updated", Progress.ProcessedAssets, 5);
	TestEqual("Progress total assets should be updated", Progress.TotalAssets, 10);
	TestEqual("Progress percentage should be calculated correctly", Progress.ProgressPercentage, 0.5f);
	TestEqual("Current asset should be updated", Progress.CurrentAsset, TEXT("TestAsset"));
	TestEqual("Progress callback should be called", ProgressCallbackCount, 1);
	TestEqual("Callback should receive correct processed count", LastProcessedAssets, 5);
	TestEqual("Callback should receive correct total count", LastTotalAssets, 10);
	
	// Test time estimation (should be calculated when assets are processed)
	TestTrue("Estimated time remaining should be non-negative", Progress.EstimatedTimeRemaining >= 0.0f);
	
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStaticLinterConfigurationTest, "BlueprintProfiler.StaticLinter.Configuration", 
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStaticLinterConfigurationTest::RunTest(const FString& Parameters)
{
	// Test scan configuration functionality
	FScanConfiguration Config;
	
	// Test include/exclude paths
	Config.IncludePaths.Add(TEXT("/Game/TestFolder"));
	Config.ExcludePaths.Add(TEXT("/Game/ExcludeFolder"));
	
	TestEqual("Include paths should be set correctly", Config.IncludePaths.Num(), 1);
	TestEqual("Exclude paths should be set correctly", Config.ExcludePaths.Num(), 1);
	TestTrue("Include path should match", Config.IncludePaths[0] == TEXT("/Game/TestFolder"));
	TestTrue("Exclude path should match", Config.ExcludePaths[0] == TEXT("/Game/ExcludeFolder"));
	
	// Test selective check enabling/disabling
	Config.EnabledChecks.Empty();
	Config.EnabledChecks.Add(ELintIssueType::DeadNode);
	Config.EnabledChecks.Add(ELintIssueType::CastAbuse);
	
	TestEqual("Should have 2 enabled checks", Config.EnabledChecks.Num(), 2);
	TestTrue("Dead node check should be enabled", Config.EnabledChecks.Contains(ELintIssueType::DeadNode));
	TestTrue("Cast abuse check should be enabled", Config.EnabledChecks.Contains(ELintIssueType::CastAbuse));
	TestFalse("Orphan node check should be disabled", Config.EnabledChecks.Contains(ELintIssueType::OrphanNode));
	TestFalse("Tick abuse check should be disabled", Config.EnabledChecks.Contains(ELintIssueType::TickAbuse));
	
	// Test multi-threading configuration
	Config.bUseMultiThreading = false;
	Config.MaxConcurrentTasks = 8;
	
	TestFalse("Multi-threading should be disabled", Config.bUseMultiThreading);
	TestEqual("Max concurrent tasks should be set correctly", Config.MaxConcurrentTasks, 8);
	
	return true;
}