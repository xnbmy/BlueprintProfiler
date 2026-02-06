#pragma once

#include "CoreMinimal.h"
#include "Engine/Blueprint.h"
#include "ProfilerDataTypes.generated.h"

/**
 * Severity levels for profiler issues
 */
UENUM(BlueprintType)
enum class ESeverity : uint8
{
	Low = 0,
	Medium = 1,
	High = 2,
	Critical = 3
};

/**
 * Types of profiler data
 */
UENUM(BlueprintType)
enum class EProfilerDataType : uint8
{
	Runtime = 0,
	Lint = 1,
	Memory = 2
};

/**
 * Types of lint issues
 */
UENUM(BlueprintType)
enum class ELintIssueType : uint8
{
	DeadNode = 0,         // 无效节点
	OrphanNode = 1,       // 孤岛节点
	CastAbuse = 2,        // Cast滥用
	TickAbuse = 3,        // Tick滥用
	UnusedFunction = 4    // 未引用的函数
};

/**
 * Node execution statistics for runtime profiling
 */
USTRUCT(BlueprintType)
struct BLUEPRINTPROFILER_API FNodeExecutionStats
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	int32 ExecutionCount = 0;

	UPROPERTY(BlueprintReadOnly)
	float TotalExecutionTime = 0.0f;

	UPROPERTY(BlueprintReadOnly)
	float MinExecutionTime = FLT_MAX;

	UPROPERTY(BlueprintReadOnly)
	float MaxExecutionTime = 0.0f;

	TArray<float> ExecutionTimes;

	// 持久化节点信息（PIE结束后对象失效时仍能显示）
	FString CachedNodeName;
	FString CachedBlueprintName;
	FGuid CachedNodeGuid;

	float GetAverageExecutionTime() const
	{
		return ExecutionCount > 0 ? TotalExecutionTime / ExecutionCount : 0.0f;
	}

	float GetExecutionsPerSecond(float RecordingDuration) const
	{
		return RecordingDuration > 0.0f ? ExecutionCount / RecordingDuration : 0.0f;
	}
};

/**
 * Runtime execution data for a specific node
 */
USTRUCT(BlueprintType)
struct BLUEPRINTPROFILER_API FNodeExecutionData
{
	GENERATED_BODY()

	// UPROPERTY(BlueprintReadOnly) removed as TWeakObjectPtr is not supported in Blueprints
	TWeakObjectPtr<UObject> BlueprintObject;

	UPROPERTY(BlueprintReadOnly)
	FString NodeName;

	UPROPERTY(BlueprintReadOnly)
	FString BlueprintName;

	UPROPERTY(BlueprintReadOnly)
	FGuid NodeGuid;

	UPROPERTY(BlueprintReadOnly)
	int32 TotalExecutions = 0;

	UPROPERTY(BlueprintReadOnly)
	float AverageExecutionsPerSecond = 0.0f;

	UPROPERTY(BlueprintReadOnly)
	float TotalExecutionTime = 0.0f;

	UPROPERTY(BlueprintReadOnly)
	float AverageExecutionTime = 0.0f;
};

/**
 * Hot node information
 */
USTRUCT(BlueprintType)
struct BLUEPRINTPROFILER_API FHotNodeInfo
{
	GENERATED_BODY()

	// UPROPERTY(BlueprintReadOnly) removed as TWeakObjectPtr is not supported in Blueprints
	TWeakObjectPtr<UObject> BlueprintObject;

	UPROPERTY(BlueprintReadOnly)
	FGuid NodeGuid;

	UPROPERTY(BlueprintReadOnly)
	FString NodeName;

	UPROPERTY(BlueprintReadOnly)
	float ExecutionsPerSecond = 0.0f;

	UPROPERTY(BlueprintReadOnly)
	float AverageExecutionTime = 0.0f;

	UPROPERTY(BlueprintReadOnly)
	ESeverity Severity = ESeverity::Low;
};

/**
 * Tick abuse information
 */
USTRUCT(BlueprintType)
struct BLUEPRINTPROFILER_API FTickAbuseInfo
{
	GENERATED_BODY()

	// UPROPERTY(BlueprintReadOnly) removed as TWeakObjectPtr is not supported in Blueprints
	TWeakObjectPtr<UObject> BlueprintObject;

	UPROPERTY(BlueprintReadOnly)
	FString ActorName;

	UPROPERTY(BlueprintReadOnly)
	FString BlueprintName;

	UPROPERTY(BlueprintReadOnly)
	int32 ComplexityScore = 0;

	UPROPERTY(BlueprintReadOnly)
	ESeverity Severity = ESeverity::Low;
};

/**
 * Lint issue information
 */
USTRUCT(BlueprintType)
struct BLUEPRINTPROFILER_API FLintIssue
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	ELintIssueType Type = ELintIssueType::DeadNode;

	UPROPERTY(BlueprintReadOnly)
	FString BlueprintPath;

	UPROPERTY(BlueprintReadOnly)
	FString NodeName;

	UPROPERTY(BlueprintReadOnly)
	FString Description;

	UPROPERTY(BlueprintReadOnly)
	ESeverity Severity = ESeverity::Low;

	UPROPERTY(BlueprintReadOnly)
	FGuid NodeGuid;
};

/**
 * Large resource reference information
 */
USTRUCT(BlueprintType)
struct BLUEPRINTPROFILER_API FLargeResourceReference
{
	GENERATED_BODY()

	// UPROPERTY(BlueprintReadOnly) removed as TWeakObjectPtr is not supported in Blueprints
	TWeakObjectPtr<UObject> ReferencingBlueprint;

	// UPROPERTY(BlueprintReadOnly) removed as TWeakObjectPtr is not supported in Blueprints
	TWeakObjectPtr<UObject> ReferencedAsset;

	UPROPERTY(BlueprintReadOnly)
	FString VariableName;

	UPROPERTY(BlueprintReadOnly)
	float AssetSize = 0.0f;

	UPROPERTY(BlueprintReadOnly)
	FString AssetType;

	UPROPERTY(BlueprintReadOnly)
	FString ReferencePath;

	// Equality operator for TArray operations (Find, AddUnique, etc.)
	bool operator==(const FLargeResourceReference& Other) const
	{
		return ReferencingBlueprint == Other.ReferencingBlueprint &&
			ReferencedAsset == Other.ReferencedAsset &&
			VariableName == Other.VariableName;
	}
};

/**
 * Reference chain information
 */
USTRUCT(BlueprintType)
struct BLUEPRINTPROFILER_API FReferenceChain
{
	GENERATED_BODY()

	TArray<TWeakObjectPtr<UObject>> Chain;

	UPROPERTY(BlueprintReadOnly)
	float TotalSize = 0.0f;

	UPROPERTY(BlueprintReadOnly)
	FString Description;
};

/**
 * Memory analysis result
 */
USTRUCT(BlueprintType)
struct BLUEPRINTPROFILER_API FMemoryAnalysisResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	float InclusiveSize = 0.0f;           // 包含大小(MB)

	UPROPERTY(BlueprintReadOnly)
	int32 ReferenceDepth = 0;             // 引用深度

	UPROPERTY(BlueprintReadOnly)
	int32 TotalReferences = 0;            // 总引用数

	TArray<FReferenceChain> ReferenceChains;
	TArray<FLargeResourceReference> LargeReferences;
};

/**
 * Recording session information for runtime profiler
 */
USTRUCT(BlueprintType)
struct BLUEPRINTPROFILER_API FRecordingSession
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	FString SessionName;

	UPROPERTY(BlueprintReadOnly)
	FDateTime StartTime;

	UPROPERTY(BlueprintReadOnly)
	FDateTime EndTime;

	UPROPERTY(BlueprintReadOnly)
	float Duration = 0.0f; // In seconds

	UPROPERTY(BlueprintReadOnly)
	int32 TotalNodesRecorded = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 TotalExecutions = 0;

	UPROPERTY(BlueprintReadOnly)
	bool bIsActive = false;

	UPROPERTY(BlueprintReadOnly)
	bool bAutoStarted = false; // Whether this session was auto-started by PIE

	FRecordingSession()
	{
		StartTime = FDateTime::Now();
		EndTime = FDateTime::MinValue();
	}
};

/**
 * Recording state information
 */
UENUM(BlueprintType)
enum class ERecordingState : uint8
{
	Stopped = 0,
	Recording = 1,
	Paused = 2
};

/**
 * Unified profiler data item for UI display
 */
struct FProfilerDataItem
{
	EProfilerDataType Type;
	ESeverity Severity;
	FString Name;
	FString BlueprintName;
	FString Category;
	FString Description;
	float Value;

	// Runtime data
	TSharedPtr<FNodeExecutionData> RuntimeData;

	// Lint data
	TSharedPtr<FLintIssue> LintData;

	// Memory data
	TSharedPtr<FLargeResourceReference> MemoryData;

	// Target object reference (for jumping to editor)
	TWeakObjectPtr<UObject> TargetObject;

	// Node GUID (for jumping to specific nodes)
	FGuid NodeGuid;

	// Raw pointers for quick access
	TWeakObjectPtr<UObject> AssetObject;

	FProfilerDataItem()
		: Type(EProfilerDataType::Runtime)
		, Severity(ESeverity::Low)
		, Value(0.0f)
	{}
};
