// Copyright xnbmy 2026. All Rights Reserved.

#include "UI/SBlueprintProfilerWidget.h"
#include "BlueprintProfilerLocalization.h"
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
#include "Kismet2/BlueprintEditorUtils.h"
#include "Developer/DesktopPlatform/Public/IDesktopPlatform.h"
#include "Developer/DesktopPlatform/Public/DesktopPlatformModule.h"
#include "Misc/FileHelper.h"
#include "Framework/Application/SlateApplication.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/DefaultValueHelper.h"
#include "Containers/Ticker.h"
#include "Animation/AnimBlueprint.h"
#include "Blueprint/UserWidget.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/World.h"

#define LOCTEXT_NAMESPACE "BlueprintProfiler"

void SBlueprintProfilerWidget::Construct(const FArguments& InArgs)
{
	// Initialize analyzers - RuntimeProfiler is a singleton
	RuntimeProfiler = TSharedPtr<FRuntimeProfiler>(&FRuntimeProfiler::Get(), [](FRuntimeProfiler*) {
		// No-op deleter since RuntimeProfiler is a singleton
	});
	StaticLinter = MakeShared<FStaticLinter>();
	MemoryAnalyzer = MakeShared<FMemoryAnalyzer>();

	// Bind to memory analyzer events
	MemoryAnalyzer->OnReferenceCountComplete.AddRaw(this, &SBlueprintProfilerWidget::OnReferenceCountAnalysisComplete);
	MemoryAnalyzer->OnAnalysisProgress.AddRaw(this, &SBlueprintProfilerWidget::OnReferenceCountProgress);

	// Initialize state
	CurrentRecordingState = ERecordingState::Stopped;
	bIsStaticScanning = false;
	bIsMemoryAnalyzing = false;
	CurrentSortBy = BP_LOCTEXT("Severity", "严重程度", "Severity").ToString();
	CurrentFilterBy = BP_LOCTEXT("All", "全部", "All").ToString();

	// Initialize sort and filter options with localization support
	SortOptions.Add(MakeShared<FString>(BP_LOCTEXT("Name", "名称", "Name").ToString()));
	SortOptions.Add(MakeShared<FString>(BP_LOCTEXT("Blueprint", "蓝图", "Blueprint").ToString()));
	SortOptions.Add(MakeShared<FString>(BP_LOCTEXT("Type", "类型", "Type").ToString()));
	SortOptions.Add(MakeShared<FString>(BP_LOCTEXT("Category", "类别", "Category").ToString()));
	SortOptions.Add(MakeShared<FString>(BP_LOCTEXT("Severity", "严重程度", "Severity").ToString()));
	SortOptions.Add(MakeShared<FString>(BP_LOCTEXT("Value", "值", "Value").ToString()));
	SortOptions.Add(MakeShared<FString>(BP_LOCTEXT("SortByExecution", "执行频率", "Execution Frequency").ToString()));
	SortOptions.Add(MakeShared<FString>(BP_LOCTEXT("SortByMemory", "内存使用", "Memory Usage").ToString()));

	FilterOptions.Add(MakeShared<FString>(BP_LOCTEXT("FilterAll", "全部", "All").ToString()));
	FilterOptions.Add(MakeShared<FString>(BP_LOCTEXT("FilterRuntime", "运行时", "Runtime").ToString()));
	FilterOptions.Add(MakeShared<FString>(BP_LOCTEXT("FilterLint", "代码检查", "Code Check").ToString()));
	FilterOptions.Add(MakeShared<FString>(BP_LOCTEXT("FilterMemory", "内存", "Memory").ToString()));
	FilterOptions.Add(MakeShared<FString>(BP_LOCTEXT("SeverityCritical", "严重", "Critical").ToString()));
	FilterOptions.Add(MakeShared<FString>(BP_LOCTEXT("SeverityHigh", "高", "High").ToString()));
	FilterOptions.Add(MakeShared<FString>(BP_LOCTEXT("SeverityMedium", "中", "Medium").ToString()));
	FilterOptions.Add(MakeShared<FString>(BP_LOCTEXT("SeverityLow", "低", "Low").ToString()));
	FilterOptions.Add(MakeShared<FString>(BP_LOCTEXT("FilterHotspots", "热点节点", "Hotspot Nodes").ToString()));
	FilterOptions.Add(MakeShared<FString>(BP_LOCTEXT("FilterDeadCode", "死代码", "Dead Code").ToString()));
	FilterOptions.Add(MakeShared<FString>(BP_LOCTEXT("FilterPerformance", "性能问题", "Performance Issues").ToString()));

	// Bind analyzer events
	StaticLinter->OnScanComplete.AddSP(this, &SBlueprintProfilerWidget::OnStaticScanComplete);
	StaticLinter->OnScanProgress.AddSP(this, &SBlueprintProfilerWidget::OnStaticScanProgress);

	// 绑定 PIE 结束事件（自动停止录制时刷新数据）
	FEditorDelegates::EndPIE.AddSP(this, &SBlueprintProfilerWidget::OnPIEEnd);

	// 注册 UI 刷新定时器（每 0.5 秒刷新一次，确保 PIE 自动开始录制时 UI 能正确更新）
	UIRefreshTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &SBlueprintProfilerWidget::TickUIRefresh),
		0.5f  // 0.5 秒间隔
	);

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(4.0f)
		[
			SNew(SSplitter)
			.Orientation(Orient_Horizontal)
			
			// Control Panel (Left Side)
			+ SSplitter::Slot()
			.Value(0.25f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("DetailsView.CategoryTop"))
				.Padding(8.0f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SNew(SVerticalBox)
						
						// Status and Progress Display (放在最上面，确保可见)
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 4)
						[
							SNew(STextBlock)
							.Text(BP_LOCTEXT("StatusSectionTitle", "状态", "Status"))
							.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
						]
						
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 2)
						[
							SAssignNew(StatusText, STextBlock)
							.Text(BP_LOCTEXT("StatusReady", "就绪", "Ready"))
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
						
						// Runtime Profiler Controls
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 8, 0, 4)
						[
							SNew(STextBlock)
							.Text(BP_LOCTEXT("RuntimeProfilerTitle", "运行时分析器", "Runtime Profiler"))
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
								.Text(BP_LOCTEXT("StartRecording", "开始", "Start"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnStartRuntimeRecording)
								.IsEnabled(this, &SBlueprintProfilerWidget::CanStartRecording)
								.ButtonStyle(FAppStyle::Get(), "FlatButton.Success")
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 4, 0)
							[
								SAssignNew(PauseRecordingButton, SButton)
								.Text(BP_LOCTEXT("PauseRecording", "暂停", "Pause"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnPauseRuntimeRecording)
								.IsEnabled(this, &SBlueprintProfilerWidget::CanPauseRecording)
								.ButtonStyle(FAppStyle::Get(), "FlatButton.Warning")
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 4, 0)
							[
								SAssignNew(ResumeRecordingButton, SButton)
								.Text(BP_LOCTEXT("ResumeRecording", "继续", "Resume"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnResumeRuntimeRecording)
								.IsEnabled(this, &SBlueprintProfilerWidget::CanResumeRecording)
								.ButtonStyle(FAppStyle::Get(), "FlatButton.Primary")
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 4, 0)
							[
								SAssignNew(StopRecordingButton, SButton)
								.Text(BP_LOCTEXT("StopRecording", "停止", "Stop"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnStopRuntimeRecording)
								.IsEnabled(this, &SBlueprintProfilerWidget::CanStopRecording)
								.ButtonStyle(FAppStyle::Get(), "FlatButton.Danger")
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SAssignNew(ResetDataButton, SButton)
								.Text(BP_LOCTEXT("ResetData", "重置", "Reset"))
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
								.Text(BP_LOCTEXT("SaveSession", "保存会话", "Save Session"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnSaveSessionData)
								.IsEnabled(this, &SBlueprintProfilerWidget::CanSaveSession)
								.ToolTipText(BP_LOCTEXT("SaveSessionTooltip", "保存当前会话数据到文件", "Save current session data to file"))
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 4, 0)
							[
								SAssignNew(LoadSessionButton, SButton)
								.Text(BP_LOCTEXT("LoadSession", "加载会话", "Load Session"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnLoadSessionData)
								.IsEnabled(this, &SBlueprintProfilerWidget::CanLoadSession)
								.ToolTipText(BP_LOCTEXT("LoadSessionTooltip", "从文件加载会话数据", "Load session data from file"))
							]

						]
						
						// PIE integration settings
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 8, 0, 4)
						[
							SNew(STextBlock)
							.Text(BP_LOCTEXT("PIEIntegrationTitle", "PIE 集成", "PIE Integration"))
							.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
						]
						
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 2)
						[
							SAssignNew(AutoStartPIECheckBox, SCheckBox)
							.IsChecked(RuntimeProfiler.IsValid() && RuntimeProfiler->GetAutoStartOnPIE() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
							.Content()
							[
								SNew(STextBlock)
								.Text(BP_LOCTEXT("AutoStartPIE", "PIE 时自动开始录制", "Auto-start recording on PIE"))
								.ToolTipText(BP_LOCTEXT("AutoStartPIETooltip", "在编辑器中开始播放时自动开始录制", "Automatically start recording when editor begins playback"))
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
							.IsChecked(RuntimeProfiler.IsValid() && RuntimeProfiler->GetAutoStopOnPIEEnd() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
							.Content()
							[
								SNew(STextBlock)
								.Text(BP_LOCTEXT("AutoStopPIE", "PIE 结束时自动停止录制", "Auto-stop recording on PIE end"))
								.ToolTipText(BP_LOCTEXT("AutoStopPIETooltip", "在编辑器中结束播放时自动停止录制", "Automatically stop recording when editor ends playback"))
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
							.Text(BP_LOCTEXT("StaticLinterTitle", "静态分析", "Static Analysis"))
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
								.Text(BP_LOCTEXT("StartScan", "扫描项目", "Scan Project"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnStartStaticScan)
								.IsEnabled(this, &SBlueprintProfilerWidget::CanStartScan)
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 4, 0)
							[
								SAssignNew(ScanFolderButton, SButton)
								.Text(BP_LOCTEXT("ScanFolders", "扫描文件夹", "Scan Folders"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnScanSelectedFolders)
								.IsEnabled(this, &SBlueprintProfilerWidget::CanStartScan)
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SAssignNew(CancelScanButton, SButton)
								.Text(BP_LOCTEXT("CancelScan", "取消扫描", "Cancel Scan"))
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
							.Text(BP_LOCTEXT("MemoryAnalyzerTitle", "引用分析器", "Reference Analyzer"))
							.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
						]
						
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 2)
						[
							SAssignNew(StartMemoryAnalysisButton, SButton)
							.Text(BP_LOCTEXT("StartMemoryAnalysis", "分析引用", "Analyze References"))
							.OnClicked(this, &SBlueprintProfilerWidget::OnStartMemoryAnalysis)
							.IsEnabled(this, &SBlueprintProfilerWidget::CanStartMemoryAnalysis)
						]
						
						// Export Controls
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 8, 0, 4)
						[
							SNew(STextBlock)
							.Text(BP_LOCTEXT("ExportTitle", "导出", "Export"))
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
								.Text(BP_LOCTEXT("ExportCSV", "导出 CSV", "Export CSV"))
								.OnClicked(this, &SBlueprintProfilerWidget::OnExportToCSV)
								.IsEnabled(this, &SBlueprintProfilerWidget::HasDataToExport)
							]
							
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(SButton)
								.Text(BP_LOCTEXT("ExportJSON", "导出 JSON", "Export JSON"))
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
								.HintText(BP_LOCTEXT("SearchHint", "搜索节点、蓝图、类别...", "Search nodes, blueprints, categories..."))
								.OnTextChanged(this, &SBlueprintProfilerWidget::OnSearchTextChanged)
							]
							
							// Clear Filters Button
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 4, 0)
							[
								SNew(SButton)
								.Text(BP_LOCTEXT("ClearFilters", "清除", "Clear"))
								.ToolTipText(BP_LOCTEXT("ClearFiltersTooltip", "清除所有搜索和筛选条件", "Clear all search and filter conditions"))
								.OnClicked_Lambda([this]()
								{
									ClearFilters();
									return FReply::Handled();
								})
							]

							// Clear Data Button
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(SButton)
								.Text(BP_LOCTEXT("ClearData", "清除数据", "Clear Data"))
								.ToolTipText(BP_LOCTEXT("ClearDataTooltip", "清除所有显示的数据（运行时数据和静态扫描数据）", "Clear all displayed data (runtime data and static scan data)"))
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
								.Text(BP_LOCTEXT("SortBy", "排序：", "Sort:"))
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
								.Text(BP_LOCTEXT("FilterBy", "筛选：", "Filter:"))
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
								.Text(BP_LOCTEXT("QuickFilterCritical", "严重", "Critical"))
								.ToolTipText(BP_LOCTEXT("QuickFilterCriticalTooltip", "仅显示严重级别的项目", "Show only critical severity items"))
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
								.Text(BP_LOCTEXT("QuickFilterRuntime", "运行时", "Runtime"))
								.ToolTipText(BP_LOCTEXT("QuickFilterRuntimeTooltip", "仅显示运行时性能数据", "Show only runtime performance data"))
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
								.Text(BP_LOCTEXT("QuickFilterLint", "代码检查", "Code Check"))
								.ToolTipText(BP_LOCTEXT("QuickFilterLintTooltip", "仅显示静态分析问题", "Show only static analysis issues"))
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
								.Text(BP_LOCTEXT("QuickFilterMemory", "内存", "Memory"))
								.ToolTipText(BP_LOCTEXT("QuickFilterMemoryTooltip", "仅显示内存分析数据", "Show only memory analysis data"))
								.ButtonStyle(FAppStyle::Get(), "FlatButton.Success")
								.OnClicked_Lambda([this]()
								{
									ApplyQuickFilter(TEXT("MemoryOnly"));
									return FReply::Handled();
								})
							]

							// Spacer
							+ SHorizontalBox::Slot()
							.FillWidth(1.0f)
							[
								SNew(SSpacer)
							]

							// Hide Engine Internal Nodes checkbox
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(8, 0, 0, 0)
							.VAlign(VAlign_Center)
							.HAlign(HAlign_Left)
							[
								SNew(SCheckBox)
								.IsChecked(ECheckBoxState::Checked)
								.OnCheckStateChanged(this, &SBlueprintProfilerWidget::OnHideEngineNodesChanged)
								[
									SNew(STextBlock)
									.Text(BP_LOCTEXT("HideEngineNodes", "隐藏引擎内部节点", "Hide Engine Internal Nodes"))
									.ToolTipText(BP_LOCTEXT("HideEngineNodesTooltip", "过滤掉引擎内置宏节点（如For Loop、While Loop等）的内部实现，只显示你自己的业务逻辑", "Filter out engine built-in macro node internals (like For Loop, While Loop, etc.) and only show your own business logic"))
								]
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
						.OnContextMenuOpening(this, &SBlueprintProfilerWidget::OnContextMenuOpening)
						.HeaderRow
						(
							SNew(SHeaderRow)
							+ SHeaderRow::Column("Type")
							.DefaultLabel(BP_LOCTEXT("TypeColumn", "类型", "Type"))
							.FillWidth(0.1f)
							
							+ SHeaderRow::Column("Name")
							.DefaultLabel(BP_LOCTEXT("NameColumn", "名称", "Name"))
							.FillWidth(0.3f)
							
							+ SHeaderRow::Column("Blueprint")
							.DefaultLabel(BP_LOCTEXT("BlueprintColumn", "蓝图", "Blueprint"))
							.FillWidth(0.25f)
							
							+ SHeaderRow::Column("Category")
							.DefaultLabel(BP_LOCTEXT("CategoryColumn", "类别", "Category"))
							.FillWidth(0.15f)
							
							+ SHeaderRow::Column("Value")
							.DefaultLabel(BP_LOCTEXT("ValueColumn", "值", "Value"))
							.FillWidth(0.1f)
							
							+ SHeaderRow::Column("Severity")
							.DefaultLabel(BP_LOCTEXT("SeverityColumn", "严重程度", "Severity"))
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
		SortComboBox->SetSelectedItem(SortOptions[4]); // 严重程度 (Severity - 索引4)
	}
	if (FilterOptions.Num() > 0)
	{
		FilterComboBox->SetSelectedItem(FilterOptions[0]); // 全部 (All)
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

void SBlueprintProfilerWidget::SetAssetReferenceData(const TArray<FAssetReferenceCount>& AssetReferences)
{
	// Clear existing memory data (we reuse Memory type for reference counts)
	AllDataItems.RemoveAll([](const TSharedPtr<FProfilerDataItem>& Item)
	{
		return Item.IsValid() && Item->Type == EProfilerDataType::Memory;
	});
	
	// Load asset registry for looking up assets
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	// Use a map to deduplicate by asset name
	TMap<FString, TSharedPtr<FProfilerDataItem>> UniqueItems;
	
	// Add reference count data
	for (const FAssetReferenceCount& RefCount : AssetReferences)
	{
		// Get the asset name (remove _C suffix if present)
		FString AssetName = RefCount.AssetName;
		if (AssetName.EndsWith(TEXT("_C")))
		{
			AssetName = AssetName.LeftChop(2);
		}
		
		// Check if we already have this asset
		if (UniqueItems.Contains(AssetName))
		{
			// Update the reference count (add to existing)
			TSharedPtr<FProfilerDataItem> ExistingItem = UniqueItems[AssetName];
			ExistingItem->Value += RefCount.ReferenceCount;
			int32 NewCount = (int32)ExistingItem->Value;
			ExistingItem->Category = FText::Format(BP_LOCTEXT("RefCountCategory", "被引用 {0} 次", "Referenced {0} times"), NewCount).ToString();
			ExistingItem->Description = FText::Format(BP_LOCTEXT("RefCountDesc", "类型: {0}, 大小: {1} MB, 被 {2} 个资产引用", "Type: {0}, Size: {1} MB, Referenced by {2} assets"),
				FText::FromString(RefCount.AssetType), RefCount.AssetSize, NewCount).ToString();
			
			// Update severity based on combined count
			ExistingItem->Severity = NewCount > 10 ? ESeverity::High :
			                        (NewCount > 5 ? ESeverity::Medium : ESeverity::Low);
			continue;
		}
		
		TSharedPtr<FProfilerDataItem> Item = MakeShared<FProfilerDataItem>();
		Item->Type = EProfilerDataType::Memory;
		
		Item->Name = AssetName;
		Item->BlueprintName = AssetName;
		Item->Category = FText::Format(BP_LOCTEXT("RefCountCategory", "被引用 {0} 次", "Referenced {0} times"), RefCount.ReferenceCount).ToString();
		Item->Description = FText::Format(BP_LOCTEXT("RefCountDesc", "类型: {0}, 大小: {1} MB, 被 {2} 个资产引用", "Type: {0}, Size: {1} MB, Referenced by {2} assets"),
			FText::FromString(RefCount.AssetType), RefCount.AssetSize, RefCount.ReferenceCount).ToString();
		Item->Value = RefCount.ReferenceCount;
		Item->Severity = RefCount.ReferenceCount > 10 ? ESeverity::High :
		                (RefCount.ReferenceCount > 5 ? ESeverity::Medium : ESeverity::Low);
		
		// Set target object for double-click navigation
		FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(RefCount.AssetPath));
		if (AssetData.IsValid())
		{
			Item->TargetObject = AssetData.GetAsset();
			Item->AssetObject = AssetData.GetAsset();
		}
		
		UniqueItems.Add(AssetName, Item);
	}
	
	// Add all unique items to the list
	for (auto& Pair : UniqueItems)
	{
		AllDataItems.Add(Pair.Value);
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
				BP_LOCTEXT("StatusRecording", "正在录制运行时数据 - 会话：{0}", "Recording runtime data - Session: {0}"),
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
				BP_LOCTEXT("StatusRecordingStopped", "录制已停止 - 会话：{0}（时长：{1}秒，节点：{2}）", "Recording stopped - Session: {0} (Duration: {1}s, Nodes: {2})"),
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
			StatusText->SetText(BP_LOCTEXT("StatusRecordingPaused", "录制已暂停", "Recording paused"));
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
			StatusText->SetText(BP_LOCTEXT("StatusRecordingResumed", "录制已继续", "Recording resumed"));
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
			StatusText->SetText(BP_LOCTEXT("StatusDataReset", "运行时数据已重置", "Runtime data reset"));
		}
	}
	return FReply::Handled();
}

FReply SBlueprintProfilerWidget::OnSaveSessionData()
{
	if (RuntimeProfiler.IsValid())
	{
		// Open file save dialog
		IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
		if (DesktopPlatform)
		{
			FString DefaultPath = RuntimeProfiler->GetSessionDataDirectory();
			FString DefaultFileName = RuntimeProfiler->GetCurrentSession().SessionName + TEXT(".json");
			TArray<FString> SavedFiles;
			
			// File types filter
			const FString FileTypes = TEXT("JSON Files (*.json)|*.json");
			
			bool bSuccess = DesktopPlatform->SaveFileDialog(
				FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
				BP_LOCTEXT("SaveSessionDialogTitle", "保存会话数据", "Save Session Data").ToString(),
				DefaultPath,
				DefaultFileName,
				FileTypes,
				0,
				SavedFiles
			);
			
			if (bSuccess && SavedFiles.Num() > 0)
			{
				RuntimeProfiler->SaveSessionData(SavedFiles[0]);
				
				if (StatusText.IsValid())
				{
					StatusText->SetText(FText::Format(
						BP_LOCTEXT("StatusSessionSaved", "会话数据已保存到：{0}", "Session data saved to: {0}"),
						FText::FromString(SavedFiles[0])
					));
				}
			}
		}
	}
	return FReply::Handled();
}

FReply SBlueprintProfilerWidget::OnLoadSessionData()
{
	if (RuntimeProfiler.IsValid())
	{
		// Open file picker dialog
		IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
		if (DesktopPlatform)
		{
			FString DefaultPath = RuntimeProfiler->GetSessionDataDirectory();
			TArray<FString> SelectedFiles;
			
			// File types filter
			const FString FileTypes = TEXT("JSON Files (*.json)|*.json|All Files (*.*)|*.*");
			
			bool bSuccess = DesktopPlatform->OpenFileDialog(
				FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
				BP_LOCTEXT("LoadSessionDialogTitle", "选择要加载的会话数据文件", "Select Session Data File to Load").ToString(),
				DefaultPath,
				TEXT(""),
				FileTypes,
				0,
				SelectedFiles
			);
			
			if (bSuccess && SelectedFiles.Num() > 0)
			{
				FString SelectedFile = SelectedFiles[0];
				bool bLoaded = RuntimeProfiler->LoadSessionData(SelectedFile);
				
				if (bLoaded)
				{
					// Refresh UI to show loaded data
					RefreshData();
				}
				
				if (StatusText.IsValid())
				{
					if (bLoaded)
					{
						StatusText->SetText(FText::Format(
							BP_LOCTEXT("StatusSessionLoaded", "会话数据已从 {0} 加载成功", "Session data loaded successfully from {0}"),
							FText::FromString(SelectedFile)
						));
					}
					else
					{
						StatusText->SetText(BP_LOCTEXT("StatusSessionLoadFailed", "加载会话数据失败", "Failed to load session data"));
					}
				}
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

		// 清除后刷新 UI 显示
		RefreshData();

		if (StatusText.IsValid())
		{
			StatusText->SetText(BP_LOCTEXT("StatusHistoryCleared", "会话历史已清除", "Session history cleared"));
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
			StatusText->SetText(BP_LOCTEXT("StatusScanning", "正在扫描蓝图...", "Scanning blueprints..."));
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
		// 打开文件夹选择对话框
		IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
		if (DesktopPlatform)
		{
			const void* ParentWindowWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
			FString SelectedFolder;
			const FString Title = BP_LOCTEXT("FolderSelectTitle", "选择要扫描的蓝图文件夹", "Select Blueprint Folder to Scan").ToString();

			// 打开文件夹选择对话框（UE 5.6 版本只支持选择单个文件夹）
			if (DesktopPlatform->OpenDirectoryDialog(
				ParentWindowWindowHandle,
				Title,
				FPaths::ProjectContentDir(),
				SelectedFolder))
			{
				// 将文件系统路径转换为虚幻引擎资产路径
				FString AssetPath;
				const FString ContentDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());

				if (SelectedFolder.StartsWith(ContentDir, ESearchCase::IgnoreCase))
				{
					// 移除 Content 目录部分，并添加 /Game 前缀
					FString RelativePath = SelectedFolder.RightChop(ContentDir.Len());
					// 移除可能的斜杠前缀
					RelativePath.RemoveFromStart(TEXT("/"));
					// 添加 /Game 前缀
					AssetPath = FString::Printf(TEXT("/Game/%s"), *RelativePath);
				}
				else
				{
					// 如果不在 Content 目录下，尝试使用相对路径
					AssetPath = SelectedFolder;
				}

				if (!AssetPath.IsEmpty())
				{
					TArray<FString> AssetPaths;
					AssetPaths.Add(AssetPath);

					bIsStaticScanning = true;

					if (StatusText.IsValid())
					{
						StatusText->SetText(FText::Format(
							BP_LOCTEXT("StatusScanningFolders", "正在扫描文件夹: {0}", "Scanning folder: {0}"),
							FText::FromString(AssetPath)
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
					StaticLinter->ScanSelectedFolders(AssetPaths);
				}
				else
				{
					if (StatusText.IsValid())
					{
						StatusText->SetText(BP_LOCTEXT("StatusInvalidFolder", "所选文件夹不在项目 Content 目录中", "Selected folder is not in the project Content directory"));
					}
				}
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
			StatusText->SetText(BP_LOCTEXT("StatusScanCancelled", "扫描已取消 - 保留了部分结果", "Scan cancelled - partial results retained"));
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
			StatusText->SetText(BP_LOCTEXT("StatusAnalyzingRefs", "正在分析资产引用关系...", "Analyzing asset reference relationships..."));
		}
		
		// Show progress bar
		if (ProgressBar.IsValid())
		{
			ProgressBar->SetPercent(0.0f);
			ProgressBar->SetVisibility(EVisibility::Visible);
		}
		
		// Clear previous data
		MemoryAnalyzer->ClearReferenceCountData();
		
		// Perform reference count analysis (async)
		MemoryAnalyzer->AnalyzeAssetReferenceCounts();
	}
	return FReply::Handled();
}

void SBlueprintProfilerWidget::OnReferenceCountProgress(float Progress)
{
	// Update progress bar
	if (ProgressBar.IsValid())
	{
		ProgressBar->SetPercent(Progress);
	}
	
	// Update status text with progress
	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::Format(
			BP_LOCTEXT("StatusAnalyzingRefsProgress", "正在分析资产引用关系... {0}%", "Analyzing asset reference relationships... {0}%"),
			FText::AsNumber(FMath::RoundToInt(Progress * 100))
		));
	}
}

void SBlueprintProfilerWidget::OnReferenceCountAnalysisComplete(const FMemoryAnalysisResult& Result)
{
	// Get top referenced assets
	TArray<FAssetReferenceCount> TopAssets = MemoryAnalyzer->GetTopReferencedAssets(100);
	
	// Update UI with results
	SetAssetReferenceData(TopAssets);
	
	// Hide progress bar
	if (ProgressBar.IsValid())
	{
		ProgressBar->SetVisibility(EVisibility::Collapsed);
	}
	
	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::Format(
			BP_LOCTEXT("StatusRefCountComplete", "引用分析完成。发现 {0} 个被引用的资产，显示前 {1} 个。", "Reference analysis complete. Found {0} referenced assets, showing top {1}."),
			FText::AsNumber(MemoryAnalyzer->GetAssetReferenceCounts().Num()),
			FText::AsNumber(TopAssets.Num())
		));
	}
	
	bIsMemoryAnalyzing = false;
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
						BP_LOCTEXT("StatusExportSuccess", "已导出 {0} 个项目到 {1}", "Exported {0} items to {1}"),
						FText::AsNumber(FilteredDataItems.Num()),
						FText::FromString(FPaths::GetCleanFilename(FilePath))
					));
				}
			}
			else
			{
				if (StatusText.IsValid())
				{
					StatusText->SetText(BP_LOCTEXT("StatusExportFailed", "保存导出文件失败", "Failed to save export file"));
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
						BP_LOCTEXT("StatusExportSuccess", "已导出 {0} 个项目到 {1}", "Exported {0} items to {1}"),
						FText::AsNumber(FilteredDataItems.Num()),
						FText::FromString(FPaths::GetCleanFilename(FilePath))
					));
				}
			}
			else
			{
				if (StatusText.IsValid())
				{
					StatusText->SetText(BP_LOCTEXT("StatusExportFailed", "保存导出文件失败", "Failed to save export file"));
				}
			}
		}
	}
	return FReply::Handled();
}

FReply SBlueprintProfilerWidget::OnRefreshData()
{
	// 清除所有显示的数据（运行时数据、静态扫描数据、会话历史）
	AllDataItems.Empty();
	FilteredDataItems.Empty();

	// 也清除 RuntimeProfiler 和 StaticLinter 中的数据
	if (RuntimeProfiler.IsValid())
	{
		RuntimeProfiler->ClearSessionHistory();
	}
	if (StaticLinter.IsValid())
	{
		StaticLinter->ClearIssues();
	}

	if (DataListView.IsValid())
	{
		DataListView->RequestListRefresh();
	}

	if (StatusText.IsValid())
	{
		StatusText->SetText(BP_LOCTEXT("StatusCleared", "数据已清除", "Data cleared"));
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
	if (!Item.IsValid())
	{
		return;
	}
	
	// Handle double-click based on item type
	switch (Item->Type)
	{
	case EProfilerDataType::Runtime:
	case EProfilerDataType::Lint:
		// For runtime/lint types, jump to the node in blueprint
		JumpToNode(Item);
		break;
		
	case EProfilerDataType::Memory:
		// For memory/reference types, navigate to the asset
		NavigateToAsset(Item);
		break;
		
	default:
		// For other types, try to navigate to blueprint/asset
		if (!Item->BlueprintName.IsEmpty())
		{
			NavigateToBlueprint(Item);
		}
		break;
	}
}

void SBlueprintProfilerWidget::OnSelectionChanged(
	TSharedPtr<FProfilerDataItem> Item,
	ESelectInfo::Type SelectInfo)
{
	// Handle selection change if needed
}

TSharedPtr<SWidget> SBlueprintProfilerWidget::OnContextMenuOpening()
{
	// Get selected item
	TArray<TSharedPtr<FProfilerDataItem>> SelectedItems = DataListView->GetSelectedItems();
	if (SelectedItems.Num() == 0)
	{
		return nullptr;
	}
	
	TSharedPtr<FProfilerDataItem> Item = SelectedItems[0];
	if (!Item.IsValid())
	{
		return nullptr;
	}
	
	FMenuBuilder MenuBuilder(true, nullptr);
	
	// Add navigation options based on item type
	MenuBuilder.BeginSection("Navigation", BP_LOCTEXT("NavigationSection", "导航", "Navigation"));
	
	// Option 1: Navigate to Blueprint (always available for runtime/lint types)
	if (Item->Type == EProfilerDataType::Runtime || Item->Type == EProfilerDataType::Lint)
	{
		MenuBuilder.AddMenuEntry(
			BP_LOCTEXT("NavigateToBlueprint", "导航到蓝图", "Navigate to Blueprint"),
			BP_LOCTEXT("NavigateToBlueprintTooltip", "在蓝图编辑器中打开蓝图", "Open blueprint in blueprint editor"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SBlueprintProfilerWidget::NavigateToBlueprint, Item))
		);
	}
	
	// Option 2: Navigate to Node (only for runtime/lint types that have node info)
	if ((Item->Type == EProfilerDataType::Runtime || Item->Type == EProfilerDataType::Lint) && 
	    (!Item->Name.IsEmpty() && Item->Name != Item->BlueprintName))
	{
		MenuBuilder.AddMenuEntry(
			BP_LOCTEXT("NavigateToNode", "导航到节点", "Navigate to Node"),
			BP_LOCTEXT("NavigateToNodeTooltip", "在蓝图编辑器中定位到具体节点", "Locate the specific node in blueprint editor"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SBlueprintProfilerWidget::NavigateToNode, Item))
		);
	}
	
	// Option 3: Navigate to Asset (for memory/reference types)
	if (Item->Type == EProfilerDataType::Memory)
	{
		MenuBuilder.AddMenuEntry(
			BP_LOCTEXT("NavigateToAsset", "导航到资产", "Navigate to Asset"),
			BP_LOCTEXT("NavigateToAssetTooltip", "在编辑器中打开资产", "Open asset in editor"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SBlueprintProfilerWidget::NavigateToAsset, Item))
		);
	}
	
	MenuBuilder.EndSection();
	
	return MenuBuilder.MakeWidget();
}

void SBlueprintProfilerWidget::NavigateToBlueprint(TSharedPtr<FProfilerDataItem> Item)
{
	if (!Item.IsValid())
	{
		return;
	}
	
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	FAssetData FoundAsset;
	
	// Get the asset name (remove _C suffix if present)
	FString AssetName = Item->BlueprintName;
	
	UE_LOG(LogTemp, Warning, TEXT("[NavigateToBlueprint] Item->Name: '%s', Item->BlueprintName: '%s'"), 
		*Item->Name, *Item->BlueprintName);
	
	if (AssetName.EndsWith(TEXT("_C")))
	{
		AssetName = AssetName.LeftChop(2);
		UE_LOG(LogTemp, Warning, TEXT("[NavigateToBlueprint] Removed _C suffix, AssetName now: '%s'"), *AssetName);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[NavigateToBlueprint] No _C suffix found, using AssetName: '%s'"), *AssetName);
	}
	
	// Method 1: Search all assets by name (accept any asset type)
	{
		TArray<FAssetData> AllAssets;
		AssetRegistry.GetAllAssets(AllAssets);
		
		// First try exact match on asset name
		for (const FAssetData& AssetData : AllAssets)
		{
			if (AssetData.AssetName.ToString() == AssetName)
			{
				FoundAsset = AssetData;
				break;
			}
		}
		
		// If not found, try partial match
		if (!FoundAsset.IsValid())
		{
			for (const FAssetData& AssetData : AllAssets)
			{
				if (AssetData.AssetName.ToString().Contains(AssetName) ||
				    AssetName.Contains(AssetData.AssetName.ToString()))
				{
					FoundAsset = AssetData;
					break;
				}
			}
		}
	}
	
	// Method 2: Search for blueprints (for blueprint types)
	if (!FoundAsset.IsValid())
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.bRecursivePaths = true;
		
		TArray<FAssetData> AssetDataArray;
		AssetRegistry.GetAssets(Filter, AssetDataArray);
		
		for (const FAssetData& AssetData : AssetDataArray)
		{
			if (AssetData.AssetName.ToString() == AssetName)
			{
				FoundAsset = AssetData;
				break;
			}
		}
	}
	
	// Method 3: Search for level assets (for level blueprints)
	if (!FoundAsset.IsValid())
	{
		FARFilter LevelFilter;
		LevelFilter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
		LevelFilter.bRecursivePaths = true;
		
		TArray<FAssetData> LevelAssets;
		AssetRegistry.GetAssets(LevelFilter, LevelAssets);
		
		for (const FAssetData& LevelAsset : LevelAssets)
		{
			if (LevelAsset.AssetName.ToString() == AssetName)
			{
				FoundAsset = LevelAsset;
				break;
			}
		}
	}
	
	if (FoundAsset.IsValid())
	{
		UE_LOG(LogTemp, Log, TEXT("[NavigateToBlueprint] Found asset: '%s' at path '%s'"), 
			*FoundAsset.AssetName.ToString(), *FoundAsset.GetObjectPathString());
		
		// Sync to content browser (navigate to asset in content browser)
		TArray<FAssetData> AssetsToSync;
		AssetsToSync.Add(FoundAsset);
		GEditor->SyncBrowserToObjects(AssetsToSync);
		
		if (StatusText.IsValid())
		{
			StatusText->SetText(FText::Format(
				BP_LOCTEXT("StatusAssetNavigated", "已在内容浏览器中导航到资产 '{0}'", "Navigated to asset '{0}' in content browser"),
				FText::FromString(FoundAsset.AssetName.ToString())
			));
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[NavigateToBlueprint] Could not find asset: '%s'"), *AssetName);
		
		if (StatusText.IsValid())
		{
			StatusText->SetText(FText::Format(
				BP_LOCTEXT("StatusAssetNotFound", "无法找到资产 '{0}'", "Could not find asset '{0}'"),
				FText::FromString(Item->BlueprintName)
			));
		}
	}
}

void SBlueprintProfilerWidget::NavigateToNode(TSharedPtr<FProfilerDataItem> Item)
{
	// Use the existing JumpToNode logic
	JumpToNode(Item);
}

void SBlueprintProfilerWidget::NavigateToAsset(TSharedPtr<FProfilerDataItem> Item)
{
	if (!Item.IsValid())
	{
		return;
	}
	
	// Try to find the asset and navigate to it in content browser
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	FAssetData FoundAsset;
	
	// Get the asset name (remove _C suffix if present)
	FString AssetName = Item->BlueprintName;
	if (AssetName.EndsWith(TEXT("_C")))
	{
		AssetName = AssetName.LeftChop(2);
	}
	
	// Method 1: Search all assets by name
	{
		TArray<FAssetData> AllAssets;
		AssetRegistry.GetAllAssets(AllAssets);
		
		// First try exact match on asset name
		for (const FAssetData& AssetData : AllAssets)
		{
			if (AssetData.AssetName.ToString() == AssetName)
			{
				FoundAsset = AssetData;
				break;
			}
		}
		
		// If not found, try partial match
		if (!FoundAsset.IsValid())
		{
			for (const FAssetData& AssetData : AllAssets)
			{
				if (AssetData.AssetName.ToString().Contains(AssetName) ||
				    AssetName.Contains(AssetData.AssetName.ToString()))
				{
					FoundAsset = AssetData;
					break;
				}
			}
		}
	}
	
	// Method 2: Try to use TargetObject if available
	if (!FoundAsset.IsValid() && Item->TargetObject.IsValid())
	{
		FString AssetPath = Item->TargetObject->GetPathName();
		FoundAsset = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
	}
	
	if (FoundAsset.IsValid())
	{
		// Sync to content browser (navigate to asset in content browser)
		TArray<FAssetData> AssetsToSync;
		AssetsToSync.Add(FoundAsset);
		GEditor->SyncBrowserToObjects(AssetsToSync);
		
		if (StatusText.IsValid())
		{
			StatusText->SetText(FText::Format(
				BP_LOCTEXT("StatusAssetNavigated", "已在内容浏览器中导航到资产 '{0}'", "Navigated to asset '{0}' in content browser"),
				FText::FromString(FoundAsset.AssetName.ToString())
			));
		}
		return;
	}
	
	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::Format(
			BP_LOCTEXT("StatusAssetNotFound", "无法找到资产 '{0}'", "Could not find asset '{0}'"),
			FText::FromString(Item->Name)
		));
	}
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
				BP_LOCTEXT("StatusShowingAll", "显示所有 {0} 个项目", "Showing all {0} items"),
				FText::AsNumber(FilteredDataItems.Num())
			));
		}
		else
		{
			StatusText->SetText(FText::Format(
				BP_LOCTEXT("StatusSearchResults", "搜索 '{0}'：{2} 项中的 {1} 项", "搜索 '{0}'：{2} 项中的 {1} 项"),
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

void SBlueprintProfilerWidget::OnHideEngineNodesChanged(ECheckBoxState NewState)
{
	bool bHide = (NewState == ECheckBoxState::Checked);
	RuntimeProfiler->SetHideEngineInternalNodes(bHide);

	// Refresh data to apply filter
	UpdateFilteredData();

	if (DataListView.IsValid())
	{
		DataListView->RequestListRefresh();
	}

}

// Data processing methods
void SBlueprintProfilerWidget::UpdateFilteredData()
{
	FilteredDataItems.Empty();

	// Get localized filter texts for comparison
	FString AllText = BP_LOCTEXT("FilterAll", "全部", "All").ToString();
	FString RuntimeText = BP_LOCTEXT("FilterRuntime", "运行时", "Runtime").ToString();
	FString LintText = BP_LOCTEXT("FilterLint", "代码检查", "Code Check").ToString();
	FString MemoryText = BP_LOCTEXT("FilterMemory", "内存", "Memory").ToString();
	FString CriticalText = BP_LOCTEXT("SeverityCritical", "严重", "Critical").ToString();
	FString HighText = BP_LOCTEXT("SeverityHigh", "高", "High").ToString();
	FString MediumText = BP_LOCTEXT("SeverityMedium", "中", "Medium").ToString();
	FString LowText = BP_LOCTEXT("SeverityLow", "低", "Low").ToString();
	FString HotspotsText = BP_LOCTEXT("FilterHotspots", "热点节点", "Hotspot Nodes").ToString();
	FString DeadCodeText = BP_LOCTEXT("FilterDeadCode", "死代码", "Dead Code").ToString();
	FString PerformanceText = BP_LOCTEXT("FilterPerformance", "性能问题", "Performance Issues").ToString();

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
		if (CurrentFilterBy != AllText)
		{
			bool bMatchesFilter = false;

			// Type filters
			if (CurrentFilterBy == RuntimeText && Item->Type == EProfilerDataType::Runtime)
			{
				bMatchesFilter = true;
			}
			else if (CurrentFilterBy == LintText && Item->Type == EProfilerDataType::Lint)
			{
				bMatchesFilter = true;
			}
			else if (CurrentFilterBy == MemoryText && Item->Type == EProfilerDataType::Memory)
			{
				bMatchesFilter = true;
			}
			// Severity filters
			else if (CurrentFilterBy == CriticalText && Item->Severity == ESeverity::Critical)
			{
				bMatchesFilter = true;
			}
			else if (CurrentFilterBy == HighText && Item->Severity == ESeverity::High)
			{
				bMatchesFilter = true;
			}
			else if (CurrentFilterBy == MediumText && Item->Severity == ESeverity::Medium)
			{
				bMatchesFilter = true;
			}
			else if (CurrentFilterBy == LowText && Item->Severity == ESeverity::Low)
			{
				bMatchesFilter = true;
			}
			// Category filters
			else if (CurrentFilterBy == HotspotsText &&
					(Item->Category.Contains(TEXT("Hot")) || Item->Category.Contains(TEXT("高频执行"))))
			{
				bMatchesFilter = true;
			}
			else if (CurrentFilterBy == DeadCodeText &&
					(Item->Category.Contains(TEXT("Dead")) || Item->Category.Contains(TEXT("孤立节点"))))
			{
				bMatchesFilter = true;
			}
			else if (CurrentFilterBy == PerformanceText &&
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
	// Get localized sort texts for comparison
	FString NameText = BP_LOCTEXT("Name", "名称", "Name").ToString();
	FString BlueprintText = BP_LOCTEXT("Blueprint", "蓝图", "Blueprint").ToString();
	FString TypeText = BP_LOCTEXT("Type", "类型", "Type").ToString();
	FString CategoryText = BP_LOCTEXT("Category", "类别", "Category").ToString();
	FString SeverityText = BP_LOCTEXT("Severity", "严重程度", "Severity").ToString();
	FString ValueText = BP_LOCTEXT("Value", "值", "Value").ToString();
	FString ExecutionText = BP_LOCTEXT("SortByExecution", "执行频率", "Execution Frequency").ToString();
	FString MemoryText = BP_LOCTEXT("SortByMemory", "内存使用", "Memory Usage").ToString();

	if (SortBy == NameText)
	{
		FilteredDataItems.Sort([](const TSharedPtr<FProfilerDataItem>& A, const TSharedPtr<FProfilerDataItem>& B)
		{
			if (!A.IsValid() || !B.IsValid()) return false;
			return A->Name.Compare(B->Name, ESearchCase::IgnoreCase) < 0;
		});
	}
	else if (SortBy == BlueprintText)
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
	else if (SortBy == TypeText)
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
	else if (SortBy == CategoryText)
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
	else if (SortBy == SeverityText)
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
	else if (SortBy == ValueText)
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
	else if (SortBy == ExecutionText)
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
	else if (SortBy == MemoryText)
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
	CurrentFilterBy = BP_LOCTEXT("FilterAll", "全部", "All").ToString();
	CurrentSortBy = BP_LOCTEXT("Severity", "严重程度", "Severity").ToString(); // 重置排序

	if (SearchBox.IsValid())
	{
		SearchBox->SetText(FText::GetEmpty());
	}

	if (FilterComboBox.IsValid() && FilterOptions.Num() > 0)
	{
		FilterComboBox->SetSelectedItem(FilterOptions[0]); // "全部" / "All"
	}

	if (SortComboBox.IsValid() && SortOptions.Num() > 0)
	{
		SortComboBox->SetSelectedItem(SortOptions[4]); // "严重程度" / "Severity" (索引4)
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
		CurrentFilterBy = BP_LOCTEXT("SeverityCritical", "严重", "Critical").ToString();
	}
	else if (FilterType == TEXT("RuntimeOnly"))
	{
		CurrentFilterBy = BP_LOCTEXT("FilterRuntime", "运行时", "Runtime").ToString();
	}
	else if (FilterType == TEXT("LintOnly"))
	{
		CurrentFilterBy = BP_LOCTEXT("FilterLint", "代码检查", "Code Check").ToString();
	}
	else if (FilterType == TEXT("MemoryOnly"))
	{
		CurrentFilterBy = BP_LOCTEXT("FilterMemory", "内存", "Memory").ToString();
	}
	else
	{
		CurrentFilterBy = BP_LOCTEXT("FilterAll", "全部", "All").ToString();
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
	Item->Name = Data.NodeName.IsEmpty() ? TEXT("未知节点") : Data.NodeName;
	Item->BlueprintName = Data.BlueprintName.IsEmpty() ? TEXT("未知蓝图") : Data.BlueprintName;
	Item->Value = Data.AverageExecutionsPerSecond;
	Item->TargetObject = Data.BlueprintObject;
	Item->NodeGuid = Data.NodeGuid;

	// Categorize based on execution characteristics
	if (Data.AverageExecutionsPerSecond > 1000.0f)
	{
		Item->Category = BP_LOCTEXT("CategoryHighFreq", "高频执行", "High Frequency").ToString();
		Item->Severity = ESeverity::Critical;
	}
	else if (Data.AverageExecutionsPerSecond > 500.0f)
	{
		Item->Category = BP_LOCTEXT("CategoryHighExec", "高执行", "High Execution").ToString();
		Item->Severity = ESeverity::High;
	}
	else if (Data.AverageExecutionsPerSecond > 100.0f)
	{
		Item->Category = BP_LOCTEXT("CategoryMediumExec", "中等执行", "Medium Execution").ToString();
		Item->Severity = ESeverity::Medium;
	}
	else if (Data.AverageExecutionsPerSecond > 0.0f)
	{
		Item->Category = BP_LOCTEXT("CategoryNormalExec", "正常执行", "Normal Execution").ToString();
		Item->Severity = ESeverity::Low;
	}
	else
	{
		Item->Category = BP_LOCTEXT("CategoryNoExec", "无执行", "No Execution").ToString();
		Item->Severity = ESeverity::Low;
	}

	// Special handling for Tick nodes
	if (Item->Name.Contains(TEXT("Tick")) || Item->Name.Contains(TEXT("Event Tick")))
	{
		Item->Category = BP_LOCTEXT("CategoryTickExec", "Tick执行", "Tick Execution").ToString();
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
	Item->Name = Issue.NodeName.IsEmpty() ? TEXT("未知节点") : Issue.NodeName;
	Item->BlueprintName = FPaths::GetBaseFilename(Issue.BlueprintPath);
	Item->Severity = Issue.Severity;
	Item->NodeGuid = Issue.NodeGuid;
	Item->Value = 1.0f; // Lint issues are binary

	// Try to load the blueprint for navigation
	if (!Issue.BlueprintPath.IsEmpty())
	{
		// Try to find the blueprint asset in the asset registry
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		// Use FName version with explicit template parameter to avoid ambiguity
		FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FName(*Issue.BlueprintPath));

		if (AssetData.IsValid())
		{
			UObject* LoadedAsset = AssetData.GetAsset();
			if (UBlueprint* Blueprint = Cast<UBlueprint>(LoadedAsset))
			{
				Item->TargetObject = Blueprint;
			}
		}
	}

	// Set detailed category and description based on issue type
	switch (Issue.Type)
	{
		case ELintIssueType::DeadNode:
			Item->Category = BP_LOCTEXT("CategoryDeadCode", "死代码", "Dead Code").ToString();
			break;
		case ELintIssueType::OrphanNode:
			Item->Category = BP_LOCTEXT("CategoryOrphanNode", "孤立节点", "Orphan Node").ToString();
			break;
		case ELintIssueType::CastAbuse:
			Item->Category = BP_LOCTEXT("CategoryCastAbuse", "性能转换", "Cast Abuse").ToString();
			break;
		case ELintIssueType::TickAbuse:
			Item->Category = BP_LOCTEXT("CategoryTickAbuse", "Tick复杂度", "Tick Abuse").ToString();
			break;
		case ELintIssueType::UnusedFunction:
			Item->Category = BP_LOCTEXT("CategoryUnusedFunc", "未引用函数", "Unused Function").ToString();
			break;
		default:
			Item->Category = BP_LOCTEXT("CategoryCodeQuality", "代码质量", "Code Quality").ToString();
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
	
	// Get the blueprint name (remove _C suffix if present)
	FString BlueprintName = Blueprint ? Blueprint->GetName() : TEXT("未知蓝图");
	if (BlueprintName.EndsWith(TEXT("_C")))
	{
		BlueprintName = BlueprintName.LeftChop(2);
	}
	
	Item->Name = BlueprintName;
	Item->BlueprintName = BlueprintName;
	Item->Value = Data.InclusiveSize;
	Item->TargetObject = Blueprint;

	// Categorize based on memory characteristics (使用中文类别)
	if (Data.LargeReferences.Num() > 0)
	{
		Item->Category = TEXT("大型引用");
	}
	else if (Data.ReferenceDepth > 10)
	{
		Item->Category = TEXT("深层引用");
	}
	else if (Data.TotalReferences > 100)
	{
		Item->Category = TEXT("多重引用");
	}
	else
	{
		Item->Category = TEXT("内存使用");
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
			return BP_LOCTEXT("SeverityCritical", "严重", "Critical");
		case ESeverity::High:
			return BP_LOCTEXT("SeverityHigh", "高", "High");
		case ESeverity::Medium:
			return BP_LOCTEXT("SeverityMedium", "中", "Medium");
		case ESeverity::Low:
		default:
			return BP_LOCTEXT("SeverityLow", "低", "Low");
	}
}

FText SBlueprintProfilerWidget::GetDataTypeText(EProfilerDataType Type) const
{
	switch (Type)
	{
		case EProfilerDataType::Runtime:
			return BP_LOCTEXT("TypeRuntime", "运行时", "Runtime");
		case EProfilerDataType::Lint:
			return BP_LOCTEXT("TypeLint", "代码检查", "Lint");
		case EProfilerDataType::Memory:
			return BP_LOCTEXT("TypeMemory", "内存", "Memory");
		default:
			return BP_LOCTEXT("TypeUnknown", "未知", "Unknown");
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
			return BP_LOCTEXT("TypeAbbrevRuntime", "运", "R");
		case EProfilerDataType::Lint:
			return BP_LOCTEXT("TypeAbbrevLint", "查", "L");
		case EProfilerDataType::Memory:
			return BP_LOCTEXT("TypeAbbrevMemory", "存", "M");
		default:
			return BP_LOCTEXT("TypeAbbrevUnknown", "?", "?");
	}
}

FLinearColor SBlueprintProfilerWidget::GetCategoryColor(const FString& Category) const
{
	if (Category.Contains(TEXT("执行")) || Category.Contains(TEXT("Execution")))
	{
		return FLinearColor(0.2f, 0.8f, 0.2f); // Green
	}
	else if (Category.Contains(TEXT("死")) || Category.Contains(TEXT("孤立节点")) ||
			 Category.Contains(TEXT("Dead")) || Category.Contains(TEXT("Orphan")))
	{
		return FLinearColor(0.8f, 0.2f, 0.2f); // Red
	}
	else if (Category.Contains(TEXT("转换")) || Category.Contains(TEXT("Tick")) ||
			 Category.Contains(TEXT("Cast")) || Category.Contains(TEXT("复杂度")))
	{
		return FLinearColor(0.8f, 0.6f, 0.2f); // Orange
	}
	else if (Category.Contains(TEXT("引用")) || Category.Contains(TEXT("内存")) ||
			 Category.Contains(TEXT("Memory")) || Category.Contains(TEXT("References")))
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
				return FText::Format(BP_LOCTEXT("ValueExecPerSecK", "{0}千次/秒", "{0}k exec/s"),
					FText::AsNumber(static_cast<int64>(FMath::RoundToFloat(Item->Value / 1000.0f))));
			}
			else
			{
				return FText::Format(BP_LOCTEXT("ValueExecPerSec", "{0}次/秒", "{0} exec/s"),
					FText::AsNumber(static_cast<int64>(FMath::RoundToFloat(Item->Value))));
			}
			
		case EProfilerDataType::Lint:
			// Lint issues are binary, show count
			return FText::AsNumber(static_cast<int64>(FMath::RoundToFloat(Item->Value)));
			
		case EProfilerDataType::Memory:
			// Format memory size
			if (Item->Value >= 1024.0f)
			{
				return FText::Format(BP_LOCTEXT("ValueMemoryGB", "{0} GB", "{0} GB"), 
					FText::AsNumber(FMath::RoundToFloat(Item->Value / 1024.0f * 100.0f) / 100.0f));
			}
			else if (Item->Value >= 1.0f)
			{
				return FText::Format(BP_LOCTEXT("ValueMemoryMB", "{0} MB", "{0} MB"), 
					FText::AsNumber(FMath::RoundToFloat(Item->Value * 100.0f) / 100.0f));
			}
			else
			{
				return FText::Format(BP_LOCTEXT("ValueMemoryKB", "{0} KB", "{0} KB"),
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

	// 获取目标对象
	UObject* TargetObject = Item->TargetObject.Get();
	
	// 1. 如果是内存/引用计数类型，直接打开资产
	if (Item->Type == EProfilerDataType::Memory)
	{
		if (TargetObject)
		{
			// 直接打开资产（可以是材质、纹理、蓝图等任何资产）
			if (GEditor)
			{
				GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(TargetObject);
				
				if (StatusText.IsValid())
				{
					StatusText->SetText(FText::Format(
						BP_LOCTEXT("StatusAssetOpened", "已打开资产 '{0}'", "已打开资产 '{0}'"),
						FText::FromString(Item->Name)
					));
				}
			}
		}
		else
		{
			// 尝试通过路径加载资产
			FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
			IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
			
			FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(Item->BlueprintName));
			if (AssetData.IsValid())
			{
				UObject* Asset = AssetData.GetAsset();
				if (Asset && GEditor)
				{
					GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);
					
					if (StatusText.IsValid())
					{
						StatusText->SetText(FText::Format(
							BP_LOCTEXT("StatusAssetOpened", "已打开资产 '{0}'", "已打开资产 '{0}'"),
							FText::FromString(Item->Name)
						));
					}
				}
			}
			else
			{
				if (StatusText.IsValid())
				{
					StatusText->SetText(FText::Format(
						BP_LOCTEXT("StatusAssetNotFound", "无法找到资产 '{0}'", "Could not find asset '{0}'"),
						FText::FromString(Item->Name)
					));
				}
			}
		}
		return;
	}

	// 2. 原有的蓝图节点跳转逻辑（用于运行时和代码检查类型）
	UBlueprint* BP = nullptr;
	if (TargetObject)
	{
		// 如果 TargetObject 是 Node，获取它所属的蓝图
		if (UEdGraphNode* Node = Cast<UEdGraphNode>(TargetObject))
		{
			BP = Node->GetTypedOuter<UBlueprint>();
		}
		else
		{
			BP = Cast<UBlueprint>(TargetObject);
		}
	}
	
	// 如果没有 TargetObject，尝试通过名称查找蓝图
	if (!BP && !Item->BlueprintName.IsEmpty())
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		
		FAssetData FoundAsset;
		
		// Get the blueprint name (remove _C suffix if present)
		FString BlueprintName = Item->BlueprintName;
		if (BlueprintName.EndsWith(TEXT("_C")))
		{
			BlueprintName = BlueprintName.LeftChop(2);
		}
		
		// Method 1: Search all assets and filter by name (most reliable)
		{
			TArray<FAssetData> AllAssets;
			AssetRegistry.GetAllAssets(AllAssets);
			
			// First try exact match on asset name
			for (const FAssetData& AssetData : AllAssets)
			{
				if (AssetData.AssetName.ToString() == BlueprintName)
				{
					// Check if it's a blueprint type
					UClass* AssetClass = AssetData.GetClass();
					if (AssetClass && AssetClass->IsChildOf(UBlueprint::StaticClass()))
					{
						FoundAsset = AssetData;
						break;
					}
				}
			}
			
			// If not found, try partial match
			if (!FoundAsset.IsValid())
			{
				for (const FAssetData& AssetData : AllAssets)
				{
					if (AssetData.AssetName.ToString().Contains(BlueprintName) ||
					    BlueprintName.Contains(AssetData.AssetName.ToString()))
					{
						UClass* AssetClass = AssetData.GetClass();
						if (AssetClass && AssetClass->IsChildOf(UBlueprint::StaticClass()))
						{
							FoundAsset = AssetData;
							break;
						}
					}
				}
			}
		}
		
		// Method 2: If still not found, try using FARFilter with recursive classes
		if (!FoundAsset.IsValid())
		{
			FARFilter Filter;
			Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
			Filter.bRecursiveClasses = true;
			Filter.bRecursivePaths = true;
			
			TArray<FAssetData> AssetDataArray;
			AssetRegistry.GetAssets(Filter, AssetDataArray);
			
			for (const FAssetData& AssetData : AssetDataArray)
			{
				if (AssetData.AssetName.ToString() == BlueprintName)
				{
					FoundAsset = AssetData;
					break;
				}
			}
		}
		
		// Method 3: Search for level assets (for level blueprints)
		if (!FoundAsset.IsValid())
		{
			FARFilter LevelFilter;
			LevelFilter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
			LevelFilter.bRecursivePaths = true;
			
			TArray<FAssetData> LevelAssets;
			AssetRegistry.GetAssets(LevelFilter, LevelAssets);
			
			for (const FAssetData& LevelAsset : LevelAssets)
			{
				if (LevelAsset.AssetName.ToString() == BlueprintName)
				{
					FoundAsset = LevelAsset;
					break;
				}
			}
		}
		
		if (FoundAsset.IsValid())
		{
			UObject* Asset = FoundAsset.GetAsset();
			
			// Try to get blueprint from asset
			if (Asset)
			{
				// Case 1: Asset is already a UBlueprint
				BP = Cast<UBlueprint>(Asset);
				
				// Case 2: Asset is a UWorld (level), get its level script blueprint
				if (!BP)
				{
					if (UWorld* World = Cast<UWorld>(Asset))
					{
						if (World->PersistentLevel)
						{
							BP = World->PersistentLevel->GetLevelScriptBlueprint(true);
						}
					}
				}
			}
		}
	}

	if (!BP)
	{
		if (StatusText.IsValid())
		{
			StatusText->SetText(FText::Format(
				BP_LOCTEXT("StatusJumpFailed", "无法找到 '{0}' 的蓝图", "无法找到 '{0}' 的蓝图"),
				FText::FromString(Item->Name)
			));
		}
		return;
	}

	// 3. 查找目标节点（使用引擎内置工具，递归搜索所有图）
	UEdGraphNode* TargetNode = nullptr;
	if (Item->NodeGuid.IsValid())
	{
		// FBlueprintEditorUtils::GetNodeByGUID 会搜索 EventGraph, Functions, Macros 等所有图
		TargetNode = FBlueprintEditorUtils::GetNodeByGUID(BP, Item->NodeGuid);
	}

	// 4. 执行跳转
	if (TargetNode)
	{
		// 使用蓝图专用 API 跳转到节点
		// 这个 API 会自动打开蓝图编辑器，定位到正确的图表，并聚焦节点
		FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(TargetNode);
	}
	else
	{
		// 如果没找到具体节点，至少把蓝图打开
		if (GEditor)
		{
			GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(BP);
		}

		if (StatusText.IsValid())
		{
			StatusText->SetText(FText::Format(
				BP_LOCTEXT("StatusNodeNotFound", "无法找到节点 '{0}'", "Could not find node '{0}'"),
				FText::FromString(BP->GetName())
			));
		}
	}
}

// Button state methods
bool SBlueprintProfilerWidget::CanStartRecording() const
{
	// 直接查询 Profiler 的实际状态，而不是使用缓存的 CurrentRecordingState
	// 这确保 PIE 自动开始录制时，按钮状态能正确更新
	return RuntimeProfiler.IsValid() && !RuntimeProfiler->IsRecording() && !RuntimeProfiler->IsPaused();
}

bool SBlueprintProfilerWidget::CanStopRecording() const
{
	// 直接查询 Profiler 的实际状态
	return RuntimeProfiler.IsValid() && (RuntimeProfiler->IsRecording() || RuntimeProfiler->IsPaused());
}

bool SBlueprintProfilerWidget::CanPauseRecording() const
{
	// 直接查询 Profiler 的实际状态
	return RuntimeProfiler.IsValid() && RuntimeProfiler->IsRecording();
}

bool SBlueprintProfilerWidget::CanResumeRecording() const
{
	// 直接查询 Profiler 的实际状态
	return RuntimeProfiler.IsValid() && RuntimeProfiler->IsPaused();
}

bool SBlueprintProfilerWidget::CanResetData() const
{
	// 直接查询 Profiler 的实际状态
	return RuntimeProfiler.IsValid() && !RuntimeProfiler->IsRecording() && !RuntimeProfiler->IsPaused();
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
	// 直接查询 Profiler 的实际状态
	if (!RuntimeProfiler.IsValid())
	{
		return BP_LOCTEXT("RecordingStateUnknown", "未知", "未知");
	}

	ERecordingState State = RuntimeProfiler->GetRecordingState();
	switch (State)
	{
		case ERecordingState::Recording:
			return BP_LOCTEXT("RecordingStateRecording", "录制中", "Recording");
		case ERecordingState::Paused:
			return BP_LOCTEXT("RecordingStatePaused", "已暂停", "Paused");
		case ERecordingState::Stopped:
		default:
			return BP_LOCTEXT("RecordingStateStopped", "已停止", "Stopped");
	}
}

FSlateColor SBlueprintProfilerWidget::GetRecordingStateColor() const
{
	// 直接查询 Profiler 的实际状态
	if (!RuntimeProfiler.IsValid())
	{
		return FLinearColor::Gray;
	}

	ERecordingState State = RuntimeProfiler->GetRecordingState();
	switch (State)
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
		return BP_LOCTEXT("NoSessionInfo", "无会话", "No Session");
	}

	FRecordingSession Session = RuntimeProfiler->GetCurrentSession();

	if (Session.SessionName.IsEmpty())
	{
		return BP_LOCTEXT("NoActiveSession", "无活动会话", "No Active Session");
	}

	// 直接查询 Profiler 的实际状态
	ERecordingState State = RuntimeProfiler->GetRecordingState();
	if (State == ERecordingState::Recording)
	{
		FTimespan ElapsedTime = FDateTime::Now() - Session.StartTime;
		return FText::Format(
			BP_LOCTEXT("ActiveSessionInfo", "{0} - {1}", "{0} - {1}"),
			FText::FromString(Session.SessionName),
			FText::FromString(FString::Printf(TEXT("%02d:%02d"),
				ElapsedTime.GetMinutes(), ElapsedTime.GetSeconds()))
		);
	}
	else if (State == ERecordingState::Paused)
	{
		return FText::Format(
			BP_LOCTEXT("PausedSessionInfo", "{0} - 已暂停", "{0} - 已暂停"),
			FText::FromString(Session.SessionName)
		);
	}
	else
	{
		return FText::Format(
			BP_LOCTEXT("StoppedSessionInfo", "{0} - 时长：{1}秒", "{0} - Duration: {1}s"),
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
				BP_LOCTEXT("StatusScanCancelled", "扫描已取消 - 保留了部分结果", "Scan cancelled - partial results retained"),
				FText::AsNumber(Issues.Num()),
				FText::AsNumber(Progress.ProcessedAssets),
				FText::AsNumber(Progress.TotalAssets)
			);
		}
		else
		{
			StatusMessage = FText::Format(
				BP_LOCTEXT("StatusScanComplete", "扫描完成 - 在 {1} 个资源中发现 {0} 个问题", "Scan complete - Found {0} issues in {1} assets"),
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

// PIE 结束时刷新数据（处理自动停止录制的情况）
void SBlueprintProfilerWidget::OnPIEEnd(bool bIsSimulating)
{
	// 延迟刷新，确保 RuntimeProfiler 已完成停止处理
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float DeltaTime)
	{
		if (RuntimeProfiler.IsValid() && RuntimeProfiler->GetRecordingState() == ERecordingState::Stopped)
		{
			// 刷新数据以显示录制结果
			SetRuntimeData(RuntimeProfiler->GetExecutionData());
			CurrentRecordingState = ERecordingState::Stopped;
			UpdateRecordingStateDisplay();

			// 更新状态文本
			if (StatusText.IsValid())
			{
				FRecordingSession Session = RuntimeProfiler->GetCurrentSession();
				StatusText->SetText(FText::Format(
					BP_LOCTEXT("StatusAutoStopped", "自动停止 - 会话：{0}（时长：{1}秒，节点：{2}）", "Auto-stopped - Session: {0} (Duration: {1}s, Nodes: {2})"),
					FText::FromString(Session.SessionName),
					FText::AsNumber(static_cast<int64>(FMath::RoundToFloat(Session.Duration))),
					FText::AsNumber(Session.TotalNodesRecorded)
				));
			}

			return false; // 只执行一次
		}
		return true; // 继续等待
	}), 0.5f);
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
			BP_LOCTEXT("StatusScanProgress", "正在扫描... {0}", "Scanning... {0}"),
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
		BP_LOCTEXT("ProgressDetails", "已处理 {0}/{1} 个资源（{2}%）- 发现 {3} 个问题", "Processed {0}/{1} assets ({2}%) - Found {3} issues"),
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
		return BP_LOCTEXT("TimeRemainingCalculating", "正在计算剩余时间...", "Calculating remaining time...");
	}
	
	// Format time remaining in a human-readable way
	int32 TotalSeconds = static_cast<int32>(FMath::RoundToFloat(Progress.EstimatedTimeRemaining));
	int32 Minutes = TotalSeconds / 60;
	int32 Seconds = TotalSeconds % 60;
	
	if (Minutes > 0)
	{
		return FText::Format(
			BP_LOCTEXT("TimeRemainingMinutes", "预计剩余时间：{0}分 {1}秒", "Estimated remaining: {0}m {1}s"),
			FText::AsNumber(Minutes),
			FText::AsNumber(Seconds)
		);
	}
	else
	{
		return FText::Format(
			BP_LOCTEXT("TimeRemainingSeconds", "预计剩余时间：{0}秒", "Estimated remaining: {0}s"),
			FText::AsNumber(Seconds)
		);
	}
}

// UI 刷新定时器回调
bool SBlueprintProfilerWidget::TickUIRefresh(float DeltaTime)
{
	// 更新缓存的录制状态（用于其他可能使用它的地方）
	if (RuntimeProfiler.IsValid())
	{
		ERecordingState NewState = RuntimeProfiler->GetRecordingState();
		if (NewState != CurrentRecordingState)
		{
			CurrentRecordingState = NewState;

			// 刷新录制状态显示
			if (RecordingStateText.IsValid())
			{
				RecordingStateText->SetText(GetRecordingStateText());
			}
			if (SessionNameText.IsValid())
			{
				SessionNameText->SetText(GetSessionInfoText());
			}

			// 如果正在录制，定期更新数据列表
			if (NewState == ERecordingState::Recording)
			{
				UpdateFilteredData();
				if (DataListView.IsValid())
				{
					DataListView->RequestListRefresh();
				}
			}
		}
	}

	// 刷新按钮状态
	if (StartRecordingButton.IsValid()) StartRecordingButton->Invalidate(EInvalidateWidget::Layout);
	if (StopRecordingButton.IsValid()) StopRecordingButton->Invalidate(EInvalidateWidget::Layout);
	if (PauseRecordingButton.IsValid()) PauseRecordingButton->Invalidate(EInvalidateWidget::Layout);
	if (ResumeRecordingButton.IsValid()) ResumeRecordingButton->Invalidate(EInvalidateWidget::Layout);
	if (ResetDataButton.IsValid()) ResetDataButton->Invalidate(EInvalidateWidget::Layout);

	// 继续定时器
	return true;
}

// 析构函数：清理定时器
SBlueprintProfilerWidget::~SBlueprintProfilerWidget()
{
	// 解绑 PIE 结束事件
	FEditorDelegates::EndPIE.RemoveAll(this);

	if (UIRefreshTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(UIRefreshTickerHandle);
		UIRefreshTickerHandle.Reset();
	}
}

#undef LOCTEXT_NAMESPACE