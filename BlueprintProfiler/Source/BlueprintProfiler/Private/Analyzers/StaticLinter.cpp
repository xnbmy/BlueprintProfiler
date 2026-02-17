// Copyright xnbmy 2026. All Rights Reserved.

#include "Analyzers/StaticLinter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/GameInstance.h"
#include "Async/Async.h"
#include "Misc/DateTime.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"

// Forward declarations
class FScanTask;

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
