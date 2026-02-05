#pragma once

#include "CoreMinimal.h"
#include "Data/ProfilerDataTypes.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"

// Forward declarations for blueprint instrumentation
struct FFrame;
struct FBlueprintInstrumentationSignal;

/**
 * Execution frame data for timeline analysis
 */
struct BLUEPRINTPROFILER_API FExecutionFrame
{
	double Timestamp;
	TWeakObjectPtr<UObject> ObjectPtr;
	float ExecutionTime;
	
	FExecutionFrame()
		: Timestamp(0.0)
		, ExecutionTime(0.0f)
	{
	}
};

/**
 * Runtime Profiler - monitors blueprint node execution performance during PIE
 */
class BLUEPRINTPROFILER_API FRuntimeProfiler
{
public:
	FRuntimeProfiler();
	~FRuntimeProfiler();

	// Recording control
	void StartRecording(const FString& SessionName = TEXT(""));
	void StopRecording();
	void ResetData();
	void PauseRecording();
	void ResumeRecording();
	bool IsRecording() const { return CurrentState == ERecordingState::Recording; }
	bool IsPaused() const { return CurrentState == ERecordingState::Paused; }
	ERecordingState GetRecordingState() const { return CurrentState; }

	// Session management
	FRecordingSession GetCurrentSession() const { return CurrentSession; }
	TArray<FRecordingSession> GetSessionHistory() const { return SessionHistory; }
	void SaveSessionData(const FString& FilePath = TEXT(""));
	bool LoadSessionData(const FString& FilePath);
	void ClearSessionHistory();

	// Data access
	TArray<FNodeExecutionData> GetExecutionData() const;
	TArray<FHotNodeInfo> GetHotNodes(float Threshold = 1000.0f) const;
	TArray<FTickAbuseInfo> GetTickAbuseActors() const;

	// Event handling
	void OnScriptInstrumentation(const FFrame& Frame, const FBlueprintInstrumentationSignal& Signal);
	void OnPIEBegin(bool bIsSimulating);
	void OnPIEEnd(bool bIsSimulating);

	// Settings for PIE integration
	void SetAutoStartOnPIE(bool bEnabled) { bAutoStartOnPIE = bEnabled; }
	bool GetAutoStartOnPIE() const { return bAutoStartOnPIE; }
	void SetAutoStopOnPIEEnd(bool bEnabled) { bAutoStopOnPIEEnd = bEnabled; }
	bool GetAutoStopOnPIEEnd() const { return bAutoStopOnPIEEnd; }

private:
	// Recording state
	ERecordingState CurrentState;
	FRecordingSession CurrentSession;
	TArray<FRecordingSession> SessionHistory;
	double RecordingStartTime;
	double PauseStartTime;
	double TotalPausedTime;

	// PIE integration settings
	bool bAutoStartOnPIE;
	bool bAutoStopOnPIEEnd;

	// Execution data
	TMap<TWeakObjectPtr<UObject>, FNodeExecutionStats> NodeStats;
	TArray<FExecutionFrame> ExecutionFrames;
	TArray<FTickAbuseInfo> TickAbuseData;

	// Blueprint instrumentation methods
	void InitializeBlueprintInstrumentation();
	void CleanupBlueprintInstrumentation();
	void EnableBlueprintInstrumentation();
	void DisableBlueprintInstrumentation();
	
	// Session management methods
	void StartNewSession(const FString& SessionName);
	void EndCurrentSession();
	void UpdateSessionStats();
	FString GenerateDefaultSessionName() const;
	FString GetSessionDataFilePath(const FString& SessionName = TEXT("")) const;
	
	// Data recording methods
	void RecordNodeExecution(const FFrame& Frame);
	void CheckForTickAbuse(UObject* Object, const FNodeExecutionStats& Stats);
	bool HasComplexTickLogic(AActor* Actor);
	void RecordTickAbuse(AActor* Actor, const FNodeExecutionStats& Stats);
	
	// Node analysis methods
	FString GetDetailedNodeName(UObject* Object) const;
	FGuid GetNodeGuid(UObject* Object) const;
	
	// Tick complexity analysis methods
	void AnalyzeTickComplexity(AActor* Actor, FTickAbuseInfo& TickAbuse) const;
	int32 AnalyzeBlueprintComplexity(UBlueprint* Blueprint) const;
	int32 AnalyzeGraphComplexity(UEdGraph* Graph) const;
	bool HasTickEvent(UEdGraph* Graph) const;
	bool IsExpensiveFunction(const FString& FunctionName) const;
};