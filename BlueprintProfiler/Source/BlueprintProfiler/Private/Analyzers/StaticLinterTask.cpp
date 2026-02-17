// Copyright xnbmy 2026. All Rights Reserved.

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
	// Process assets in batches to avoid blocking the game thread for too long
	TArray<FLintIssue> AllIssues;
	FThreadSafeCounter ProcessedCount;
	int32 TotalAssets = Assets.Num();
	int32 CurrentIndex = 0;

	UE_LOG(LogTemp, Log, TEXT("FScanTask: Starting to process %d assets with frame splitting"), TotalAssets);

	// Process assets one by one, yielding control back to the game thread between each asset
	while (CurrentIndex < TotalAssets && !IsCancelRequested())
	{
		TSharedPtr<FStaticLinter> LinterPin = LinterWeak.Pin();
		if (!LinterPin.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("FScanTask: Linter was destroyed, aborting scan"));
			return;
		}

		if (LinterPin->IsCancelRequested())
		{
			UE_LOG(LogTemp, Log, TEXT("FScanTask: Scan was cancelled after %d assets"), ProcessedCount.GetValue());
			break;
		}

		const FAssetData& AssetData = Assets[CurrentIndex];

		// Process this blueprint on the game thread (required for blueprint loading)
		// But use a synchronous approach within the async task
		TArray<FLintIssue> AssetIssues;
		
		// Execute on game thread but wait for completion
		FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(false);
		bool bProcessed = false;
		
		AsyncTask(ENamedThreads::GameThread, [this, &AssetData, &AssetIssues, &LinterPin, &CompletionEvent, &bProcessed]()
		{
			if (LinterPin.IsValid())
			{
				LinterPin->ProcessBlueprint(AssetData, Config, AssetIssues);
				bProcessed = true;
			}
			CompletionEvent->Trigger();
		});

		// Wait for game thread processing with timeout to allow UI updates
		while (!CompletionEvent->Wait(10)) // 10ms timeout to allow UI thread to process
		{
			// Check cancellation during wait
			TSharedPtr<FStaticLinter> CheckLinter = LinterWeak.Pin();
			if (!CheckLinter.IsValid() || CheckLinter->IsCancelRequested())
			{
				break;
			}
		}
		
		FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);

		if (bProcessed)
		{
			// Accumulate issues
			{
				FScopeLock Lock(&LinterPin->GetIssuesLock());
				AllIssues.Append(AssetIssues);
			}

			int32 CurrentCount = ProcessedCount.Increment();

			// Update progress on game thread
			AsyncTask(ENamedThreads::GameThread, [this, CurrentCount, TotalAssets, AssetData]()
			{
				TSharedPtr<FStaticLinter> UpdateLinter = LinterWeak.Pin();
				if (UpdateLinter.IsValid())
				{
					UpdateLinter->UpdateScanProgress(CurrentCount, TotalAssets, AssetData.AssetName.ToString());
				}
			});

			// Log progress for debugging
			UE_LOG(LogTemp, Log, TEXT("FScanTask: Processed %d/%d - %s (%d issues found)"),
				CurrentCount, TotalAssets, *AssetData.AssetName.ToString(), AssetIssues.Num());
		}

		CurrentIndex++;

		// Small sleep to allow UI thread to process events
		FPlatformProcess::Sleep(0.001f); // 1ms sleep between assets
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
		TSharedPtr<FStaticLinter> CompleteLinter = LinterWeak.Pin();
		if (CompleteLinter.IsValid())
		{
			CompleteLinter->CompleteScan(AllIssues);
		}
	});
}

bool FScanTask::IsCancelRequested() const
{
	TSharedPtr<FStaticLinter> LinterPin = LinterWeak.Pin();
	return LinterPin.IsValid() && LinterPin->IsCancelRequested();
}
