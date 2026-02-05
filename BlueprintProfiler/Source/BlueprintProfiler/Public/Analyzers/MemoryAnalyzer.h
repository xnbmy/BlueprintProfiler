#pragma once

#include "CoreMinimal.h"
#include "Data/ProfilerDataTypes.h"
#include "Engine/Blueprint.h"

DECLARE_MULTICAST_DELEGATE_OneParam(FOnAnalysisComplete, const FMemoryAnalysisResult& /* Result */);

/**
 * Reference node for building reference trees
 */
struct BLUEPRINTPROFILER_API FReferenceNode
{
	TWeakObjectPtr<UObject> Object;
	FString ObjectName;
	FString ObjectType;
	float ObjectSize;
	TArray<TSharedPtr<FReferenceNode>> Children;

	FReferenceNode()
		: ObjectSize(0.0f)
	{
	}
};

/**
 * Memory Analyzer - analyzes blueprint memory usage and reference relationships
 */
class BLUEPRINTPROFILER_API FMemoryAnalyzer
{
public:
	FMemoryAnalyzer();
	~FMemoryAnalyzer();

	// Analysis control
	void AnalyzeBlueprint(UBlueprint* Blueprint);
	void AnalyzeBlueprintAsync(UBlueprint* Blueprint, FOnAnalysisComplete OnComplete);
	void CancelAnalysis();
	bool IsAnalysisInProgress() const { return bAnalysisInProgress; }

	// Results access
	FMemoryAnalysisResult GetAnalysisResult(UBlueprint* Blueprint) const;
	TArray<FLargeResourceReference> GetLargeResourceReferences(float SizeThresholdMB = 10.0f) const;
	TArray<FReferenceChain> GetReferenceChains(UBlueprint* Blueprint) const;

	// Large resource alert detection
	TArray<FLargeResourceReference> DetectLargeResourceAlerts(UBlueprint* Blueprint, float SizeThresholdMB = 10.0f) const;
	TArray<FLargeResourceReference> GetLargeResourceAlertsForProject(float SizeThresholdMB = 10.0f) const;
	void SetLargeResourceThreshold(float ThresholdMB);
	float GetLargeResourceThreshold() const { return LargeResourceThresholdMB; }

	// Events
	FOnAnalysisComplete OnAnalysisComplete;

	// Internal methods for async task (Exposed for FMemoryAnalysisTask)
	void CalculateInclusiveSize(UBlueprint* Blueprint, FMemoryAnalysisResult& Result);
	void CompleteAnalysis(UBlueprint* Blueprint, const FMemoryAnalysisResult& Result);

private:
	// Analysis methods
	void TraceReferenceChains(UObject* Object, TArray<FReferenceChain>& OutChains, int32 MaxDepth = 10);
	void FindLargeResourceReferences(UBlueprint* Blueprint, TArray<FLargeResourceReference>& OutReferences);
	float CalculateObjectSize(UObject* Object) const;
	void BuildReferenceTree(UObject* RootObject, TSharedPtr<FReferenceNode> ParentNode, TSet<UObject*>& VisitedObjects, int32 CurrentDepth, int32 MaxDepth);
	void BuildReferenceTreeWithReferenceFinder(UObject* RootObject, TSharedPtr<FReferenceNode> ParentNode, TSet<UObject*>& VisitedObjects, int32 CurrentDepth, int32 MaxDepth);

	// Large resource analysis methods
	void AnalyzePropertyForLargeResources(UBlueprint* Blueprint, class FProperty* Property, TArray<FLargeResourceReference>& OutReferences, float ThresholdMB = 10.0f);
	void AnalyzeGraphForLargeResources(UBlueprint* Blueprint, class UEdGraph* Graph, TArray<FLargeResourceReference>& OutReferences, float ThresholdMB = 10.0f);
	void AnalyzeVariableNode(UBlueprint* Blueprint, class UK2Node* VariableNode, TArray<FLargeResourceReference>& OutReferences, float ThresholdMB = 10.0f);

	// Utility methods
	bool IsLargeResource(UObject* Object, float ThresholdMB) const;
	FString GetObjectTypeName(UObject* Object) const;

private:
	TMap<TWeakObjectPtr<UObject>, FMemoryAnalysisResult> AnalysisResults;
	TArray<FLargeResourceReference> LargeResourceReferences;
	bool bAnalysisInProgress;
	bool bCancelRequested;
	float LargeResourceThresholdMB;

	// Async task management
	TSharedPtr<class FAsyncTask<class FMemoryAnalysisTask>> CurrentAnalysisTask;
	mutable FCriticalSection ResultsLock;
};