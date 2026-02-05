#include "Analyzers/RuntimeProfiler.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Editor.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MacroInstance.h"
#include "K2Node.h"
#include "Engine/Engine.h"
#include "HAL/PlatformFilemanager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "Stats/Stats2.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "HAL/IConsoleManager.h"
#include "Stats/StatsData.h"
#include "TimerManager.h"
#include "UObject/Script.h"
#include "Blueprint/BlueprintExceptionInfo.h"
#include "Kismet2/KismetDebugUtilities.h"
#include "Kismet2/Breakpoint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/Async.h"

// Stats for blueprint profiling
DECLARE_STATS_GROUP(TEXT("BlueprintProfiler"), STATGROUP_BlueprintProfiler, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("Blueprint Function Execution"), STAT_BlueprintFunctionExecution, STATGROUP_BlueprintProfiler);

FRuntimeProfiler::FRuntimeProfiler()
	: CurrentState(ERecordingState::Stopped)
	, RecordingStartTime(0.0)
	, PauseStartTime(0.0)
	, TotalPausedTime(0.0)
	, bAutoStartOnPIE(false)
	, bAutoStopOnPIEEnd(true)
	, bIsInstrumentationEnabled(false)
	, bTracepointsActive(false)
	, bSkipRecording(false)
	, TotalEventsProcessed(0)
	, LastLoggingTime(0.0)
{
	// Bind to PIE events
	FEditorDelegates::BeginPIE.AddRaw(this, &FRuntimeProfiler::OnPIEBegin);
	FEditorDelegates::EndPIE.AddRaw(this, &FRuntimeProfiler::OnPIEEnd);

	// Initialize blueprint instrumentation delegate
	InitializeBlueprintInstrumentation();
}

FRuntimeProfiler::~FRuntimeProfiler()
{
	// Unbind from PIE events
	FEditorDelegates::BeginPIE.RemoveAll(this);
	FEditorDelegates::EndPIE.RemoveAll(this);
	
	// Stop recording if still active
	if (CurrentState == ERecordingState::Recording)
	{
		StopRecording();
	}
	
	// Clean up blueprint instrumentation
	CleanupBlueprintInstrumentation();
}

void FRuntimeProfiler::StartRecording(const FString& SessionName)
{
	if (CurrentState == ERecordingState::Recording)
	{
		return;
	}

	// End current session if it exists
	if (CurrentState != ERecordingState::Stopped)
	{
		EndCurrentSession();
	}

	// Start new session
	StartNewSession(SessionName);

	CurrentState = ERecordingState::Recording;
	RecordingStartTime = FPlatformTime::Seconds();
	TotalPausedTime = 0.0;
	NodeStats.Empty();
	ExecutionFrames.Empty();
	TickAbuseData.Empty();
	TotalEventsProcessed = 0;
	LastLoggingTime = 0.0;

	// 绑定到断点触发委托
	ScriptExceptionDelegateHandle = FBlueprintCoreDelegates::OnScriptException.AddRaw(this, &FRuntimeProfiler::OnScriptExceptionTrace);

	// 异步设置蓝图追踪点（避免阻塞UI）
	SetupTracepointsForAllBlueprintsAsync();

	UE_LOG(LogTemp, Log, TEXT("Runtime profiler recording started - Session: %s"), *CurrentSession.SessionName);
}

void FRuntimeProfiler::StopRecording()
{
	if (CurrentState == ERecordingState::Stopped)
	{
		return;
	}

	CurrentState = ERecordingState::Stopped;

	// 标记为不活动，防止回调中继续记录
	bTracepointsActive = false;

	// 解绑断点触发委托
	if (ScriptExceptionDelegateHandle.IsValid())
	{
		FBlueprintCoreDelegates::OnScriptException.Remove(ScriptExceptionDelegateHandle);
		ScriptExceptionDelegateHandle.Reset();
		UE_LOG(LogTemp, Log, TEXT("[PROFILER] Unbound from OnScriptException delegate"));
	}

	// 移除所有追踪点并恢复原始断点状态
	RemoveTracepointsFromAllBlueprints();

	// End current session and save to history
	EndCurrentSession();

	UE_LOG(LogTemp, Log, TEXT("Runtime profiler recording stopped - Session: %s, Total events processed: %llu"),
		*CurrentSession.SessionName, TotalEventsProcessed);
}

void FRuntimeProfiler::PauseRecording()
{
	if (CurrentState != ERecordingState::Recording)
	{
		return;
	}

	CurrentState = ERecordingState::Paused;
	PauseStartTime = FPlatformTime::Seconds();

	// 暂停时禁用追踪点回调（避免不必要的开销）
	bSkipRecording = true;

	UE_LOG(LogTemp, Log, TEXT("Runtime profiler recording paused"));
}

void FRuntimeProfiler::ResumeRecording()
{
	if (CurrentState != ERecordingState::Paused)
	{
		return;
	}

	CurrentState = ERecordingState::Recording;

	// Add paused time to total
	TotalPausedTime += FPlatformTime::Seconds() - PauseStartTime;
	PauseStartTime = 0.0;

	// 恢复时启用追踪点回调
	bSkipRecording = false;

	UE_LOG(LogTemp, Log, TEXT("Runtime profiler recording resumed"));
}

void FRuntimeProfiler::ResetData()
{
	if (CurrentState == ERecordingState::Recording)
	{
		StopRecording();
	}
	
	CurrentState = ERecordingState::Stopped;
	NodeStats.Empty();
	ExecutionFrames.Empty();
	TickAbuseData.Empty();
	RecordingStartTime = 0.0;
	TotalPausedTime = 0.0;
	
	// Reset current session
	CurrentSession = FRecordingSession();
	
	UE_LOG(LogTemp, Log, TEXT("Runtime profiler data reset"));
}

TArray<FNodeExecutionData> FRuntimeProfiler::GetExecutionData() const
{
	TArray<FNodeExecutionData> Result;
	
	float RecordingDuration = 0.0f;
	if (CurrentState == ERecordingState::Recording)
	{
		RecordingDuration = (FPlatformTime::Seconds() - RecordingStartTime) - TotalPausedTime;
	}
	else if (CurrentState == ERecordingState::Paused)
	{
		RecordingDuration = (PauseStartTime - RecordingStartTime) - TotalPausedTime;
	}
	else if (ExecutionFrames.Num() > 0)
	{
		RecordingDuration = CurrentSession.Duration;
	}
	
	for (const auto& StatsPair : NodeStats)
	{
		if (!StatsPair.Key.IsValid())
		{
			continue;
		}
		
		FNodeExecutionData Data;
		Data.BlueprintObject = StatsPair.Key;
		Data.TotalExecutions = StatsPair.Value.ExecutionCount;
		Data.TotalExecutionTime = StatsPair.Value.TotalExecutionTime;
		Data.AverageExecutionTime = StatsPair.Value.GetAverageExecutionTime();
		Data.AverageExecutionsPerSecond = StatsPair.Value.GetExecutionsPerSecond(RecordingDuration);
		
		// Try to get blueprint and node names
		if (UObject* Object = StatsPair.Key.Get())
		{
			// Case 1: Object is a UEdGraphNode (most common case)
			if (UEdGraphNode* Node = Cast<UEdGraphNode>(Object))
			{
				Data.NodeName = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
				Data.NodeGuid = Node->NodeGuid;

				// Get blueprint from node's graph
				if (Node->GetGraph())
				{
					if (UBlueprint* Blueprint = Cast<UBlueprint>(Node->GetGraph()->GetOuter()))
					{
						Data.BlueprintName = Blueprint->GetName();
					}
					else
					{
						// Try to get blueprint from class
						if (UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(Node->GetClass()))
						{
							if (UBlueprint* GeneratedBy = Cast<UBlueprint>(BPClass->ClassGeneratedBy))
							{
								Data.BlueprintName = GeneratedBy->GetName();
							}
						}
					}
				}
			}
			// Case 2: Object is a UBlueprint
			else if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
			{
				Data.BlueprintName = Blueprint->GetName();
				Data.NodeName = TEXT("Blueprint");
			}
			// Case 3: Object has a BlueprintGeneratedClass
			else if (UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(Object->GetClass()))
			{
				if (UBlueprint* GeneratedByBlueprint = Cast<UBlueprint>(BPClass->ClassGeneratedBy))
				{
					Data.BlueprintName = GeneratedByBlueprint->GetName();
					Data.NodeName = Object->GetName();
				}
			}
			// Fallback: Unknown
			else
			{
				Data.BlueprintName = TEXT("Unknown Blueprint");
				Data.NodeName = Object->GetName();
			}
		}
		
		Result.Add(Data);
	}
	
	return Result;
}

// Session management methods
void FRuntimeProfiler::StartNewSession(const FString& SessionName)
{
	CurrentSession = FRecordingSession();
	CurrentSession.SessionName = SessionName.IsEmpty() ? GenerateDefaultSessionName() : SessionName;
	CurrentSession.StartTime = FDateTime::Now();
	CurrentSession.bIsActive = true;
	CurrentSession.bAutoStarted = false; // Will be set by PIE integration if needed
	
	UE_LOG(LogTemp, Log, TEXT("Started new recording session: %s"), *CurrentSession.SessionName);
}

void FRuntimeProfiler::EndCurrentSession()
{
	if (!CurrentSession.bIsActive)
	{
		return;
	}
	
	CurrentSession.EndTime = FDateTime::Now();
	CurrentSession.bIsActive = false;
	
	// Calculate session statistics
	UpdateSessionStats();
	
	// Add to session history
	SessionHistory.Add(CurrentSession);
	
	// Keep only last 50 sessions to prevent memory bloat
	if (SessionHistory.Num() > 50)
	{
		SessionHistory.RemoveAt(0, SessionHistory.Num() - 50);
	}
	
	UE_LOG(LogTemp, Log, TEXT("Ended recording session: %s (Duration: %.2fs, Nodes: %d, Executions: %d)"), 
		*CurrentSession.SessionName, CurrentSession.Duration, CurrentSession.TotalNodesRecorded, CurrentSession.TotalExecutions);
}

void FRuntimeProfiler::UpdateSessionStats()
{
	if (CurrentSession.StartTime != FDateTime::MinValue() && CurrentSession.EndTime != FDateTime::MinValue())
	{
		FTimespan Duration = CurrentSession.EndTime - CurrentSession.StartTime;
		CurrentSession.Duration = Duration.GetTotalSeconds() - TotalPausedTime;
	}
	
	CurrentSession.TotalNodesRecorded = NodeStats.Num();
	CurrentSession.TotalExecutions = 0;
	
	for (const auto& StatsPair : NodeStats)
	{
		CurrentSession.TotalExecutions += StatsPair.Value.ExecutionCount;
	}
}

FString FRuntimeProfiler::GenerateDefaultSessionName() const
{
	FDateTime Now = FDateTime::Now();
	return FString::Printf(TEXT("Session_%s"), *Now.ToString(TEXT("%Y%m%d_%H%M%S")));
}

void FRuntimeProfiler::SaveSessionData(const FString& FilePath)
{
	FString SavePath = FilePath.IsEmpty() ? GetSessionDataFilePath() : FilePath;
	
	// Create JSON object with session data
	TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
	
	// Add current session info
	TSharedPtr<FJsonObject> SessionJson = MakeShareable(new FJsonObject);
	SessionJson->SetStringField(TEXT("SessionName"), CurrentSession.SessionName);
	SessionJson->SetStringField(TEXT("StartTime"), CurrentSession.StartTime.ToString());
	SessionJson->SetStringField(TEXT("EndTime"), CurrentSession.EndTime.ToString());
	SessionJson->SetNumberField(TEXT("Duration"), CurrentSession.Duration);
	SessionJson->SetNumberField(TEXT("TotalNodesRecorded"), CurrentSession.TotalNodesRecorded);
	SessionJson->SetNumberField(TEXT("TotalExecutions"), CurrentSession.TotalExecutions);
	SessionJson->SetBoolField(TEXT("bAutoStarted"), CurrentSession.bAutoStarted);
	
	JsonObject->SetObjectField(TEXT("Session"), SessionJson);
	
	// Add execution data
	TArray<TSharedPtr<FJsonValue>> ExecutionDataArray;
	TArray<FNodeExecutionData> ExecutionData = GetExecutionData();
	
	for (const FNodeExecutionData& Data : ExecutionData)
	{
		TSharedPtr<FJsonObject> DataJson = MakeShareable(new FJsonObject);
		DataJson->SetStringField(TEXT("NodeName"), Data.NodeName);
		DataJson->SetStringField(TEXT("BlueprintName"), Data.BlueprintName);
		DataJson->SetNumberField(TEXT("TotalExecutions"), Data.TotalExecutions);
		DataJson->SetNumberField(TEXT("AverageExecutionsPerSecond"), Data.AverageExecutionsPerSecond);
		DataJson->SetNumberField(TEXT("TotalExecutionTime"), Data.TotalExecutionTime);
		DataJson->SetNumberField(TEXT("AverageExecutionTime"), Data.AverageExecutionTime);
		
		ExecutionDataArray.Add(MakeShareable(new FJsonValueObject(DataJson)));
	}
	
	JsonObject->SetArrayField(TEXT("ExecutionData"), ExecutionDataArray);
	
	// Save to file
	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
	
	if (FFileHelper::SaveStringToFile(OutputString, *SavePath))
	{
		UE_LOG(LogTemp, Log, TEXT("Session data saved to: %s"), *SavePath);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to save session data to: %s"), *SavePath);
	}
}

bool FRuntimeProfiler::LoadSessionData(const FString& FilePath)
{
	FString LoadPath = FilePath.IsEmpty() ? GetSessionDataFilePath() : FilePath;
	
	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *LoadPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("Failed to load session data from: %s"), *LoadPath);
		return false;
	}
	
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
	
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to parse session data JSON from: %s"), *LoadPath);
		return false;
	}
	
	// Load session info
	TSharedPtr<FJsonObject> SessionJson = JsonObject->GetObjectField(TEXT("Session"));
	if (SessionJson.IsValid())
	{
		FRecordingSession LoadedSession;
		LoadedSession.SessionName = SessionJson->GetStringField(TEXT("SessionName"));
		FDateTime::Parse(SessionJson->GetStringField(TEXT("StartTime")), LoadedSession.StartTime);
		FDateTime::Parse(SessionJson->GetStringField(TEXT("EndTime")), LoadedSession.EndTime);
		LoadedSession.Duration = SessionJson->GetNumberField(TEXT("Duration"));
		LoadedSession.TotalNodesRecorded = SessionJson->GetIntegerField(TEXT("TotalNodesRecorded"));
		LoadedSession.TotalExecutions = SessionJson->GetIntegerField(TEXT("TotalExecutions"));
		LoadedSession.bAutoStarted = SessionJson->GetBoolField(TEXT("bAutoStarted"));
		LoadedSession.bIsActive = false;
		
		// Add to session history if not already present
		bool bAlreadyExists = false;
		for (const FRecordingSession& ExistingSession : SessionHistory)
		{
			if (ExistingSession.SessionName == LoadedSession.SessionName && 
				ExistingSession.StartTime == LoadedSession.StartTime)
			{
				bAlreadyExists = true;
				break;
			}
		}
		
		if (!bAlreadyExists)
		{
			SessionHistory.Add(LoadedSession);
		}
	}
	
	UE_LOG(LogTemp, Log, TEXT("Session data loaded from: %s"), *LoadPath);
	return true;
}

void FRuntimeProfiler::ClearSessionHistory()
{
	SessionHistory.Empty();
	UE_LOG(LogTemp, Log, TEXT("Session history cleared"));
}

FString FRuntimeProfiler::GetSessionDataFilePath(const FString& SessionName) const
{
	FString FileName = SessionName.IsEmpty() ? CurrentSession.SessionName : SessionName;
	FileName = FileName.Replace(TEXT(" "), TEXT("_"));
	FileName = FileName.Replace(TEXT(":"), TEXT("-"));
	
	FString SaveDir = FPaths::ProjectSavedDir() / TEXT("BlueprintProfiler") / TEXT("Sessions");
	FString FilePath = SaveDir / (FileName + TEXT(".json"));
	
	// Ensure directory exists
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*SaveDir))
	{
		PlatformFile.CreateDirectoryTree(*SaveDir);
	}
	
	return FilePath;
}

TArray<FHotNodeInfo> FRuntimeProfiler::GetHotNodes(float Threshold) const
{
	TArray<FHotNodeInfo> Result;
	
	float RecordingDuration = IsRecording() ? 
		(FPlatformTime::Seconds() - RecordingStartTime) : 
		(ExecutionFrames.Num() > 0 ? ExecutionFrames.Last().Timestamp - RecordingStartTime : 0.0f);
	
	for (const auto& StatsPair : NodeStats)
	{
		if (!StatsPair.Key.IsValid())
		{
			continue;
		}
		
		float ExecutionsPerSecond = StatsPair.Value.GetExecutionsPerSecond(RecordingDuration);
		if (ExecutionsPerSecond >= Threshold)
		{
			FHotNodeInfo HotNode;
			HotNode.BlueprintObject = StatsPair.Key;
			HotNode.ExecutionsPerSecond = ExecutionsPerSecond;
			HotNode.AverageExecutionTime = StatsPair.Value.GetAverageExecutionTime();
			
			// Determine severity based on execution frequency and time
			float PerformanceImpact = ExecutionsPerSecond * HotNode.AverageExecutionTime;
			
			if (ExecutionsPerSecond > 5000.0f || PerformanceImpact > 10.0f)
			{
				HotNode.Severity = ESeverity::Critical;
			}
			else if (ExecutionsPerSecond > 3000.0f || PerformanceImpact > 5.0f)
			{
				HotNode.Severity = ESeverity::High;
			}
			else if (ExecutionsPerSecond > 2000.0f || PerformanceImpact > 2.0f)
			{
				HotNode.Severity = ESeverity::Medium;
			}
			else
			{
				HotNode.Severity = ESeverity::Low;
			}
			
			// Try to get detailed node information
			if (UObject* Object = StatsPair.Key.Get())
			{
				HotNode.NodeName = GetDetailedNodeName(Object);
				HotNode.NodeGuid = GetNodeGuid(Object);
			}
			
			Result.Add(HotNode);
		}
	}
	
	// Sort by performance impact (executions per second * average execution time)
	Result.Sort([](const FHotNodeInfo& A, const FHotNodeInfo& B)
	{
		float ImpactA = A.ExecutionsPerSecond * A.AverageExecutionTime;
		float ImpactB = B.ExecutionsPerSecond * B.AverageExecutionTime;
		return ImpactA > ImpactB;
	});
	
	return Result;
}

TArray<FTickAbuseInfo> FRuntimeProfiler::GetTickAbuseActors() const
{
	return TickAbuseData;
}

void FRuntimeProfiler::OnPIEBegin(bool bIsSimulating)
{
	// Auto-start recording if configured to do so
	if (bAutoStartOnPIE && CurrentState == ERecordingState::Stopped)
	{
		FString SessionName = FString::Printf(TEXT("PIE_Session_%s"), *FDateTime::Now().ToString(TEXT("%H%M%S")));
		StartRecording(SessionName);
		CurrentSession.bAutoStarted = true;
		
		UE_LOG(LogTemp, Log, TEXT("PIE began, auto-started runtime profiler recording: %s"), *SessionName);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("PIE began, runtime profiler ready (auto-start: %s)"), 
			bAutoStartOnPIE ? TEXT("enabled") : TEXT("disabled"));
	}
}

void FRuntimeProfiler::OnPIEEnd(bool bIsSimulating)
{
	// Auto-stop recording when PIE ends if configured to do so
	if (bAutoStopOnPIEEnd && CurrentState == ERecordingState::Recording)
	{
		StopRecording();
		UE_LOG(LogTemp, Log, TEXT("PIE ended, auto-stopped runtime profiler recording"));
	}
	else if (CurrentState == ERecordingState::Recording)
	{
		UE_LOG(LogTemp, Log, TEXT("PIE ended, runtime profiler still recording (auto-stop disabled)"));
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("PIE ended, runtime profiler was not recording"));
	}
}

void FRuntimeProfiler::InitializeBlueprintInstrumentation()
{
	// Initialize blueprint core delegates for instrumentation
	// Note: This is a simplified implementation as FBlueprintCoreDelegates may not be available in all UE versions
	UE_LOG(LogTemp, Log, TEXT("Blueprint instrumentation initialized"));
}

void FRuntimeProfiler::CleanupBlueprintInstrumentation()
{
	// Clean up any blueprint instrumentation hooks
	if (bIsInstrumentationEnabled)
	{
		// 取消委托绑定
		if (InstrumentationDelegateHandle.IsValid())
		{
			FBlueprintCoreDelegates::OnScriptProfilingEvent.Remove(InstrumentationDelegateHandle);
			InstrumentationDelegateHandle.Reset();
			UE_LOG(LogTemp, Log, TEXT("Unbound from OnScriptProfilingEvent during cleanup"));
		}

		bIsInstrumentationEnabled = false;
	}

	UE_LOG(LogTemp, Log, TEXT("Blueprint instrumentation cleaned up"));
}

void FRuntimeProfiler::EnableBlueprintInstrumentation()
{
	// Enable blueprint execution monitoring for UE 5.6
	// This is the CORRECT way to enable blueprint profiling

	// 1. Force enable blueprint instrumentation via console variable
	// This corresponds to console command: bp.EnableInstrumentation 1
	static IConsoleVariable* BlueprintInstrumentationCV = IConsoleManager::Get().FindConsoleVariable(TEXT("bp.EnableInstrumentation"));
	if (BlueprintInstrumentationCV)
	{
		BlueprintInstrumentationCV->Set(1, ECVF_SetByCode);
		UE_LOG(LogTemp, Warning, TEXT("Forced bp.EnableInstrumentation = 1 for profiling"));
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("CRITICAL: Could not find bp.EnableInstrumentation console variable! Blueprint profiling will not work."));
	}

	// 2. Bind the core delegate to receive node execution events
	// In UE 5.6, the correct delegate is OnScriptProfilingEvent
	InstrumentationDelegateHandle = FBlueprintCoreDelegates::OnScriptProfilingEvent.AddRaw(this, &FRuntimeProfiler::OnScriptProfilingEvent);
	bIsInstrumentationEnabled = true;
	UE_LOG(LogTemp, Log, TEXT("Blueprint instrumentation enabled - bound to OnScriptProfilingEvent"));
}

void FRuntimeProfiler::OnScriptProfilingEvent(const FScriptInstrumentationSignal& Signal)
{
	// [验证日志] 激进的日志记录来验证回调是否被触发
	// 这将帮助我们确认蓝图仪表化是否真正工作
	static int32 CallCount = 0;
	CallCount++;

	// [性能过滤] 1. 只要"节点进入"事件，忽略退出或其他调试事件
	if (Signal.GetType() != EScriptInstrumentation::NodeEntry &&
		Signal.GetType() != EScriptInstrumentation::PureNodeEntry)
	{
		return;
	}

	// 每 100 次调用记录一次，避免日志爆炸
	if (CallCount % 100 == 1)
	{
		UE_LOG(LogTemp, Error, TEXT("[PROFILER] OnScriptProfilingEvent CALLED! Count: %d, EventType: %d"),
			CallCount, (int32)Signal.GetType());
	}

	// [状态检查] 只在录制状态下记录数据
	if (CurrentState != ERecordingState::Recording)
	{
		return;
	}

	// [对象验证] 检查 ContextObject 是否有效（使用公共方法）
	if (!Signal.IsContextObjectValid())
	{
		return;
	}

	// 由于 FScriptInstrumentationSignal 的 ContextObject 和 Function 是 protected 成员，
	// 我们无法直接访问它们。这里使用一个简化的方法来记录执行次数。
	// 我们使用调用计数器和一个简单的键来跟踪执行。

	// 线程安全地更新执行统计
	FScopeLock Lock(&DataMutex);

	// 使用一个通用的计数器来记录总的蓝图节点执行次数
	static TWeakObjectPtr<UObject> BlueprintExecutionKey(nullptr);
	FNodeExecutionStats& Stats = NodeStats.FindOrAdd(BlueprintExecutionKey);
	Stats.ExecutionCount++;

	// 记录执行时间
	double CurrentTime = FPlatformTime::Seconds();
	float ExecutionTime = 0.001f; // 1ms 基础时间
	Stats.TotalExecutionTime += ExecutionTime;
	Stats.MinExecutionTime = FMath::Min(Stats.MinExecutionTime, ExecutionTime);
	Stats.MaxExecutionTime = FMath::Max(Stats.MaxExecutionTime, ExecutionTime);
	Stats.ExecutionTimes.Add(ExecutionTime);

	// 保持执行时间数组在合理范围内
	if (Stats.ExecutionTimes.Num() > 100)
	{
		Stats.ExecutionTimes.RemoveAt(0);
	}

	// 记录执行帧
	FExecutionFrame Frame;
	Frame.Timestamp = CurrentTime;
	Frame.ObjectPtr = BlueprintExecutionKey;
	Frame.ExecutionTime = ExecutionTime;
	ExecutionFrames.Add(Frame);

	// 保持执行帧数组在合理范围内
	if (ExecutionFrames.Num() > 5000)
	{
		ExecutionFrames.RemoveAt(0, 100);
	}

	// 定期记录数据收集进度
	if (CallCount % 1000 == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[PROFILER] Data Collection: Total executions: %d"),
			CallCount);
	}
}

void FRuntimeProfiler::DisableBlueprintInstrumentation()
{
	// Disable blueprint execution monitoring for UE 5.6
	if (bIsInstrumentationEnabled)
	{
		bIsInstrumentationEnabled = false;

		// 取消委托绑定
		if (InstrumentationDelegateHandle.IsValid())
		{
			FBlueprintCoreDelegates::OnScriptProfilingEvent.Remove(InstrumentationDelegateHandle);
			InstrumentationDelegateHandle.Reset();
			UE_LOG(LogTemp, Log, TEXT("Unbound from OnScriptProfilingEvent"));
		}

		// Clear the sampling timer
		if (UWorld* World = GEngine ? GEngine->GetWorld() : nullptr)
		{
			World->GetTimerManager().ClearTimer(SamplingTimerHandle);
		}

		UE_LOG(LogTemp, Log, TEXT("Blueprint instrumentation disabled"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Blueprint instrumentation was not enabled"));
	}
}

void FRuntimeProfiler::RecordNodeExecution(const FFrame& Frame)
{
	if (!Frame.Object || CurrentState != ERecordingState::Recording)
	{
		return;
	}

	// Get the current time for execution timing
	double CurrentTime = FPlatformTime::Seconds();

	// Create a weak pointer to the object to avoid holding strong references
	TWeakObjectPtr<UObject> ObjectPtr(Frame.Object);

	// Find or create execution stats for this object
	FNodeExecutionStats& Stats = NodeStats.FindOrAdd(ObjectPtr);
	Stats.ExecutionCount++;

	// Calculate execution time (simplified - in real implementation this would be more sophisticated)
	float ExecutionTime = FMath::RandRange(0.0001f, 0.01f); // Mock execution time for now
	Stats.TotalExecutionTime += ExecutionTime;
	Stats.MinExecutionTime = FMath::Min(Stats.MinExecutionTime, ExecutionTime);
	Stats.MaxExecutionTime = FMath::Max(Stats.MaxExecutionTime, ExecutionTime);
	Stats.ExecutionTimes.Add(ExecutionTime);

	// Keep execution times array manageable
	if (Stats.ExecutionTimes.Num() > 1000)
	{
		Stats.ExecutionTimes.RemoveAt(0, 100);
	}

	// Record execution frame for timeline analysis
	FExecutionFrame ExecutionFrame;
	ExecutionFrame.Timestamp = CurrentTime;
	ExecutionFrame.ObjectPtr = ObjectPtr;
	ExecutionFrame.ExecutionTime = ExecutionTime;
	ExecutionFrames.Add(ExecutionFrame);

	// Keep execution frames manageable
	if (ExecutionFrames.Num() > 10000)
	{
		ExecutionFrames.RemoveAt(0, 1000);
	}

	// Check for potential tick abuse
	CheckForTickAbuse(Frame.Object, Stats);
}

void FRuntimeProfiler::CollectBlueprintExecutionData()
{
	// Collect blueprint execution data through sampling
	// This is called periodically by the timer

	if (CurrentState != ERecordingState::Recording)
	{
		return;
	}

	// Get the PIE world
	UWorld* World = nullptr;
	if (GEngine)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.World() && Context.World()->IsPlayInEditor())
			{
				World = Context.World();
				break;
			}
		}
	}

	if (!World)
	{
		// No PIE world, try to get any world
		World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
	}

	if (!World)
	{
		return;
	}

	double CurrentTime = FPlatformTime::Seconds();

	// Iterate through all actors in the world using TObjectIterator
	for (TObjectIterator<AActor> ActorIt; ActorIt; ++ActorIt)
	{
		AActor* Actor = *ActorIt;
		if (!Actor || Actor->GetWorld() != World)
		{
			continue;
		}

		// Check if this actor has a blueprint generated class
		if (UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(Actor->GetClass()))
		{
			// Create a weak pointer to track this blueprint
			TWeakObjectPtr<UObject> ObjectPtr(Actor);

			// Find or create execution stats
			FNodeExecutionStats& Stats = NodeStats.FindOrAdd(ObjectPtr);

			// Increment execution count (we sample, not track every execution)
			Stats.ExecutionCount++;

			// Estimate execution time based on blueprint complexity
			float EstimatedTime = 0.001f; // Base 1ms per tick

			// Add complexity penalty
			if (Actor->GetComponents().Num() > 10)
			{
				EstimatedTime += 0.0005f * (Actor->GetComponents().Num() - 10);
			}

			Stats.TotalExecutionTime += EstimatedTime;
			Stats.MinExecutionTime = FMath::Min(Stats.MinExecutionTime, EstimatedTime);
			Stats.MaxExecutionTime = FMath::Max(Stats.MaxExecutionTime, EstimatedTime);
			Stats.ExecutionTimes.Add(EstimatedTime);

			// Keep execution times array manageable
			if (Stats.ExecutionTimes.Num() > 100)
			{
				Stats.ExecutionTimes.RemoveAt(0);
			}

			// Record execution frame
			FExecutionFrame ExecutionFrame;
			ExecutionFrame.Timestamp = CurrentTime;
			ExecutionFrame.ObjectPtr = ObjectPtr;
			ExecutionFrame.ExecutionTime = EstimatedTime;
			ExecutionFrames.Add(ExecutionFrame);

			// Keep execution frames manageable
			if (ExecutionFrames.Num() > 5000)
			{
				ExecutionFrames.RemoveAt(0, 100);
			}

			// Check for tick abuse
			CheckForTickAbuse(Actor, Stats);
		}
	}

	// Also check blueprint components
	for (TObjectIterator<UActorComponent> CompIt; CompIt; ++CompIt)
	{
		UActorComponent* Component = *CompIt;
		if (!Component || !Component->IsRegistered() || !Component->GetOwner())
		{
			continue;
		}

		// Only check components in the current world
		if (Component->GetWorld() != World)
		{
			continue;
		}

		// Only check components that are blueprint-based
		if (UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(Component->GetClass()))
		{
			TWeakObjectPtr<UObject> ObjectPtr(Component);
			FNodeExecutionStats& Stats = NodeStats.FindOrAdd(ObjectPtr);
			Stats.ExecutionCount++;

			float EstimatedTime = 0.0005f; // Components are generally faster
			Stats.TotalExecutionTime += EstimatedTime;
			Stats.ExecutionTimes.Add(EstimatedTime);

			if (Stats.ExecutionTimes.Num() > 50)
			{
				Stats.ExecutionTimes.RemoveAt(0);
			}
		}
	}

	// Log collection status periodically
	static int32 CollectionCount = 0;
	CollectionCount++;
	if (CollectionCount % 10 == 0) // Log every 1 second (10 * 0.1s)
	{
		UE_LOG(LogTemp, VeryVerbose, TEXT("Blueprint profiler sampled %d actors and %d components"),
			NodeStats.Num(), ExecutionFrames.Num());
	}
}

void FRuntimeProfiler::CheckForTickAbuse(UObject* Object, const FNodeExecutionStats& Stats)
{
	if (!Object)
	{
		return;
	}
	
	// Check if this is an actor with potential tick abuse
	if (AActor* Actor = Cast<AActor>(Object))
	{
		// Calculate executions per second
		float RecordingDuration = FPlatformTime::Seconds() - RecordingStartTime;
		float ExecutionsPerSecond = Stats.GetExecutionsPerSecond(RecordingDuration);
		
		// If executing very frequently, it might be tick abuse
		if (ExecutionsPerSecond > 60.0f) // More than 60 times per second suggests tick usage
		{
			// Check if this actor has complex tick logic
			if (HasComplexTickLogic(Actor))
			{
				// Record as potential tick abuse
				RecordTickAbuse(Actor, Stats);
			}
		}
	}
}

bool FRuntimeProfiler::HasComplexTickLogic(AActor* Actor)
{
	if (!Actor)
	{
		return false;
	}
	
	// Check if the actor's blueprint has complex tick logic
	if (UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(Actor->GetClass()))
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(BPClass->ClassGeneratedBy))
		{
			// Simplified complexity check - in real implementation this would analyze the blueprint graph
			// For now, we'll use a heuristic based on the blueprint's size and complexity
			return Blueprint->FunctionGraphs.Num() > 5 || Blueprint->MacroGraphs.Num() > 3;
		}
	}
	
	return false;
}

void FRuntimeProfiler::RecordTickAbuse(AActor* Actor, const FNodeExecutionStats& Stats)
{
	if (!Actor)
	{
		return;
	}
	
	// Create tick abuse info
	FTickAbuseInfo TickAbuse;
	TickAbuse.BlueprintObject = TWeakObjectPtr<UObject>(Actor);
	TickAbuse.ActorName = Actor->GetName();
	
	// Get blueprint name
	if (UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(Actor->GetClass()))
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(BPClass->ClassGeneratedBy))
		{
			TickAbuse.BlueprintName = Blueprint->GetName();
		}
	}
	
	// Analyze tick complexity in detail
	AnalyzeTickComplexity(Actor, TickAbuse);
	
	// Determine severity based on complexity score and execution frequency
	float RecordingDuration = FPlatformTime::Seconds() - RecordingStartTime;
	float ExecutionsPerSecond = Stats.GetExecutionsPerSecond(RecordingDuration);
	float PerformanceImpact = ExecutionsPerSecond * Stats.GetAverageExecutionTime() * TickAbuse.ComplexityScore;
	
	if (PerformanceImpact > 1000.0f || TickAbuse.ComplexityScore > 100)
	{
		TickAbuse.Severity = ESeverity::Critical;
	}
	else if (PerformanceImpact > 500.0f || TickAbuse.ComplexityScore > 75)
	{
		TickAbuse.Severity = ESeverity::High;
	}
	else if (PerformanceImpact > 200.0f || TickAbuse.ComplexityScore > 50)
	{
		TickAbuse.Severity = ESeverity::Medium;
	}
	else
	{
		TickAbuse.Severity = ESeverity::Low;
	}
	
	// Store the tick abuse info (avoid duplicates)
	bool bFound = false;
	for (FTickAbuseInfo& ExistingAbuse : TickAbuseData)
	{
		if (ExistingAbuse.BlueprintObject == TickAbuse.BlueprintObject)
		{
			// Update existing entry with latest data
			ExistingAbuse = TickAbuse;
			bFound = true;
			break;
		}
	}
	
	if (!bFound)
	{
		TickAbuseData.Add(TickAbuse);
	}
}

FString FRuntimeProfiler::GetDetailedNodeName(UObject* Object) const
{
	if (!Object)
	{
		return TEXT("Unknown");
	}
	
	// Try to get more detailed information about the object
	FString DetailedName = Object->GetName();
	
	// If it's an actor, include the actor class name
	if (AActor* Actor = Cast<AActor>(Object))
	{
		DetailedName = FString::Printf(TEXT("%s (%s)"), *Actor->GetName(), *Actor->GetClass()->GetName());
	}
	// If it's a component, include the component type
	else if (UActorComponent* Component = Cast<UActorComponent>(Object))
	{
		DetailedName = FString::Printf(TEXT("%s (%s)"), *Component->GetName(), *Component->GetClass()->GetName());
	}
	// If it's from a blueprint, try to get the blueprint name
	else if (UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(Object->GetClass()))
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(BPClass->ClassGeneratedBy))
		{
			DetailedName = FString::Printf(TEXT("%s [%s]"), *Object->GetName(), *Blueprint->GetName());
		}
	}
	
	return DetailedName;
}

FGuid FRuntimeProfiler::GetNodeGuid(UObject* Object) const
{
	// In a real implementation, this would extract the actual node GUID from the blueprint
	// For now, we'll generate a deterministic GUID based on the object
	if (!Object)
	{
		return FGuid();
	}
	
	// Create a deterministic GUID based on object name and class
	FString ObjectIdentifier = FString::Printf(TEXT("%s_%s"), *Object->GetName(), *Object->GetClass()->GetName());
	uint32 Hash = GetTypeHash(ObjectIdentifier);
	
	// Convert hash to GUID (simplified approach)
	FGuid NodeGuid;
	NodeGuid.A = Hash;
	NodeGuid.B = Hash >> 16;
	NodeGuid.C = Hash >> 8;
	NodeGuid.D = Hash >> 24;
	
	return NodeGuid;
}

void FRuntimeProfiler::AnalyzeTickComplexity(AActor* Actor, FTickAbuseInfo& TickAbuse) const
{
	if (!Actor)
	{
		return;
	}
	
	int32 ComplexityScore = 0;
	
	// Check if the actor's blueprint has complex tick logic
	if (UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(Actor->GetClass()))
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(BPClass->ClassGeneratedBy))
		{
			// Analyze blueprint complexity
			ComplexityScore += AnalyzeBlueprintComplexity(Blueprint);
		}
	}
	
	// Check component complexity
	for (UActorComponent* Component : Actor->GetComponents().Array())
	{
		if (Component && Component->PrimaryComponentTick.bCanEverTick)
		{
			ComplexityScore += 10; // Each ticking component adds complexity
			
			// Check if component is from blueprint
			if (UBlueprintGeneratedClass* ComponentBPClass = Cast<UBlueprintGeneratedClass>(Component->GetClass()))
			{
				if (UBlueprint* ComponentBlueprint = Cast<UBlueprint>(ComponentBPClass->ClassGeneratedBy))
				{
					ComplexityScore += AnalyzeBlueprintComplexity(ComponentBlueprint) / 2; // Component complexity is weighted less
				}
			}
		}
	}
	
	TickAbuse.ComplexityScore = ComplexityScore;
}

int32 FRuntimeProfiler::AnalyzeBlueprintComplexity(UBlueprint* Blueprint) const
{
	if (!Blueprint)
	{
		return 0;
	}
	
	int32 ComplexityScore = 0;
	
	// Analyze function graphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph)
		{
			ComplexityScore += AnalyzeGraphComplexity(Graph);
		}
	}
	
	// Analyze macro graphs (they can be called from tick)
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (Graph)
		{
			ComplexityScore += AnalyzeGraphComplexity(Graph) / 2; // Macros are weighted less
		}
	}
	
	// Check for event graphs (including tick events)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph)
		{
			ComplexityScore += AnalyzeGraphComplexity(Graph);
			
			// Extra penalty for tick events
			if (HasTickEvent(Graph))
			{
				ComplexityScore += 20;
			}
		}
	}
	
	return ComplexityScore;
}

int32 FRuntimeProfiler::AnalyzeGraphComplexity(UEdGraph* Graph) const
{
	if (!Graph)
	{
		return 0;
	}
	
	int32 ComplexityScore = 0;
	
	// Count nodes and analyze their types
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}
		
		ComplexityScore += 1; // Base complexity for each node
		
		// Higher complexity for certain node types
		if (UK2Node_CallFunction* FunctionCall = Cast<UK2Node_CallFunction>(Node))
		{
			ComplexityScore += 2; // Function calls add complexity
			
			// Check for expensive function calls
			if (FunctionCall->GetTargetFunction())
			{
				FString FunctionName = FunctionCall->GetTargetFunction()->GetName();
				if (IsExpensiveFunction(FunctionName))
				{
					ComplexityScore += 10;
				}
			}
		}
		else if (Cast<UK2Node_Event>(Node))
		{
			ComplexityScore += 3; // Events add complexity
		}
		else if (Cast<UK2Node_CustomEvent>(Node))
		{
			ComplexityScore += 2; // Custom events add complexity
		}
	}
	
	return ComplexityScore;
}

bool FRuntimeProfiler::HasTickEvent(UEdGraph* Graph) const
{
	if (!Graph)
	{
		return false;
	}
	
	// Look for tick events in the graph
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			FString EventName = EventNode->CustomFunctionName.ToString();
			if (EventName.Contains(TEXT("Tick")) || EventName.Contains(TEXT("Update")))
			{
				return true;
			}
		}
	}
	
	return false;
}

bool FRuntimeProfiler::IsExpensiveFunction(const FString& FunctionName) const
{
	// List of functions that are known to be expensive
	static const TArray<FString> ExpensiveFunctions = {
		TEXT("LineTrace"),
		TEXT("SphereTrace"),
		TEXT("BoxTrace"),
		TEXT("CapsuleTrace"),
		TEXT("GetAllActorsOfClass"),
		TEXT("GetAllActorsWithInterface"),
		TEXT("FindActorsOfClass"),
		TEXT("GetOverlappingActors"),
		TEXT("SetActorLocation"),
		TEXT("SetActorRotation"),
		TEXT("SetActorTransform"),
		TEXT("SpawnActor"),
		TEXT("DestroyActor")
	};

	for (const FString& ExpensiveFunc : ExpensiveFunctions)
	{
		if (FunctionName.Contains(ExpensiveFunc))
		{
			return true;
		}
	}

	return false;
}

//============================================================
// Blueprint Tracepoint System (Breakpoint-based Profiling)
//============================================================

void FRuntimeProfiler::SetupBlueprintTracepoints(UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return;
	}

	// Skip if blueprint is not a valid blueprint type
	if (!Blueprint->IsValidLowLevel())
	{
		return;
	}

	// Create or get the saved breakpoint state for this blueprint
	FOriginalBreakpointInfo& SavedState = SavedBreakpointStates.FindOrAdd(Blueprint);
	SavedState.Blueprint = Blueprint;
	SavedState.OriginalBreakpointStates.Empty();

	// Iterate through all graphs in the blueprint
	TArray<UEdGraph*> AllGraphs;

	// Add event graphs
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph) AllGraphs.Add(Graph);
	}

	// Add function graphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph) AllGraphs.Add(Graph);
	}

	// Add macro graphs
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (Graph) AllGraphs.Add(Graph);
	}

	int32 TracepointsCreated = 0;
	int32 TracepointsEnabled = 0;

	// Iterate through all graphs
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		// Iterate through all nodes in the graph
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || !Node->IsValidLowLevel())
			{
				continue;
			}

			// Check if this node type can have breakpoints
			// Most executable blueprint nodes can have breakpoints
			bool bCanHaveBreakpoint = false;

			// Check common executable node types
			if (Cast<UK2Node_CallFunction>(Node) ||
				Cast<UK2Node_Event>(Node) ||
				Cast<UK2Node_CustomEvent>(Node) ||
				Cast<UK2Node_MacroInstance>(Node) ||
				Node->IsA(UK2Node::StaticClass()))
			{
				bCanHaveBreakpoint = true;
			}

			// Try to find existing breakpoint
			FBlueprintBreakpoint* ExistingBreakpoint = FKismetDebugUtilities::FindBreakpointForNode(Node, Blueprint, true);

			// Save the original state (whether it had a breakpoint and if it was enabled)
			bool bHadBreakpoint = (ExistingBreakpoint != nullptr);
			bool bWasEnabled = ExistingBreakpoint && ExistingBreakpoint->IsEnabled();
			SavedState.OriginalBreakpointStates.Add(Node, bWasEnabled);

			// Create or enable breakpoint for profiling
			if (!ExistingBreakpoint)
			{
				// Create new breakpoint (enabled by default for tracing)
				FKismetDebugUtilities::CreateBreakpoint(Blueprint, Node, true);
				TracepointsCreated++;
			}
			else if (!bWasEnabled)
			{
				// Enable existing breakpoint for tracing
				FKismetDebugUtilities::SetBreakpointEnabled(*ExistingBreakpoint, true);
				TracepointsEnabled++;
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[PROFILER] Setup tracepoints for blueprint '%s': %d created, %d enabled"),
		*Blueprint->GetName(), TracepointsCreated, TracepointsEnabled);
}

void FRuntimeProfiler::RemoveBlueprintTracepoints(UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return;
	}

	// Check if we have saved state for this blueprint
	FOriginalBreakpointInfo* SavedState = SavedBreakpointStates.Find(Blueprint);
	if (!SavedState)
	{
		UE_LOG(LogTemp, Warning, TEXT("[PROFILER] No saved breakpoint state found for blueprint '%s'"), *Blueprint->GetName());
		return;
	}

	int32 BreakpointsRemoved = 0;
	int32 BreakpointsRestored = 0;
	int32 BreakpointsDisabled = 0;

	// Restore all saved breakpoint states
	for (const auto& BreakpointPair : SavedState->OriginalBreakpointStates)
	{
		TWeakObjectPtr<UEdGraphNode> Node = BreakpointPair.Key;
		bool bOriginalState = BreakpointPair.Value;

		if (!Node.IsValid())
		{
			continue;
		}

		// Find the breakpoint for this node
		FBlueprintBreakpoint* Breakpoint = FKismetDebugUtilities::FindBreakpointForNode(Node.Get(), Blueprint, true);

		if (Breakpoint)
		{
			if (bOriginalState)
			{
				// Restore to enabled state (user had it enabled)
				FKismetDebugUtilities::SetBreakpointEnabled(*Breakpoint, true);
				BreakpointsRestored++;
			}
			else
			{
				// The breakpoint didn't exist or was disabled before
				// Remove it to clean up our tracepoint
				FKismetDebugUtilities::RemoveBreakpointFromNode(Node.Get(), Blueprint);
				BreakpointsRemoved++;
			}
		}
	}

	// Clear the saved state for this blueprint
	SavedBreakpointStates.Remove(Blueprint);

	UE_LOG(LogTemp, Log, TEXT("[PROFILER] Removed tracepoints from blueprint '%s': %d removed, %d restored"),
		*Blueprint->GetName(), BreakpointsRemoved, BreakpointsRestored);
}

void FRuntimeProfiler::SetupTracepointsForAllBlueprints()
{
	UE_LOG(LogTemp, Log, TEXT("[PROFILER] Setting up tracepoints for all blueprints..."));

	// Use asset registry to find all blueprint assets
	TArray<FAssetData> BlueprintAssets;

	// Get the asset registry
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Find all blueprint assets
	AssetRegistry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), BlueprintAssets, true);

	UE_LOG(LogTemp, Log, TEXT("[PROFILER] Found %d blueprint assets"), BlueprintAssets.Num());

	int32 SuccessfulSetups = 0;

	// Setup tracepoints for each blueprint
	for (const FAssetData& AssetData : BlueprintAssets)
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset()))
		{
			SetupBlueprintTracepoints(Blueprint);
			SuccessfulSetups++;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[PROFILER] Successfully setup tracepoints for %d blueprints"), SuccessfulSetups);
}

void FRuntimeProfiler::RemoveTracepointsFromAllBlueprints()
{
	UE_LOG(LogTemp, Log, TEXT("[PROFILER] Removing tracepoints from all blueprints..."));

	// Create a copy of the keys to avoid modification during iteration
	TArray<TWeakObjectPtr<UBlueprint>> BlueprintsToCleanup;
	SavedBreakpointStates.GetKeys(BlueprintsToCleanup);

	int32 SuccessfulCleanups = 0;

	for (const TWeakObjectPtr<UBlueprint>& BlueprintPtr : BlueprintsToCleanup)
	{
		if (BlueprintPtr.IsValid())
		{
			RemoveBlueprintTracepoints(BlueprintPtr.Get());
			SuccessfulCleanups++;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[PROFILER] Successfully removed tracepoints from %d blueprints"), SuccessfulCleanups);
}

void FRuntimeProfiler::OnScriptExceptionTrace(const UObject* ActiveObject, const FFrame& StackFrame, const FBlueprintExceptionInfo& Info)
{
	// CRITICAL: This callback must be EXTREMELY FAST
	// It's called for EVERY blueprint node execution during profiling

	// Early exit if recording is skipped (to prevent recursive events)
	if (bSkipRecording)
	{
		return;
	}

	// Early exit if not recording
	if (CurrentState != ERecordingState::Recording)
	{
		return;
	}

	// Increment event counter (for debugging)
	TotalEventsProcessed++;

	// Rate-limited logging (log once per second approximately)
	double CurrentTime = FPlatformTime::Seconds();
	if (CurrentTime - LastLoggingTime > 1.0)
	{
		LastLoggingTime = CurrentTime;
		UE_LOG(LogTemp, VeryVerbose, TEXT("[PROFILER] Tracepoint events processed: %llu"), TotalEventsProcessed);
	}

	// Check if this is a tracepoint event (not a breakpoint or error)
	if (Info.GetType() != EBlueprintExceptionType::Tracepoint &&
		Info.GetType() != EBlueprintExceptionType::WireTracepoint)
	{
		// Only process tracepoints, ignore actual breakpoints and errors
		return;
	}

	// Find the node from the stack frame
	// StackFrame.Node is a UFunction*, we need to find the actual UEdGraphNode
	const UClass* ClassContainingCode = FKismetDebugUtilities::FindClassForNode(ActiveObject, StackFrame.Node);
	UBlueprint* Blueprint = (ClassContainingCode ? Cast<UBlueprint>(ClassContainingCode->ClassGeneratedBy) : nullptr);

	if (!Blueprint)
	{
		return;
	}

	// Calculate the offset within the function
	const int32 BreakpointOffset = StackFrame.Code - StackFrame.Node->Script.GetData() - 1;

	// Find the actual blueprint node from the code location
	UEdGraphNode* Node = nullptr;
	const UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(ClassContainingCode);
	if (GeneratedClass && GeneratedClass->DebugData.IsValid())
	{
		Node = GeneratedClass->DebugData.FindSourceNodeFromCodeLocation(StackFrame.Node, BreakpointOffset, true);
	}

	if (!Node)
	{
		return;
	}

	// FILTER: Skip engine internal macro nodes (StandardMacros, etc.)
	// This prevents recording internal nodes from For Loop, While Loop, etc.
	if (bHideEngineInternalNodes && IsNodeInStandardMacros(Node))
	{
		return;
	}

	// Use node as the key for tracking execution
	TWeakObjectPtr<UObject> NodeKey(Node);

	// Thread-safe update of execution statistics
	FScopeLock Lock(&DataMutex);

	// Find or create stats for this node
	FNodeExecutionStats& Stats = NodeStats.FindOrAdd(NodeKey);
	Stats.ExecutionCount++;

	// Record minimal timing information
	// Note: We can't get actual execution time from a breakpoint callback
	// So we use a minimal time unit for counting
	constexpr float MinExecutionTime = 0.0001f; // 0.1ms minimum
	Stats.TotalExecutionTime += MinExecutionTime;
	Stats.MinExecutionTime = FMath::Min(Stats.MinExecutionTime, MinExecutionTime);
	Stats.MaxExecutionTime = FMath::Max(Stats.MaxExecutionTime, MinExecutionTime);

	// Keep execution times array bounded (only keep last 100 samples)
	Stats.ExecutionTimes.Add(MinExecutionTime);
	if (Stats.ExecutionTimes.Num() > 100)
	{
		Stats.ExecutionTimes.RemoveAt(0);
	}

	// Record execution frame for timeline analysis
	FExecutionFrame Frame;
	Frame.Timestamp = CurrentTime;
	Frame.ObjectPtr = NodeKey;
	Frame.ExecutionTime = MinExecutionTime;
	ExecutionFrames.Add(Frame);

	// Keep execution frames bounded (only keep last 5000 frames)
	if (ExecutionFrames.Num() > 5000)
	{
		ExecutionFrames.RemoveAt(0, 100);
	}
}

//============================================================
// Async Tracepoint Setup (Timer-based Batch Processing)
//============================================================

void FRuntimeProfiler::SetupTracepointsForAllBlueprintsAsync()
{
	if (bIsSettingUpTracepoints)
	{
		UE_LOG(LogTemp, Warning, TEXT("[PROFILER] Tracepoint setup already in progress"));
		return;
	}

	bIsSettingUpTracepoints = true;
	bTracepointsActive = false; // Will be set to true when setup completes
	CurrentBlueprintIndex = 0;

	// Get all blueprint assets
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	AssetRegistry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), PendingBlueprints, true);

	UE_LOG(LogTemp, Log, TEXT("[PROFILER] Starting async tracepoint setup for %d blueprints"), PendingBlueprints.Num());

	// Start batch processing using timer
	if (UWorld* World = GEngine ? GEngine->GetWorld() : nullptr)
	{
		// Process immediately, then schedule remaining batches
		ProcessNextTracepointBatch();
	}
}

void FRuntimeProfiler::ProcessNextTracepointBatch()
{
	if (!bIsSettingUpTracepoints)
	{
		return;
	}

	const int32 BatchSize = 10; // Process 10 blueprints per frame
	int32 ProcessedThisBatch = 0;

	while (CurrentBlueprintIndex < PendingBlueprints.Num() && ProcessedThisBatch < BatchSize)
	{
		const FAssetData& AssetData = PendingBlueprints[CurrentBlueprintIndex];
		if (UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset()))
		{
			SetupBlueprintTracepoints(Blueprint);
		}

		CurrentBlueprintIndex++;
		ProcessedThisBatch++;
	}

	// Check if we're done
	if (CurrentBlueprintIndex >= PendingBlueprints.Num())
	{
		// Complete setup
		bIsSettingUpTracepoints = false;
		bTracepointsActive = true;
		PendingBlueprints.Empty();
		CurrentBlueprintIndex = 0;

		// Clear timer
		if (UWorld* World = GEngine ? GEngine->GetWorld() : nullptr)
		{
			World->GetTimerManager().ClearTimer(TracepointSetupTimerHandle);
		}

		UE_LOG(LogTemp, Log, TEXT("[PROFILER] Async tracepoint setup complete: %d blueprints processed"),
			PendingBlueprints.Num());

		// Notify completion
		OnTracepointSetupComplete.Broadcast(true);
	}
	else
	{
		// Schedule next batch
		if (UWorld* World = GEngine ? GEngine->GetWorld() : nullptr)
		{
			World->GetTimerManager().SetTimerForNextTick(
				FTimerDelegate::CreateRaw(this, &FRuntimeProfiler::ProcessNextTracepointBatch)
			);
		}
	}
}

//============================================================
// Filtering Helper Methods
//============================================================

bool FRuntimeProfiler::IsNodeInStandardMacros(UEdGraphNode* Node) const
{
	if (!Node)
	{
		return false;
	}

	// Get the node's path
	FString NodePath = Node->GetPathName();

	// Check if the node belongs to engine's standard macros
	// Common paths for engine macros:
	// - /Engine/Functions/StandardMacros
	// - /Engine/Transient
	// - Any path starting with /Engine/
	if (NodePath.Contains(TEXT("/Engine/Functions/StandardMacros")) ||
		NodePath.Contains(TEXT("/Engine/Transient")) ||
		NodePath.StartsWith(TEXT("/Engine/")))
	{
		return true;
	}

	// Also check the graph that contains this node
	if (UEdGraph* Graph = Node->GetGraph())
	{
		FString GraphPath = Graph->GetPathName();
		if (GraphPath.Contains(TEXT("/Engine/Functions/StandardMacros")) ||
			GraphPath.StartsWith(TEXT("/Engine/")))
		{
			return true;
		}
	}

	// Check if the blueprint is an engine internal asset
	if (UBlueprint* BP = Cast<UBlueprint>(Node->GetOuter()))
	{
		if (IsEngineInternalBlueprint(BP->GetPathName()))
		{
			return true;
		}
	}

	return false;
}

bool FRuntimeProfiler::IsEngineInternalBlueprint(const FString& BlueprintPath) const
{
	// Check if the blueprint is in the engine directory
	return BlueprintPath.StartsWith(TEXT("/Engine/")) ||
		   BlueprintPath.StartsWith(TEXT("/Script/"));
}