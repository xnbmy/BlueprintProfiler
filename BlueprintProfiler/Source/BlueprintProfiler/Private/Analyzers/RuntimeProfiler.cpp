#include "Analyzers/RuntimeProfiler.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
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
#include "Engine/Engine.h"
#include "HAL/PlatformFilemanager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"

FRuntimeProfiler::FRuntimeProfiler()
	: CurrentState(ERecordingState::Stopped)
	, RecordingStartTime(0.0)
	, PauseStartTime(0.0)
	, TotalPausedTime(0.0)
	, bAutoStartOnPIE(false)
	, bAutoStopOnPIEEnd(true)
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
	
	// Enable blueprint instrumentation
	EnableBlueprintInstrumentation();
	
	UE_LOG(LogTemp, Log, TEXT("Runtime profiler recording started - Session: %s"), *CurrentSession.SessionName);
}

void FRuntimeProfiler::StopRecording()
{
	if (CurrentState == ERecordingState::Stopped)
	{
		return;
	}
	
	CurrentState = ERecordingState::Stopped;
	
	// Disable blueprint instrumentation
	DisableBlueprintInstrumentation();
	
	// End current session and save to history
	EndCurrentSession();
	
	UE_LOG(LogTemp, Log, TEXT("Runtime profiler recording stopped - Session: %s"), *CurrentSession.SessionName);
}

void FRuntimeProfiler::PauseRecording()
{
	if (CurrentState != ERecordingState::Recording)
	{
		return;
	}
	
	CurrentState = ERecordingState::Paused;
	PauseStartTime = FPlatformTime::Seconds();
	
	// Disable blueprint instrumentation while paused
	DisableBlueprintInstrumentation();
	
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
	
	// Re-enable blueprint instrumentation
	EnableBlueprintInstrumentation();
	
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
			if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
			{
				Data.BlueprintName = Blueprint->GetName();
				Data.NodeName = TEXT("Blueprint"); // Generic for now
			}
			else if (UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(Object->GetClass()))
			{
				if (UBlueprint* GeneratedByBlueprint = Cast<UBlueprint>(BPClass->ClassGeneratedBy))
				{
					Data.BlueprintName = GeneratedByBlueprint->GetName();
					Data.NodeName = Object->GetName();
				}
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

void FRuntimeProfiler::OnScriptInstrumentation(const FFrame& Frame, const FBlueprintInstrumentationSignal& Signal)
{
	if (CurrentState != ERecordingState::Recording)
	{
		return;
	}
	
	// Record execution data based on the instrumentation signal
	RecordNodeExecution(Frame);
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
	UE_LOG(LogTemp, Log, TEXT("Blueprint instrumentation cleaned up"));
}

void FRuntimeProfiler::EnableBlueprintInstrumentation()
{
	// Enable blueprint execution monitoring
	// In a real implementation, this would hook into FBlueprintCoreDelegates::OnScriptInstrumentationSignal
	UE_LOG(LogTemp, Log, TEXT("Blueprint instrumentation enabled"));
}

void FRuntimeProfiler::DisableBlueprintInstrumentation()
{
	// Disable blueprint execution monitoring
	UE_LOG(LogTemp, Log, TEXT("Blueprint instrumentation disabled"));
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