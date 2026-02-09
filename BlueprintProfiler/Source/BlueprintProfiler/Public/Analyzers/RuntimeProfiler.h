#pragma once

#include "CoreMinimal.h"
#include "Data/ProfilerDataTypes.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"

// Forward declarations for blueprint instrumentation
struct FFrame;
struct FScriptInstrumentationSignal;

// Forward declarations for breakpoint system
struct FBlueprintBreakpoint;
struct FBlueprintExceptionInfo;

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
 * This is a singleton to ensure only one instance handles PIE events
 */
class BLUEPRINTPROFILER_API FRuntimeProfiler
{
public:
	// Singleton access
	static FRuntimeProfiler& Get();

	// Prevent copying and moving
	FRuntimeProfiler(const FRuntimeProfiler&) = delete;
	FRuntimeProfiler& operator=(const FRuntimeProfiler&) = delete;
	FRuntimeProfiler(FRuntimeProfiler&&) = delete;
	FRuntimeProfiler& operator=(FRuntimeProfiler&&) = delete;

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
	FString GetSessionDataDirectory() const;

	// Data access
	TArray<FNodeExecutionData> GetExecutionData() const;
	TArray<FHotNodeInfo> GetHotNodes(float Threshold = 1000.0f) const;
	TArray<FTickAbuseInfo> GetTickAbuseActors() const;

	// Event handling
	void OnScriptProfilingEvent(const FScriptInstrumentationSignal& Signal);
	void OnPIEBegin(bool bIsSimulating);
	void OnPIEEnd(bool bIsSimulating);

	// Settings for PIE integration
	void SetAutoStartOnPIE(bool bEnabled) { bAutoStartOnPIE = bEnabled; }
	bool GetAutoStartOnPIE() const { return bAutoStartOnPIE; }
	void SetAutoStopOnPIEEnd(bool bEnabled) { bAutoStopOnPIEEnd = bEnabled; }
	bool GetAutoStopOnPIEEnd() const { return bAutoStopOnPIEEnd; }

	// Async tracepoint setup completion delegate
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnTracepointSetupComplete, bool /* bSuccess */);
	FOnTracepointSetupComplete OnTracepointSetupComplete;

	// Check if async setup is in progress
	bool IsSettingUpTracepoints() const { return bIsSettingUpTracepoints; }

	// Filtering options
	void SetHideEngineInternalNodes(bool bHide) { bHideEngineInternalNodes = bHide; }
	bool GetHideEngineInternalNodes() const { return bHideEngineInternalNodes; }

	// Destructor is public for TUniquePtr cleanup
	~FRuntimeProfiler();

private:
	FRuntimeProfiler();

	// Singleton instance
	static TUniquePtr<FRuntimeProfiler> Instance;
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
	bool bIsInstrumentationEnabled;

	// Filtering options
	bool bHideEngineInternalNodes = true;  // Default to true for better UX

	// Blueprint instrumentation delegate handle
	FDelegateHandle InstrumentationDelegateHandle;

	// Script exception (breakpoint) delegate handle for tracing
	FDelegateHandle ScriptExceptionDelegateHandle;

	// Thread safety for data recording
	mutable FCriticalSection DataMutex;

	// Breakpoint system state management
	// Maps: Blueprint -> Array of (Node -> Original Breakpoint State)
	// We need to save/restore breakpoints so we don't interfere with user's debugging
	struct FOriginalBreakpointInfo
	{
		TWeakObjectPtr<UBlueprint> Blueprint;
		TMap<TWeakObjectPtr<UEdGraphNode>, bool> OriginalBreakpointStates;
	};
	TMap<TWeakObjectPtr<UBlueprint>, FOriginalBreakpointInfo> SavedBreakpointStates;

	// Execution data
	TMap<TWeakObjectPtr<UObject>, FNodeExecutionStats> NodeStats;
	TArray<FExecutionFrame> ExecutionFrames;
	TArray<FTickAbuseInfo> TickAbuseData;
	
	// Loaded session data for display (persisted across PIE sessions)
	TArray<FNodeExecutionData> LoadedSessionData;

	// Timer for periodic blueprint execution collection
	FTimerHandle SamplingTimerHandle;

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
	void CollectBlueprintExecutionData();
	void CheckForTickAbuse(UObject* Object, const FNodeExecutionStats& Stats);
	bool HasComplexTickLogic(AActor* Actor);
	void RecordTickAbuse(AActor* Actor, const FNodeExecutionStats& Stats);
	
	// Node analysis methods
	FString GetDetailedNodeName(UObject* Object) const;
	FGuid GetNodeGuid(UObject* Object) const;

	// Filtering helper methods
	bool IsNodeInStandardMacros(UEdGraphNode* Node) const;
	bool IsEngineInternalBlueprint(const FString& BlueprintPath) const;
	
	// Tick complexity analysis methods
	void AnalyzeTickComplexity(AActor* Actor, FTickAbuseInfo& TickAbuse) const;
	int32 AnalyzeBlueprintComplexity(UBlueprint* Blueprint) const;
	int32 AnalyzeGraphComplexity(UEdGraph* Graph) const;
	bool HasTickEvent(UEdGraph* Graph) const;
	bool IsExpensiveFunction(const FString& FunctionName) const;

	// Breakpoint tracing methods
	void SetupBlueprintTracepoints(UBlueprint* Blueprint);
	void RemoveBlueprintTracepoints(UBlueprint* Blueprint);
	void SetupTracepointsForAllBlueprints();
	void SetupTracepointsForAllBlueprintsAsync();  // Async version
	void RemoveTracepointsFromAllBlueprints();
	void OnScriptExceptionTrace(const UObject* ActiveObject, const FFrame& StackFrame, const FBlueprintExceptionInfo& Info);
	bool IsTracepointActive() const { return bTracepointsActive; }
	void ProcessNextTracepointBatch();  // Called by timer for batch processing

private:
	// Tracepoint state
	bool bTracepointsActive = false;
	bool bSkipRecording = false;  // Flag to skip recording to avoid recursive events
	bool bIsSettingUpTracepoints = false;  // Async setup in progress
	uint64 TotalEventsProcessed = 0;  // For debugging
	uint64 LastLoggingTime = 0;      // Rate limit logging

	// Async tracepoint setup state
	TArray<FAssetData> PendingBlueprints;  // Blueprints waiting for tracepoint setup
	int32 CurrentBlueprintIndex = 0;       // Current processing index
	FTimerHandle TracepointSetupTimerHandle;  // Timer for batch processing
};