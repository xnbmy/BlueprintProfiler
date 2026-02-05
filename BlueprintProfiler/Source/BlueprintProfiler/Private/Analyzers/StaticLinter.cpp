#include "Analyzers/StaticLinter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "K2Node.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MacroInstance.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Async/Async.h"
#include "Misc/ScopedSlowTask.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/DateTime.h"
#include "Framework/Application/SlateApplication.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPtr.h"
#include "HAL/PlatformProcess.h"

/**
 * Async task for blueprint scanning
 */
class FScanTask : public FNonAbandonableTask
{
public:
	FScanTask(TSharedPtr<FStaticLinter> InLinter, const TArray<FAssetData>& InAssets, const FScanConfiguration& InConfig)
		: LinterWeak(InLinter)
		, Assets(InAssets)
		, Config(InConfig)
	{
	}

	void DoWork()
	{
		// Asset loading must happen on game thread, so we dispatch all work there
		// This method just queues the work and waits for completion

		TArray<FLintIssue> AllIssues;
		FThreadSafeCounter ProcessedCount;

		UE_LOG(LogTemp, Log, TEXT("FScanTask: Starting to process %d assets on game thread"), Assets.Num());

		// Process all assets on game thread (required for blueprint loading)
		AsyncTask(ENamedThreads::GameThread, [this, &AllIssues, &ProcessedCount]()
		{
			TSharedPtr<FStaticLinter> LinterPin = LinterWeak.Pin();
			if (!LinterPin.IsValid())
			{
				UE_LOG(LogTemp, Warning, TEXT("FScanTask: Linter was destroyed, aborting scan"));
				return;
			}

			for (const FAssetData& AssetData : Assets)
			{
				if (LinterPin->IsCancelRequested())
				{
					UE_LOG(LogTemp, Log, TEXT("FScanTask: Scan was cancelled after %d assets"), ProcessedCount.GetValue());
					break;
				}

				// Process this blueprint
				TArray<FLintIssue> AssetIssues;
				LinterPin->ProcessBlueprint(AssetData, Config, AssetIssues);

				// Accumulate issues
				{
					FScopeLock Lock(&LinterPin->GetIssuesLock());
					AllIssues.Append(AssetIssues);
				}

				int32 CurrentCount = ProcessedCount.Increment();

				// Update progress
				LinterPin->UpdateScanProgress(CurrentCount, Assets.Num(), AssetData.AssetName.ToString());

				// Log progress for debugging
				UE_LOG(LogTemp, Log, TEXT("FScanTask: Processed %d/%d - %s (%d issues found)"),
					CurrentCount, Assets.Num(), *AssetData.AssetName.ToString(), AssetIssues.Num());
			}
		});

		// Wait for game thread processing to complete
		// Since DoWork runs on a thread pool thread, we need to wait for the game thread task
		while (ProcessedCount.GetValue() < Assets.Num() && !LinterWeak.Pin()->IsCancelRequested())
		{
			FPlatformProcess::Sleep(0.01f); // Sleep 10ms

			// Check if linter is still valid
			TSharedPtr<FStaticLinter> LinterPin = LinterWeak.Pin();
			if (!LinterPin.IsValid())
			{
				UE_LOG(LogTemp, Warning, TEXT("FScanTask: Linter was destroyed during scan"));
				return;
			}
		}

		TSharedPtr<FStaticLinter> LinterPin = LinterWeak.Pin();
		if (LinterPin.IsValid())
		{
			UE_LOG(LogTemp, Log, TEXT("FScanTask: Completed processing %d assets, found %d total issues"),
				ProcessedCount.GetValue(), AllIssues.Num());
		}

		// Complete scan on game thread (async)
		AsyncTask(ENamedThreads::GameThread, [this, AllIssues]()
		{
			TSharedPtr<FStaticLinter> LinterPin = LinterWeak.Pin();
			if (LinterPin.IsValid())
			{
				LinterPin->CompleteScan(AllIssues);
			}
		});
	}

	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FScanTask, STATGROUP_ThreadPoolAsyncTasks);
	}

private:
	TWeakPtr<FStaticLinter> LinterWeak;
	TArray<FAssetData> Assets;
	FScanConfiguration Config;
};

FStaticLinter::FStaticLinter()
	: bScanInProgress(false)
	, bCancelRequested(false)
	, bTaskComplete(true)
{
}

FStaticLinter::~FStaticLinter()
{
	// Mark as cancelled to prevent CompleteScan from accessing this object
	bCancelRequested = true;
	bScanInProgress = false;

	// Wait for the background task and its async completion callback to finish
	if (CurrentScanTask.IsValid())
	{
		// Wait for the background DoWork to complete
		CurrentScanTask->EnsureCompletion();

		// Wait a bit longer for the async CompleteScan callback to execute
		// The callback is scheduled on the game thread, so we need to give it time to run
		double WaitStartTime = FPlatformTime::Seconds();
		const double MaxWaitTime = 5.0; // Wait up to 5 seconds

		while (!bTaskComplete && (FPlatformTime::Seconds() - WaitStartTime) < MaxWaitTime)
		{
			FPlatformProcess::Sleep(0.01f); // Sleep 10ms

			// Pump the message queue to allow async tasks to run
			FSlateApplication::Get().PumpMessages();
		}

		if (!bTaskComplete)
		{
			UE_LOG(LogTemp, Warning, TEXT("StaticLinter destructor: Task completion callback did not execute in time"));
		}
	}

	// Now it's safe to reset
	CurrentScanTask.Reset();
}

void FStaticLinter::ScanProject(const FScanConfiguration& Config)
{
	TArray<FString> ProjectPaths;
	ProjectPaths.Add(TEXT("/Game")); // Scan entire project content
	
	TArray<FAssetData> Assets = GetBlueprintAssets(ProjectPaths);
	ScanBlueprints(Assets, Config);
}

void FStaticLinter::ScanFolder(const FString& FolderPath, const FScanConfiguration& Config)
{
	TArray<FString> Paths;
	Paths.Add(FolderPath);
	
	TArray<FAssetData> Assets = GetBlueprintAssets(Paths);
	ScanBlueprints(Assets, Config);
}

void FStaticLinter::ScanSelectedFolders(const TArray<FString>& FolderPaths, const FScanConfiguration& Config)
{
	TArray<FAssetData> AllAssets;
	
	// Collect assets from all selected folders
	for (const FString& FolderPath : FolderPaths)
	{
		TArray<FAssetData> FolderAssets = GetBlueprintAssetsInFolder(FolderPath, true);
		AllAssets.Append(FolderAssets);
	}
	
	// Remove duplicates
	TSet<FString> UniqueAssetPaths;
	TArray<FAssetData> UniqueAssets;
	
	for (const FAssetData& Asset : AllAssets)
	{
		FString AssetPath = Asset.GetObjectPathString();
		if (!UniqueAssetPaths.Contains(AssetPath))
		{
			UniqueAssetPaths.Add(AssetPath);
			UniqueAssets.Add(Asset);
		}
	}
	
	UE_LOG(LogTemp, Log, TEXT("Scanning %d folders, found %d unique blueprint assets"), 
		FolderPaths.Num(), UniqueAssets.Num());
	
	ScanBlueprints(UniqueAssets, Config);
}

void FStaticLinter::ScanBlueprints(const TArray<FAssetData>& Blueprints, const FScanConfiguration& Config)
{
	if (bScanInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("Scan already in progress"));
		return;
	}

	// Filter assets based on configuration
	TArray<FAssetData> FilteredAssets;
	for (const FAssetData& Asset : Blueprints)
	{
		if (ShouldProcessAsset(Asset, Config))
		{
			FilteredAssets.Add(Asset);
		}
	}

	if (FilteredAssets.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("No blueprints found to scan"));
		OnScanComplete.Broadcast(TArray<FLintIssue>());
		return;
	}

	StartAsyncScan(FilteredAssets, Config);
}

void FStaticLinter::CancelScan()
{
	if (bScanInProgress)
	{
		bCancelRequested = true;
		CurrentProgress.bWasCancelled = true;

		UE_LOG(LogTemp, Log, TEXT("Scan cancellation requested - processed %d/%d assets"),
			CurrentProgress.ProcessedAssets, CurrentProgress.TotalAssets);

		if (CurrentScanTask.IsValid())
		{
			// Wait for background task to complete
			CurrentScanTask->EnsureCompletion();

			// Don't reset here - let CompleteScan() handle the reset
			// or it will be reset when next scan starts
		}

		bScanInProgress = false;
		bCancelRequested = false;

		// Preserve partial results if any were found
		UE_LOG(LogTemp, Log, TEXT("Scan cancelled - %d issues found in %d processed assets"),
			Issues.Num(), CurrentProgress.ProcessedAssets);

		// Broadcast completion with partial results
		OnScanComplete.Broadcast(Issues);
	}
}

TArray<FLintIssue> FStaticLinter::GetIssuesByType(ELintIssueType Type) const
{
	TArray<FLintIssue> FilteredIssues;
	
	for (const FLintIssue& Issue : Issues)
	{
		if (Issue.Type == Type)
		{
			FilteredIssues.Add(Issue);
		}
	}
	
	return FilteredIssues;
}

void FStaticLinter::StartAsyncScan(const TArray<FAssetData>& Assets, const FScanConfiguration& Config)
{
	// Ensure any previous task is completed
	if (CurrentScanTask.IsValid())
	{
		CurrentScanTask->EnsureCompletion();
		CurrentScanTask.Reset();
	}

	// Clear previous self-reference
	SelfReference.Reset();

	bScanInProgress = true;
	bCancelRequested = false;
	bTaskComplete = false; // Task is starting, mark as incomplete
	Issues.Empty();

	CurrentProgress.TotalAssets = Assets.Num();
	CurrentProgress.ProcessedAssets = 0;
	CurrentProgress.IssuesFound = 0;
	CurrentProgress.ProgressPercentage = 0.0f;
	CurrentProgress.EstimatedTimeRemaining = 0.0f;
	CurrentProgress.StartTime = FDateTime::Now();
	CurrentProgress.bIsCompleted = false;
	CurrentProgress.bWasCancelled = false;

	UE_LOG(LogTemp, Log, TEXT("Starting scan of %d assets with %s threading"),
		Assets.Num(), Config.bUseMultiThreading ? TEXT("multi") : TEXT("single"));

	if (Config.bUseMultiThreading && Assets.Num() > 1)
	{
		// Create a self-reference to keep this object alive during async operation
		// The custom deleter ensures we don't actually delete the object (it's owned by the widget)
		SelfReference = TSharedPtr<FStaticLinter>(this, [](FStaticLinter* /*Linter*/) {
			// No-op deleter - object is owned by its parent (the widget)
		});

		CurrentScanTask = MakeShared<FAsyncTask<FScanTask>>(SelfReference, Assets, Config);
		CurrentScanTask->StartBackgroundTask();

		UE_LOG(LogTemp, Log, TEXT("Async scan task started in background"));
	}
	else
	{
		// Process synchronously for single assets or when multi-threading is disabled
		TArray<FLintIssue> AllIssues;

		for (int32 AssetIndex = 0; AssetIndex < Assets.Num(); AssetIndex++)
		{
			if (bCancelRequested)
			{
				CurrentProgress.bWasCancelled = true;
				break;
			}

			const FAssetData& AssetData = Assets[AssetIndex];
			TArray<FLintIssue> AssetIssues;
			ProcessBlueprint(AssetData, Config, AssetIssues);
			AllIssues.Append(AssetIssues);

			UpdateScanProgress(AssetIndex + 1, Assets.Num(), AssetData.AssetName.ToString());
		}

		CompleteScan(AllIssues);
	}
}

void FStaticLinter::ProcessBlueprint(const FAssetData& AssetData, const FScanConfiguration& Config, TArray<FLintIssue>& OutIssues)
{
	// Update current asset being processed
	CurrentProgress.CurrentAsset = AssetData.AssetName.ToString();

	UE_LOG(LogTemp, Log, TEXT("Processing blueprint: %s"), *CurrentProgress.CurrentAsset);

	UBlueprint* Blueprint = nullptr;

	// Try to get the blueprint - GetAsset() will synchronously load if not already loaded
	// This is necessary because blueprints in the editor may not be loaded into memory
	Blueprint = Cast<UBlueprint>(AssetData.GetAsset());

	if (!Blueprint)
	{
		UE_LOG(LogTemp, Warning, TEXT("Failed to load blueprint: %s"), *AssetData.GetObjectPathString());
		return;
	}

	// Validate blueprint state - but allow blueprints that haven't been fully compiled
	if (!Blueprint->GeneratedClass && !Blueprint->SkeletonGeneratedClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("Blueprint has no generated class (not yet compiled): %s"), *Blueprint->GetName());
		// Don't skip - we can still analyze the graph structure even without compiled class
	}

	// Check if blueprint is valid for analysis
	if (Blueprint->UbergraphPages.Num() == 0 && Blueprint->FunctionGraphs.Num() == 0 && Blueprint->MacroGraphs.Num() == 0)
	{
		UE_LOG(LogTemp, Verbose, TEXT("Blueprint has no graphs to analyze: %s"), *Blueprint->GetName());
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("Analyzing blueprint: %s (%d uber graphs, %d function graphs, %d macro graphs)"),
		*Blueprint->GetName(), Blueprint->UbergraphPages.Num(), Blueprint->FunctionGraphs.Num(), Blueprint->MacroGraphs.Num());

	// Run enabled checks with error handling
	try
	{
		int32 InitialIssueCount = OutIssues.Num();

		if (Config.EnabledChecks.Contains(ELintIssueType::DeadNode))
		{
			DetectDeadNodes(Blueprint, OutIssues);
		}

		if (Config.EnabledChecks.Contains(ELintIssueType::OrphanNode))
		{
			DetectOrphanNodes(Blueprint, OutIssues);
		}

		if (Config.EnabledChecks.Contains(ELintIssueType::CastAbuse))
		{
			DetectCastAbuse(Blueprint, OutIssues);
		}

		if (Config.EnabledChecks.Contains(ELintIssueType::TickAbuse))
		{
			DetectTickAbuse(Blueprint, OutIssues);
		}

		int32 IssuesFound = OutIssues.Num() - InitialIssueCount;
		if (IssuesFound > 0)
		{
			UE_LOG(LogTemp, Log, TEXT("Found %d issues in blueprint: %s"), IssuesFound, *Blueprint->GetName());
		}
	}
	catch (...)
	{
		UE_LOG(LogTemp, Error, TEXT("Exception occurred while analyzing blueprint: %s"), *Blueprint->GetName());
	}
}

void FStaticLinter::DetectDeadNodes(UBlueprint* Blueprint, TArray<FLintIssue>& OutIssues)
{
	if (!Blueprint)
	{
		return;
	}

	// Get all graphs to analyze
	TArray<UEdGraph*> AllGraphs = GetAllGraphs(Blueprint);

	// Track all referenced variables and functions
	TSet<FName> ReferencedVariables;
	TSet<FName> ReferencedFunctions;
	TSet<FGuid> ReferencedCustomEvents;

	// First pass: collect all references
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			// Track variable references
			if (UK2Node_VariableGet* VarGetNode = Cast<UK2Node_VariableGet>(Node))
			{
				// Check if this variable get node has any output connections
				bool bHasOutputConnections = false;
				for (UEdGraphPin* Pin : VarGetNode->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0)
					{
						bHasOutputConnections = true;
						ReferencedVariables.Add(VarGetNode->VariableReference.GetMemberName());
						break;
					}
				}
			}
			else if (UK2Node_VariableSet* VarSetNode = Cast<UK2Node_VariableSet>(Node))
			{
				// Variable set nodes always count as references
				ReferencedVariables.Add(VarSetNode->VariableReference.GetMemberName());
			}
			else if (UK2Node_CallFunction* FuncCallNode = Cast<UK2Node_CallFunction>(Node))
			{
				// Track function calls
				if (FuncCallNode->FunctionReference.GetMemberName() != NAME_None)
				{
					ReferencedFunctions.Add(FuncCallNode->FunctionReference.GetMemberName());
				}
			}
			else if (UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
			{
				// Track custom event references
				ReferencedCustomEvents.Add(CustomEventNode->NodeGuid);
			}
		}
	}

	// Second pass: find unreferenced variables and functions
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			// Check for unreferenced variable get nodes
			if (UK2Node_VariableGet* VarGetNode = Cast<UK2Node_VariableGet>(Node))
			{
				bool bHasConnections = false;
				for (UEdGraphPin* Pin : VarGetNode->Pins)
				{
					if (Pin && Pin->LinkedTo.Num() > 0)
					{
						bHasConnections = true;
						break;
					}
				}

				if (!bHasConnections)
				{
					FLintIssue Issue;
					Issue.Type = ELintIssueType::DeadNode;
					Issue.BlueprintPath = Blueprint->GetPathName();
					Issue.NodeName = VarGetNode->VariableReference.GetMemberName().ToString();
					Issue.Description = FString::Printf(TEXT("Variable '%s' is retrieved but never used"), *Issue.NodeName);
					Issue.Severity = CalculateIssueSeverity(ELintIssueType::DeadNode);
					Issue.NodeGuid = VarGetNode->NodeGuid;
					
					OutIssues.Add(Issue);
				}
			}
			// Check for unreferenced function definitions
			else if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				// Skip all built-in events (Receive*)
				FName EventName = EventNode->GetFunctionName();
				if (EventName.ToString().StartsWith(TEXT("Receive")))
				{
					continue; // Skip all Receive* events
				}

				// Skip interface events - they are called by the blueprint system automatically
				if (EventNode->IsInterfaceEventNode())
				{
					continue;
				}

				// Check if this custom event is referenced
				bool bIsReferenced = ReferencedFunctions.Contains(EventName);

				if (!bIsReferenced)
				{
					// Also check for direct event calls by GUID
					bIsReferenced = ReferencedCustomEvents.Contains(EventNode->NodeGuid);
				}

				if (!bIsReferenced)
				{
					FLintIssue Issue;
					Issue.Type = ELintIssueType::DeadNode;
					Issue.BlueprintPath = Blueprint->GetPathName();
					Issue.NodeName = EventName.ToString();
					Issue.Description = FString::Printf(TEXT("自定义事件 '%s' 已定义但从未被调用"), *Issue.NodeName);
					Issue.Severity = ESeverity::Low; // 未调用的事件不一定严重
					Issue.NodeGuid = EventNode->NodeGuid;

					OutIssues.Add(Issue);
				}
			}
		}
	}

	// Check for unreferenced blueprint variables
	for (FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		if (!ReferencedVariables.Contains(Variable.VarName))
		{
			FLintIssue Issue;
			Issue.Type = ELintIssueType::DeadNode;
			Issue.BlueprintPath = Blueprint->GetPathName();
			Issue.NodeName = Variable.VarName.ToString();
			Issue.Description = FString::Printf(TEXT("Blueprint variable '%s' is declared but never used"), *Issue.NodeName);
			Issue.Severity = CalculateIssueSeverity(ELintIssueType::DeadNode);
			// Note: Variables don't have NodeGuid, so we leave it empty
			
			OutIssues.Add(Issue);
		}
	}
}

void FStaticLinter::DetectOrphanNodes(UBlueprint* Blueprint, TArray<FLintIssue>& OutIssues)
{
	if (!Blueprint)
	{
		return;
	}

	// Get all graphs to analyze
	TArray<UEdGraph*> AllGraphs = GetAllGraphs(Blueprint);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			// Skip Event nodes - they're entry points and don't need to be connected
			if (Node->IsA<UK2Node_Event>() || Node->IsA<UK2Node_CustomEvent>())
			{
				continue;
			}

			// Skip macro instance nodes - they are references to other graphs
			if (Node->IsA<UK2Node_MacroInstance>())
			{
				continue;
			}

			// Check for pure nodes (computation nodes) without execution connections
			if (UK2Node* K2Node = Cast<UK2Node>(Node))
			{
				if (K2Node->IsNodePure())
				{
					bool bHasOutputConnections = false;
					bool bHasInputConnections = false;

					// Check all pins for connections
					for (UEdGraphPin* Pin : K2Node->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0)
						{
							bHasOutputConnections = true;
						}
						else if (Pin && Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0)
						{
							bHasInputConnections = true;
						}
					}

					// Only report if node has NO connections at all (both input and output disconnected)
					// This is a much stricter check to avoid false positives
					if (!bHasOutputConnections && !bHasInputConnections)
					{
						// Skip constant nodes and utility nodes that don't need connections
						FString NodeClass = K2Node->GetClass()->GetName();
						FString NodeTitle = K2Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

						// Skip nodes that are typically standalone
						if (NodeClass.Contains(TEXT("Literal")) ||
							NodeClass.Contains(TEXT("Constant")) ||
							NodeTitle.Contains(TEXT("Make")) ||  // Make nodes are typically standalone
							NodeTitle.Contains(TEXT("Select")) ||  // Select nodes are typically standalone
							NodeTitle.Contains(TEXT("Branch"))) // Branch nodes are standalone
						{
							continue;
						}

						FLintIssue Issue;
						Issue.Type = ELintIssueType::OrphanNode;
						Issue.BlueprintPath = Blueprint->GetPathName();
						Issue.NodeName = NodeTitle;
						Issue.Description = FString::Printf(TEXT("纯节点 '%s' 没有任何连接"), *Issue.NodeName);
						Issue.Severity = ESeverity::Low; // 孤立节点不是严重问题
						Issue.NodeGuid = K2Node->NodeGuid;

						OutIssues.Add(Issue);
					}
				}
			}
			// Check for non-pure nodes with execution pins but no connections
			else
			{
				bool bHasAnyConnections = false;
				bool bHasExecutionPins = false;

				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin)
					{
						// Check for execution pins
						if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
						{
							bHasExecutionPins = true;
						}

						// Check for any connections
						if (Pin->LinkedTo.Num() > 0)
						{
							bHasAnyConnections = true;
						}
					}
				}

				// Only report if it has execution pins but completely disconnected
				if (bHasExecutionPins && !bHasAnyConnections)
				{
					// Skip certain node types that are OK to be disconnected
					if (!Node->IsA<UK2Node_Event>() &&
						!Node->IsA<UK2Node_CustomEvent>())
					{
						FLintIssue Issue;
						Issue.Type = ELintIssueType::OrphanNode;
						Issue.BlueprintPath = Blueprint->GetPathName();
						Issue.NodeName = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
						Issue.Description = FString::Printf(TEXT("执行节点 '%s' 有执行引脚但没有连接"), *Issue.NodeName);
						Issue.Severity = ESeverity::Medium;
						Issue.NodeGuid = Node->NodeGuid;

						OutIssues.Add(Issue);
					}
				}
			}
		}
	}
}

void FStaticLinter::DetectCastAbuse(UBlueprint* Blueprint, TArray<FLintIssue>& OutIssues)
{
	if (!Blueprint)
	{
		return;
	}

	// Get all graphs to analyze
	TArray<UEdGraph*> AllGraphs = GetAllGraphs(Blueprint);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			// Look for cast nodes
			if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
			{
				bool bIsInProblematicContext = false;
				ESeverity CastSeverity = ESeverity::Low;
				FString ContextDescription;

				// Analyze the context of this cast node
				TSet<UEdGraphNode*> VisitedNodes;
				if (IsNodeInTickContext(CastNode, VisitedNodes))
				{
					bIsInProblematicContext = true;
					CastSeverity = ESeverity::High;
					ContextDescription = TEXT("in Tick event context");
				}
				else if (IsNodeInLoopContext(CastNode, VisitedNodes))
				{
					bIsInProblematicContext = true;
					CastSeverity = ESeverity::Medium;
					ContextDescription = TEXT("in loop context");
				}
				else if (IsNodeInFrequentlyCalledFunction(CastNode, Graph))
				{
					bIsInProblematicContext = true;
					CastSeverity = ESeverity::Medium;
					ContextDescription = TEXT("in frequently called function");
				}

				// Check if it's a hard reference cast (more expensive)
				bool bIsHardReferenceCast = IsHardReferenceCast(CastNode);
				if (bIsHardReferenceCast && bIsInProblematicContext)
				{
					CastSeverity = ESeverity::High;
				}

				if (bIsInProblematicContext)
				{
					FLintIssue Issue;
					Issue.Type = ELintIssueType::CastAbuse;
					Issue.BlueprintPath = Blueprint->GetPathName();
					Issue.NodeName = CastNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
					Issue.Description = FString::Printf(TEXT("Cast node '%s' %s may cause performance issues %s"), 
						*Issue.NodeName, 
						bIsHardReferenceCast ? TEXT("(hard reference)") : TEXT(""),
						*ContextDescription);
					Issue.Severity = CastSeverity;
					Issue.NodeGuid = CastNode->NodeGuid;
					
					OutIssues.Add(Issue);
				}
			}
		}
	}
}

bool FStaticLinter::IsNodeInTickContext(UEdGraphNode* Node, TSet<UEdGraphNode*>& VisitedNodes) const
{
	if (!Node || VisitedNodes.Contains(Node))
	{
		return false;
	}

	VisitedNodes.Add(Node);

	// Check if this is a tick event node
	if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
	{
		FName EventName = EventNode->GetFunctionName();
		if (EventName == TEXT("ReceiveTick") || EventName == TEXT("Tick"))
		{
			return true;
		}
	}

	// Trace backwards through execution pins to find the source
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (LinkedPin && LinkedPin->GetOwningNode())
				{
					if (IsNodeInTickContext(LinkedPin->GetOwningNode(), VisitedNodes))
					{
						return true;
					}
				}
			}
		}
	}

	return false;
}

bool FStaticLinter::IsNodeInLoopContext(UEdGraphNode* Node, TSet<UEdGraphNode*>& VisitedNodes) const
{
	if (!Node || VisitedNodes.Contains(Node))
	{
		return false;
	}

	VisitedNodes.Add(Node);

	// Check if this is a loop node
	FString NodeClass = Node->GetClass()->GetName();
	if (NodeClass.Contains(TEXT("ForLoop")) || 
		NodeClass.Contains(TEXT("WhileLoop")) || 
		NodeClass.Contains(TEXT("ForEach")))
	{
		return true;
	}

	// Trace backwards through execution pins
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (LinkedPin && LinkedPin->GetOwningNode())
				{
					if (IsNodeInLoopContext(LinkedPin->GetOwningNode(), VisitedNodes))
					{
						return true;
					}
				}
			}
		}
	}

	return false;
}

bool FStaticLinter::IsNodeInFrequentlyCalledFunction(UEdGraphNode* Node, UEdGraph* Graph) const
{
	if (!Graph)
	{
		return false;
	}

	// Check if the graph is a function graph with certain naming patterns
	FString GraphName = Graph->GetName();
	
	// Common patterns for frequently called functions
	TArray<FString> FrequentPatterns = {
		TEXT("Update"),
		TEXT("Process"),
		TEXT("Calculate"),
		TEXT("Check"),
		TEXT("Validate"),
		TEXT("GetCurrent"),
		TEXT("IsValid")
	};

	for (const FString& Pattern : FrequentPatterns)
	{
		if (GraphName.Contains(Pattern))
		{
			return true;
		}
	}

	return false;
}

bool FStaticLinter::IsHardReferenceCast(UK2Node_DynamicCast* CastNode) const
{
	if (!CastNode)
	{
		return false;
	}

	// Check if the target class is a hard reference (not an interface)
	UClass* TargetClass = CastNode->TargetType;
	if (TargetClass && !TargetClass->HasAnyClassFlags(CLASS_Interface))
	{
		// Additional checks for expensive casts
		FString ClassName = TargetClass->GetName();
		
		// Actor casts are generally more expensive
		if (TargetClass->IsChildOf(AActor::StaticClass()))
		{
			return true;
		}
		
		// Component casts can also be expensive
		if (TargetClass->IsChildOf(UActorComponent::StaticClass()))
		{
			return true;
		}
	}

	return false;
}

void FStaticLinter::DetectTickAbuse(UBlueprint* Blueprint, TArray<FLintIssue>& OutIssues)
{
	if (!Blueprint)
	{
		return;
	}

	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (!Graph)
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			// Look for Event Tick nodes
			if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				if (EventNode->GetFunctionName() == TEXT("ReceiveTick") || 
					EventNode->GetFunctionName() == TEXT("Tick"))
				{
					// Count connected nodes to estimate complexity
					int32 ConnectedNodeCount = 0;
					TSet<UEdGraphNode*> VisitedNodes;
					CountConnectedNodes(EventNode, VisitedNodes, ConnectedNodeCount);

					// Flag tick events with high complexity
					if (ConnectedNodeCount > 10) // Arbitrary threshold
					{
						FLintIssue Issue;
						Issue.Type = ELintIssueType::TickAbuse;
						Issue.BlueprintPath = Blueprint->GetPathName();
						Issue.NodeName = TEXT("Event Tick");
						Issue.Description = FString::Printf(TEXT("Tick event has high complexity (%d connected nodes)"), ConnectedNodeCount);
						Issue.Severity = CalculateIssueSeverity(ELintIssueType::TickAbuse, ConnectedNodeCount);
						Issue.NodeGuid = EventNode->NodeGuid;
						
						OutIssues.Add(Issue);
					}
				}
			}
		}
	}
}

void FStaticLinter::CountConnectedNodes(UEdGraphNode* StartNode, TSet<UEdGraphNode*>& VisitedNodes, int32& NodeCount)
{
	if (!StartNode || VisitedNodes.Contains(StartNode))
	{
		return;
	}

	VisitedNodes.Add(StartNode);
	NodeCount++;

	// Follow execution pins
	for (UEdGraphPin* Pin : StartNode->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == TEXT("exec"))
		{
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (LinkedPin && LinkedPin->GetOwningNode())
				{
					CountConnectedNodes(LinkedPin->GetOwningNode(), VisitedNodes, NodeCount);
				}
			}
		}
	}
}

TArray<FAssetData> FStaticLinter::GetBlueprintAssets(const TArray<FString>& Paths) const
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Ensure asset registry is loaded
	if (!AssetRegistry.IsLoadingAssets())
	{
		AssetRegistry.SearchAllAssets(true);
	}

	TArray<FAssetData> Assets;
	
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	Filter.bIncludeOnlyOnDiskAssets = false; // Include both on-disk and in-memory assets
	
	for (const FString& Path : Paths)
	{
		Filter.PackagePaths.Add(FName(*Path));
	}

	AssetRegistry.GetAssets(Filter, Assets);
	
	// Log scanning information
	UE_LOG(LogTemp, Log, TEXT("Found %d blueprint assets in %d paths"), Assets.Num(), Paths.Num());
	for (const FString& Path : Paths)
	{
		UE_LOG(LogTemp, Verbose, TEXT("Scanning path: %s"), *Path);
	}
	
	return Assets;
}

TArray<FAssetData> FStaticLinter::GetBlueprintAssetsInFolder(const FString& FolderPath, bool bRecursive) const
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Ensure asset registry is loaded
	if (!AssetRegistry.IsLoadingAssets())
	{
		AssetRegistry.SearchAllAssets(true);
	}

	TArray<FAssetData> Assets;
	
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = bRecursive;
	Filter.bIncludeOnlyOnDiskAssets = false;
	Filter.PackagePaths.Add(FName(*FolderPath));

	AssetRegistry.GetAssets(Filter, Assets);
	
	UE_LOG(LogTemp, Verbose, TEXT("Found %d blueprint assets in folder: %s (recursive: %s)"), 
		Assets.Num(), *FolderPath, bRecursive ? TEXT("true") : TEXT("false"));
	
	return Assets;
}

bool FStaticLinter::ShouldProcessAsset(const FAssetData& AssetData, const FScanConfiguration& Config) const
{
	FString AssetPath = AssetData.GetObjectPathString();
	
	// Check exclude paths
	for (const FString& ExcludePath : Config.ExcludePaths)
	{
		if (AssetPath.Contains(ExcludePath))
		{
			return false;
		}
	}
	
	// Check include paths (if specified)
	if (Config.IncludePaths.Num() > 0)
	{
		bool bInIncludePath = false;
		for (const FString& IncludePath : Config.IncludePaths)
		{
			if (AssetPath.Contains(IncludePath))
			{
				bInIncludePath = true;
				break;
			}
		}
		
		if (!bInIncludePath)
		{
			return false;
		}
	}
	
	return true;
}

ESeverity FStaticLinter::CalculateIssueSeverity(ELintIssueType Type, int32 Count) const
{
	switch (Type)
	{
		case ELintIssueType::DeadNode:
			return ESeverity::Low;
			
		case ELintIssueType::OrphanNode:
			return ESeverity::Low;
			
		case ELintIssueType::CastAbuse:
			return ESeverity::Medium;
			
		case ELintIssueType::TickAbuse:
			if (Count > 50)
				return ESeverity::Critical;
			else if (Count > 25)
				return ESeverity::High;
			else if (Count > 10)
				return ESeverity::Medium;
			else
				return ESeverity::Low;
			
		default:
			return ESeverity::Low;
	}
}

void FStaticLinter::UpdateScanProgress(int32 ProcessedAssets, int32 TotalAssets, const FString& CurrentAssetName)
{
	CurrentProgress.ProcessedAssets = ProcessedAssets;
	CurrentProgress.TotalAssets = TotalAssets;
	CurrentProgress.ProgressPercentage = TotalAssets > 0 ? (float)ProcessedAssets / (float)TotalAssets : 0.0f;
	
	if (!CurrentAssetName.IsEmpty())
	{
		CurrentProgress.CurrentAsset = CurrentAssetName;
	}
	
	// Calculate estimated time remaining
	if (ProcessedAssets > 0)
	{
		FTimespan ElapsedTime = FDateTime::Now() - CurrentProgress.StartTime;
		double ElapsedSeconds = ElapsedTime.GetTotalSeconds();
		double AverageTimePerAsset = ElapsedSeconds / ProcessedAssets;
		int32 RemainingAssets = TotalAssets - ProcessedAssets;
		CurrentProgress.EstimatedTimeRemaining = RemainingAssets * AverageTimePerAsset;
	}
	else
	{
		CurrentProgress.EstimatedTimeRemaining = 0.0f;
	}
	
	OnScanProgress.Broadcast(ProcessedAssets, TotalAssets);
	
	UE_LOG(LogTemp, Verbose, TEXT("Scan progress: %d/%d (%.1f%%) - ETA: %.1fs - Current: %s"), 
		ProcessedAssets, TotalAssets, CurrentProgress.ProgressPercentage * 100.0f, 
		CurrentProgress.EstimatedTimeRemaining, *CurrentProgress.CurrentAsset);
}

void FStaticLinter::CompleteScan(const TArray<FLintIssue>& AllIssues)
{
	// Check if we're being called during destruction
	// If bScanInProgress is already false and bCancelRequested is false, it means
	// CancelScan() was called and we should not proceed
	if (!bScanInProgress && !bCancelRequested)
	{
		return;
	}

	{
		FScopeLock Lock(&IssuesLock);
		Issues = AllIssues;
	}

	CurrentProgress.IssuesFound = Issues.Num();
	CurrentProgress.bIsCompleted = true;
	bScanInProgress = false;
	bCancelRequested = false;

	// Mark task as complete - this allows safe destruction
	bTaskComplete = true;

	FTimespan TotalTime = FDateTime::Now() - CurrentProgress.StartTime;
	double TotalSeconds = TotalTime.GetTotalSeconds();

	OnScanComplete.Broadcast(Issues);

	UE_LOG(LogTemp, Log, TEXT("Scan completed: %d issues found in %d assets (%.2fs total, %.3fs per asset)"),
		Issues.Num(), CurrentProgress.TotalAssets, TotalSeconds,
		CurrentProgress.TotalAssets > 0 ? TotalSeconds / CurrentProgress.TotalAssets : 0.0);

	// Clear self-reference now that scan is complete
	// This is safe because CompleteScan is called on the game thread after DoWork finishes
	SelfReference.Reset();
}

TArray<UEdGraph*> FStaticLinter::GetAllGraphs(UBlueprint* Blueprint) const
{
	TArray<UEdGraph*> AllGraphs;
	
	if (!Blueprint)
	{
		return AllGraphs;
	}

	// Add ubergraph pages (event graphs)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph)
		{
			AllGraphs.Add(Graph);
		}
	}

	// Add function graphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph)
		{
			AllGraphs.Add(Graph);
		}
	}

	// Add macro graphs
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (Graph)
		{
			AllGraphs.Add(Graph);
		}
	}

	return AllGraphs;
}