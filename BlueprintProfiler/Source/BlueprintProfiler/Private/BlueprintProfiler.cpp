#include "BlueprintProfiler.h"
#include "BlueprintProfilerStyle.h"
#include "BlueprintProfilerCommands.h"
#include "BlueprintProfilerLocalization.h"
#include "UI/SBlueprintProfilerWidget.h"
#include "LevelEditor.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "ToolMenus.h"

static const FName BlueprintProfilerTabName("BlueprintProfiler");

#define LOCTEXT_NAMESPACE "BlueprintProfiler"

void FBlueprintProfilerModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	
	FBlueprintProfilerStyle::Initialize();
	FBlueprintProfilerStyle::ReloadTextures();

	FBlueprintProfilerCommands::Register();
	
	PluginCommands = MakeShareable(new FUICommandList);

	PluginCommands->MapAction(
		FBlueprintProfilerCommands::Get().OpenPluginWindow,
		FExecuteAction::CreateRaw(this, &FBlueprintProfilerModule::PluginButtonClicked),
		FCanExecuteAction());

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FBlueprintProfilerModule::RegisterMenus));
	
	FText TabTitle = FBlueprintProfilerLocalization::IsChinese() ?
		FText::FromString(TEXT("蓝图分析器")) :
		FText::FromString(TEXT("Blueprint Profiler"));
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(BlueprintProfilerTabName, FOnSpawnTab::CreateRaw(this, &FBlueprintProfilerModule::OnSpawnPluginTab))
		.SetDisplayName(TabTitle)
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void FBlueprintProfilerModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.

	UToolMenus::UnRegisterStartupCallback(this);

	UToolMenus::UnregisterOwner(this);

	FBlueprintProfilerStyle::Shutdown();

	FBlueprintProfilerCommands::Unregister();

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(BlueprintProfilerTabName);
}

TSharedRef<SDockTab> FBlueprintProfilerModule::OnSpawnPluginTab(const FSpawnTabArgs& SpawnTabArgs)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			// Put your tab content here!
			SNew(SBlueprintProfilerWidget)
		];
}

void FBlueprintProfilerModule::PluginButtonClicked()
{
	FGlobalTabmanager::Get()->TryInvokeTab(BlueprintProfilerTabName);
}

void FBlueprintProfilerModule::RegisterMenus()
{
	// Owner will be used for cleanup in call to UToolMenus::UnregisterOwner
	FToolMenuOwnerScoped OwnerScoped(this);

	// Get localized menu text
	FText MenuLabel = FBlueprintProfilerLocalization::IsChinese() ? 
		FText::FromString(TEXT("蓝图分析器")) : 
		FText::FromString(TEXT("Blueprint Profiler"));
	FText MenuTooltip = FBlueprintProfilerLocalization::IsChinese() ? 
		FText::FromString(TEXT("打开蓝图分析器窗口")) : 
		FText::FromString(TEXT("Open Blueprint Profiler window"));

	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
		{
			FToolMenuSection& Section = Menu->FindOrAddSection("WindowLayout");
			FToolMenuEntry& Entry = Section.AddEntry(FToolMenuEntry::InitMenuEntry(
				FName("BlueprintProfiler"),
				MenuLabel,
				MenuTooltip,
				FSlateIcon(FBlueprintProfilerStyle::GetStyleSetName(), "BlueprintProfiler.PluginAction"),
				FUIAction(
					FExecuteAction::CreateRaw(this, &FBlueprintProfilerModule::PluginButtonClicked),
					FCanExecuteAction()
				)
			));
		}
	}

	{
		UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar");
		{
			FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("Settings");
			{
				FToolMenuEntry& Entry = Section.AddEntry(FToolMenuEntry::InitToolBarButton(
					FBlueprintProfilerCommands::Get().OpenPluginWindow,
					MenuLabel,
					MenuTooltip
				));
				Entry.SetCommandList(PluginCommands);
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FBlueprintProfilerModule, BlueprintProfiler)