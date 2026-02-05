#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Data/ProfilerDataTypes.h"

class SButton;
class STextBlock;
class SProgressBar;

/**
 * Main Blueprint Profiler Dashboard Widget
 * Provides unified interface for all analysis results
 */
class BLUEPRINTPROFILER_API SBlueprintProfilerWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBlueprintProfilerWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	~SBlueprintProfilerWidget();  // 析构函数，用于清理定时器

	// Data management
	void RefreshData();
	void SetRuntimeData(const TArray<FNodeExecutionData>& Data);
	void SetLintIssues(const TArray<FLintIssue>& Issues);
	void SetMemoryData(const TArray<FMemoryAnalysisResult>& Data);

private:
	// UI event handlers
	FReply OnStartRuntimeRecording();
	FReply OnStopRuntimeRecording();
	FReply OnPauseRuntimeRecording();
	FReply OnResumeRuntimeRecording();
	FReply OnResetRuntimeData();
	FReply OnSaveSessionData();
	FReply OnLoadSessionData();
	FReply OnClearSessionHistory();
	FReply OnToggleAutoStartPIE();
	FReply OnToggleAutoStopPIE();
	FReply OnStartStaticScan();
	FReply OnScanSelectedFolders();
	FReply OnCancelStaticScan();
	FReply OnStartMemoryAnalysis();
	FReply OnExportToCSV();
	FReply OnExportToJSON();
	FReply OnRefreshData();

	// List view handlers
	TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<FProfilerDataItem> Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnItemDoubleClicked(TSharedPtr<FProfilerDataItem> Item);
	void OnSelectionChanged(TSharedPtr<FProfilerDataItem> Item, ESelectInfo::Type SelectInfo);

	// Filter and search handlers
	void OnSearchTextChanged(const FText& Text);
	void OnSortSelectionChanged(TSharedPtr<FString> Selection, ESelectInfo::Type SelectInfo);
	void OnFilterSelectionChanged(TSharedPtr<FString> Selection, ESelectInfo::Type SelectInfo);
	void OnHideEngineNodesChanged(ECheckBoxState NewState);

	// Data processing
	void UpdateFilteredData();
	void SortData(const FString& SortBy);
	void FilterData(const FString& FilterBy);
	void ClearFilters();
	void ApplyQuickFilter(const FString& FilterType);
	bool MatchesSearchCriteria(TSharedPtr<FProfilerDataItem> Item, const FString& SearchText) const;
	TSharedPtr<FProfilerDataItem> CreateDataItemFromRuntimeData(const FNodeExecutionData& Data);
	TSharedPtr<FProfilerDataItem> CreateDataItemFromLintIssue(const FLintIssue& Issue);
	TSharedPtr<FProfilerDataItem> CreateDataItemFromMemoryData(const FMemoryAnalysisResult& Data, UBlueprint* Blueprint);

	// Utility methods
	FLinearColor GetSeverityColor(ESeverity Severity) const;
	FText GetSeverityText(ESeverity Severity) const;
	FText GetDataTypeText(EProfilerDataType Type) const;
	FLinearColor GetDataTypeColor(EProfilerDataType Type) const;
	FText GetDataTypeAbbreviation(EProfilerDataType Type) const;
	FLinearColor GetCategoryColor(const FString& Category) const;
	FText GetFormattedValue(TSharedPtr<FProfilerDataItem> Item) const;
	FText GetFormattedTooltip(TSharedPtr<FProfilerDataItem> Item) const;
	FString GetBlueprintPath(TSharedPtr<FProfilerDataItem> Item) const;
	void JumpToNode(TSharedPtr<FProfilerDataItem> Item);

private:
	// UI Components
	TSharedPtr<SListView<TSharedPtr<FProfilerDataItem>>> DataListView;
	TSharedPtr<SSearchBox> SearchBox;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> SortComboBox;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> FilterComboBox;
	
	// Control buttons
	TSharedPtr<SButton> StartRecordingButton;
	TSharedPtr<SButton> StopRecordingButton;
	TSharedPtr<SButton> PauseRecordingButton;
	TSharedPtr<SButton> ResumeRecordingButton;
	TSharedPtr<SButton> ResetDataButton;
	TSharedPtr<SButton> SaveSessionButton;
	TSharedPtr<SButton> LoadSessionButton;
	TSharedPtr<SButton> ClearHistoryButton;
	TSharedPtr<SButton> StartScanButton;
	TSharedPtr<SButton> ScanFolderButton;
	TSharedPtr<SButton> CancelScanButton;
	TSharedPtr<SButton> StartMemoryAnalysisButton;
	
	// PIE integration controls
	TSharedPtr<class SCheckBox> AutoStartPIECheckBox;
	TSharedPtr<class SCheckBox> AutoStopPIECheckBox;
	
	// Session info display
	TSharedPtr<STextBlock> SessionNameText;
	TSharedPtr<STextBlock> SessionDurationText;
	TSharedPtr<STextBlock> RecordingStateText;
	
	// Status display
	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<STextBlock> ProgressDetailsText;
	TSharedPtr<STextBlock> TimeRemainingText;
	TSharedPtr<SProgressBar> ProgressBar;

	// Data management
	TArray<TSharedPtr<FProfilerDataItem>> AllDataItems;
	TArray<TSharedPtr<FProfilerDataItem>> FilteredDataItems;
	
	// Filter options
	TArray<TSharedPtr<FString>> SortOptions;
	TArray<TSharedPtr<FString>> FilterOptions;
	
	// Current state
	FString CurrentSearchText;
	FString CurrentSortBy;
	FString CurrentFilterBy;
	ERecordingState CurrentRecordingState;
	bool bIsStaticScanning;
	bool bIsMemoryAnalyzing;

	// Button state methods
	bool CanStartRecording() const;
	bool CanStopRecording() const;
	bool CanPauseRecording() const;
	bool CanResumeRecording() const;
	bool CanResetData() const;
	bool CanSaveSession() const;
	bool CanLoadSession() const;
	bool CanStartScan() const;
	bool CanCancelScan() const;
	bool CanStartMemoryAnalysis() const;
	bool HasDataToExport() const;

	// Recording state helpers
	FText GetRecordingStateText() const;
	FSlateColor GetRecordingStateColor() const;
	FText GetSessionInfoText() const;
	void UpdateRecordingStateDisplay();

	// Event callbacks for analyzers
	void OnStaticScanComplete(const TArray<FLintIssue>& Issues);
	void OnStaticScanProgress(int32 ProcessedAssets, int32 TotalAssets);

	// PIE event callbacks
	void OnPIEEnd(bool bIsSimulating = false);

	// Progress display helpers
	void UpdateProgressDisplay();
	FText GetProgressText() const;
	FText GetTimeRemainingText() const;

	// Analyzer references
	TSharedPtr<class FRuntimeProfiler> RuntimeProfiler;
	TSharedPtr<class FStaticLinter> StaticLinter;
	TSharedPtr<class FMemoryAnalyzer> MemoryAnalyzer;

	// UI refresh ticker
	FTSTicker::FDelegateHandle UIRefreshTickerHandle;
	bool TickUIRefresh(float DeltaTime);  // 返回 true 继续定时器，false 停止
};