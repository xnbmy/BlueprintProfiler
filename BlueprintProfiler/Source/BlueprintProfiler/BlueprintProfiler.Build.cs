using UnrealBuildTool;

public class BlueprintProfiler : ModuleRules
{
	public BlueprintProfiler(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"UnrealEd",
				"Slate",
				"SlateCore",
				"EditorStyle",
				"EditorWidgets",
				"ToolMenus",
				"BlueprintGraph",
				"KismetCompiler",
				"GraphEditor",
				"Json",
				"JsonUtilities"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AssetRegistry",
				"BlueprintGraph",
				"Kismet",
				"KismetCompiler",
				"PropertyEditor",
				"WorkspaceMenuStructure",
				"LevelEditor",
				"Projects",
				"InputCore",
				"DesktopPlatform",
				"UnrealEd",
				"Slate",
				"SlateCore",
				"GraphEditor"
			}
		);
	}
}