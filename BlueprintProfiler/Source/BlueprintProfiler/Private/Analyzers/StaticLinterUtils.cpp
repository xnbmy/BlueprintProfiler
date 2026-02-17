// Copyright xnbmy 2026. All Rights Reserved.

#include "Analyzers/StaticLinter.h"
#include "Engine/Blueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/GameInstance.h"

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
