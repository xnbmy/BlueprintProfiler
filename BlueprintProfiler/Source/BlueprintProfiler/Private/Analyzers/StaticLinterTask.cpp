#include "Analyzers/StaticLinter.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"

FScanTask::FScanTask(TSharedPtr<FStaticLinter> InLinter, const TArray<FAssetData>& InAssets, const FScanConfiguration& InConfig)
	: LinterWeak(InLinter)
	, Assets(InAssets)
	, Config(InConfig)
{
}

void FScanTask::DoWork()
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
