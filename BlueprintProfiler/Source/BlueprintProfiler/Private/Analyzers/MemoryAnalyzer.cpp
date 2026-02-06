#include "Analyzers/MemoryAnalyzer.h"
#include "Engine/Blueprint.h"
#include "Engine/Texture2D.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Sound/SoundWave.h"
#include "Materials/Material.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/UObjectIterator.h"
#include "Async/Async.h"
#include "K2Node.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/UnrealType.h"
#include "Engine/UserDefinedStruct.h"
#include "Engine/DataTable.h"
#include "Particles/ParticleSystem.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimBlueprint.h"

/**
 * Async task for memory analysis
 */
class FMemoryAnalysisTask : public FNonAbandonableTask
{
public:
	FMemoryAnalysisTask(FMemoryAnalyzer* InAnalyzer, UBlueprint* InBlueprint)
		: Analyzer(InAnalyzer)
		, Blueprint(InBlueprint)
	{
	}

	void DoWork()
	{
		if (!Analyzer || !Blueprint)
		{
			return;
		}

		FMemoryAnalysisResult Result;
		Analyzer->CalculateInclusiveSize(Blueprint, Result);

		// Complete analysis on game thread
		AsyncTask(ENamedThreads::GameThread, [this, Result]()
		{
			if (Analyzer)
			{
				Analyzer->CompleteAnalysis(Blueprint, Result);
			}
		});
	}

	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FMemoryAnalysisTask, STATGROUP_ThreadPoolAsyncTasks);
	}

private:
	FMemoryAnalyzer* Analyzer;
	UBlueprint* Blueprint;
};



FMemoryAnalyzer::FMemoryAnalyzer()
	: bAnalysisInProgress(false)
	, bCancelRequested(false)
	, LargeResourceThresholdMB(10.0f) // Default 10MB threshold
{
}

FMemoryAnalyzer::~FMemoryAnalyzer()
{
	CancelAnalysis();
}

void FMemoryAnalyzer::AnalyzeBlueprint(UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		UE_LOG(LogTemp, Warning, TEXT("Cannot analyze null blueprint"));
		return;
	}

	if (bAnalysisInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("Analysis already in progress"));
		return;
	}

	FMemoryAnalysisResult Result;
	CalculateInclusiveSize(Blueprint, Result);

	{
		FScopeLock Lock(&ResultsLock);
		AnalysisResults.Add(TWeakObjectPtr<UObject>(Blueprint), Result);
	}

	OnAnalysisComplete.Broadcast(Result);
}

void FMemoryAnalyzer::AnalyzeBlueprintAsync(UBlueprint* Blueprint, FOnAnalysisComplete OnComplete)
{
	if (!Blueprint)
	{
		UE_LOG(LogTemp, Warning, TEXT("Cannot analyze null blueprint"));
		return;
	}

	if (bAnalysisInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("Analysis already in progress"));
		return;
	}

	bAnalysisInProgress = true;
	bCancelRequested = false;

	// Bind completion callback - wrap the multicast delegate in a lambda
	OnAnalysisComplete.AddLambda([OnComplete](const FMemoryAnalysisResult& Result) {
		OnComplete.Broadcast(Result);
	});

	// Start async analysis
	CurrentAnalysisTask = MakeShared<FAsyncTask<FMemoryAnalysisTask>>(this, Blueprint);
	CurrentAnalysisTask->StartBackgroundTask();
}

void FMemoryAnalyzer::CancelAnalysis()
{
	if (bAnalysisInProgress)
	{
		bCancelRequested = true;

		if (CurrentAnalysisTask.IsValid())
		{
			CurrentAnalysisTask->EnsureCompletion();
			CurrentAnalysisTask.Reset();
		}

		bAnalysisInProgress = false;
		bCancelRequested = false;

		UE_LOG(LogTemp, Log, TEXT("Memory analysis cancelled"));
	}
}

FMemoryAnalysisResult FMemoryAnalyzer::GetAnalysisResult(UBlueprint* Blueprint) const
{
	FScopeLock Lock(&ResultsLock);
	
	if (const FMemoryAnalysisResult* Result = AnalysisResults.Find(TWeakObjectPtr<UObject>(Blueprint)))
	{
		return *Result;
	}

	return FMemoryAnalysisResult();
}

TArray<FLargeResourceReference> FMemoryAnalyzer::GetLargeResourceReferences(float SizeThresholdMB) const
{
	TArray<FLargeResourceReference> FilteredReferences;

	for (const FLargeResourceReference& Reference : LargeResourceReferences)
	{
		if (Reference.AssetSize >= SizeThresholdMB)
		{
			FilteredReferences.Add(Reference);
		}
	}

	return FilteredReferences;
}

TArray<FReferenceChain> FMemoryAnalyzer::GetReferenceChains(UBlueprint* Blueprint) const
{
	FMemoryAnalysisResult Result = GetAnalysisResult(Blueprint);
	return Result.ReferenceChains;
}

void FMemoryAnalyzer::CalculateInclusiveSize(UBlueprint* Blueprint, FMemoryAnalysisResult& Result)
{
	if (!Blueprint)
	{
		return;
	}

	// Calculate direct size of the blueprint
	Result.InclusiveSize = CalculateObjectSize(Blueprint) / (1024.0f * 1024.0f); // Convert to MB

	// Find all referenced objects
	TArray<FReferenceChain> ReferenceChains;
	TraceReferenceChains(Blueprint, ReferenceChains);
	Result.ReferenceChains = ReferenceChains;

	// Calculate total inclusive size
	TSet<UObject*> CountedObjects;
	CountedObjects.Add(Blueprint);

	for (const FReferenceChain& Chain : ReferenceChains)
	{
		for (const TWeakObjectPtr<UObject>& ObjectPtr : Chain.Chain)
		{
			if (UObject* Object = ObjectPtr.Get())
			{
				if (!CountedObjects.Contains(Object))
				{
					Result.InclusiveSize += CalculateObjectSize(Object) / (1024.0f * 1024.0f);
					CountedObjects.Add(Object);
				}
			}
		}
	}

	Result.ReferenceDepth = 0;
	for (const FReferenceChain& Chain : ReferenceChains)
	{
		Result.ReferenceDepth = FMath::Max(Result.ReferenceDepth, Chain.Chain.Num());
	}

	Result.TotalReferences = CountedObjects.Num() - 1; // Exclude the blueprint itself

	// Find large resource references
	TArray<FLargeResourceReference> LargeReferences;
	FindLargeResourceReferences(Blueprint, LargeReferences);
	Result.LargeReferences = LargeReferences;

	// Update global large resource references
	{
		FScopeLock Lock(&ResultsLock);
		LargeResourceReferences.Append(LargeReferences);
	}
}

void FMemoryAnalyzer::TraceReferenceChains(UObject* Object, TArray<FReferenceChain>& OutChains, int32 MaxDepth)
{
	if (!Object || MaxDepth <= 0)
	{
		return;
	}

	// Build reference tree using proper UE reference finding
	TSharedPtr<FReferenceNode> RootNode = MakeShared<FReferenceNode>();
	RootNode->Object = Object;
	RootNode->ObjectName = Object->GetName();
	RootNode->ObjectType = GetObjectTypeName(Object);
	RootNode->ObjectSize = CalculateObjectSize(Object);

	TSet<UObject*> VisitedObjects;
	BuildReferenceTreeWithReferenceFinder(Object, RootNode, VisitedObjects, 0, MaxDepth);

	// Convert tree to chains
	TFunction<void(TSharedPtr<FReferenceNode>, TArray<TWeakObjectPtr<UObject>>&)> ConvertToChains;
	ConvertToChains = [&](TSharedPtr<FReferenceNode> Node, TArray<TWeakObjectPtr<UObject>>& CurrentChain)
	{
		if (!Node.IsValid())
		{
			return;
		}

		CurrentChain.Add(Node->Object);

		if (Node->Children.Num() == 0)
		{
			// Leaf node - create chain
			FReferenceChain Chain;
			Chain.Chain = CurrentChain;
			Chain.TotalSize = 0.0f;
			
			for (const TWeakObjectPtr<UObject>& ObjectPtr : CurrentChain)
			{
				if (UObject* ChainObject = ObjectPtr.Get())
				{
					Chain.TotalSize += CalculateObjectSize(ChainObject);
				}
			}
			
			Chain.Description = FString::Printf(TEXT("Reference chain: %s -> ... (%d objects, %.2f MB)"), 
				*Object->GetName(), CurrentChain.Num(), Chain.TotalSize / (1024.0f * 1024.0f));
			OutChains.Add(Chain);
		}
		else
		{
			// Continue traversing children
			for (TSharedPtr<FReferenceNode> Child : Node->Children)
			{
				ConvertToChains(Child, CurrentChain);
			}
		}

		CurrentChain.RemoveAt(CurrentChain.Num() - 1);
	};

	TArray<TWeakObjectPtr<UObject>> InitialChain;
	ConvertToChains(RootNode, InitialChain);
}

void FMemoryAnalyzer::BuildReferenceTreeWithReferenceFinder(UObject* RootObject, TSharedPtr<FReferenceNode> ParentNode, TSet<UObject*>& VisitedObjects, int32 CurrentDepth, int32 MaxDepth)
{
	if (!RootObject || !ParentNode.IsValid() || CurrentDepth >= MaxDepth || VisitedObjects.Contains(RootObject))
	{
		return;
	}

	VisitedObjects.Add(RootObject);

	// Find objects referenced by this object using UE's reference system
	TArray<UObject*> ReferencedObjects;
	
	// Use FReferenceFinder to find actual references
	class FSimpleReferenceFinder : public FReferenceCollector
	{
	public:
		TArray<UObject*>& References;
		TSet<UObject*>& Visited;
		
		FSimpleReferenceFinder(TArray<UObject*>& InReferences, TSet<UObject*>& InVisited)
			: References(InReferences), Visited(InVisited) {}
		
		virtual void HandleObjectReference(UObject*& Object, const UObject* ReferencingObject, const FProperty* ReferencingProperty) override
		{
			if (Object && !Visited.Contains(Object) && Object != ReferencingObject)
			{
				// Only include significant objects (assets, components, etc.)
				if (Object->IsAsset() || 
					Object->IsA<UActorComponent>() || 
					Object->IsA<UTexture>() || 
					Object->IsA<UStaticMesh>() || 
					Object->IsA<USkeletalMesh>() || 
					Object->IsA<USoundWave>() || 
					Object->IsA<UMaterial>() ||
					Object->IsA<UParticleSystem>() ||
					Object->IsA<UAnimSequence>())
				{
					References.AddUnique(Object);
				}
			}
		}
		
		virtual bool IsIgnoringArchetypeRef() const override { return false; }
		virtual bool IsIgnoringTransient() const override { return true; }
	};
	
	FSimpleReferenceFinder ReferenceFinder(ReferencedObjects, VisitedObjects);
	RootObject->CallAddReferencedObjects(ReferenceFinder);

	// Create child nodes for found references
	for (UObject* ReferencedObject : ReferencedObjects)
	{
		if (ReferencedObject && !VisitedObjects.Contains(ReferencedObject))
		{
			TSharedPtr<FReferenceNode> ChildNode = MakeShared<FReferenceNode>();
			ChildNode->Object = ReferencedObject;
			ChildNode->ObjectName = ReferencedObject->GetName();
			ChildNode->ObjectType = GetObjectTypeName(ReferencedObject);
			ChildNode->ObjectSize = CalculateObjectSize(ReferencedObject);
			
			ParentNode->Children.Add(ChildNode);
			
			// Recursively build children (with depth limit)
			if (CurrentDepth < MaxDepth - 1)
			{
				BuildReferenceTreeWithReferenceFinder(ReferencedObject, ChildNode, VisitedObjects, CurrentDepth + 1, MaxDepth);
			}
		}
	}
}

void FMemoryAnalyzer::FindLargeResourceReferences(UBlueprint* Blueprint, TArray<FLargeResourceReference>& OutReferences)
{
	if (!Blueprint)
	{
		return;
	}

	// Use the configured threshold
	float ThresholdMB = LargeResourceThresholdMB;

	// Analyze blueprint variables for large resource references
	if (UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass))
	{
		// Check all properties in the blueprint class
		for (FProperty* Property = GeneratedClass->PropertyLink; Property; Property = Property->PropertyLinkNext)
		{
			AnalyzePropertyForLargeResources(Blueprint, Property, OutReferences, ThresholdMB);
		}
	}

	// Analyze blueprint graphs for asset references
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph)
		{
			AnalyzeGraphForLargeResources(Blueprint, Graph, OutReferences, ThresholdMB);
		}
	}

	// Analyze function graphs
	for (UEdGraph* FunctionGraph : Blueprint->FunctionGraphs)
	{
		if (FunctionGraph)
		{
			AnalyzeGraphForLargeResources(Blueprint, FunctionGraph, OutReferences, ThresholdMB);
		}
	}

	// Analyze macro graphs
	for (UEdGraph* MacroGraph : Blueprint->MacroGraphs)
	{
		if (MacroGraph)
		{
			AnalyzeGraphForLargeResources(Blueprint, MacroGraph, OutReferences, ThresholdMB);
		}
	}

	// Log summary
	if (OutReferences.Num() > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("Found %d large resource references in blueprint %s (threshold: %.2f MB)"), 
			OutReferences.Num(), *Blueprint->GetName(), ThresholdMB);
	}
}

void FMemoryAnalyzer::AnalyzePropertyForLargeResources(UBlueprint* Blueprint, FProperty* Property, TArray<FLargeResourceReference>& OutReferences, float ThresholdMB)
{
	if (!Property || !Blueprint)
	{
		return;
	}

	// Check object properties
	if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
	{
		// Get default object to check default values
		if (UObject* DefaultObject = Blueprint->GeneratedClass->GetDefaultObject())
		{
			UObject* ReferencedObject = ObjectProperty->GetObjectPropertyValue_InContainer(DefaultObject);
			if (ReferencedObject && IsLargeResource(ReferencedObject, ThresholdMB))
			{
				FLargeResourceReference Reference;
				Reference.ReferencingBlueprint = Blueprint;
				Reference.ReferencedAsset = ReferencedObject;
				Reference.VariableName = Property->GetName();
				Reference.AssetSize = CalculateObjectSize(ReferencedObject) / (1024.0f * 1024.0f);
				Reference.AssetType = GetObjectTypeName(ReferencedObject);
				Reference.ReferencePath = ReferencedObject->GetPathName();
				
				OutReferences.Add(Reference);
			}
		}
	}
	// Check soft object properties
	else if (FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
	{
		if (UObject* DefaultObject = Blueprint->GeneratedClass->GetDefaultObject())
		{
			FSoftObjectPtr SoftObjectPtr = SoftObjectProperty->GetPropertyValue_InContainer(DefaultObject);
			if (UObject* ReferencedObject = SoftObjectPtr.LoadSynchronous())
			{
				if (IsLargeResource(ReferencedObject, ThresholdMB))
				{
					FLargeResourceReference Reference;
					Reference.ReferencingBlueprint = Blueprint;
					Reference.ReferencedAsset = ReferencedObject;
					Reference.VariableName = Property->GetName();
					Reference.AssetSize = CalculateObjectSize(ReferencedObject) / (1024.0f * 1024.0f);
					Reference.AssetType = GetObjectTypeName(ReferencedObject);
					Reference.ReferencePath = ReferencedObject->GetPathName();
					
					OutReferences.Add(Reference);
				}
			}
		}
	}
	// Check array properties
	else if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		if (FObjectProperty* InnerObjectProperty = CastField<FObjectProperty>(ArrayProperty->Inner))
		{
			if (UObject* DefaultObject = Blueprint->GeneratedClass->GetDefaultObject())
			{
				FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(DefaultObject));
				for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
				{
					UObject* ReferencedObject = InnerObjectProperty->GetObjectPropertyValue(ArrayHelper.GetRawPtr(Index));
					if (ReferencedObject && IsLargeResource(ReferencedObject, ThresholdMB))
					{
						FLargeResourceReference Reference;
						Reference.ReferencingBlueprint = Blueprint;
						Reference.ReferencedAsset = ReferencedObject;
						Reference.VariableName = FString::Printf(TEXT("%s[%d]"), *Property->GetName(), Index);
						Reference.AssetSize = CalculateObjectSize(ReferencedObject) / (1024.0f * 1024.0f);
						Reference.AssetType = GetObjectTypeName(ReferencedObject);
						Reference.ReferencePath = ReferencedObject->GetPathName();
						
						OutReferences.Add(Reference);
					}
				}
			}
		}
	}
}

void FMemoryAnalyzer::AnalyzeGraphForLargeResources(UBlueprint* Blueprint, UEdGraph* Graph, TArray<FLargeResourceReference>& OutReferences, float ThresholdMB)
{
	if (!Graph || !Blueprint)
	{
		return;
	}

	// Analyze all nodes in the graph
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UK2Node* K2Node = Cast<UK2Node>(Node))
		{
			// Check variable get/set nodes
			if (UK2Node_VariableGet* VarGetNode = Cast<UK2Node_VariableGet>(K2Node))
			{
				AnalyzeVariableNode(Blueprint, VarGetNode, OutReferences, ThresholdMB);
			}
			else if (UK2Node_VariableSet* VarSetNode = Cast<UK2Node_VariableSet>(K2Node))
			{
				AnalyzeVariableNode(Blueprint, VarSetNode, OutReferences, ThresholdMB);
			}
			
			// Check for literal asset references in node pins
			for (UEdGraphPin* Pin : K2Node->Pins)
			{
				if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
				{
					if (Pin->DefaultObject.Get() != nullptr)
					{
						UObject* ReferencedObject = Pin->DefaultObject.Get();
						if (ReferencedObject && IsLargeResource(ReferencedObject, ThresholdMB))
						{
							FLargeResourceReference Reference;
							Reference.ReferencingBlueprint = Blueprint;
							Reference.ReferencedAsset = ReferencedObject;
							Reference.VariableName = FString::Printf(TEXT("Node_%s_Pin_%s"), *K2Node->GetName(), *Pin->PinName.ToString());
							Reference.AssetSize = CalculateObjectSize(ReferencedObject) / (1024.0f * 1024.0f);
							Reference.AssetType = GetObjectTypeName(ReferencedObject);
							Reference.ReferencePath = ReferencedObject->GetPathName();
							
							OutReferences.Add(Reference);
						}
					}
				}
			}
		}
	}
}

void FMemoryAnalyzer::AnalyzeVariableNode(UBlueprint* Blueprint, UK2Node* VariableNode, TArray<FLargeResourceReference>& OutReferences, float ThresholdMB)
{
	if (!VariableNode || !Blueprint)
	{
		return;
	}

	// Get the variable property this node references
	FName VariableName = NAME_None;
	if (UK2Node_VariableGet* VarGetNode = Cast<UK2Node_VariableGet>(VariableNode))
	{
		VariableName = VarGetNode->VariableReference.GetMemberName();
	}
	else if (UK2Node_VariableSet* VarSetNode = Cast<UK2Node_VariableSet>(VariableNode))
	{
		VariableName = VarSetNode->VariableReference.GetMemberName();
	}

	if (VariableName != NAME_None && Blueprint->GeneratedClass)
	{
		if (FProperty* Property = Blueprint->GeneratedClass->FindPropertyByName(VariableName))
		{
			AnalyzePropertyForLargeResources(Blueprint, Property, OutReferences, ThresholdMB);
		}
	}
}

float FMemoryAnalyzer::CalculateObjectSize(UObject* Object) const
{
	if (!Object)
	{
		return 0.0f;
	}

	// Note: GetResourceSizeEx must be called on game thread
	// For background thread analysis, return 0 or use a different approach
	return 0.0f;
}

void FMemoryAnalyzer::BuildReferenceTree(UObject* RootObject, TSharedPtr<FReferenceNode> ParentNode, TSet<UObject*>& VisitedObjects, int32 CurrentDepth, int32 MaxDepth)
{
	if (!RootObject || !ParentNode.IsValid() || CurrentDepth >= MaxDepth || VisitedObjects.Contains(RootObject))
	{
		return;
	}

	VisitedObjects.Add(RootObject);

	// Find objects referenced by this object
	TArray<UObject*> ReferencedObjects;
	
	// This is a simplified reference finding - real implementation would use FReferenceFinder
	// For now, we'll just add some mock references for demonstration
	
	if (CurrentDepth < 2) // Limit depth for demo
	{
		// Add some mock child references
		for (int32 i = 0; i < FMath::RandRange(0, 3); ++i)
		{
			TSharedPtr<FReferenceNode> ChildNode = MakeShared<FReferenceNode>();
			ChildNode->Object = RootObject; // Mock reference
			ChildNode->ObjectName = FString::Printf(TEXT("Child_%d"), i);
			ChildNode->ObjectType = TEXT("MockObject");
			ChildNode->ObjectSize = FMath::RandRange(1000.0f, 10000.0f);
			
			ParentNode->Children.Add(ChildNode);
		}
	}
}

bool FMemoryAnalyzer::IsLargeResource(UObject* Object, float ThresholdMB) const
{
	if (!Object)
	{
		return false;
	}

	float SizeMB = CalculateObjectSize(Object) / (1024.0f * 1024.0f);
	
	// Apply different thresholds based on asset type
	if (Object->IsA<UTexture2D>())
	{
		// Textures: Check for 2048x2048 or larger (as per requirements)
		if (UTexture2D* Texture = Cast<UTexture2D>(Object))
		{
			return (Texture->GetSizeX() >= 2048 && Texture->GetSizeY() >= 2048) || SizeMB >= ThresholdMB;
		}
	}
	else if (Object->IsA<UStaticMesh>())
	{
		// Static meshes: Use size threshold
		return SizeMB >= (ThresholdMB * 0.5f); // Lower threshold for meshes
	}
	else if (Object->IsA<USkeletalMesh>())
	{
		// Skeletal meshes: Use size threshold
		return SizeMB >= (ThresholdMB * 0.5f); // Lower threshold for skeletal meshes
	}
	else if (Object->IsA<USoundWave>())
	{
		// Audio: Use size threshold
		return SizeMB >= (ThresholdMB * 2.0f); // Higher threshold for audio
	}
	else if (Object->IsA<UParticleSystem>())
	{
		// Particle systems: Use size threshold
		return SizeMB >= (ThresholdMB * 0.25f); // Lower threshold for particles
	}
	else if (Object->IsA<UAnimSequence>())
	{
		// Animation sequences: Use size threshold
		return SizeMB >= (ThresholdMB * 0.5f); // Lower threshold for animations
	}
	
	// Default threshold for other asset types
	return SizeMB >= ThresholdMB;
}

FString FMemoryAnalyzer::GetObjectTypeName(UObject* Object) const
{
	if (!Object)
	{
		return TEXT("Unknown");
	}

	return Object->GetClass()->GetName();
}

void FMemoryAnalyzer::CompleteAnalysis(UBlueprint* Blueprint, const FMemoryAnalysisResult& Result)
{
	{
		FScopeLock Lock(&ResultsLock);
		AnalysisResults.Add(TWeakObjectPtr<UObject>(Blueprint), Result);
	}

	bAnalysisInProgress = false;
	bCancelRequested = false;

	if (CurrentAnalysisTask.IsValid())
	{
		CurrentAnalysisTask.Reset();
	}

	OnAnalysisComplete.Broadcast(Result);

	UE_LOG(LogTemp, Log, TEXT("Memory analysis completed for blueprint: %s (Size: %.2f MB)"), 
		*Blueprint->GetName(), Result.InclusiveSize);
}

TArray<FLargeResourceReference> FMemoryAnalyzer::DetectLargeResourceAlerts(UBlueprint* Blueprint, float SizeThresholdMB) const
{
	TArray<FLargeResourceReference> Alerts;
	
	if (!Blueprint)
	{
		return Alerts;
	}

	// Get analysis result for this blueprint
	FMemoryAnalysisResult Result = GetAnalysisResult(Blueprint);
	
	// Filter large references based on threshold
	for (const FLargeResourceReference& Reference : Result.LargeReferences)
	{
		if (Reference.AssetSize >= SizeThresholdMB)
		{
			Alerts.Add(Reference);
		}
	}

	// If no analysis result exists, perform quick analysis
	if (Result.InclusiveSize == 0.0f)
	{
		TArray<FLargeResourceReference> QuickReferences;
		const_cast<FMemoryAnalyzer*>(this)->FindLargeResourceReferences(Blueprint, QuickReferences);
		
		for (const FLargeResourceReference& Reference : QuickReferences)
		{
			if (Reference.AssetSize >= SizeThresholdMB)
			{
				Alerts.Add(Reference);
			}
		}
	}

	return Alerts;
}

TArray<FLargeResourceReference> FMemoryAnalyzer::GetLargeResourceAlertsForProject(float SizeThresholdMB) const
{
	TArray<FLargeResourceReference> ProjectAlerts;
	
	FScopeLock Lock(&ResultsLock);
	
	// Collect alerts from all analyzed blueprints
	for (const auto& ResultPair : AnalysisResults)
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(ResultPair.Key.Get()))
		{
			TArray<FLargeResourceReference> BlueprintAlerts = DetectLargeResourceAlerts(Blueprint, SizeThresholdMB);
			ProjectAlerts.Append(BlueprintAlerts);
		}
	}

	// Also include global large resource references
	for (const FLargeResourceReference& Reference : LargeResourceReferences)
	{
		if (Reference.AssetSize >= SizeThresholdMB)
		{
			ProjectAlerts.AddUnique(Reference);
		}
	}

	// Sort by asset size (largest first)
	ProjectAlerts.Sort([](const FLargeResourceReference& A, const FLargeResourceReference& B)
	{
		return A.AssetSize > B.AssetSize;
	});

	return ProjectAlerts;
}

void FMemoryAnalyzer::SetLargeResourceThreshold(float ThresholdMB)
{
	LargeResourceThresholdMB = FMath::Max(0.1f, ThresholdMB); // Minimum 0.1MB threshold
	UE_LOG(LogTemp, Log, TEXT("Large resource threshold set to %.2f MB"), LargeResourceThresholdMB);
}

void FMemoryAnalyzer::AnalyzeAssetReferenceCounts()
{
	if (bAnalysisInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("Analysis already in progress"));
		return;
	}

	bAnalysisInProgress = true;
	bCancelRequested = false;
	AssetReferenceCounts.Empty();

	UE_LOG(LogTemp, Log, TEXT("Starting asset reference count analysis..."));

	// Find all assets in the project (on game thread)
	TArray<UObject*> AllAssets;
	FindAllAssets(AllAssets);

	UE_LOG(LogTemp, Log, TEXT("Found %d assets to analyze"), AllAssets.Num());

	// Store assets for processing
	PendingAssets = AllAssets;
	CurrentAssetIndex = 0;
	ReferenceCountMap.Empty();

	// Start processing on next tick
	FTimerHandle TimerHandle;
	GEditor->GetTimerManager()->SetTimer(TimerHandle, [this]()
	{
		ProcessNextAssetBatch();
	}, 0.01f, false);
}

void FMemoryAnalyzer::ProcessNextAssetBatch()
{
	if (bCancelRequested)
	{
		CompleteReferenceCountAnalysis();
		return;
	}

	// Process a small batch of assets per frame to avoid blocking
	int32 BatchSize = 10;
	int32 ProcessedInBatch = 0;

	while (CurrentAssetIndex < PendingAssets.Num() && ProcessedInBatch < BatchSize)
	{
		UObject* Asset = PendingAssets[CurrentAssetIndex];
		if (Asset)
		{
			CountAssetReferences(Asset, ReferenceCountMap);
		}

		CurrentAssetIndex++;
		ProcessedInBatch++;
	}

	// Update progress
	if (OnAnalysisProgress.IsBound())
	{
		float Progress = (float)CurrentAssetIndex / (float)PendingAssets.Num();
		OnAnalysisProgress.Broadcast(Progress);
	}

	// Check if we're done
	if (CurrentAssetIndex >= PendingAssets.Num())
	{
		CompleteReferenceCountAnalysis();
	}
	else
	{
		// Schedule next batch
		FTimerHandle TimerHandle;
		GEditor->GetTimerManager()->SetTimer(TimerHandle, [this]()
		{
			ProcessNextAssetBatch();
		}, 0.01f, false);
	}
}

void FMemoryAnalyzer::CompleteReferenceCountAnalysis()
{
	FScopeLock Lock(&ResultsLock);

	// Convert map to array and sort
	for (const auto& Pair : ReferenceCountMap)
	{
		AssetReferenceCounts.Add(Pair.Value);
	}

	// Sort by reference count (descending)
	AssetReferenceCounts.Sort();

	PendingAssets.Empty();
	ReferenceCountMap.Empty();
	CurrentAssetIndex = 0;

	bAnalysisInProgress = false;
	bCancelRequested = false;

	UE_LOG(LogTemp, Log, TEXT("Asset reference count analysis complete. Found %d referenced assets"), AssetReferenceCounts.Num());

	// Broadcast completion
	FMemoryAnalysisResult DummyResult;
	OnReferenceCountComplete.Broadcast(DummyResult);
}

TArray<FAssetReferenceCount> FMemoryAnalyzer::GetAssetReferenceCounts() const
{
	FScopeLock Lock(&ResultsLock);
	return AssetReferenceCounts;
}

TArray<FAssetReferenceCount> FMemoryAnalyzer::GetTopReferencedAssets(int32 Count) const
{
	FScopeLock Lock(&ResultsLock);
	
	TArray<FAssetReferenceCount> TopAssets;
	int32 NumToReturn = FMath::Min(Count, AssetReferenceCounts.Num());
	
	for (int32 i = 0; i < NumToReturn; i++)
	{
		TopAssets.Add(AssetReferenceCounts[i]);
	}
	
	return TopAssets;
}

void FMemoryAnalyzer::ClearReferenceCountData()
{
	FScopeLock Lock(&ResultsLock);
	AssetReferenceCounts.Empty();
}

void FMemoryAnalyzer::FindAllAssets(TArray<UObject*>& OutAssets)
{
	// Find all assets using UObject iterator
	for (TObjectIterator<UObject> It; It; ++It)
	{
		UObject* Object = *It;
		if (!Object)
		{
			continue;
		}

		// Only include assets (not transient objects)
		if (Object->IsAsset() && IsValid(Object))
		{
			OutAssets.Add(Object);
		}
	}
}

void FMemoryAnalyzer::CountAssetReferences(UObject* Asset, TMap<FString, FAssetReferenceCount>& OutReferenceCounts)
{
	if (!Asset)
	{
		return;
	}

	// Count references for this asset
	int32 ReferenceCount = CountAssetReferencesInternal(Asset);

	FString AssetPath = Asset->GetPathName();
	
	// Create or update reference count entry
	FAssetReferenceCount& RefCount = OutReferenceCounts.FindOrAdd(AssetPath);
	RefCount.AssetPath = AssetPath;
	RefCount.AssetName = Asset->GetName();
	RefCount.AssetType = Asset->GetClass()->GetName();
	RefCount.AssetSize = CalculateObjectSize(Asset) / (1024.0f * 1024.0f);
	RefCount.ReferenceCount = ReferenceCount;
}

int32 FMemoryAnalyzer::CountAssetReferencesInternal(UObject* Asset)
{
	if (!Asset)
	{
		return 0;
	}

	// Use asset registry to find referencers
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Get package name from asset path
	FString AssetPath = Asset->GetPathName();
	FName PackageName = FName(*FPackageName::ObjectPathToPackageName(AssetPath));
	
	if (PackageName.IsNone())
	{
		return 0;
	}

	// Get all assets that reference this asset
	TArray<FAssetIdentifier> Referencers;
	AssetRegistry.GetReferencers(PackageName, Referencers);

	// Count valid referencers (excluding self-references)
	int32 Count = 0;
	for (const FAssetIdentifier& Referencer : Referencers)
	{
		if (!Referencer.PackageName.IsNone() && Referencer.PackageName != PackageName)
		{
			Count++;
		}
	}

	return Count;
}