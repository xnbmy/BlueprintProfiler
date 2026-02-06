#include "Analyzers/StaticLinter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Engine/GameInstance.h"
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

		if (Config.EnabledChecks.Contains(ELintIssueType::UnusedFunction))
		{
			DetectUnusedFunctions(Blueprint, OutIssues);
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
					// 获取节点标题
					FString NodeTitle = K2Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

					// 跳过特殊的纯节点（不需要输出连接）
					// 1. 变更路线节点（Set Return Value）- 特殊的纯节点
					if (NodeTitle.Contains(TEXT("变更路线")) ||
						NodeTitle.Contains(TEXT("Set Return")) ||
						NodeTitle.Contains(TEXT("Return")) ||
						NodeTitle.Contains(TEXT("返回")))
					{
						continue;
					}

					// 2. 字面量和常量节点
					FString NodeClass = K2Node->GetClass()->GetName();
					if (NodeClass.Contains(TEXT("Literal")) ||
						NodeClass.Contains(TEXT("Constant")))
					{
						continue;
					}

					// 3. 工具节点（Make, Select, Branch 等）
					if (NodeTitle.Contains(TEXT("Make")) ||
						NodeTitle.Contains(TEXT("Select")) ||
						NodeTitle.Contains(TEXT("Branch")) ||
						NodeTitle.Contains(TEXT("Break")) ||
						NodeTitle.Contains(TEXT("Append")))
					{
						continue;
					}

					bool bHasDataOutputConnections = false;

					// Check data output pins for connections (ignore exec pins for pure nodes)
					for (UEdGraphPin* Pin : K2Node->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Output &&
							Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec &&
							Pin->LinkedTo.Num() > 0)
						{
							bHasDataOutputConnections = true;
							break;
						}
					}

					// Report if pure node has no data output connections
					if (!bHasDataOutputConnections)
					{
						FLintIssue Issue;
						Issue.Type = ELintIssueType::OrphanNode;
						Issue.BlueprintPath = Blueprint->GetPathName();
						Issue.NodeName = NodeTitle;
						Issue.Description = FString::Printf(TEXT("纯节点 '%s' 的输出没有连接到任何节点"), *Issue.NodeName);
						Issue.Severity = ESeverity::Low; // 孤立节点不是严重问题
						Issue.NodeGuid = K2Node->NodeGuid;

						OutIssues.Add(Issue);
					}
				}
				// Check for nodes with execution pins (non-pure nodes)
				else
				{
					// 首先检查是否应该跳过此节点（在检查引脚连接之前）
					FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
					bool bShouldSkip = false;

					// 1. 跳过 Event 和 CustomEvent（入口节点）
					if (Node->IsA<UK2Node_Event>() || Node->IsA<UK2Node_CustomEvent>())
					{
						bShouldSkip = true;
					}
					// 2. 跳过构造脚本节点（按标题判断）
					else if (NodeTitle.Contains(TEXT("构造脚本")) ||
						NodeTitle.Contains(TEXT("Construction Script")))
					{
						bShouldSkip = true;
					}
					// 3. 跳过接口相关节点（通常是输入事件或增强输入节点）
					// 这些节点名称通常包含特定关键词
					else if (NodeTitle.Contains(TEXT("Thumbstick")) ||
						NodeTitle.Contains(TEXT("Touch")) ||
						NodeTitle.Contains(TEXT("Input Action")) ||
						NodeTitle.Contains(TEXT("Input Axis")) ||
						NodeTitle.Contains(TEXT("Enhanced Input")) ||
						NodeTitle.Contains(TEXT("IA_")) ||  // Input Action 缩写
						NodeTitle.Contains(TEXT("IM_")))    // Input Modifier 缩写
					{
						bShouldSkip = true;
					}

					// 如果不应该跳过，再检查执行引脚连接状态
					if (!bShouldSkip)
					{
						bool bHasExecInputConnected = false;
						bool bHasExecOutputConnected = false;
						bool bHasExecutionPins = false;

						// 检查所有执行引脚的连接状态
						for (UEdGraphPin* Pin : Node->Pins)
						{
							if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
							{
								bHasExecutionPins = true;
								if (Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0)
								{
									bHasExecInputConnected = true;
								}
								else if (Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0)
								{
									bHasExecOutputConnected = true;
								}
							}
						}

						// 只有当执行引脚完全未连接（既没有输入连接也没有输出连接）时才报告
						// 这会正确跳过：
						// - Event 节点（没有输入但有输出）
						// - Set Return Value 节点（可能有输入但没有输出）
						// - 其他正常连接的节点
						// 但会检测到：
						// - 完全未连接的孤立节点（如未连接的打印节点）
						if (bHasExecutionPins && !bHasExecInputConnected && !bHasExecOutputConnected)
						{
							FLintIssue Issue;
							Issue.Type = ELintIssueType::OrphanNode;
							Issue.BlueprintPath = Blueprint->GetPathName();
							Issue.NodeName = NodeTitle;
							Issue.Description = FString::Printf(TEXT("执行节点 '%s' 未连接到任何执行流程（孤立节点）"), *Issue.NodeName);
							Issue.Severity = ESeverity::High;
							Issue.NodeGuid = K2Node->NodeGuid;

							OutIssues.Add(Issue);
						}
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
	
	// Exclude GameInstance blueprints
	UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
	if (Blueprint)
	{
		if (Blueprint->GeneratedClass)
		{
			if (Blueprint->GeneratedClass->IsChildOf(UGameInstance::StaticClass()))
			{
				UE_LOG(LogTemp, Log, TEXT("Excluding GameInstance blueprint: %s"), *Blueprint->GetName());
				return false;
			}
		}
		else if (Blueprint->ParentClass)
		{
			if (Blueprint->ParentClass->IsChildOf(UGameInstance::StaticClass()))
			{
				UE_LOG(LogTemp, Log, TEXT("Excluding GameInstance blueprint: %s"), *Blueprint->GetName());
				return false;
			}
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

void FStaticLinter::DetectUnusedFunctions(UBlueprint* Blueprint, TArray<FLintIssue>& OutIssues)
{
	if (!Blueprint)
	{
		return;
	}

	// 0. 首先跳过接口 Blueprint（BPI_ 开头）
	//    接口中的函数不需要被调用，它们是被其他 Blueprint 实现的
	FString BlueprintName = Blueprint->GetName();
	if (BlueprintName.StartsWith(TEXT("BPI_")))
	{
		// 这是接口 Blueprint，不检测未引用函数
		return;
	}

	// 1. 跳过 GameInstance 蓝图
	if (Blueprint->GeneratedClass && Blueprint->GeneratedClass->IsChildOf(UGameInstance::StaticClass()))
	{
		return;
	}
	else if (Blueprint->ParentClass && Blueprint->ParentClass->IsChildOf(UGameInstance::StaticClass()))
	{
		return;
	}

	// 获取项目中所有 Blueprint 来检测函数调用
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	if (!AssetRegistry.IsLoadingAssets())
	{
		AssetRegistry.SearchAllAssets(true);
	}

	TArray<FAssetData> AllBlueprintAssets;
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	Filter.PackagePaths.Add(FName(TEXT("/Game")));
	AssetRegistry.GetAssets(Filter, AllBlueprintAssets);

	// 收集所有被调用的函数名
	TSet<FName> ReferencedFunctions;
	TMap<FName, int32> FunctionCallCount; // 函数名 -> 调用次数

	for (const FAssetData& AssetData : AllBlueprintAssets)
	{
		UBlueprint* BP = Cast<UBlueprint>(AssetData.GetAsset());
		if (!BP)
		{
			continue;
		}

		TArray<UEdGraph*> AllGraphs = GetAllGraphs(BP);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph)
			{
				continue;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_CallFunction* CallFuncNode = Cast<UK2Node_CallFunction>(Node))
				{
					FName FunctionName = CallFuncNode->FunctionReference.GetMemberName();
					if (FunctionName != NAME_None)
					{
						ReferencedFunctions.Add(FunctionName);
						FunctionCallCount.FindOrAdd(FunctionName)++;
					}
				}
				// 也检查自定义事件调用
				else if (UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
				{
					// 自定义事件可能被其他节点通过 GUID 调用
				}
			}
		}
	}

	// 检查当前 Blueprint 的函数是否被引用
	for (UEdGraph* FunctionGraph : Blueprint->FunctionGraphs)
	{
		if (!FunctionGraph)
		{
			continue;
		}

		FName FunctionName = FunctionGraph->GetFName();
		FString FunctionNameStr = FunctionName.ToString();

		// ========== 跳过系统自带函数和接口函数的判断标准 ==========

		// 1. 以 "Receive" 开头（事件）
		if (FunctionNameStr.StartsWith(TEXT("Receive")))
		{
			continue;
		}

		// 2. 常见的引擎接口函数命名模式
		//    这些函数通常来自接口，不应该被报告为未引用
		TArray<FString> InterfaceFunctionPatterns = {
			TEXT("GetPlayerState"),
			TEXT("GetController"),
			TEXT("GetPawn"),
			TEXT("GetCharacter"),
			TEXT("GetOwner"),
			TEXT("GetGameInstance"),
			TEXT("GetWorld"),
			TEXT("GetLevel"),
			TEXT("GetParent"),
			TEXT("IsA"),
			TEXT("IsValid"),
			TEXT("K2_"),           // K2_ 开头的函数通常是引擎生成的
			TEXT("Execute"),       // Execute 相关函数
			TEXT("Ubergraph"),     // Ubergraph 相关函数
			TEXT("UserConstructionScript"),
			TEXT("ConstructionScript"),
			// 常见接口前缀
			TEXT("HasAuthority"),   // INetworkInterface
			TEXT("GetNetConnection"),
			TEXT("GetNetMode"),
			TEXT("IsNetMode"),
		};

		bool bIsEnginePattern = false;
		for (const FString& Pattern : InterfaceFunctionPatterns)
		{
			if (FunctionNameStr.Contains(Pattern))
			{
				bIsEnginePattern = true;
				break;
			}
		}
		if (bIsEnginePattern)
		{
			continue;
		}

		// 3. 检查是否是 Override 函数（覆盖父类虚函数）
		bool bIsOverrideFunction = false;
		if (Blueprint->ParentClass)
		{
			UFunction* ParentFunction = Blueprint->ParentClass->FindFunctionByName(FunctionName);
			if (ParentFunction)
			{
				bIsOverrideFunction = true;
			}
		}

		if (bIsOverrideFunction)
		{
			continue;
		}

		// 4. 跳过来自引擎或第三方插件的 Blueprint
		FString BlueprintPath = Blueprint->GetPathName();
		if (BlueprintPath.StartsWith(TEXT("/Engine/")) ||
			BlueprintPath.StartsWith(TEXT("/Game/")) == false)
		{
			continue;
		}

		// 5. 跳过接口函数（通过检查继承链）
		{
			bool bIsInterfaceFunction = false;
			UClass* CurrentClass = Blueprint->GeneratedClass ? Blueprint->GeneratedClass : Blueprint->ParentClass;

			while (CurrentClass && !bIsInterfaceFunction)
			{
				// 检查当前类实现的所有接口
				for (const FImplementedInterface& Interface : CurrentClass->Interfaces)
				{
					if (Interface.Class && Interface.Class->FindFunctionByName(FunctionName))
					{
						bIsInterfaceFunction = true;
						break;
					}
				}

				if (bIsInterfaceFunction)
				{
					break;
				}

				// 向上检查父类
				CurrentClass = CurrentClass->GetSuperClass();
			}

			if (bIsInterfaceFunction)
			{
				continue;
			}
		}

		// 6. 检查函数是否被引用
		bool bIsReferenced = false;

		// 直接检查函数名是否在引用列表中
		if (ReferencedFunctions.Contains(FunctionName))
		{
			bIsReferenced = true;
		}

		// 7. 更全面的跨蓝图引用检查
		if (!bIsReferenced)
		{
			// 遍历所有其他 Blueprint，检查是否有对这个函数的引用
			for (const FAssetData& AssetData : AllBlueprintAssets)
			{
				UBlueprint* BP = Cast<UBlueprint>(AssetData.GetAsset());
				if (!BP || BP == Blueprint)
				{
					continue;  // 跳过自己
				}

				// 检查这个 Blueprint 是否引用了当前函数
				for (UEdGraph* Graph : GetAllGraphs(BP))
				{
					if (!Graph)
					{
						continue;
					}

					for (UEdGraphNode* Node : Graph->Nodes)
					{
						if (UK2Node_CallFunction* CallFuncNode = Cast<UK2Node_CallFunction>(Node))
						{
							// 检查函数引用
							FName CalledFunctionName = CallFuncNode->FunctionReference.GetMemberName();
							if (CalledFunctionName == FunctionName)
							{
								bIsReferenced = true;
								break;
							}

							// 检查函数引用的完整路径
							FString FunctionPath = CallFuncNode->FunctionReference.GetMemberName().ToString();
							if (FunctionPath.Contains(FunctionNameStr))
							{
								bIsReferenced = true;
								break;
							}
						}
					}

					if (bIsReferenced)
					{
						break;
					}
				}

				if (bIsReferenced)
				{
					break;
				}
			}
		}

		// 调试：输出未引用的函数名
		if (!bIsReferenced)
		{
			UE_LOG(LogTemp, Warning, TEXT("未引用函数: %s (在 Blueprint: %s)"), *FunctionNameStr, *Blueprint->GetName());
			UE_LOG(LogTemp, Warning, TEXT("  ReferencedFunctions 包含此函数: %d"), ReferencedFunctions.Contains(FunctionName));
		}

		if (bIsReferenced)
		{
			continue;  // 函数被引用，跳过
		}

		// 函数未被引用，报告问题
		FLintIssue Issue;
		Issue.Type = ELintIssueType::UnusedFunction;
		Issue.BlueprintPath = Blueprint->GetPathName();
		Issue.NodeName = FunctionNameStr;
		Issue.Description = FString::Printf(TEXT("函数 '%s' 已定义但从未被调用"), *FunctionNameStr);
		Issue.Severity = ESeverity::Medium; // 未引用的函数是中等严重度
		Issue.NodeGuid = FGuid(); // 函数图没有 NodeGuid，留空

		OutIssues.Add(Issue);
	}
}