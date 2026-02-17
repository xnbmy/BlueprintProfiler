// Copyright xnbmy 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/ProfilerDataTypes.h"
#include "Engine/Blueprint.h"
#include "AssetRegistry/AssetData.h"
#include "Async/AsyncWork.h"

DECLARE_MULTICAST_DELEGATE_OneParam(FOnScanComplete, const TArray<FLintIssue>& /* Issues */);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnScanProgress, int32 /* ProcessedAssets */, int32 /* TotalAssets */);

/**
 * Scan configuration for static linting
 */
struct BLUEPRINTPROFILER_API FScanConfiguration
{
	TArray<FString> IncludePaths;
	TArray<FString> ExcludePaths;
	TSet<ELintIssueType> EnabledChecks;
	bool bUseMultiThreading = true;
	int32 MaxConcurrentTasks = 4;

	FScanConfiguration()
	{
		// Enable all checks by default
		EnabledChecks.Add(ELintIssueType::DeadNode);
		EnabledChecks.Add(ELintIssueType::OrphanNode);
		EnabledChecks.Add(ELintIssueType::CastAbuse);
		EnabledChecks.Add(ELintIssueType::TickAbuse);
		EnabledChecks.Add(ELintIssueType::UnusedFunction);
	}
};

/**
 * Scan progress information
 */
struct BLUEPRINTPROFILER_API FScanProgress
{
	int32 TotalAssets = 0;
	int32 ProcessedAssets = 0;
	int32 IssuesFound = 0;
	FString CurrentAsset;
	float ProgressPercentage = 0.0f;
	float EstimatedTimeRemaining = 0.0f; // In seconds
	FDateTime StartTime;
	bool bIsCompleted = false;
	bool bWasCancelled = false;
};

/**
 * Static Linter - scans blueprint assets for code quality issues
 */
class BLUEPRINTPROFILER_API FStaticLinter
{
public:
	FStaticLinter();
	~FStaticLinter();

	// Scan control
	void ScanProject(const FScanConfiguration& Config = FScanConfiguration());
	void ScanFolder(const FString& FolderPath, const FScanConfiguration& Config = FScanConfiguration());
	void ScanSelectedFolders(const TArray<FString>& FolderPaths, const FScanConfiguration& Config = FScanConfiguration());
	void ScanBlueprints(const TArray<FAssetData>& Blueprints, const FScanConfiguration& Config = FScanConfiguration());
	void CancelScan();
	bool IsScanInProgress() const { return bScanInProgress; }

	// Results access
	TArray<FLintIssue> GetIssues() const { return Issues; }
	TArray<FLintIssue> GetIssuesByType(ELintIssueType Type) const;
	FScanProgress GetScanProgress() const { return CurrentProgress; }
	void ClearIssues() { Issues.Empty(); }

	// Events
	FOnScanComplete OnScanComplete;
	FOnScanProgress OnScanProgress;

	// Internal methods for async task (Exposed for testing)
	void UpdateScanProgress(int32 ProcessedAssets, int32 TotalAssets, const FString& CurrentAssetName = TEXT(""));

	// Internal methods accessed by async task
	void ProcessBlueprint(const FAssetData& AssetData, const FScanConfiguration& Config, TArray<FLintIssue>& OutIssues);
	void CompleteScan(const TArray<FLintIssue>& AllIssues);
	bool IsCancelRequested() const { return bCancelRequested; }
	FCriticalSection& GetIssuesLock() { return IssuesLock; }

private:
	// Scanning methods
	void StartAsyncScan(const TArray<FAssetData>& Assets, const FScanConfiguration& Config);

	// Detection methods
	void DetectDeadNodes(UBlueprint* Blueprint, TArray<FLintIssue>& OutIssues);
	void DetectOrphanNodes(UBlueprint* Blueprint, TArray<FLintIssue>& OutIssues);
	void DetectCastAbuse(UBlueprint* Blueprint, TArray<FLintIssue>& OutIssues);
	void DetectTickAbuse(UBlueprint* Blueprint, TArray<FLintIssue>& OutIssues);
	void DetectUnusedFunctions(UBlueprint* Blueprint, TArray<FLintIssue>& OutIssues);

	// Utility methods
	TArray<FAssetData> GetBlueprintAssets(const TArray<FString>& Paths) const;
	TArray<FAssetData> GetBlueprintAssetsInFolder(const FString& FolderPath, bool bRecursive = true) const;
	bool ShouldProcessAsset(const FAssetData& AssetData, const FScanConfiguration& Config) const;
	ESeverity CalculateIssueSeverity(ELintIssueType Type, int32 Count = 1) const;
	void CountConnectedNodes(UEdGraphNode* StartNode, TSet<UEdGraphNode*>& VisitedNodes, int32& NodeCount);
	TArray<UEdGraph*> GetAllGraphs(UBlueprint* Blueprint) const;
	
	// Context analysis helpers
	bool IsNodeInTickContext(UEdGraphNode* Node, TSet<UEdGraphNode*>& VisitedNodes) const;
	bool IsNodeInLoopContext(UEdGraphNode* Node, TSet<UEdGraphNode*>& VisitedNodes) const;
	bool IsNodeInFrequentlyCalledFunction(UEdGraphNode* Node, UEdGraph* Graph) const;
	bool IsHardReferenceCast(class UK2Node_DynamicCast* CastNode) const;

private:
	TArray<FLintIssue> Issues;
	FScanProgress CurrentProgress;
	bool bScanInProgress;
	bool bCancelRequested;

	// Async task management
	TSharedPtr<class FAsyncTask<class FScanTask>> CurrentScanTask;
	bool bTaskComplete;  // Tracks if the async CompleteScan has been called
	FCriticalSection IssuesLock;

	// Self-reference to keep object alive during async operations
	TSharedPtr<FStaticLinter> SelfReference;

	// Static members for cross-blueprint reference tracking
	static TSet<FName> ReferencedFunctions;
	static TMap<FName, int32> FunctionCallCount;
};

/**
 * Async task for blueprint scanning
 */
class FScanTask : public FNonAbandonableTask
{
public:
	FScanTask(TSharedPtr<FStaticLinter> InLinter, const TArray<FAssetData>& InAssets, const FScanConfiguration& InConfig);

	void DoWork();

	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FScanTask, STATGROUP_ThreadPoolAsyncTasks);
	}

	/** Check if scan has been cancelled */
	bool IsCancelRequested() const;

private:
	TWeakPtr<FStaticLinter> LinterWeak;
	TArray<FAssetData> Assets;
	FScanConfiguration Config;
};