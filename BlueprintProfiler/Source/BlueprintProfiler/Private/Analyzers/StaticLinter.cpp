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

/**
 * Async task for blueprint scanning
 */
class FScanTask : public FNonAbandonableTask
{
public:
	FScanTask(FStaticLinter* InLinter, const TArray<FAssetData>& InAssets, const FScanConfiguration& InConfig)
		: Linter(InLinter)
		, Assets(InAssets)
		, Config(InConfig)
	{
	}

	void DoWork()
	{
		if (!Linter)
		{
			return;
		}

		TArray<FLintIssue> AllIssues;
		int32 ProcessedCount = 0;

		for (const FAssetData& AssetData : Assets)
		{
			if (Linter->IsCancelRequested())
			{
				break;
			}

			TArray<FLintIssue> AssetIssues;
			Linter->ProcessBlueprint(AssetData, Config, AssetIssues);
			
			{
				FScopeLock Lock(&Linter->GetIssuesLock());
				AllIssues.Append(AssetIssues);
			}

			ProcessedCount++;
			
			// Update progress on game thread with current asset name
			FString CurrentAssetName = AssetData.AssetName.ToString();
			AsyncTask(ENamedThreads::GameThread, [this, ProcessedCount, CurrentAssetName]()
			{
				if (Linter)
				{
					Linter->UpdateScanProgress(ProcessedCount, Assets.Num(), CurrentAssetName);
				}
			});
			
			// Add small delay to prevent overwhelming the system
			if (Config.bUseMultiThreading && ProcessedCount % 10 == 0)
			{
				FPlatformProcess::Sleep(0.001f); // 1ms delay every 10 assets
			}
		}

		// Complete scan on game thread
		AsyncTask(ENamedThreads::GameThread, [this, AllIssues]()
		{
			if (Linter)
			{
				Linter->CompleteScan(AllIssues);
			}
		});
	}

	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FScanTask, STATGROUP_ThreadPoolAsyncTasks);
	}

private:
	FStaticLinter* Linter;
	TArray<FAssetData> Assets;
	FScanConfiguration Config;
};

FStaticLinter::FStaticLinter()
	: bScanInProgress(false)
	, bCancelRequested(false)
{
}

FStaticLinter::~FStaticLinter()
{
	CancelScan();
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
			// Wait for task to complete gracefully
			CurrentScanTask->EnsureCompletion();
			CurrentScanTask.Reset();
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
	bScanInProgress = true;
	bCancelRequested = false;
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
		// Use async task for multi-threading
		CurrentScanTask = MakeShared<FAsyncTask<FScanTask>>(this, Assets, Config);
		CurrentScanTask->StartBackgroundTask();
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

	UE_LOG(LogTemp, Verbose, TEXT("Processing blueprint: %s"), *CurrentProgress.CurrentAsset);

	// Load the blueprint with error handling
	UBlueprint* Blueprint = nullptr;
	try
	{
		Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
	}
	catch (...)
	{
		UE_LOG(LogTemp, Error, TEXT("Exception occurred while loading blueprint: %s"), *AssetData.GetObjectPathString());
		return;
	}

	if (!Blueprint)
	{
		UE_LOG(LogTemp, Warning, TEXT("Failed to load blueprint: %s"), *AssetData.GetObjectPathString());
		return;
	}

	// Validate blueprint state
	if (!Blueprint->GeneratedClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("Blueprint has no generated class: %s"), *Blueprint->GetName());
		return;
	}

	// Check if blueprint is valid for analysis
	if (Blueprint->UbergraphPages.Num() == 0 && Blueprint->FunctionGraphs.Num() == 0)
	{
		UE_LOG(LogTemp, Verbose, TEXT("Blueprint has no graphs to analyze: %s"), *Blueprint->GetName());
		return;
	}

	UE_LOG(LogTemp, Verbose, TEXT("Processing blueprint: %s (%d graphs)"), 
		*Blueprint->GetName(), Blueprint->UbergraphPages.Num() + Blueprint->FunctionGraphs.Num());

	// Run enabled checks with error handling
	try
	{
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
				// Skip built-in events like BeginPlay, Tick, etc.
				FName EventName = EventNode->GetFunctionName();
				if (EventName != TEXT("ReceiveBeginPlay") && 
					EventName != TEXT("ReceiveTick") && 
					EventName != TEXT("ReceiveEndPlay") &&
					EventName != TEXT("BeginPlay") &&
					EventName != TEXT("Tick") &&
					EventName != TEXT("EndPlay"))
				{
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
						Issue.Description = FString::Printf(TEXT("Custom event '%s' is defined but never called"), *Issue.NodeName);
						Issue.Severity = CalculateIssueSeverity(ELintIssueType::DeadNode);
						Issue.NodeGuid = EventNode->NodeGuid;
						
						OutIssues.Add(Issue);
					}
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

			// Check for pure nodes (computation nodes) without execution connections
			if (UK2Node* K2Node = Cast<UK2Node>(Node))
			{
				if (K2Node->IsNodePure())
				{
					bool bHasOutputConnections = false;
					bool bHasInputConnections = false;
					
					// Check if any output pins are connected
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

					// A pure node is orphaned if it has no output connections
					// or if it has no input connections (making it useless)
					if (!bHasOutputConnections)
					{
						FLintIssue Issue;
						Issue.Type = ELintIssueType::OrphanNode;
						Issue.BlueprintPath = Blueprint->GetPathName();
						Issue.NodeName = K2Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
						Issue.Description = FString::Printf(TEXT("Pure node '%s' has no output connections and serves no purpose"), *Issue.NodeName);
						Issue.Severity = CalculateIssueSeverity(ELintIssueType::OrphanNode);
						Issue.NodeGuid = K2Node->NodeGuid;
						
						OutIssues.Add(Issue);
					}
					else if (!bHasInputConnections && K2Node->Pins.Num() > 1) // Has pins but no inputs
					{
						// Check if this is a constant/literal node (which is OK to have no inputs)
						bool bIsConstantNode = false;
						FString NodeClass = K2Node->GetClass()->GetName();
						
						// Skip constant/literal nodes as they're expected to have no inputs
						if (NodeClass.Contains(TEXT("Literal")) || 
							NodeClass.Contains(TEXT("Constant")) ||
							K2Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(TEXT("Get")))
						{
							bIsConstantNode = true;
						}

						if (!bIsConstantNode)
						{
							FLintIssue Issue;
							Issue.Type = ELintIssueType::OrphanNode;
							Issue.BlueprintPath = Blueprint->GetPathName();
							Issue.NodeName = K2Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
							Issue.Description = FString::Printf(TEXT("Pure node '%s' has no input connections but expects inputs"), *Issue.NodeName);
							Issue.Severity = CalculateIssueSeverity(ELintIssueType::OrphanNode);
							Issue.NodeGuid = K2Node->NodeGuid;
							
							OutIssues.Add(Issue);
						}
					}
				}
			}
			// Also check for non-pure nodes that are completely disconnected
			else if (UEdGraphNode* GraphNode = Cast<UEdGraphNode>(Node))
			{
				bool bHasAnyConnections = false;
				bool bHasExecutionPins = false;
				
				for (UEdGraphPin* Pin : GraphNode->Pins)
				{
					if (Pin)
					{
						if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
						{
							bHasExecutionPins = true;
						}
						
						if (Pin->LinkedTo.Num() > 0)
						{
							bHasAnyConnections = true;
						}
					}
				}

				// If node has execution pins but no connections at all, it's orphaned
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
						Issue.Description = FString::Printf(TEXT("Node '%s' has execution pins but no connections"), *Issue.NodeName);
						Issue.Severity = CalculateIssueSeverity(ELintIssueType::OrphanNode);
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
	{
		FScopeLock Lock(&IssuesLock);
		Issues = AllIssues;
	}
	
	CurrentProgress.IssuesFound = Issues.Num();
	CurrentProgress.bIsCompleted = true;
	bScanInProgress = false;
	bCancelRequested = false;
	
	if (CurrentScanTask.IsValid())
	{
		CurrentScanTask.Reset();
	}
	
	FTimespan TotalTime = FDateTime::Now() - CurrentProgress.StartTime;
	double TotalSeconds = TotalTime.GetTotalSeconds();
	
	OnScanComplete.Broadcast(Issues);
	
	UE_LOG(LogTemp, Log, TEXT("Scan completed: %d issues found in %d assets (%.2fs total, %.3fs per asset)"), 
		Issues.Num(), CurrentProgress.TotalAssets, TotalSeconds, 
		CurrentProgress.TotalAssets > 0 ? TotalSeconds / CurrentProgress.TotalAssets : 0.0);
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