#include "UI/SBlueprintProfilerWidget.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableViewBase.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "EditorStyleSet.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Analyzers/RuntimeProfiler.h"
#include "Analyzers/StaticLinter.h"
#include "Analyzers/MemoryAnalyzer.h"
#include "BlueprintEditorModule.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Developer/DesktopPlatform/Public/IDesktopPlatform.h"
#include "Developer/DesktopPlatform/Public/DesktopPlatformModule.h"
#include "Misc/FileHelper.h"
#include "Framework/Application/SlateApplication.h"
#include "AssetRegistry/AssetRegistryModule.h"

#define LOCTEXT_NAMESPACE "SBlueprintProfilerWidget"

void SBlueprintProfilerWidget::Construct(const FArguments& InArgs)
{
	// Initialize analyzers
	RuntimeProfiler = MakeShared<FRuntimeProfiler>();
	StaticLinter = MakeShared<FStaticLinter>();
	MemoryAnalyzer = MakeShared<FMemoryAnalyzer>();

	// Initialize state
	CurrentRecordingState = ERecordingState::Stopped;
	bIsStaticScanning = false;
	bIsMemoryAnalyzing = false;
	CurrentSortBy = TEXT("Severity");
	CurrentFilterBy = TEXT("All");

	// Initialize sort and filter options
	SortOptions.Add(MakeShared<FString>(TEXT("Name")));
	SortOptions.Add(MakeShared<FString>(TEXT("Blueprint")));
	SortOptions.Add(MakeShared<FString>(TEXT("Type")));
	SortOptions.Add(MakeShared<FString>(TEXT("Category")));
	SortOptions.Add(MakeShared<FString>(TEXT("Severity")));
	SortOptions.Add(MakeShared<FString>(TEXT("Value")));
	SortOptions.Add(MakeShared<FString>(TEXT("Execution Frequency")));
	SortOptions.Add(MakeShared<FString>(TEXT("Memory Usage")));

	FilterOptions.Add(MakeShared<FString>(TEXT("All")));
	FilterOptions.Add(MakeShared<FString>(TEXT("Runtime")));
	FilterOptions.Add(MakeShared<FString>(TEXT("Lint")));
	FilterOptions.Add(MakeShared<FString>(TEXT("Memory")));
	FilterOptions.Add(MakeShared<FString>(TEXT("Critical")));
	FilterOptions.Add(MakeShared<FString>(TEXT("High")));
	FilterOptions.Add(MakeShared<FString>(TEXT("Medium")));
	FilterOptions.Add(MakeShared<FString>(TEXT("Low")));
	FilterOptions.Add(MakeShared<FString>(TEXT("Hot Nodes")));
	FilterOptions.Add(MakeShared<FString>(TEXT("Dead Code")));
	FilterOptions.Add(MakeShared<FString>(TEXT("Performance Issues")));

	// Bind analyzer events
	StaticLinter->OnScanComplete.AddSP(this, &SBlueprintProfilerWidget::OnStaticScanComplete);
	StaticLinter->OnScanProgress.AddSP(this, &SBlueprintProfilerWidget::OnStaticScanProgress);

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(4.0f)
		[
			SNew(SSplitter)
			.Orientation(Orient_Vertical)
			
			// Control Panel
			+ SSplitter::Slot()
			.Value(0.3f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("DetailsView.CategoryTop"))
				.Padding(8.0f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SNew(SVerticalBox)
						
						// Runtime Profiler Controls
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 4)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("RuntimeProfilerTitle", "Runtime Profiler"))
							.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
						]
						
						// Session info display
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 2)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 8, 0)
							[
								SAssignNew(RecordingStateText, STextBlock)
								.Text(this, &SBlueprintProfilerWidget::GetRecordingStateText)
								.ColorAndOpacity(this, &SBlueprintProfilerWidget::GetRecordingStateColor)
								.Font(FAppStyle::GetFontStyle("BoldFont"))
							]
							
							+ SHorizontalBox::Slot()
							.FillWidth(1.0f)
							[
								SAssignNew(SessionNameText, STextBlock)
								.Text(this, &SBlueprintProfilerWidget::GetSessionInfoText)
								.Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
							]
						]
						
						// Recording control buttons
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 2)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 4, 0)
							[
								SAssignNew(StartRecordingButton, SButton)
								.Text(LOCTEXT("StartRecording", "Start"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnStartRuntimeRecording)
								.IsEnabled(this, &SBlueprintProfilerWidget::CanStartRecording)
								.ButtonStyle(FAppStyle::Get(), "FlatButton.Success")
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 4, 0)
							[
								SAssignNew(PauseRecordingButton, SButton)
								.Text(LOCTEXT("PauseRecording", "Pause"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnPauseRuntimeRecording)
								.IsEnabled(this, &SBlueprintProfilerWidget::CanPauseRecording)
								.ButtonStyle(FAppStyle::Get(), "FlatButton.Warning")
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 4, 0)
							[
								SAssignNew(ResumeRecordingButton, SButton)
								.Text(LOCTEXT("ResumeRecording", "Resume"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnResumeRuntimeRecording)
								.IsEnabled(this, &SBlueprintProfilerWidget::CanResumeRecording)
								.ButtonStyle(FAppStyle::Get(), "FlatButton.Primary")
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 4, 0)
							[
								SAssignNew(StopRecordingButton, SButton)
								.Text(LOCTEXT("StopRecording", "Stop"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnStopRuntimeRecording)
								.IsEnabled(this, &SBlueprintProfilerWidget::CanStopRecording)
								.ButtonStyle(FAppStyle::Get(), "FlatButton.Danger")
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SAssignNew(ResetDataButton, SButton)
								.Text(LOCTEXT("ResetData", "Reset"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnResetRuntimeData)
								.IsEnabled(this, &SBlueprintProfilerWidget::CanResetData)
							]
						]
						
						// Session management buttons
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 2)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 4, 0)
							[
								SAssignNew(SaveSessionButton, SButton)
								.Text(LOCTEXT("SaveSession", "Save Session"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnSaveSessionData)
								.IsEnabled(this, &SBlueprintProfilerWidget::CanSaveSession)
								.ToolTipText(LOCTEXT("SaveSessionTooltip", "Save current session data to file"))
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 4, 0)
							[
								SAssignNew(LoadSessionButton, SButton)
								.Text(LOCTEXT("LoadSession", "Load Session"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnLoadSessionData)
								.IsEnabled(this, &SBlueprintProfilerWidget::CanLoadSession)
								.ToolTipText(LOCTEXT("LoadSessionTooltip", "Load session data from file"))
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SAssignNew(ClearHistoryButton, SButton)
								.Text(LOCTEXT("ClearHistory", "Clear History"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnClearSessionHistory)
								.ToolTipText(LOCTEXT("ClearHistoryTooltip", "Clear all session history"))
							]
						]
						
						// PIE integration settings
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 8, 0, 4)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("PIEIntegrationTitle", "PIE Integration"))
							.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
						]
						
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 2)
						[
							SAssignNew(AutoStartPIECheckBox, SCheckBox)
							.Content()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("AutoStartPIE", "Auto-start recording on PIE"))
								.ToolTipText(LOCTEXT("AutoStartPIETooltip", "Automatically start recording when Play In Editor begins"))
							]
							.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
							{
								if (RuntimeProfiler.IsValid())
								{
									RuntimeProfiler->SetAutoStartOnPIE(NewState == ECheckBoxState::Checked);
								}
							})
						]
						
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 2)
						[
							SAssignNew(AutoStopPIECheckBox, SCheckBox)
							.IsChecked(ECheckBoxState::Checked) // Default to enabled
							.Content()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("AutoStopPIE", "Auto-stop recording on PIE end"))
								.ToolTipText(LOCTEXT("AutoStopPIETooltip", "Automatically stop recording when Play In Editor ends"))
							]
							.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
							{
								if (RuntimeProfiler.IsValid())
								{
									RuntimeProfiler->SetAutoStopOnPIEEnd(NewState == ECheckBoxState::Checked);
								}
							})
						]
						
						// Static Linter Controls
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 8, 0, 4)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("StaticLinterTitle", "Static Linter"))
							.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
						]
						
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 2)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 4, 0)
							[
								SAssignNew(StartScanButton, SButton)
								.Text(LOCTEXT("StartScan", "Scan Project"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnStartStaticScan)
								.IsEnabled(this, &SBlueprintProfilerWidget::CanStartScan)
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 4, 0)
							[
								SAssignNew(ScanFolderButton, SButton)
								.Text(LOCTEXT("ScanFolders", "Scan Folders"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnScanSelectedFolders)
								.IsEnabled(this, &SBlueprintProfilerWidget::CanStartScan)
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SAssignNew(CancelScanButton, SButton)
								.Text(LOCTEXT("CancelScan", "Cancel Scan"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnCancelStaticScan)
								.IsEnabled(this, &SBlueprintProfilerWidget::CanCancelScan)
							]
						]
						
						// Memory Analyzer Controls
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 8, 0, 4)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("MemoryAnalyzerTitle", "Memory Analyzer"))
							.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
						]
						
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 2)
						[
							SAssignNew(StartMemoryAnalysisButton, SButton)
							.Text(LOCTEXT("StartMemoryAnalysis", "Analyze Memory"))
							.OnClicked(this, &SBlueprintProfilerWidget::OnStartMemoryAnalysis)
							.IsEnabled(this, &SBlueprintProfilerWidget::CanStartMemoryAnalysis)
						]
						
						// Status and Progress
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 8, 0, 4)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("StatusTitle", "Status"))
							.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
						]
						
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 2)
						[
							SAssignNew(StatusText, STextBlock)
							.Text(LOCTEXT("StatusReady", "Ready"))
						]
						
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 2)
						[
							SAssignNew(ProgressDetailsText, STextBlock)
							.Text(FText::GetEmpty())
							.Visibility(EVisibility::Collapsed)
						]
						
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 2)
						[
							SAssignNew(TimeRemainingText, STextBlock)
							.Text(FText::GetEmpty())
							.Visibility(EVisibility::Collapsed)
						]
						
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 2)
						[
							SAssignNew(ProgressBar, SProgressBar)
							.Percent(0.0f)
							.Visibility(EVisibility::Collapsed)
						]
						
						// Export Controls
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 8, 0, 4)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("ExportTitle", "Export"))
							.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
						]
						
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 2)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 4, 0)
							[
								SNew(SButton)
								.Text(LOCTEXT("ExportCSV", "Export CSV"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnExportToCSV)
								.IsEnabled(this, &SBlueprintProfilerWidget::HasDataToExport)
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(SButton)
								.Text(LOCTEXT("ExportJSON", "Export JSON"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnExportToJSON)
								.IsEnabled(this, &SBlueprintProfilerWidget::HasDataToExport)
							]
						]
					]
				]
			]
			
			// Data Display Panel
			+ SSplitter::Slot()
			.Value(0.7f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("DetailsView.CategoryTop"))
				.Padding(8.0f)
				[
					SNew(SVerticalBox)
					
					// Filter and Search Controls
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0, 0, 0, 8)
					[
						SNew(SVerticalBox)
						
						// Search and main controls row
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 0, 0, 4)
						[
							SNew(SHorizontalBox)
							
							// Search Box
							+ SHorizontalBox::Slot()
							.FillWidth(1.0f)
							.Padding(0, 0, 4, 0)
							[
								SAssignNew(SearchBox, SSearchBox)
								.HintText(LOCTEXT("SearchHint", "Search nodes, blueprints, categories..."))
								.OnTextChanged(this, &SBlueprintProfilerWidget::OnSearchTextChanged)
							]
							
							// Clear Filters Button
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 4, 0)
							[
								SNew(SButton)
								.Text(LOCTEXT("ClearFilters", "Clear"))
								.ToolTipText(LOCTEXT("ClearFiltersTooltip", "Clear all search and filter criteria"))
								.OnClicked_Lambda([this]()
								{
									ClearFilters();
									return FReply::Handled();
								})
							]
							
							// Refresh Button
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(SButton)
								.Text(LOCTEXT("Refresh", "Refresh"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnRefreshData)
							]
						]
						
						// Sort and Filter controls row
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 0, 0, 4)
						[
							SNew(SHorizontalBox)
							
							// Sort Combo
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 4, 0)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("SortBy", "Sort by:"))
								.Margin(FMargin(0, 4, 4, 0))
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 8, 0)
							[
								SAssignNew(SortComboBox, SComboBox<TSharedPtr<FString>>)
								.OptionsSource(&SortOptions)
								.OnGenerateWidget_Lambda([](TSharedPtr<FString> Option)
								{
									return SNew(STextBlock).Text(FText::FromString(*Option));
								})
								.OnSelectionChanged(this, &SBlueprintProfilerWidget::OnSortSelectionChanged)
								[
									SNew(STextBlock)
									.Text_Lambda([this]()
									{
										return FText::FromString(CurrentSortBy);
									})
								]
							]
							
							// Filter Combo
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 4, 0)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("FilterBy", "Filter:"))
								.Margin(FMargin(0, 4, 4, 0))
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SAssignNew(FilterComboBox, SComboBox<TSharedPtr<FString>>)
								.OptionsSource(&FilterOptions)
								.OnGenerateWidget_Lambda([](TSharedPtr<FString> Option)
								{
									return SNew(STextBlock).Text(FText::FromString(*Option));
								})
								.OnSelectionChanged(this, &SBlueprintProfilerWidget::OnFilterSelectionChanged)
								[
									SNew(STextBlock)
									.Text_Lambda([this]()
									{
										return FText::FromString(CurrentFilterBy);
									})
								]
							]
						]
						
						// Quick filter buttons row
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(SHorizontalBox)
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 2, 0)
							[
								SNew(SButton)
								.Text(LOCTEXT("QuickFilterCritical", "Critical"))
								.ToolTipText(LOCTEXT("QuickFilterCriticalTooltip", "Show only critical severity items"))
								.ButtonStyle(FAppStyle::Get(), "FlatButton.Danger")
								.OnClicked_Lambda([this]()
								{
									ApplyQuickFilter(TEXT("CriticalOnly"));
									return FReply::Handled();
								})
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(2, 0, 2, 0)
							[
								SNew(SButton)
								.Text(LOCTEXT("QuickFilterRuntime", "Runtime"))
								.ToolTipText(LOCTEXT("QuickFilterRuntimeTooltip", "Show only runtime profiling data"))
								.ButtonStyle(FAppStyle::Get(), "FlatButton.Primary")
								.OnClicked_Lambda([this]()
								{
									ApplyQuickFilter(TEXT("RuntimeOnly"));
									return FReply::Handled();
								})
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(2, 0, 2, 0)
							[
								SNew(SButton)
								.Text(LOCTEXT("QuickFilterLint", "Lint"))
								.ToolTipText(LOCTEXT("QuickFilterLintTooltip", "Show only static analysis issues"))
								.ButtonStyle(FAppStyle::Get(), "FlatButton.Warning")
								.OnClicked_Lambda([this]()
								{
									ApplyQuickFilter(TEXT("LintOnly"));
									return FReply::Handled();
								})
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(2, 0, 0, 0)
							[
								SNew(SButton)
								.Text(LOCTEXT("QuickFilterMemory", "Memory"))
								.ToolTipText(LOCTEXT("QuickFilterMemoryTooltip", "Show only memory analysis data"))
								.ButtonStyle(FAppStyle::Get(), "FlatButton.Success")
								.OnClicked_Lambda([this]()
								{
									ApplyQuickFilter(TEXT("MemoryOnly"));
									return FReply::Handled();
								})
							]
						]
					]
					
					// Data List View
					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					[
						SAssignNew(DataListView, SListView<TSharedPtr<FProfilerDataItem>>)
						.ListItemsSource(&FilteredDataItems)
						.OnGenerateRow(this, &SBlueprintProfilerWidget::OnGenerateRow)
						.OnMouseButtonDoubleClick(this, &SBlueprintProfilerWidget::OnItemDoubleClicked)
						.OnSelectionChanged(this, &SBlueprintProfilerWidget::OnSelectionChanged)
						.HeaderRow
						(
							SNew(SHeaderRow)
							+ SHeaderRow::Column("Type")
							.DefaultLabel(LOCTEXT("TypeColumn", "Type"))
							.FillWidth(0.1f)
							
							+ SHeaderRow::Column("Name")
							.DefaultLabel(LOCTEXT("NameColumn", "Name"))
							.FillWidth(0.3f)
							
							+ SHeaderRow::Column("Blueprint")
							.DefaultLabel(LOCTEXT("BlueprintColumn", "Blueprint"))
							.FillWidth(0.25f)
							
							+ SHeaderRow::Column("Category")
							.DefaultLabel(LOCTEXT("CategoryColumn", "Category"))
							.FillWidth(0.15f)
							
							+ SHeaderRow::Column("Value")
							.DefaultLabel(LOCTEXT("ValueColumn", "Value"))
							.FillWidth(0.1f)
							
							+ SHeaderRow::Column("Severity")
							.DefaultLabel(LOCTEXT("SeverityColumn", "Severity"))
							.FillWidth(0.1f)
						)
					]
				]
			]
		]
	];

	// Set initial combo box selections
	if (SortOptions.Num() > 0)
	{
		SortComboBox->SetSelectedItem(SortOptions[3]); // Severity
	}
	if (FilterOptions.Num() > 0)
	{
		FilterComboBox->SetSelectedItem(FilterOptions[0]); // All
	}

	RefreshData();
}

// Data management methods
void SBlueprintProfilerWidget::RefreshData()
{
	AllDataItems.Empty();
	
	// Collect runtime data
	if (RuntimeProfiler.IsValid())
	{
		TArray<FNodeExecutionData> RuntimeData = RuntimeProfiler->GetExecutionData();
		for (const FNodeExecutionData& Data : RuntimeData)
		{
			AllDataItems.Add(CreateDataItemFromRuntimeData(Data));
		}
	}
	
	// Collect lint issues
	if (StaticLinter.IsValid())
	{
		TArray<FLintIssue> LintIssues = StaticLinter->GetIssues();
		for (const FLintIssue& Issue : LintIssues)
		{
			AllDataItems.Add(CreateDataItemFromLintIssue(Issue));
		}
	}
	
	UpdateFilteredData();
	
	if (DataListView.IsValid())
	{
		DataListView->RequestListRefresh();
	}
}

void SBlueprintProfilerWidget::SetRuntimeData(const TArray<FNodeExecutionData>& Data)
{
	// Clear existing runtime data
	AllDataItems.RemoveAll([](const TSharedPtr<FProfilerDataItem>& Item)
	{
		return Item.IsValid() && Item->Type == EProfilerDataType::Runtime;
	});
	
	// Add new runtime data
	for (const FNodeExecutionData& ExecutionData : Data)
	{
		AllDataItems.Add(CreateDataItemFromRuntimeData(ExecutionData));
	}
	
	UpdateFilteredData();
	
	if (DataListView.IsValid())
	{
		DataListView->RequestListRefresh();
	}
}

void SBlueprintProfilerWidget::SetLintIssues(const TArray<FLintIssue>& Issues)
{
	// Clear existing lint data
	AllDataItems.RemoveAll([](const TSharedPtr<FProfilerDataItem>& Item)
	{
		return Item.IsValid() && Item->Type == EProfilerDataType::Lint;
	});
	
	// Add new lint issues
	for (const FLintIssue& Issue : Issues)
	{
		AllDataItems.Add(CreateDataItemFromLintIssue(Issue));
	}
	
	UpdateFilteredData();
	
	if (DataListView.IsValid())
	{
		DataListView->RequestListRefresh();
	}
}

void SBlueprintProfilerWidget::SetMemoryData(const TArray<FMemoryAnalysisResult>& Data)
{
	// Clear existing memory data
	AllDataItems.RemoveAll([](const TSharedPtr<FProfilerDataItem>& Item)
	{
		return Item.IsValid() && Item->Type == EProfilerDataType::Memory;
	});
	
	// Add new memory data
	for (const FMemoryAnalysisResult& MemoryData : Data)
	{
		// Note: Need blueprint reference to create proper data item
		// This will be implemented when memory analyzer is complete
	}
	
	UpdateFilteredData();
	
	if (DataListView.IsValid())
	{
		DataListView->RequestListRefresh();
	}
}

// UI event handlers
FReply SBlueprintProfilerWidget::OnStartRuntimeRecording()
{
	if (RuntimeProfiler.IsValid())
	{
		FString SessionName = FString::Printf(TEXT("Manual_Session_%s"), *FDateTime::Now().ToString(TEXT("%H%M%S")));
		RuntimeProfiler->StartRecording(SessionName);
		CurrentRecordingState = RuntimeProfiler->GetRecordingState();
		
		UpdateRecordingStateDisplay();
		
		if (StatusText.IsValid())
		{
			StatusText->SetText(FText::Format(
				LOCTEXT("StatusRecording", "Recording runtime data - Session: {0}"),
				FText::FromString(SessionName)
			));
		}
	}
	return FReply::Handled();
}

FReply SBlueprintProfilerWidget::OnStopRuntimeRecording()
{
	if (RuntimeProfiler.IsValid())
	{
		RuntimeProfiler->StopRecording();
		CurrentRecordingState = RuntimeProfiler->GetRecordingState();
		
		// Refresh data to show recorded results
		SetRuntimeData(RuntimeProfiler->GetExecutionData());
		
		UpdateRecordingStateDisplay();
		
		if (StatusText.IsValid())
		{
			FRecordingSession Session = RuntimeProfiler->GetCurrentSession();
			StatusText->SetText(FText::Format(
				LOCTEXT("StatusRecordingStopped", "Recording stopped - Session: {0} (Duration: {1}s, Nodes: {2})"),
				FText::FromString(Session.SessionName),
				FText::AsNumber(static_cast<int64>(FMath::RoundToFloat(Session.Duration))),
				FText::AsNumber(Session.TotalNodesRecorded)
			));
		}
	}
	return FReply::Handled();
}

FReply SBlueprintProfilerWidget::OnPauseRuntimeRecording()
{
	if (RuntimeProfiler.IsValid())
	{
		RuntimeProfiler->PauseRecording();
		CurrentRecordingState = RuntimeProfiler->GetRecordingState();
		
		UpdateRecordingStateDisplay();
		
		if (StatusText.IsValid())
		{
			StatusText->SetText(LOCTEXT("StatusRecordingPaused", "Recording paused"));
		}
	}
	return FReply::Handled();
}

FReply SBlueprintProfilerWidget::OnResumeRuntimeRecording()
{
	if (RuntimeProfiler.IsValid())
	{
		RuntimeProfiler->ResumeRecording();
		CurrentRecordingState = RuntimeProfiler->GetRecordingState();
		
		UpdateRecordingStateDisplay();
		
		if (StatusText.IsValid())
		{
			StatusText->SetText(LOCTEXT("StatusRecordingResumed", "Recording resumed"));
		}
	}
	return FReply::Handled();
}

FReply SBlueprintProfilerWidget::OnResetRuntimeData()
{
	if (RuntimeProfiler.IsValid())
	{
		RuntimeProfiler->ResetData();
		CurrentRecordingState = RuntimeProfiler->GetRecordingState();
		
		// Clear runtime data from display
		AllDataItems.RemoveAll([](const TSharedPtr<FProfilerDataItem>& Item)
		{
			return Item.IsValid() && Item->Type == EProfilerDataType::Runtime;
		});
		
		UpdateFilteredData();
		UpdateRecordingStateDisplay();
		
		if (DataListView.IsValid())
		{
			DataListView->RequestListRefresh();
		}
		
		if (StatusText.IsValid())
		{
			StatusText->SetText(LOCTEXT("StatusDataReset", "Runtime data reset"));
		}
	}
	return FReply::Handled();
}

FReply SBlueprintProfilerWidget::OnSaveSessionData()
{
	if (RuntimeProfiler.IsValid())
	{
		RuntimeProfiler->SaveSessionData();
		
		if (StatusText.IsValid())
		{
			FRecordingSession Session = RuntimeProfiler->GetCurrentSession();
			StatusText->SetText(FText::Format(
				LOCTEXT("StatusSessionSaved", "Session data saved: {0}"),
				FText::FromString(Session.SessionName)
			));
		}
	}
	return FReply::Handled();
}

FReply SBlueprintProfilerWidget::OnLoadSessionData()
{
	if (RuntimeProfiler.IsValid())
	{
		// For now, load the most recent session file
		// In a full implementation, this would open a file picker dialog
		bool bLoaded = RuntimeProfiler->LoadSessionData(TEXT(""));
		
		if (StatusText.IsValid())
		{
			if (bLoaded)
			{
				StatusText->SetText(LOCTEXT("StatusSessionLoaded", "Session data loaded successfully"));
			}
			else
			{
				StatusText->SetText(LOCTEXT("StatusSessionLoadFailed", "Failed to load session data"));
			}
		}
	}
	return FReply::Handled();
}

FReply SBlueprintProfilerWidget::OnClearSessionHistory()
{
	if (RuntimeProfiler.IsValid())
	{
		RuntimeProfiler->ClearSessionHistory();
		
		if (StatusText.IsValid())
		{
			StatusText->SetText(LOCTEXT("StatusHistoryCleared", "Session history cleared"));
		}
	}
	return FReply::Handled();
}

FReply SBlueprintProfilerWidget::OnStartStaticScan()
{
	if (StaticLinter.IsValid())
	{
		bIsStaticScanning = true;
		
		if (StatusText.IsValid())
		{
			StatusText->SetText(LOCTEXT("StatusScanning", "Scanning blueprints..."));
		}
		
		if (ProgressBar.IsValid())
		{
			ProgressBar->SetVisibility(EVisibility::Visible);
			ProgressBar->SetPercent(0.0f);
		}
		
		if (ProgressDetailsText.IsValid())
		{
			ProgressDetailsText->SetVisibility(EVisibility::Visible);
		}
		
		if (TimeRemainingText.IsValid())
		{
			TimeRemainingText->SetVisibility(EVisibility::Visible);
		}
		
		// Start scan on game thread
		StaticLinter->ScanProject();
	}
	return FReply::Handled();
}

FReply SBlueprintProfilerWidget::OnScanSelectedFolders()
{
	if (StaticLinter.IsValid())
	{
		// For now, implement a simple folder selection - in a real implementation,
		// this would open a folder picker dialog
		TArray<FString> SelectedFolders;
		
		// Example folders - in practice, these would come from a folder picker dialog
		SelectedFolders.Add(TEXT("/Game/Blueprints"));
		SelectedFolders.Add(TEXT("/Game/Characters"));
		
		if (SelectedFolders.Num() > 0)
		{
			bIsStaticScanning = true;
			
			if (StatusText.IsValid())
			{
				StatusText->SetText(FText::Format(
					LOCTEXT("StatusScanningFolders", "Scanning {0} selected folders..."),
					FText::AsNumber(SelectedFolders.Num())
				));
			}
			
			if (ProgressBar.IsValid())
			{
				ProgressBar->SetVisibility(EVisibility::Visible);
				ProgressBar->SetPercent(0.0f);
			}
			
			if (ProgressDetailsText.IsValid())
			{
				ProgressDetailsText->SetVisibility(EVisibility::Visible);
			}
			
			if (TimeRemainingText.IsValid())
			{
				TimeRemainingText->SetVisibility(EVisibility::Visible);
			}
			
			// Start folder scan
			StaticLinter->ScanSelectedFolders(SelectedFolders);
		}
		else
		{
			if (StatusText.IsValid())
			{
				StatusText->SetText(LOCTEXT("StatusNoFoldersSelected", "No folders selected for scanning"));
			}
		}
	}
	return FReply::Handled();
}

FReply SBlueprintProfilerWidget::OnCancelStaticScan()
{
	if (StaticLinter.IsValid())
	{
		StaticLinter->CancelScan();
		bIsStaticScanning = false;
		
		if (StatusText.IsValid())
		{
			StatusText->SetText(LOCTEXT("StatusScanCancelled", "Scan cancelled - partial results preserved"));
		}
		
		if (ProgressBar.IsValid())
		{
			ProgressBar->SetVisibility(EVisibility::Collapsed);
		}
		
		if (ProgressDetailsText.IsValid())
		{
			ProgressDetailsText->SetVisibility(EVisibility::Collapsed);
		}
		
		if (TimeRemainingText.IsValid())
		{
			TimeRemainingText->SetVisibility(EVisibility::Collapsed);
		}
	}
	return FReply::Handled();
}

FReply SBlueprintProfilerWidget::OnStartMemoryAnalysis()
{
	if (MemoryAnalyzer.IsValid())
	{
		bIsMemoryAnalyzing = true;
		
		if (StatusText.IsValid())
		{
			StatusText->SetText(LOCTEXT("StatusAnalyzing", "Analyzing memory..."));
		}
		
		// Get all blueprint assets to analyze
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		
		TArray<FAssetData> BlueprintAssets;
		AssetRegistry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), BlueprintAssets);
		
		if (BlueprintAssets.Num() > 0)
		{
			// Analyze the first blueprint as an example
			if (UBlueprint* Blueprint = Cast<UBlueprint>(BlueprintAssets[0].GetAsset()))
			{
				// Perform synchronous analysis for now
				MemoryAnalyzer->AnalyzeBlueprint(Blueprint);
				
				// Get analysis results
				FMemoryAnalysisResult Result = MemoryAnalyzer->GetAnalysisResult(Blueprint);
				TArray<FLargeResourceReference> LargeResources = MemoryAnalyzer->GetLargeResourceReferences(10.0f);
				
				// Update UI with results
				TArray<FMemoryAnalysisResult> Results;
				Results.Add(Result);
				SetMemoryData(Results);
				
				if (StatusText.IsValid())
				{
					StatusText->SetText(FText::Format(
						LOCTEXT("StatusMemoryComplete", "Memory analysis complete. Found {0} large resources."),
						FText::AsNumber(LargeResources.Num())
					));
				}
			}
		}
		else
		{
			if (StatusText.IsValid())
			{
				StatusText->SetText(LOCTEXT("StatusNoBlueprints", "No blueprints found to analyze"));
			}
		}
		
		bIsMemoryAnalyzing = false;
	}
	return FReply::Handled();
}

FReply SBlueprintProfilerWidget::OnExportToCSV()
{
	if (!HasDataToExport())
	{
		return FReply::Handled();
	}

	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (DesktopPlatform)
	{
		TArray<FString> SaveFilenames;
		FString DefaultPath = FPaths::ProjectSavedDir();
		FString DefaultFile = FString::Printf(TEXT("BlueprintProfiler_%s.csv"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
		
		bool bOpened = DesktopPlatform->SaveFileDialog(
			FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
			TEXT("Export to CSV"),
			DefaultPath,
			DefaultFile,
			TEXT("CSV Files (*.csv)|*.csv"),
			EFileDialogFlags::None,
			SaveFilenames
		);

		if (bOpened && SaveFilenames.Num() > 0)
		{
			FString FilePath = SaveFilenames[0];
			FString CSVContent = TEXT("Type,Name,Blueprint,Category,Value,Severity\n");
			
			for (const TSharedPtr<FProfilerDataItem>& Item : FilteredDataItems)
			{
				if (Item.IsValid())
				{
					CSVContent += FString::Printf(TEXT("%s,%s,%s,%s,%.2f,%s\n"),
						*GetDataTypeText(Item->Type).ToString(),
						*Item->Name.Replace(TEXT(","), TEXT(" ")),
						*Item->BlueprintName,
						*Item->Category,
						Item->Value,
						*GetSeverityText(Item->Severity).ToString()
					);
				}
			}
			
			if (FFileHelper::SaveStringToFile(CSVContent, *FilePath))
			{
				if (StatusText.IsValid())
				{
					StatusText->SetText(FText::Format(
						LOCTEXT("StatusExportSuccess", "Exported {0} items to {1}"),
						FText::AsNumber(FilteredDataItems.Num()),
						FText::FromString(FPaths::GetCleanFilename(FilePath))
					));
				}
			}
			else
			{
				if (StatusText.IsValid())
				{
					StatusText->SetText(LOCTEXT("StatusExportFailed", "Failed to save CSV file"));
				}
			}
		}
	}
	return FReply::Handled();
}

FReply SBlueprintProfilerWidget::OnExportToJSON()
{
	if (!HasDataToExport())
	{
		return FReply::Handled();
	}

	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (DesktopPlatform)
	{
		TArray<FString> SaveFilenames;
		FString DefaultPath = FPaths::ProjectSavedDir();
		FString DefaultFile = FString::Printf(TEXT("BlueprintProfiler_%s.json"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
		
		bool bOpened = DesktopPlatform->SaveFileDialog(
			FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
			TEXT("Export to JSON"),
			DefaultPath,
			DefaultFile,
			TEXT("JSON Files (*.json)|*.json"),
			EFileDialogFlags::None,
			SaveFilenames
		);

		if (bOpened && SaveFilenames.Num() > 0)
		{
			FString FilePath = SaveFilenames[0];
			
			TSharedPtr<FJsonObject> RootObject = MakeShareable(new FJsonObject);
			TArray<TSharedPtr<FJsonValue>> ItemsArray;
			
			for (const TSharedPtr<FProfilerDataItem>& Item : FilteredDataItems)
			{
				if (Item.IsValid())
				{
					TSharedPtr<FJsonObject> ItemObject = MakeShareable(new FJsonObject);
					ItemObject->SetStringField(TEXT("Type"), GetDataTypeText(Item->Type).ToString());
					ItemObject->SetStringField(TEXT("Name"), Item->Name);
					ItemObject->SetStringField(TEXT("Blueprint"), Item->BlueprintName);
					ItemObject->SetStringField(TEXT("Category"), Item->Category);
					ItemObject->SetNumberField(TEXT("Value"), Item->Value);
					ItemObject->SetStringField(TEXT("Severity"), GetSeverityText(Item->Severity).ToString());
					
					ItemsArray.Add(MakeShareable(new FJsonValueObject(ItemObject)));
				}
			}
			
			RootObject->SetArrayField(TEXT("Items"), ItemsArray);
			RootObject->SetStringField(TEXT("ExportDate"), FDateTime::Now().ToString());
			RootObject->SetNumberField(TEXT("TotalItems"), static_cast<double>(FilteredDataItems.Num()));
			
			FString OutputString;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
			FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer);
			
			if (FFileHelper::SaveStringToFile(OutputString, *FilePath))
			{
				if (StatusText.IsValid())
				{
					StatusText->SetText(FText::Format(
						LOCTEXT("StatusExportSuccess", "Exported {0} items to {1}"),
						FText::AsNumber(FilteredDataItems.Num()),
						FText::FromString(FPaths::GetCleanFilename(FilePath))
					));
				}
			}
			else
			{
				if (StatusText.IsValid())
				{
					StatusText->SetText(LOCTEXT("StatusExportFailed", "Failed to save JSON file"));
				}
			}
		}
	}
	return FReply::Handled();
}

FReply SBlueprintProfilerWidget::OnRefreshData()
{
	RefreshData();
	
	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::Format(
			LOCTEXT("StatusRefreshed", "Refreshed - {0} items"),
			FText::AsNumber(FilteredDataItems.Num())
		));
	}
	
	return FReply::Handled();
}

// List view handlers
TSharedRef<ITableRow> SBlueprintProfilerWidget::OnGenerateRow(
	TSharedPtr<FProfilerDataItem> Item,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	if (!Item.IsValid())
	{
		return SNew(STableRow<TSharedPtr<FProfilerDataItem>>, OwnerTable);
	}

	return SNew(STableRow<TSharedPtr<FProfilerDataItem>>, OwnerTable)
		[
			SNew(SHorizontalBox)
			
			// Type with icon
			+ SHorizontalBox::Slot()
			.FillWidth(0.1f)
			.Padding(4, 2)
			.VAlign(VAlign_Center)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0, 0, 4, 0)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("NoBorder"))
					.BorderBackgroundColor(GetDataTypeColor(Item->Type))
					.Padding(2)
					[
						SNew(STextBlock)
						.Text(GetDataTypeAbbreviation(Item->Type))
						.Font(FAppStyle::GetFontStyle("SmallFont"))
						.ColorAndOpacity(FLinearColor::White)
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(GetDataTypeText(Item->Type))
					.Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
				]
			]
			
			// Name with tooltip
			+ SHorizontalBox::Slot()
			.FillWidth(0.3f)
			.Padding(4, 2)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->Name))
				.Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
				.ToolTipText(GetFormattedTooltip(Item))
				.AutoWrapText(false)
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
			
			// Blueprint with path info
			+ SHorizontalBox::Slot()
			.FillWidth(0.25f)
			.Padding(4, 2)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->BlueprintName))
				.Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
				.ToolTipText(FText::FromString(GetBlueprintPath(Item)))
				.AutoWrapText(false)
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
			
			// Category with formatted display
			+ SHorizontalBox::Slot()
			.FillWidth(0.15f)
			.Padding(4, 2)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->Category))
				.Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
				.ColorAndOpacity(GetCategoryColor(Item->Category))
			]
			
			// Value with formatted units
			+ SHorizontalBox::Slot()
			.FillWidth(0.1f)
			.Padding(4, 2)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(GetFormattedValue(Item))
				.Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
				.Justification(ETextJustify::Right)
			]
			
			// Severity with enhanced visual
			+ SHorizontalBox::Slot()
			.FillWidth(0.1f)
			.Padding(4, 2)
			.VAlign(VAlign_Center)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("DetailsView.CategoryMiddle"))
				.BorderBackgroundColor(GetSeverityColor(Item->Severity))
				.Padding(FMargin(6, 2))
				[
					SNew(STextBlock)
					.Text(GetSeverityText(Item->Severity))
					.Font(FAppStyle::GetFontStyle("SmallFont"))
					.ColorAndOpacity(FLinearColor::White)
					.Justification(ETextJustify::Center)
				]
			]
		];
}

void SBlueprintProfilerWidget::OnItemDoubleClicked(TSharedPtr<FProfilerDataItem> Item)
{
	if (Item.IsValid())
	{
		JumpToNode(Item);
	}
}

void SBlueprintProfilerWidget::OnSelectionChanged(
	TSharedPtr<FProfilerDataItem> Item,
	ESelectInfo::Type SelectInfo)
{
	// Handle selection change if needed
}

// Filter and search handlers
void SBlueprintProfilerWidget::OnSearchTextChanged(const FText& Text)
{
	CurrentSearchText = Text.ToString();
	UpdateFilteredData();
	
	if (DataListView.IsValid())
	{
		DataListView->RequestListRefresh();
	}
	
	// Update status to show search results count
	if (StatusText.IsValid())
	{
		if (CurrentSearchText.IsEmpty())
		{
			StatusText->SetText(FText::Format(
				LOCTEXT("StatusShowingAll", "Showing all {0} items"),
				FText::AsNumber(FilteredDataItems.Num())
			));
		}
		else
		{
			StatusText->SetText(FText::Format(
				LOCTEXT("StatusSearchResults", "Search '{0}': {1} of {2} items"),
				FText::FromString(CurrentSearchText),
				FText::AsNumber(FilteredDataItems.Num()),
				FText::AsNumber(AllDataItems.Num())
			));
		}
	}
}

void SBlueprintProfilerWidget::OnSortSelectionChanged(
	TSharedPtr<FString> Selection,
	ESelectInfo::Type SelectInfo)
{
	if (Selection.IsValid())
	{
		CurrentSortBy = *Selection;
		SortData(CurrentSortBy);
		
		if (DataListView.IsValid())
		{
			DataListView->RequestListRefresh();
		}
	}
}

void SBlueprintProfilerWidget::OnFilterSelectionChanged(
	TSharedPtr<FString> Selection,
	ESelectInfo::Type SelectInfo)
{
	if (Selection.IsValid())
	{
		CurrentFilterBy = *Selection;
		UpdateFilteredData();
		
		if (DataListView.IsValid())
		{
			DataListView->RequestListRefresh();
		}
	}
}
// Data processing methods
void SBlueprintProfilerWidget::UpdateFilteredData()
{
	FilteredDataItems.Empty();
	
	for (const TSharedPtr<FProfilerDataItem>& Item : AllDataItems)
	{
		if (!Item.IsValid())
		{
			continue;
		}
		
		// Apply search filter with enhanced matching
		if (!CurrentSearchText.IsEmpty())
		{
			if (!MatchesSearchCriteria(Item, CurrentSearchText))
			{
				continue;
			}
		}
		
		// Apply type/severity filter with enhanced options
		if (CurrentFilterBy != TEXT("All"))
		{
			bool bMatchesFilter = false;
			
			// Type filters
			if (CurrentFilterBy == TEXT("Runtime") && Item->Type == EProfilerDataType::Runtime)
			{
				bMatchesFilter = true;
			}
			else if (CurrentFilterBy == TEXT("Lint") && Item->Type == EProfilerDataType::Lint)
			{
				bMatchesFilter = true;
			}
			else if (CurrentFilterBy == TEXT("Memory") && Item->Type == EProfilerDataType::Memory)
			{
				bMatchesFilter = true;
			}
			// Severity filters
			else if (CurrentFilterBy == TEXT("Critical") && Item->Severity == ESeverity::Critical)
			{
				bMatchesFilter = true;
			}
			else if (CurrentFilterBy == TEXT("High") && Item->Severity == ESeverity::High)
			{
				bMatchesFilter = true;
			}
			else if (CurrentFilterBy == TEXT("Medium") && Item->Severity == ESeverity::Medium)
			{
				bMatchesFilter = true;
			}
			else if (CurrentFilterBy == TEXT("Low") && Item->Severity == ESeverity::Low)
			{
				bMatchesFilter = true;
			}
			// Category filters
			else if (CurrentFilterBy == TEXT("Hot Nodes") && 
					(Item->Category.Contains(TEXT("Hot")) || Item->Category.Contains(TEXT("High Execution"))))
			{
				bMatchesFilter = true;
			}
			else if (CurrentFilterBy == TEXT("Dead Code") && 
					(Item->Category.Contains(TEXT("Dead")) || Item->Category.Contains(TEXT("Orphan"))))
			{
				bMatchesFilter = true;
			}
			else if (CurrentFilterBy == TEXT("Performance Issues") && 
					(Item->Category.Contains(TEXT("Cast")) || Item->Category.Contains(TEXT("Tick"))))
			{
				bMatchesFilter = true;
			}
			
			if (!bMatchesFilter)
			{
				continue;
			}
		}
		
		FilteredDataItems.Add(Item);
	}
	
	// Apply current sort
	SortData(CurrentSortBy);
}

void SBlueprintProfilerWidget::SortData(const FString& SortBy)
{
	if (SortBy == TEXT("Name"))
	{
		FilteredDataItems.Sort([](const TSharedPtr<FProfilerDataItem>& A, const TSharedPtr<FProfilerDataItem>& B)
		{
			if (!A.IsValid() || !B.IsValid()) return false;
			return A->Name.Compare(B->Name, ESearchCase::IgnoreCase) < 0;
		});
	}
	else if (SortBy == TEXT("Blueprint"))
	{
		FilteredDataItems.Sort([](const TSharedPtr<FProfilerDataItem>& A, const TSharedPtr<FProfilerDataItem>& B)
		{
			if (!A.IsValid() || !B.IsValid()) return false;
			int32 BlueprintCompare = A->BlueprintName.Compare(B->BlueprintName, ESearchCase::IgnoreCase);
			if (BlueprintCompare == 0)
			{
				// Secondary sort by name if blueprints are the same
				return A->Name.Compare(B->Name, ESearchCase::IgnoreCase) < 0;
			}
			return BlueprintCompare < 0;
		});
	}
	else if (SortBy == TEXT("Type"))
	{
		FilteredDataItems.Sort([](const TSharedPtr<FProfilerDataItem>& A, const TSharedPtr<FProfilerDataItem>& B)
		{
			if (!A.IsValid() || !B.IsValid()) return false;
			if ((int32)A->Type == (int32)B->Type)
			{
				// Secondary sort by severity if types are the same
				return (int32)A->Severity > (int32)B->Severity;
			}
			return (int32)A->Type < (int32)B->Type;
		});
	}
	else if (SortBy == TEXT("Category"))
	{
		FilteredDataItems.Sort([](const TSharedPtr<FProfilerDataItem>& A, const TSharedPtr<FProfilerDataItem>& B)
		{
			if (!A.IsValid() || !B.IsValid()) return false;
			int32 CategoryCompare = A->Category.Compare(B->Category, ESearchCase::IgnoreCase);
			if (CategoryCompare == 0)
			{
				// Secondary sort by severity if categories are the same
				return (int32)A->Severity > (int32)B->Severity;
			}
			return CategoryCompare < 0;
		});
	}
	else if (SortBy == TEXT("Severity"))
	{
		FilteredDataItems.Sort([](const TSharedPtr<FProfilerDataItem>& A, const TSharedPtr<FProfilerDataItem>& B)
		{
			if (!A.IsValid() || !B.IsValid()) return false;
			if ((int32)A->Severity == (int32)B->Severity)
			{
				// Secondary sort by value if severities are the same
				return A->Value > B->Value;
			}
			return (int32)A->Severity > (int32)B->Severity; // Higher severity first
		});
	}
	else if (SortBy == TEXT("Value"))
	{
		FilteredDataItems.Sort([](const TSharedPtr<FProfilerDataItem>& A, const TSharedPtr<FProfilerDataItem>& B)
		{
			if (!A.IsValid() || !B.IsValid()) return false;
			if (FMath::IsNearlyEqual(A->Value, B->Value, 0.01f))
			{
				// Secondary sort by severity if values are nearly equal
				return (int32)A->Severity > (int32)B->Severity;
			}
			return A->Value > B->Value; // Higher values first
		});
	}
	else if (SortBy == TEXT("Execution Frequency"))
	{
		// Special sort for runtime data by execution frequency
		FilteredDataItems.Sort([](const TSharedPtr<FProfilerDataItem>& A, const TSharedPtr<FProfilerDataItem>& B)
		{
			if (!A.IsValid() || !B.IsValid()) return false;
			
			// Prioritize runtime data
			if (A->Type == EProfilerDataType::Runtime && B->Type != EProfilerDataType::Runtime)
			{
				return true;
			}
			if (B->Type == EProfilerDataType::Runtime && A->Type != EProfilerDataType::Runtime)
			{
				return false;
			}
			
			return A->Value > B->Value;
		});
	}
	else if (SortBy == TEXT("Memory Usage"))
	{
		// Special sort for memory data
		FilteredDataItems.Sort([](const TSharedPtr<FProfilerDataItem>& A, const TSharedPtr<FProfilerDataItem>& B)
		{
			if (!A.IsValid() || !B.IsValid()) return false;
			
			// Prioritize memory data
			if (A->Type == EProfilerDataType::Memory && B->Type != EProfilerDataType::Memory)
			{
				return true;
			}
			if (B->Type == EProfilerDataType::Memory && A->Type != EProfilerDataType::Memory)
			{
				return false;
			}
			
			return A->Value > B->Value;
		});
	}
}

void SBlueprintProfilerWidget::FilterData(const FString& FilterBy)
{
	// This is handled in UpdateFilteredData()
}

void SBlueprintProfilerWidget::ClearFilters()
{
	CurrentSearchText.Empty();
	CurrentFilterBy = TEXT("All");
	
	if (SearchBox.IsValid())
	{
		SearchBox->SetText(FText::GetEmpty());
	}
	
	if (FilterComboBox.IsValid() && FilterOptions.Num() > 0)
	{
		FilterComboBox->SetSelectedItem(FilterOptions[0]); // "All"
	}
	
	UpdateFilteredData();
	
	if (DataListView.IsValid())
	{
		DataListView->RequestListRefresh();
	}
}

void SBlueprintProfilerWidget::ApplyQuickFilter(const FString& FilterType)
{
	if (FilterType == TEXT("CriticalOnly"))
	{
		CurrentFilterBy = TEXT("Critical");
	}
	else if (FilterType == TEXT("RuntimeOnly"))
	{
		CurrentFilterBy = TEXT("Runtime");
	}
	else if (FilterType == TEXT("LintOnly"))
	{
		CurrentFilterBy = TEXT("Lint");
	}
	else if (FilterType == TEXT("MemoryOnly"))
	{
		CurrentFilterBy = TEXT("Memory");
	}
	else
	{
		CurrentFilterBy = TEXT("All");
	}
	
	// Update combo box selection
	if (FilterComboBox.IsValid())
	{
		for (int32 i = 0; i < FilterOptions.Num(); ++i)
		{
			if (FilterOptions[i].IsValid() && *FilterOptions[i] == CurrentFilterBy)
			{
				FilterComboBox->SetSelectedItem(FilterOptions[i]);
				break;
			}
		}
	}
	
	UpdateFilteredData();
	
	if (DataListView.IsValid())
	{
		DataListView->RequestListRefresh();
	}
}

bool SBlueprintProfilerWidget::MatchesSearchCriteria(TSharedPtr<FProfilerDataItem> Item, const FString& SearchText) const
{
	if (!Item.IsValid() || SearchText.IsEmpty())
	{
		return true;
	}
	
	FString SearchLower = SearchText.ToLower();
	
	// Check all searchable fields
	TArray<FString> SearchableFields = {
		Item->Name.ToLower(),
		Item->BlueprintName.ToLower(),
		Item->Category.ToLower(),
		GetDataTypeText(Item->Type).ToString().ToLower(),
		GetSeverityText(Item->Severity).ToString().ToLower(),
		GetFormattedValue(Item).ToString().ToLower(),
		GetBlueprintPath(Item).ToLower()
	};
	
	// Support multiple search terms (space-separated)
	TArray<FString> SearchTerms;
	SearchLower.ParseIntoArray(SearchTerms, TEXT(" "), true);
	
	for (const FString& Term : SearchTerms)
	{
		bool bTermFound = false;
		for (const FString& Field : SearchableFields)
		{
			if (Field.Contains(Term))
			{
				bTermFound = true;
				break;
			}
		}
		
		if (!bTermFound)
		{
			return false; // All terms must match
		}
	}
	
	return true;
}

TSharedPtr<FProfilerDataItem> SBlueprintProfilerWidget::CreateDataItemFromRuntimeData(const FNodeExecutionData& Data)
{
	TSharedPtr<FProfilerDataItem> Item = MakeShared<FProfilerDataItem>();
	
	Item->Type = EProfilerDataType::Runtime;
	Item->Name = Data.NodeName.IsEmpty() ? TEXT("Unknown Node") : Data.NodeName;
	Item->BlueprintName = Data.BlueprintName.IsEmpty() ? TEXT("Unknown Blueprint") : Data.BlueprintName;
	Item->Value = Data.AverageExecutionsPerSecond;
	Item->TargetObject = Data.BlueprintObject;
	
	// Categorize based on execution characteristics
	if (Data.AverageExecutionsPerSecond > 1000.0f)
	{
		Item->Category = TEXT("Hot Execution");
		Item->Severity = ESeverity::Critical;
	}
	else if (Data.AverageExecutionsPerSecond > 500.0f)
	{
		Item->Category = TEXT("High Execution");
		Item->Severity = ESeverity::High;
	}
	else if (Data.AverageExecutionsPerSecond > 100.0f)
	{
		Item->Category = TEXT("Medium Execution");
		Item->Severity = ESeverity::Medium;
	}
	else if (Data.AverageExecutionsPerSecond > 0.0f)
	{
		Item->Category = TEXT("Normal Execution");
		Item->Severity = ESeverity::Low;
	}
	else
	{
		Item->Category = TEXT("No Execution");
		Item->Severity = ESeverity::Low;
	}
	
	// Special handling for Tick nodes
	if (Item->Name.Contains(TEXT("Tick")) || Item->Name.Contains(TEXT("Event Tick")))
	{
		Item->Category = TEXT("Tick Execution");
		// Tick nodes are more critical at lower thresholds
		if (Data.AverageExecutionsPerSecond > 60.0f) // More than 60 FPS
		{
			Item->Severity = ESeverity::High;
		}
	}
	
	return Item;
}

TSharedPtr<FProfilerDataItem> SBlueprintProfilerWidget::CreateDataItemFromLintIssue(const FLintIssue& Issue)
{
	TSharedPtr<FProfilerDataItem> Item = MakeShared<FProfilerDataItem>();
	
	Item->Type = EProfilerDataType::Lint;
	Item->Name = Issue.NodeName.IsEmpty() ? TEXT("Unknown Node") : Issue.NodeName;
	Item->BlueprintName = FPaths::GetBaseFilename(Issue.BlueprintPath);
	Item->Severity = Issue.Severity;
	Item->NodeGuid = Issue.NodeGuid;
	Item->Value = 1.0f; // Lint issues are binary
	
	// Set detailed category and description based on issue type
	switch (Issue.Type)
	{
		case ELintIssueType::DeadNode:
			Item->Category = TEXT("Dead Code");
			break;
		case ELintIssueType::OrphanNode:
			Item->Category = TEXT("Orphaned Node");
			break;
		case ELintIssueType::CastAbuse:
			Item->Category = TEXT("Performance Cast");
			break;
		case ELintIssueType::TickAbuse:
			Item->Category = TEXT("Tick Complexity");
			break;
		default:
			Item->Category = TEXT("Code Quality");
			break;
	}
	
	// Enhance name with issue description if available
	if (!Issue.Description.IsEmpty())
	{
		Item->Name = FString::Printf(TEXT("%s (%s)"), *Item->Name, *Issue.Description);
	}
	
	return Item;
}

TSharedPtr<FProfilerDataItem> SBlueprintProfilerWidget::CreateDataItemFromMemoryData(
	const FMemoryAnalysisResult& Data,
	UBlueprint* Blueprint)
{
	TSharedPtr<FProfilerDataItem> Item = MakeShared<FProfilerDataItem>();
	
	Item->Type = EProfilerDataType::Memory;
	Item->Name = Blueprint ? Blueprint->GetName() : TEXT("Unknown Blueprint");
	Item->BlueprintName = Item->Name;
	Item->Value = Data.InclusiveSize;
	Item->TargetObject = Blueprint;
	
	// Categorize based on memory characteristics
	if (Data.LargeReferences.Num() > 0)
	{
		Item->Category = TEXT("Large References");
	}
	else if (Data.ReferenceDepth > 10)
	{
		Item->Category = TEXT("Deep References");
	}
	else if (Data.TotalReferences > 100)
	{
		Item->Category = TEXT("Many References");
	}
	else
	{
		Item->Category = TEXT("Memory Usage");
	}
	
	// Determine severity based on memory size and reference complexity
	float SeverityScore = Data.InclusiveSize;
	if (Data.ReferenceDepth > 5)
	{
		SeverityScore *= 1.5f; // Penalty for deep references
	}
	if (Data.LargeReferences.Num() > 0)
	{
		SeverityScore *= 2.0f; // Penalty for large references
	}
	
	if (SeverityScore > 100.0f) // > 100MB equivalent
	{
		Item->Severity = ESeverity::Critical;
	}
	else if (SeverityScore > 50.0f) // > 50MB equivalent
	{
		Item->Severity = ESeverity::High;
	}
	else if (SeverityScore > 10.0f) // > 10MB equivalent
	{
		Item->Severity = ESeverity::Medium;
	}
	else
	{
		Item->Severity = ESeverity::Low;
	}
	
	// Enhance name with memory info
	Item->Name = FString::Printf(TEXT("%s (%.1f MB, %d refs)"), 
		*Item->Name, Data.InclusiveSize, Data.TotalReferences);
	
	return Item;
}

// Utility methods
FLinearColor SBlueprintProfilerWidget::GetSeverityColor(ESeverity Severity) const
{
	switch (Severity)
	{
		case ESeverity::Critical:
			return FLinearColor::Red;
		case ESeverity::High:
			return FLinearColor(1.0f, 0.5f, 0.0f); // Orange
		case ESeverity::Medium:
			return FLinearColor::Yellow;
		case ESeverity::Low:
		default:
			return FLinearColor::Green;
	}
}

FText SBlueprintProfilerWidget::GetSeverityText(ESeverity Severity) const
{
	switch (Severity)
	{
		case ESeverity::Critical:
			return LOCTEXT("SeverityCritical", "Critical");
		case ESeverity::High:
			return LOCTEXT("SeverityHigh", "High");
		case ESeverity::Medium:
			return LOCTEXT("SeverityMedium", "Medium");
		case ESeverity::Low:
		default:
			return LOCTEXT("SeverityLow", "Low");
	}
}

FText SBlueprintProfilerWidget::GetDataTypeText(EProfilerDataType Type) const
{
	switch (Type)
	{
		case EProfilerDataType::Runtime:
			return LOCTEXT("TypeRuntime", "Runtime");
		case EProfilerDataType::Lint:
			return LOCTEXT("TypeLint", "Lint");
		case EProfilerDataType::Memory:
			return LOCTEXT("TypeMemory", "Memory");
		default:
			return LOCTEXT("TypeUnknown", "Unknown");
	}
}

FLinearColor SBlueprintProfilerWidget::GetDataTypeColor(EProfilerDataType Type) const
{
	switch (Type)
	{
		case EProfilerDataType::Runtime:
			return FLinearColor(0.2f, 0.6f, 1.0f); // Blue
		case EProfilerDataType::Lint:
			return FLinearColor(1.0f, 0.6f, 0.2f); // Orange
		case EProfilerDataType::Memory:
			return FLinearColor(0.6f, 0.2f, 1.0f); // Purple
		default:
			return FLinearColor::Gray;
	}
}

FText SBlueprintProfilerWidget::GetDataTypeAbbreviation(EProfilerDataType Type) const
{
	switch (Type)
	{
		case EProfilerDataType::Runtime:
			return LOCTEXT("TypeAbbrevRuntime", "RT");
		case EProfilerDataType::Lint:
			return LOCTEXT("TypeAbbrevLint", "LT");
		case EProfilerDataType::Memory:
			return LOCTEXT("TypeAbbrevMemory", "MEM");
		default:
			return LOCTEXT("TypeAbbrevUnknown", "?");
	}
}

FLinearColor SBlueprintProfilerWidget::GetCategoryColor(const FString& Category) const
{
	if (Category.Contains(TEXT("Execution")))
	{
		return FLinearColor(0.2f, 0.8f, 0.2f); // Green
	}
	else if (Category.Contains(TEXT("Dead")) || Category.Contains(TEXT("Orphan")))
	{
		return FLinearColor(0.8f, 0.2f, 0.2f); // Red
	}
	else if (Category.Contains(TEXT("Cast")) || Category.Contains(TEXT("Tick")))
	{
		return FLinearColor(0.8f, 0.6f, 0.2f); // Orange
	}
	else if (Category.Contains(TEXT("Memory")))
	{
		return FLinearColor(0.6f, 0.2f, 0.8f); // Purple
	}
	else
	{
		return FLinearColor(0.5f, 0.5f, 0.5f); // Gray
	}
}

FText SBlueprintProfilerWidget::GetFormattedValue(TSharedPtr<FProfilerDataItem> Item) const
{
	if (!Item.IsValid())
	{
		return FText::GetEmpty();
	}

	switch (Item->Type)
	{
		case EProfilerDataType::Runtime:
			// Format execution frequency
			if (Item->Value >= 1000.0f)
			{
				return FText::Format(LOCTEXT("ValueExecPerSecK", "{0}K/s"),
					FText::AsNumber(static_cast<int64>(FMath::RoundToFloat(Item->Value / 1000.0f))));
			}
			else
			{
				return FText::Format(LOCTEXT("ValueExecPerSec", "{0}/s"),
					FText::AsNumber(static_cast<int64>(FMath::RoundToFloat(Item->Value))));
			}
			
		case EProfilerDataType::Lint:
			// Lint issues are binary, show count
			return FText::AsNumber(static_cast<int64>(FMath::RoundToFloat(Item->Value)));
			
		case EProfilerDataType::Memory:
			// Format memory size
			if (Item->Value >= 1024.0f)
			{
				return FText::Format(LOCTEXT("ValueMemoryGB", "{0} GB"), 
					FText::AsNumber(FMath::RoundToFloat(Item->Value / 1024.0f * 100.0f) / 100.0f));
			}
			else if (Item->Value >= 1.0f)
			{
				return FText::Format(LOCTEXT("ValueMemoryMB", "{0} MB"), 
					FText::AsNumber(FMath::RoundToFloat(Item->Value * 100.0f) / 100.0f));
			}
			else
			{
				return FText::Format(LOCTEXT("ValueMemoryKB", "{0} KB"),
					FText::AsNumber(static_cast<int64>(FMath::RoundToFloat(Item->Value * 1024.0f))));
			}
			
		default:
			return FText::AsNumber(static_cast<int64>(FMath::RoundToFloat(Item->Value)));
	}
}

FText SBlueprintProfilerWidget::GetFormattedTooltip(TSharedPtr<FProfilerDataItem> Item) const
{
	if (!Item.IsValid())
	{
		return FText::GetEmpty();
	}

	FString TooltipText;
	
	// Add basic info
	TooltipText += FString::Printf(TEXT("Name: %s\n"), *Item->Name);
	TooltipText += FString::Printf(TEXT("Blueprint: %s\n"), *Item->BlueprintName);
	TooltipText += FString::Printf(TEXT("Category: %s\n"), *Item->Category);
	TooltipText += FString::Printf(TEXT("Type: %s\n"), *GetDataTypeText(Item->Type).ToString());
	TooltipText += FString::Printf(TEXT("Severity: %s\n"), *GetSeverityText(Item->Severity).ToString());
	
	// Add type-specific info
	switch (Item->Type)
	{
		case EProfilerDataType::Runtime:
			TooltipText += FString::Printf(TEXT("Executions per second: %.2f\n"), Item->Value);
			TooltipText += TEXT("Double-click to jump to node in blueprint editor");
			break;
			
		case EProfilerDataType::Lint:
			TooltipText += TEXT("Code quality issue detected\n");
			TooltipText += TEXT("Double-click to jump to problematic node");
			break;
			
		case EProfilerDataType::Memory:
			TooltipText += FString::Printf(TEXT("Memory usage: %.2f MB\n"), Item->Value);
			TooltipText += TEXT("Double-click to analyze memory references");
			break;
	}
	
	return FText::FromString(TooltipText);
}

FString SBlueprintProfilerWidget::GetBlueprintPath(TSharedPtr<FProfilerDataItem> Item) const
{
	if (!Item.IsValid() || !Item->TargetObject.IsValid())
	{
		return Item.IsValid() ? Item->BlueprintName : TEXT("Unknown");
	}

	if (UObject* Object = Item->TargetObject.Get())
	{
		return Object->GetPathName();
	}
	
	return Item->BlueprintName;
}

void SBlueprintProfilerWidget::JumpToNode(TSharedPtr<FProfilerDataItem> Item)
{
	if (!Item.IsValid())
	{
		return;
	}
	
	UObject* ObjectToFocus = nullptr;
	UBlueprint* BlueprintToOpen = nullptr;
	
	// Case 1: TargetObject is valid
	if (Item->TargetObject.IsValid())
	{
		ObjectToFocus = Item->TargetObject.Get();
		if (UBlueprint* BP = Cast<UBlueprint>(ObjectToFocus))
		{
			BlueprintToOpen = BP;
			ObjectToFocus = nullptr; // Focus on BP itself, or use NodeGuid if available
		}
	}
	
	// Case 2: We have a Blueprint path (if TargetObject is invalid but we have a path)
	// For now we rely on TargetObject being valid as it's set during creation
	
	if (BlueprintToOpen)
	{
		// Open the blueprint editor
		if (GEditor)
		{
			GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(BlueprintToOpen);
		}
		
		// If we have a Node GUID, try to find and focus the node
		if (Item->NodeGuid.IsValid())
		{
			// Find node by GUID in the blueprint's graphs
			UEdGraphNode* TargetNode = nullptr;
			
			// Check ubergraphs
			for (UEdGraph* Graph : BlueprintToOpen->UbergraphPages)
			{
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (Node->NodeGuid == Item->NodeGuid)
					{
						TargetNode = Node;
						break;
					}
				}
				if (TargetNode) break;
			}
			
			// Check function graphs if not found
			if (!TargetNode)
			{
				for (UEdGraph* Graph : BlueprintToOpen->FunctionGraphs)
				{
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						if (Node->NodeGuid == Item->NodeGuid)
						{
							TargetNode = Node;
							break;
						}
					}
					if (TargetNode) break;
				}
			}
			
			if (TargetNode)
			{
				// In UE 5.6+, use the asset editor subsystem to focus on the node
				if (GEditor)
				{
					UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
					if (AssetEditorSubsystem && BlueprintToOpen)
					{
						// The blueprint is already open, now we need to find a way to jump to the node
						// For now, just ensure the editor is focused
						AssetEditorSubsystem->OpenEditorForAsset(BlueprintToOpen);
					}
				}
			}
		}
	}
	else if (ObjectToFocus)
	{
		// In UE 5.6+, use the asset editor subsystem to focus on the object
		if (GEditor)
		{
			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (AssetEditorSubsystem)
			{
				AssetEditorSubsystem->OpenEditorForAsset(ObjectToFocus);
			}
		}
	}
	else
	{
		if (StatusText.IsValid())
		{
			StatusText->SetText(FText::Format(
				LOCTEXT("StatusJumpFailed", "Could not find target for '{0}'"),
				FText::FromString(Item->Name)
			));
		}
	}
}

// Button state methods
bool SBlueprintProfilerWidget::CanStartRecording() const
{
	return CurrentRecordingState == ERecordingState::Stopped && RuntimeProfiler.IsValid();
}

bool SBlueprintProfilerWidget::CanStopRecording() const
{
	return (CurrentRecordingState == ERecordingState::Recording || CurrentRecordingState == ERecordingState::Paused) && RuntimeProfiler.IsValid();
}

bool SBlueprintProfilerWidget::CanPauseRecording() const
{
	return CurrentRecordingState == ERecordingState::Recording && RuntimeProfiler.IsValid();
}

bool SBlueprintProfilerWidget::CanResumeRecording() const
{
	return CurrentRecordingState == ERecordingState::Paused && RuntimeProfiler.IsValid();
}

bool SBlueprintProfilerWidget::CanResetData() const
{
	return CurrentRecordingState == ERecordingState::Stopped && RuntimeProfiler.IsValid();
}

bool SBlueprintProfilerWidget::CanSaveSession() const
{
	return RuntimeProfiler.IsValid() && (RuntimeProfiler->GetExecutionData().Num() > 0 || RuntimeProfiler->GetSessionHistory().Num() > 0);
}

bool SBlueprintProfilerWidget::CanLoadSession() const
{
	return RuntimeProfiler.IsValid() && CurrentRecordingState == ERecordingState::Stopped;
}

bool SBlueprintProfilerWidget::CanStartScan() const
{
	return !bIsStaticScanning && StaticLinter.IsValid();
}

bool SBlueprintProfilerWidget::CanCancelScan() const
{
	return bIsStaticScanning && StaticLinter.IsValid();
}

bool SBlueprintProfilerWidget::CanStartMemoryAnalysis() const
{
	return !bIsMemoryAnalyzing && MemoryAnalyzer.IsValid();
}

bool SBlueprintProfilerWidget::HasDataToExport() const
{
	return AllDataItems.Num() > 0;
}

// Recording state helpers
FText SBlueprintProfilerWidget::GetRecordingStateText() const
{
	switch (CurrentRecordingState)
	{
		case ERecordingState::Recording:
			return LOCTEXT("RecordingStateRecording", "RECORDING");
		case ERecordingState::Paused:
			return LOCTEXT("RecordingStatePaused", "PAUSED");
		case ERecordingState::Stopped:
		default:
			return LOCTEXT("RecordingStateStopped", "STOPPED");
	}
}

FSlateColor SBlueprintProfilerWidget::GetRecordingStateColor() const
{
	switch (CurrentRecordingState)
	{
		case ERecordingState::Recording:
			return FLinearColor::Red;
		case ERecordingState::Paused:
			return FLinearColor::Yellow;
		case ERecordingState::Stopped:
		default:
			return FLinearColor::Gray;
	}
}

FText SBlueprintProfilerWidget::GetSessionInfoText() const
{
	if (!RuntimeProfiler.IsValid())
	{
		return LOCTEXT("NoSessionInfo", "No session");
	}
	
	FRecordingSession Session = RuntimeProfiler->GetCurrentSession();
	
	if (Session.SessionName.IsEmpty())
	{
		return LOCTEXT("NoActiveSession", "No active session");
	}
	
	if (CurrentRecordingState == ERecordingState::Recording)
	{
		FTimespan ElapsedTime = FDateTime::Now() - Session.StartTime;
		return FText::Format(
			LOCTEXT("ActiveSessionInfo", "{0} - {1}"),
			FText::FromString(Session.SessionName),
			FText::FromString(FString::Printf(TEXT("%02d:%02d"), 
				ElapsedTime.GetMinutes(), ElapsedTime.GetSeconds()))
		);
	}
	else if (CurrentRecordingState == ERecordingState::Paused)
	{
		return FText::Format(
			LOCTEXT("PausedSessionInfo", "{0} - PAUSED"),
			FText::FromString(Session.SessionName)
		);
	}
	else
	{
		return FText::Format(
			LOCTEXT("StoppedSessionInfo", "{0} - Duration: {1}s"),
			FText::FromString(Session.SessionName),
			FText::AsNumber(static_cast<int64>(FMath::RoundToFloat(Session.Duration)))
		);
	}
}

void SBlueprintProfilerWidget::UpdateRecordingStateDisplay()
{
	if (RuntimeProfiler.IsValid())
	{
		CurrentRecordingState = RuntimeProfiler->GetRecordingState();
	}
}

// Event callbacks for analyzers
void SBlueprintProfilerWidget::OnStaticScanComplete(const TArray<FLintIssue>& Issues)
{
	bIsStaticScanning = false;
	
	if (StaticLinter.IsValid())
	{
		FScanProgress Progress = StaticLinter->GetScanProgress();
		
		FText StatusMessage;
		if (Progress.bWasCancelled)
		{
			StatusMessage = FText::Format(
				LOCTEXT("StatusScanCancelled", "Scan cancelled - {0} issues found in {1}/{2} assets"),
				FText::AsNumber(Issues.Num()),
				FText::AsNumber(Progress.ProcessedAssets),
				FText::AsNumber(Progress.TotalAssets)
			);
		}
		else
		{
			StatusMessage = FText::Format(
				LOCTEXT("StatusScanComplete", "Scan complete - {0} issues found in {1} assets"),
				FText::AsNumber(Issues.Num()),
				FText::AsNumber(Progress.TotalAssets)
			);
		}
		
		if (StatusText.IsValid())
		{
			StatusText->SetText(StatusMessage);
		}
	}
	
	if (ProgressBar.IsValid())
	{
		ProgressBar->SetVisibility(EVisibility::Collapsed);
	}
	
	if (ProgressDetailsText.IsValid())
	{
		ProgressDetailsText->SetVisibility(EVisibility::Collapsed);
	}
	
	if (TimeRemainingText.IsValid())
	{
		TimeRemainingText->SetVisibility(EVisibility::Collapsed);
	}
	
	SetLintIssues(Issues);
}

void SBlueprintProfilerWidget::OnStaticScanProgress(int32 ProcessedAssets, int32 TotalAssets)
{
	if (ProgressBar.IsValid())
	{
		float Progress = TotalAssets > 0 ? (float)ProcessedAssets / (float)TotalAssets : 0.0f;
		ProgressBar->SetPercent(Progress);
	}
	
	UpdateProgressDisplay();
}

void SBlueprintProfilerWidget::UpdateProgressDisplay()
{
	if (!StaticLinter.IsValid())
	{
		return;
	}
	
	FScanProgress Progress = StaticLinter->GetScanProgress();
	
	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::Format(
			LOCTEXT("StatusScanProgress", "Scanning... {0}"),
			FText::FromString(Progress.CurrentAsset)
		));
	}
	
	if (ProgressDetailsText.IsValid())
	{
		ProgressDetailsText->SetText(GetProgressText());
	}
	
	if (TimeRemainingText.IsValid())
	{
		TimeRemainingText->SetText(GetTimeRemainingText());
	}
}

FText SBlueprintProfilerWidget::GetProgressText() const
{
	if (!StaticLinter.IsValid())
	{
		return FText::GetEmpty();
	}
	
	FScanProgress Progress = StaticLinter->GetScanProgress();
	
	return FText::Format(
		LOCTEXT("ProgressDetails", "{0}/{1} assets processed ({2}%) - {3} issues found"),
		FText::AsNumber(Progress.ProcessedAssets),
		FText::AsNumber(Progress.TotalAssets),
		FText::AsNumber(static_cast<int64>(FMath::RoundToFloat(Progress.ProgressPercentage * 100.0f))),
		FText::AsNumber(Progress.IssuesFound)
	);
}

FText SBlueprintProfilerWidget::GetTimeRemainingText() const
{
	if (!StaticLinter.IsValid())
	{
		return FText::GetEmpty();
	}
	
	FScanProgress Progress = StaticLinter->GetScanProgress();
	
	if (Progress.EstimatedTimeRemaining <= 0.0f || Progress.ProcessedAssets == 0)
	{
		return LOCTEXT("TimeRemainingCalculating", "Calculating time remaining...");
	}
	
	// Format time remaining in a human-readable way
	int32 TotalSeconds = static_cast<int32>(FMath::RoundToFloat(Progress.EstimatedTimeRemaining));
	int32 Minutes = TotalSeconds / 60;
	int32 Seconds = TotalSeconds % 60;
	
	if (Minutes > 0)
	{
		return FText::Format(
			LOCTEXT("TimeRemainingMinutes", "Estimated time remaining: {0}m {1}s"),
			FText::AsNumber(Minutes),
			FText::AsNumber(Seconds)
		);
	}
	else
	{
		return FText::Format(
			LOCTEXT("TimeRemainingSeconds", "Estimated time remaining: {0}s"),
			FText::AsNumber(Seconds)
		);
	}
}

#undef LOCTEXT_NAMESPACE