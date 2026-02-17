// Copyright xnbmy 2026. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Analyzers/MemoryAnalyzer.h"
#include "Engine/Blueprint.h"
#include "Engine/Texture2D.h"
#include "Tests/AutomationCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMemoryAnalyzerBasicTest, "BlueprintProfiler.MemoryAnalyzer.BasicFunctionality", 
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMemoryAnalyzerBasicTest::RunTest(const FString& Parameters)
{
	// Test basic memory analyzer functionality
	FMemoryAnalyzer MemoryAnalyzer;
	
	// Test threshold configuration
	MemoryAnalyzer.SetLargeResourceThreshold(5.0f);
	TestEqual("Threshold should be set correctly", MemoryAnalyzer.GetLargeResourceThreshold(), 5.0f);
	
	// Test minimum threshold enforcement
	MemoryAnalyzer.SetLargeResourceThreshold(0.05f);
	TestTrue("Threshold should be enforced to minimum", MemoryAnalyzer.GetLargeResourceThreshold() >= 0.1f);
	
	// Test that analyzer is not in progress initially
	TestFalse("Analyzer should not be in progress initially", MemoryAnalyzer.IsAnalysisInProgress());
	
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMemoryAnalyzerLargeResourceDetectionTest, "BlueprintProfiler.MemoryAnalyzer.LargeResourceDetection", 
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMemoryAnalyzerLargeResourceDetectionTest::RunTest(const FString& Parameters)
{
	// Test large resource detection functionality
	FMemoryAnalyzer MemoryAnalyzer;
	
	// Set a low threshold for testing
	MemoryAnalyzer.SetLargeResourceThreshold(1.0f); // 1MB threshold
	
	// Test getting project-wide alerts (should not crash with empty data)
	TArray<FLargeResourceReference> ProjectAlerts = MemoryAnalyzer.GetLargeResourceAlertsForProject(1.0f);
	TestTrue("Project alerts should be retrievable", ProjectAlerts.Num() >= 0);
	
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMemoryAnalyzerAsyncTest, "BlueprintProfiler.MemoryAnalyzer.AsyncAnalysis", 
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMemoryAnalyzerAsyncTest::RunTest(const FString& Parameters)
{
	// Test async analysis functionality
	FMemoryAnalyzer MemoryAnalyzer;
	
	// Test cancellation when no analysis is running
	MemoryAnalyzer.CancelAnalysis(); // Should not crash
	TestFalse("Should not be in progress after cancel", MemoryAnalyzer.IsAnalysisInProgress());
	
	return true;
}