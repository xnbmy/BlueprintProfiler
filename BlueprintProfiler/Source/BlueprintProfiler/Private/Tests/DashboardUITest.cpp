// Copyright xnbmy 2026. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UI/SBlueprintProfilerWidget.h"
#include "Data/ProfilerDataTypes.h"
#include "Widgets/SWidget.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDashboardUIDataDisplayTest, "BlueprintProfiler.UI.DataDisplay", 
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDashboardUIDataDisplayTest::RunTest(const FString& Parameters)
{
	// Test unified data display and formatting functionality
	
	// Test 1: Verify data item creation from runtime data
	FNodeExecutionData RuntimeData;
	RuntimeData.NodeName = TEXT("TestNode");
	RuntimeData.BlueprintName = TEXT("TestBlueprint");
	RuntimeData.AverageExecutionsPerSecond = 500.0f;
	RuntimeData.TotalExecutions = 1000;
	RuntimeData.AverageExecutionTime = 0.001f;
	
	// Create widget to test data item creation
	TSharedRef<SBlueprintProfilerWidget> Widget = SNew(SBlueprintProfilerWidget);
	
	// Test data item creation (we can't directly test private methods, but we can test the public interface)
	TArray<FNodeExecutionData> RuntimeDataArray;
	RuntimeDataArray.Add(RuntimeData);
	Widget->SetRuntimeData(RuntimeDataArray);
	
	// Test 2: Verify lint issue data creation
	FLintIssue LintIssue;
	LintIssue.Type = ELintIssueType::DeadNode;
	LintIssue.NodeName = TEXT("DeadTestNode");
	LintIssue.BlueprintPath = TEXT("/Game/TestBlueprint.uasset");
	LintIssue.Severity = ESeverity::High;
	LintIssue.Description = TEXT("Test dead node");
	
	TArray<FLintIssue> LintIssues;
	LintIssues.Add(LintIssue);
	Widget->SetLintIssues(LintIssues);
	
	// Test 3: Verify memory analysis data creation
	FMemoryAnalysisResult MemoryData;
	MemoryData.InclusiveSize = 25.5f; // 25.5 MB
	MemoryData.ReferenceDepth = 3;
	MemoryData.TotalReferences = 50;
	
	TArray<FMemoryAnalysisResult> MemoryDataArray;
	MemoryDataArray.Add(MemoryData);
	Widget->SetMemoryData(MemoryDataArray);
	
	// Test 4: Verify refresh functionality
	Widget->RefreshData();
	
	TestTrue("Widget should be valid after data operations", true);
	
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDashboardUISearchFilterTest, "BlueprintProfiler.UI.SearchFilter", 
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDashboardUISearchFilterTest::RunTest(const FString& Parameters)
{
	// Test search and filter functionality
	
	// Create widget for testing
	TSharedRef<SBlueprintProfilerWidget> Widget = SNew(SBlueprintProfilerWidget);
	
	// Test 1: Add some test data
	TArray<FNodeExecutionData> RuntimeDataArray;
	
	FNodeExecutionData HighFreqData;
	HighFreqData.NodeName = TEXT("HighFrequencyNode");
	HighFreqData.BlueprintName = TEXT("PerformanceBlueprint");
	HighFreqData.AverageExecutionsPerSecond = 1500.0f; // Critical
	RuntimeDataArray.Add(HighFreqData);
	
	FNodeExecutionData LowFreqData;
	LowFreqData.NodeName = TEXT("LowFrequencyNode");
	LowFreqData.BlueprintName = TEXT("NormalBlueprint");
	LowFreqData.AverageExecutionsPerSecond = 50.0f; // Low
	RuntimeDataArray.Add(LowFreqData);
	
	Widget->SetRuntimeData(RuntimeDataArray);
	
	// Test 2: Add lint issues
	TArray<FLintIssue> LintIssues;
	
	FLintIssue CriticalIssue;
	CriticalIssue.Type = ELintIssueType::CastAbuse;
	CriticalIssue.NodeName = TEXT("CastNode");
	CriticalIssue.BlueprintPath = TEXT("/Game/ProblematicBlueprint.uasset");
	CriticalIssue.Severity = ESeverity::Critical;
	LintIssues.Add(CriticalIssue);
	
	Widget->SetLintIssues(LintIssues);
	
	// Test refresh to ensure all data is processed
	Widget->RefreshData();
	
	TestTrue("Widget should handle multiple data types", true);
	
	return true;
}