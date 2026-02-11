#include "Analyzers/StaticLinter.h"
#include "BlueprintProfilerLocalization.h"
#include "Engine/Blueprint.h"
#include "K2Node.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_AssignDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/GameInstance.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"

// 存储跨蓝图引用的函数
TSet<FName> FStaticLinter::ReferencedFunctions;
TMap<FName, int32> FStaticLinter::FunctionCallCount;

void FStaticLinter::DetectDeadNodes(UBlueprint* Blueprint, TArray<FLintIssue>& OutIssues)
{
	if (!Blueprint)
	{
		return;
	}

	// Get all graphs to analyze
	TArray<UEdGraph*> AllGraphs = GetAllGraphs(Blueprint);

	// Track all referenced variables and functions
	TSet<FName> LocalReferencedVariables;
	TSet<FName> LocalReferencedFunctions;
	TSet<FGuid> LocalReferencedCustomEvents;

	// First pass: collect all references
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			// Track variable references
			if (UK2Node_VariableGet* VarGetNode = Cast<UK2Node_VariableGet>(Node))
			{
				// Check if this variable get node has any output connections
				bool bHasOutputConnections = false;
				for (UEdGraphPin* Pin : VarGetNode->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0)
					{
						bHasOutputConnections = true;
						LocalReferencedVariables.Add(VarGetNode->VariableReference.GetMemberName());
						break;
					}
				}
			}
			else if (UK2Node_VariableSet* VarSetNode = Cast<UK2Node_VariableSet>(Node))
			{
				// Variable set nodes always count as references
				LocalReferencedVariables.Add(VarSetNode->VariableReference.GetMemberName());
			}
			else if (UK2Node_CallFunction* FuncCallNode = Cast<UK2Node_CallFunction>(Node))
			{
				// Track function calls
				if (FuncCallNode->FunctionReference.GetMemberName() != NAME_None)
				{
					LocalReferencedFunctions.Add(FuncCallNode->FunctionReference.GetMemberName());
					ReferencedFunctions.Add(FuncCallNode->FunctionReference.GetMemberName());
					FunctionCallCount.FindOrAdd(FuncCallNode->FunctionReference.GetMemberName())++;
				}
			}
			else if (UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
			{
				// Track custom event references
				LocalReferencedCustomEvents.Add(CustomEventNode->NodeGuid);
			}
			// Track event dispatcher references
			else if (UK2Node_BaseMCDelegate* DelegateNode = Cast<UK2Node_BaseMCDelegate>(Node))
			{
				// Get the delegate name (event dispatcher name)
				FName DelegateName = DelegateNode->GetPropertyName();
				if (DelegateName != NAME_None)
				{
					// Track the delegate as referenced
					LocalReferencedFunctions.Add(DelegateName);
					ReferencedFunctions.Add(DelegateName);
					
					// For AddDelegate and AssignDelegate nodes, also track the connected custom event
					if (Cast<UK2Node_AddDelegate>(Node) || Cast<UK2Node_AssignDelegate>(Node))
					{
						// Use GetDelegatePin() to get the delegate input pin
						UEdGraphPin* DelegatePin = DelegateNode->GetDelegatePin();
						if (DelegatePin && DelegatePin->LinkedTo.Num() > 0)
						{
							// The connected node should be a custom event
							for (UEdGraphPin* LinkedPin : DelegatePin->LinkedTo)
							{
								if (LinkedPin && LinkedPin->GetOwningNode())
								{
									UEdGraphNode* ConnectedNode = LinkedPin->GetOwningNode();
									if (UK2Node_CustomEvent* BoundEventNode = Cast<UK2Node_CustomEvent>(ConnectedNode))
									{
										FName EventName = BoundEventNode->GetFunctionName();
										if (EventName != NAME_None)
										{
											LocalReferencedFunctions.Add(EventName);
											ReferencedFunctions.Add(EventName);
											LocalReferencedCustomEvents.Add(BoundEventNode->NodeGuid);
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}

	// Second pass: find unreferenced variables and functions
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			// Check for unreferenced variable get nodes
			if (UK2Node_VariableGet* VarGetNode = Cast<UK2Node_VariableGet>(Node))
			{
				bool bHasConnections = false;
				for (UEdGraphPin* Pin : VarGetNode->Pins)
				{
					if (Pin && Pin->LinkedTo.Num() > 0)
					{
						bHasConnections = true;
						break;
					}
				}

				if (!bHasConnections)
				{
					FLintIssue Issue;
					Issue.Type = ELintIssueType::DeadNode;
					Issue.BlueprintPath = Blueprint->GetPathName();
					Issue.NodeName = VarGetNode->VariableReference.GetMemberName().ToString();
					if (FBlueprintProfilerLocalization::IsChinese())
					{
						Issue.Description = FString::Printf(TEXT("变量 '%s' 被获取但从未使用"), *Issue.NodeName);
					}
					else
					{
						Issue.Description = FString::Printf(TEXT("Variable '%s' is retrieved but never used"), *Issue.NodeName);
					}
					Issue.Severity = CalculateIssueSeverity(ELintIssueType::DeadNode);
					Issue.NodeGuid = VarGetNode->NodeGuid;
					
					OutIssues.Add(Issue);
				}
			}
			// Skip Component Bound Events - they are triggered by component events (overlap, hit, etc.)
		if (UK2Node_ComponentBoundEvent* ComponentBoundEvent = Cast<UK2Node_ComponentBoundEvent>(Node))
		{
			continue;
		}
		// Check for unreferenced function definitions
		else if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			// Skip all built-in events (Receive*)
			FName EventName = EventNode->GetFunctionName();
			if (EventName.ToString().StartsWith(TEXT("Receive")))
			{
				continue; // Skip all Receive* events
			}

			// Skip interface events - they are called by the blueprint system automatically
			if (EventNode->IsInterfaceEventNode())
			{
				continue;
			}

			// Check if this custom event is referenced
			bool bIsReferenced = LocalReferencedFunctions.Contains(EventName) || ReferencedFunctions.Contains(EventName);

			if (!bIsReferenced)
			{
				// Also check for direct event calls by GUID
				bIsReferenced = LocalReferencedCustomEvents.Contains(EventNode->NodeGuid);
			}

			if (!bIsReferenced)
			{
				FLintIssue Issue;
				Issue.Type = ELintIssueType::DeadNode;
				Issue.BlueprintPath = Blueprint->GetPathName();
				Issue.NodeName = EventName.ToString();
			if (FBlueprintProfilerLocalization::IsChinese())
			{
				Issue.Description = FString::Printf(TEXT("自定义事件 '%s' 已定义但从未被调用"), *Issue.NodeName);
			}
			else
			{
				Issue.Description = FString::Printf(TEXT("Custom event '%s' is defined but never called"), *Issue.NodeName);
			}
			Issue.Severity = ESeverity::Low; // 未调用的事件不一定严重
				Issue.NodeGuid = EventNode->NodeGuid;

				OutIssues.Add(Issue);
			}
		}
	}
	}

	// Check for unreferenced blueprint variables
	for (FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		// Skip Event Dispatchers (multicast delegates) - they are not regular variables
		// Event Dispatchers are detected separately via UK2Node_BaseMCDelegate nodes
		if (Variable.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
		{
			continue;
		}
		
		if (!LocalReferencedVariables.Contains(Variable.VarName))
		{
			FLintIssue Issue;
			Issue.Type = ELintIssueType::DeadNode;
			Issue.BlueprintPath = Blueprint->GetPathName();
			Issue.NodeName = Variable.VarName.ToString();
			if (FBlueprintProfilerLocalization::IsChinese())
			{
				Issue.Description = FString::Printf(TEXT("蓝图变量 '%s' 已声明但从未使用"), *Issue.NodeName);
			}
			else
			{
				Issue.Description = FString::Printf(TEXT("Blueprint variable '%s' is declared but never used"), *Issue.NodeName);
			}
			Issue.Severity = CalculateIssueSeverity(ELintIssueType::DeadNode);
			// Note: Variables don't have NodeGuid, so we leave it empty
			
			OutIssues.Add(Issue);
		}
	}
	
	// Check for unreferenced Event Dispatchers
	// Event Dispatchers are stored as variables but referenced via UK2Node_BaseMCDelegate nodes
	for (FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		// Only check Event Dispatchers
		if (Variable.VarType.PinCategory != UEdGraphSchema_K2::PC_MCDelegate)
		{
			continue;
		}
		
		FName DispatcherName = Variable.VarName;
		bool bIsReferenced = LocalReferencedFunctions.Contains(DispatcherName) || ReferencedFunctions.Contains(DispatcherName);
		
		if (!bIsReferenced)
		{
			FLintIssue Issue;
			Issue.Type = ELintIssueType::DeadNode;
			Issue.BlueprintPath = Blueprint->GetPathName();
			Issue.NodeName = DispatcherName.ToString();
			if (FBlueprintProfilerLocalization::IsChinese())
			{
				Issue.Description = FString::Printf(TEXT("事件分发器 '%s' 已声明但从未使用"), *Issue.NodeName);
			}
			else
			{
				Issue.Description = FString::Printf(TEXT("Event Dispatcher '%s' is declared but never used"), *Issue.NodeName);
			}
			Issue.Severity = ESeverity::Low; // Event dispatchers being unused is low severity
			// Note: Event Dispatchers don't have NodeGuid, so we leave it empty
			
			OutIssues.Add(Issue);
		}
	}
}

void FStaticLinter::DetectOrphanNodes(UBlueprint* Blueprint, TArray<FLintIssue>& OutIssues)
{
	if (!Blueprint)
	{
		return;
	}

	// Skip interface blueprints - their functions are called by other blueprints that implement the interface
	// Interface functions don't need to be connected to execution flow in the interface itself
	if (Blueprint->BlueprintType == BPTYPE_Interface)
	{
		return;
	}

	// Get all graphs to analyze
	TArray<UEdGraph*> AllGraphs = GetAllGraphs(Blueprint);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			// Skip Event nodes - they're entry points and don't need to be connected
			if (Node->IsA<UK2Node_Event>() || Node->IsA<UK2Node_CustomEvent>() || Node->IsA<UK2Node_ComponentBoundEvent>())
			{
				continue;
			}

			// Skip macro instance nodes - they are references to other graphs
			if (Node->IsA<UK2Node_MacroInstance>())
			{
				continue;
			}

			// Skip tunnel nodes (macro entry/exit nodes) - they are entry points in macro graphs
			// Use class name check to avoid including K2Node_Tunnel.h which may cause build issues
			FString NodeClassName = Node->GetClass()->GetName();
			if (NodeClassName.Contains(TEXT("K2Node_Tunnel")))
			{
				// Tunnel nodes in macro graphs act as entry/exit points and should not be flagged as orphans
				continue;
			}

			// Store class name for later use (avoid redefinition)
			const FString& NodeClassNameRef = NodeClassName;

			// Check for pure nodes (computation nodes) without execution connections
			if (UK2Node* K2Node = Cast<UK2Node>(Node))
			{
				if (K2Node->IsNodePure())
				{
					// 获取节点标题
					FString NodeTitle = K2Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

					// 跳过特殊的纯节点（不需要输出连接）
					// 1. 变更路线节点（Reroute Node）- 特殊的纯节点
					if (NodeTitle.Contains(TEXT("变更路线")) ||
						NodeTitle.Contains(TEXT("Reroute")) ||
						NodeTitle.Contains(TEXT("Set Return")) ||
						NodeTitle.Contains(TEXT("Return")) ||
						NodeTitle.Contains(TEXT("返回")))
					{
						continue;
					}

					// 2. 字面量和常量节点
					FString NodeClass = K2Node->GetClass()->GetName();
					if (NodeClass.Contains(TEXT("Literal")) ||
						NodeClass.Contains(TEXT("Constant")))
					{
						continue;
					}

					// 3. 跳过纯工具节点（Make, Select, Append 等）
					// 注意：Branch 和 Break 可能有执行引脚，不在此处跳过，让后面的逻辑处理
					if (NodeTitle.Contains(TEXT("Make")) ||
						NodeTitle.Contains(TEXT("Select")) ||
						NodeTitle.Contains(TEXT("Append")))
					{
						continue;
					}

					bool bHasDataOutputConnections = false;
					bool bHasDataInputConnections = false;

					// Check data pins for connections (ignore exec pins for pure nodes)
					for (UEdGraphPin* Pin : K2Node->Pins)
					{
						if (Pin && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
						{
							if (Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0)
							{
								bHasDataOutputConnections = true;
							}
							else if (Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0)
							{
								bHasDataInputConnections = true;
							}
						}
					}

					// Report if pure node has no data output connections (输出未连接)
					if (!bHasDataOutputConnections)
					{
						FLintIssue Issue;
						Issue.Type = ELintIssueType::OrphanNode;
						Issue.BlueprintPath = Blueprint->GetPathName();
						Issue.NodeName = NodeTitle;
						if (FBlueprintProfilerLocalization::IsChinese())
						{
							Issue.Description = FString::Printf(TEXT("纯节点 '%s' 的输出没有连接到任何节点"), *Issue.NodeName);
						}
						else
						{
							Issue.Description = FString::Printf(TEXT("Pure node '%s' has no output connections"), *Issue.NodeName);
						}
						Issue.Severity = ESeverity::Low;
						Issue.NodeGuid = K2Node->NodeGuid;

						OutIssues.Add(Issue);
					}
					// Report if pure node has data inputs but no data output connections (有输入但无输出)
					else if (bHasDataInputConnections && !bHasDataOutputConnections)
					{
						FLintIssue Issue;
						Issue.Type = ELintIssueType::OrphanNode;
						Issue.BlueprintPath = Blueprint->GetPathName();
						Issue.NodeName = NodeTitle;
						if (FBlueprintProfilerLocalization::IsChinese())
						{
							Issue.Description = FString::Printf(TEXT("纯节点 '%s' 有输入但输出未连接"), *Issue.NodeName);
						}
						else
						{
							Issue.Description = FString::Printf(TEXT("Pure node '%s' has inputs but no output connections"), *Issue.NodeName);
						}
						Issue.Severity = ESeverity::Low;
						Issue.NodeGuid = K2Node->NodeGuid;

						OutIssues.Add(Issue);
					}
				}
				// Check for nodes with execution pins (non-pure nodes)
				else
				{
					// 首先检查是否应该跳过此节点（在检查引脚连接之前）
					FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
					bool bShouldSkip = false;

					// 1. 跳过 Event、CustomEvent、FunctionEntry 和 ComponentBoundEvent（入口节点）
					if (Node->IsA<UK2Node_Event>() || Node->IsA<UK2Node_CustomEvent>() || 
					    Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_ComponentBoundEvent>())
					{
						bShouldSkip = true;
					}
					// 2. 跳过构造脚本节点（按标题判断）
					else if (NodeTitle.Contains(TEXT("构造脚本")) ||
						NodeTitle.Contains(TEXT("Construction Script")))
					{
						bShouldSkip = true;
					}
					// 3. 跳过输入操作节点（Input Action）- 这些是由输入系统触发的事件节点
				// 包括增强输入系统(Enhanced Input)和传统输入系统的输入操作
				else if (NodeTitle.Contains(TEXT("Thumbstick")) ||
					NodeTitle.Contains(TEXT("Touch")) ||
					NodeTitle.Contains(TEXT("Input Action")) ||
					NodeTitle.Contains(TEXT("Input Axis")) ||
					NodeTitle.Contains(TEXT("Enhanced Input")) ||
					NodeTitle.Contains(TEXT("IA_")) ||  // Input Action 缩写
					NodeTitle.Contains(TEXT("IM_")) ||
					NodeTitle.Contains(TEXT("输入操作")) ||  // 中文：输入操作
					NodeTitle.Contains(TEXT("Pressed")) ||  // 输入按下事件
					NodeTitle.Contains(TEXT("Released")) ||  // 输入释放事件
					NodeTitle.Contains(TEXT("Key")))  // 按键事件
				{
					bShouldSkip = true;
				}
				// 4. 检查节点类型 - 如果是输入操作节点类也跳过
				else
				{
					if (NodeClassNameRef.Contains(TEXT("Input")) ||
						NodeClassNameRef.Contains(TEXT("EnhancedInput")))
					{
						bShouldSkip = true;
					}
				}

					// 如果不应该跳过，再检查执行引脚连接状态
				if (!bShouldSkip)
				{
					bool bHasExecInput = false;
					bool bHasExecInputConnected = false;
					bool bHasExecOutput = false;
					bool bHasExecOutputConnected = false;
					bool bHasExecutionPins = false;

					// 检查所有执行引脚的连接状态
					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
						{
							bHasExecutionPins = true;
							if (Pin->Direction == EGPD_Input)
							{
								bHasExecInput = true;
								if (Pin->LinkedTo.Num() > 0)
								{
									bHasExecInputConnected = true;
								}
							}
							else if (Pin->Direction == EGPD_Output)
							{
								bHasExecOutput = true;
								if (Pin->LinkedTo.Num() > 0)
								{
									bHasExecOutputConnected = true;
								}
							}
						}
					}

					// 5. 自动检测入口节点：如果节点有执行输出但没有执行输入，说明是入口节点（如事件、输入操作等）
					if (bHasExecOutput && !bHasExecInput)
					{
						// 这是入口节点，不需要上游连接
						bShouldSkip = true;
					}

					// 当输入执行引脚未连接时报告（输出引脚连接不影响）
					// 这会正确跳过：
					// - Event 节点（没有输入但有输出）- 已在上面跳过
					// - 正常连接的节点（输入已连接）
					// 会报告：
					// - Orphan node (input not connected, regardless of output connection)
					if (!bShouldSkip && bHasExecutionPins && bHasExecInput && !bHasExecInputConnected)
						{
							FLintIssue Issue;
							Issue.Type = ELintIssueType::OrphanNode;
							Issue.BlueprintPath = Blueprint->GetPathName();
							Issue.NodeName = NodeTitle;
							if (FBlueprintProfilerLocalization::IsChinese())
							{
								Issue.Description = FString::Printf(TEXT("执行节点 '%s' 未连接到任何执行流程（孤立节点）"), *Issue.NodeName);
							}
							else
							{
								Issue.Description = FString::Printf(TEXT("Execution node '%s' is not connected to any execution flow (orphan node)"), *Issue.NodeName);
							}
							Issue.Severity = ESeverity::High;
							Issue.NodeGuid = K2Node->NodeGuid;

							OutIssues.Add(Issue);
						}
					}
				}
			}
		}
	}
}

void FStaticLinter::DetectCastAbuse(UBlueprint* Blueprint, TArray<FLintIssue>& OutIssues)
{
	if (!Blueprint)
	{
		return;
	}

	// Get all graphs to analyze
	TArray<UEdGraph*> AllGraphs = GetAllGraphs(Blueprint);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			// Look for cast nodes
			if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
			{
				bool bIsInProblematicContext = false;
				ESeverity CastSeverity = ESeverity::Low;
				FString ContextDescription;

				// Analyze the context of this cast node
				TSet<UEdGraphNode*> VisitedNodes;
				if (IsNodeInTickContext(CastNode, VisitedNodes))
				{
					bIsInProblematicContext = true;
					CastSeverity = ESeverity::High;
					ContextDescription = TEXT("in Tick event context");
				}
				else if (IsNodeInLoopContext(CastNode, VisitedNodes))
				{
					bIsInProblematicContext = true;
					CastSeverity = ESeverity::Medium;
					ContextDescription = TEXT("in loop context");
				}
				else if (IsNodeInFrequentlyCalledFunction(CastNode, Graph))
				{
					bIsInProblematicContext = true;
					CastSeverity = ESeverity::Medium;
					ContextDescription = TEXT("in frequently called function");
				}

				// Check if it's a hard reference cast (more expensive)
				bool bIsHardReferenceCast = IsHardReferenceCast(CastNode);
				if (bIsHardReferenceCast && bIsInProblematicContext)
				{
					CastSeverity = ESeverity::High;
				}

				if (bIsInProblematicContext)
				{
					FLintIssue Issue;
					Issue.Type = ELintIssueType::CastAbuse;
					Issue.BlueprintPath = Blueprint->GetPathName();
					Issue.NodeName = CastNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
					Issue.Description = FString::Printf(TEXT("Cast node '%s' %s may cause performance issues %s"), 
						*Issue.NodeName, 
						bIsHardReferenceCast ? TEXT("(hard reference)") : TEXT(""),
						*ContextDescription);
					Issue.Severity = CastSeverity;
					Issue.NodeGuid = CastNode->NodeGuid;
					
					OutIssues.Add(Issue);
				}
			}
		}
	}
}

void FStaticLinter::DetectTickAbuse(UBlueprint* Blueprint, TArray<FLintIssue>& OutIssues)
{
	if (!Blueprint)
	{
		return;
	}

	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (!Graph)
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			// Look for Event Tick nodes
			if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				if (EventNode->GetFunctionName() == TEXT("ReceiveTick") || 
					EventNode->GetFunctionName() == TEXT("Tick"))
				{
					// Count connected nodes to estimate complexity
					int32 ConnectedNodeCount = 0;
					TSet<UEdGraphNode*> VisitedNodes;
					CountConnectedNodes(EventNode, VisitedNodes, ConnectedNodeCount);

					// Flag tick events with high complexity
					if (ConnectedNodeCount > 10) // Arbitrary threshold
					{
						FLintIssue Issue;
						Issue.Type = ELintIssueType::TickAbuse;
						Issue.BlueprintPath = Blueprint->GetPathName();
						Issue.NodeName = TEXT("Event Tick");
						Issue.Description = FString::Printf(TEXT("Tick event has high complexity (%d connected nodes)"), ConnectedNodeCount);
						Issue.Severity = CalculateIssueSeverity(ELintIssueType::TickAbuse, ConnectedNodeCount);
						Issue.NodeGuid = EventNode->NodeGuid;
						
						OutIssues.Add(Issue);
					}
				}
			}
		}
	}
}

void FStaticLinter::DetectUnusedFunctions(UBlueprint* Blueprint, TArray<FLintIssue>& OutIssues)
{
	if (!Blueprint)
	{
		return;
	}

	// 0. 首先跳过接口 Blueprint（BPI_ 开头）
	//    接口中的函数不需要被调用，它们是被其他 Blueprint 实现的
	FString BlueprintName = Blueprint->GetName();
	if (BlueprintName.StartsWith(TEXT("BPI_")))
	{
		// 这是接口 Blueprint，不检测未引用函数
		return;
	}

	// 1. 跳过 GameInstance 蓝图
	if (Blueprint->GeneratedClass && Blueprint->GeneratedClass->IsChildOf(UGameInstance::StaticClass()))
	{
		return;
	}
	else if (Blueprint->ParentClass && Blueprint->ParentClass->IsChildOf(UGameInstance::StaticClass()))
	{
		return;
	}

	// 获取项目中所有 Blueprint 来检测函数调用
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	if (!AssetRegistry.IsLoadingAssets())
	{
		AssetRegistry.SearchAllAssets(true);
	}

	TArray<FAssetData> AllBlueprintAssets;
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	Filter.PackagePaths.Add(FName(TEXT("/Game")));
	AssetRegistry.GetAssets(Filter, AllBlueprintAssets);

	

	for (const FAssetData& AssetData : AllBlueprintAssets)
	{
		UBlueprint* BP = Cast<UBlueprint>(AssetData.GetAsset());
		if (!BP)
		{
			continue;
		}

		TArray<UEdGraph*> AllGraphs = GetAllGraphs(BP);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph)
			{
				continue;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_CallFunction* CallFuncNode = Cast<UK2Node_CallFunction>(Node))
			{
				FName FunctionName = CallFuncNode->FunctionReference.GetMemberName();
				if (FunctionName != NAME_None)
				{
					ReferencedFunctions.Add(FunctionName);
					FunctionCallCount.FindOrAdd(FunctionName)++;
				}
				
				// 也记录函数引用的完整路径（包含类名），用于跨蓝图引用检测
				FString FullFunctionPath = CallFuncNode->FunctionReference.GetMemberParentClass() ? 
					CallFuncNode->FunctionReference.GetMemberParentClass()->GetName() + TEXT(".") + FunctionName.ToString() :
					FunctionName.ToString();
				if (!FullFunctionPath.IsEmpty())
				{
					ReferencedFunctions.Add(FName(*FullFunctionPath));
				}

				// 检查 SetTimer 节点 - 通过函数名字符串引用函数
				// 检查是否是 SetTimer 相关函数（包括设置、清除、暂停、获取等所有定时器操作）
				if (FunctionName.ToString().StartsWith(TEXT("K2_SetTimer")) ||
					FunctionName.ToString().StartsWith(TEXT("K2_ClearTimer")) ||
					FunctionName.ToString().StartsWith(TEXT("K2_PauseTimer")) ||
					FunctionName.ToString().StartsWith(TEXT("K2_UnPauseTimer")) ||
					FunctionName.ToString().StartsWith(TEXT("K2_IsTimer")) ||
					FunctionName.ToString().StartsWith(TEXT("K2_GetTimer")) ||
					FunctionName.ToString().StartsWith(TEXT("K2_DoesTimer")))
				{
					UE_LOG(LogTemp, Log, TEXT("找到 Timer 节点: %s"), *FunctionName.ToString());
					// 查找 FunctionName 引脚
					for (UEdGraphPin* Pin : Node->Pins)
					{
						FString PinNameStr = Pin->PinName.ToString();
						UE_LOG(LogTemp, Log, TEXT("  引脚: %s, 类型: %s"), *PinNameStr, *Pin->PinType.PinCategory.ToString());
						if (Pin && PinNameStr == TEXT("FunctionName") && Pin->LinkedTo.Num() == 0)
						{
							// 获取函数名字符串值（使用 DefaultValue）
							FString TimerFunctionName = Pin->DefaultValue;
							UE_LOG(LogTemp, Log, TEXT("  找到 FunctionName 引脚, 值: '%s'"), *TimerFunctionName);
							if (!TimerFunctionName.IsEmpty())
							{
								ReferencedFunctions.Add(FName(*TimerFunctionName));
								UE_LOG(LogTemp, Log, TEXT("检测到 Timer 引用函数: %s (节点: %s)"), *TimerFunctionName, *FunctionName.ToString());
							}
						}
					}
				}
			}
			// 也检查自定义事件调用
			else if (UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
			{
				// 自定义事件可能被其他节点通过 GUID 调用
			}
			// 检查事件分发器绑定 - 这是跨蓝图引用的重要来源
			else if (UK2Node_BaseMCDelegate* DelegateNode = Cast<UK2Node_BaseMCDelegate>(Node))
			{
				FName DelegateName = DelegateNode->GetPropertyName();
				if (DelegateName != NAME_None)
				{
					ReferencedFunctions.Add(DelegateName);
					
					// 对于 AddDelegate 和 AssignDelegate，还要记录被绑定的自定义事件
					if (Cast<UK2Node_AddDelegate>(Node) || Cast<UK2Node_AssignDelegate>(Node))
					{
						// 使用 GetDelegatePin() 获取委托输入引脚
						UEdGraphPin* DelegatePin = DelegateNode->GetDelegatePin();
						if (DelegatePin && DelegatePin->LinkedTo.Num() > 0)
						{
							for (UEdGraphPin* LinkedPin : DelegatePin->LinkedTo)
							{
								if (LinkedPin && LinkedPin->GetOwningNode())
								{
									UEdGraphNode* ConnectedNode = LinkedPin->GetOwningNode();
									if (UK2Node_CustomEvent* BoundEvent = Cast<UK2Node_CustomEvent>(ConnectedNode))
									{
										FName EventName = BoundEvent->GetFunctionName();
										if (EventName != NAME_None)
										{
											ReferencedFunctions.Add(EventName);
										}
									}
								}
							}
						}
					}
				}
			}
			}
		}
	}

	// 检查当前 Blueprint 的函数是否被引用
	for (UEdGraph* FunctionGraph : Blueprint->FunctionGraphs)
	{
		if (!FunctionGraph)
		{
			continue;
		}

		FName FunctionName = FunctionGraph->GetFName();
		FString FunctionNameStr = FunctionName.ToString();

		// ========== 跳过系统自带函数和接口函数的判断标准 ==========

		// 1. 以 "Receive" 开头（事件）
		if (FunctionNameStr.StartsWith(TEXT("Receive")))
		{
			continue;
		}

		// 2. 常见的引擎接口函数命名模式
		//    这些函数通常来自接口，不应该被报告为未引用
		TArray<FString> InterfaceFunctionPatterns = {
			TEXT("GetPlayerState"),
			TEXT("GetController"),
			TEXT("GetPawn"),
			TEXT("GetCharacter"),
			TEXT("GetOwner"),
			TEXT("GetGameInstance"),
			TEXT("GetWorld"),
			TEXT("GetLevel"),
			TEXT("GetParent"),
			TEXT("IsA"),
			TEXT("IsValid"),
			TEXT("K2_"),           // K2_ 开头的函数通常是引擎生成的
			TEXT("Execute"),       // Execute 相关函数
			TEXT("Ubergraph"),     // Ubergraph 相关函数
			TEXT("UserConstructionScript"),
			TEXT("ConstructionScript"),
			// 常见接口前缀
			TEXT("HasAuthority"),   // INetworkInterface
			TEXT("GetNetConnection"),
			TEXT("GetNetMode"),
			TEXT("IsNetMode"),
		};

		bool bIsEnginePattern = false;
		for (const FString& Pattern : InterfaceFunctionPatterns)
		{
			if (FunctionNameStr.Contains(Pattern))
			{
				bIsEnginePattern = true;
				break;
			}
		}
		if (bIsEnginePattern)
		{
			continue;
		}

		// 3. 检查是否是 Override 函数（覆盖父类虚函数）
		bool bIsOverrideFunction = false;
		if (Blueprint->ParentClass)
		{
			UFunction* ParentFunction = Blueprint->ParentClass->FindFunctionByName(FunctionName);
			if (ParentFunction)
			{
				bIsOverrideFunction = true;
			}
		}

		if (bIsOverrideFunction)
		{
			continue;
		}

		// 4. 跳过来自引擎或第三方插件的 Blueprint
		FString BlueprintPath = Blueprint->GetPathName();
		if (BlueprintPath.StartsWith(TEXT("/Engine/")) ||
			BlueprintPath.StartsWith(TEXT("/Game/")) == false)
		{
			continue;
		}

		// 5. 跳过接口函数（通过检查继承链）
		{
			bool bIsInterfaceFunction = false;
			UClass* CurrentClass = Blueprint->GeneratedClass ? Blueprint->GeneratedClass : Blueprint->ParentClass;

			while (CurrentClass && !bIsInterfaceFunction)
			{
				// 检查当前类实现的所有接口
				for (const FImplementedInterface& Interface : CurrentClass->Interfaces)
				{
					if (Interface.Class && Interface.Class->FindFunctionByName(FunctionName))
					{
						bIsInterfaceFunction = true;
						break;
					}
				}

				if (bIsInterfaceFunction)
				{
					break;
				}

				// 向上检查父类
				CurrentClass = CurrentClass->GetSuperClass();
			}

			if (bIsInterfaceFunction)
			{
				continue;
			}
		}

		// 6. 使用虚幻引擎原生的引用检测机制检查函数是否被引用
		// FBlueprintEditorUtils::IsFunctionUsed 会自动搜索当前蓝图和所有引用该蓝图的其他蓝图
		bool bIsReferenced = FBlueprintEditorUtils::IsFunctionUsed(Blueprint, FunctionName);

		// 7. 检查我们自己收集的引用列表（包括 SetTimer 等通过函数名字符串的引用）
		if (!bIsReferenced)
		{
			bIsReferenced = ReferencedFunctions.Contains(FunctionName);
			if (bIsReferenced)
			{
				UE_LOG(LogTemp, Log, TEXT("函数 '%s' 通过自定义引用检测找到"), *FunctionNameStr);
			}
		}

		// 调试：输出未引用的函数名
		if (!bIsReferenced)
		{
			UE_LOG(LogTemp, Warning, TEXT("未引用函数: %s (在 Blueprint: %s)"), *FunctionNameStr, *Blueprint->GetName());
			UE_LOG(LogTemp, Warning, TEXT("  ReferencedFunctions 包含此函数: %d"), ReferencedFunctions.Contains(FunctionName));
		}

		if (bIsReferenced)
		{
			continue;  // 函数被引用，跳过
		}

		// 函数未被引用，报告问题
		FLintIssue Issue;
		Issue.Type = ELintIssueType::UnusedFunction;
		Issue.BlueprintPath = Blueprint->GetPathName();
		Issue.NodeName = FunctionNameStr;
		if (FBlueprintProfilerLocalization::IsChinese())
		{
			Issue.Description = FString::Printf(TEXT("函数 '%s' 已定义但从未被调用"), *FunctionNameStr);
		}
		else
		{
			Issue.Description = FString::Printf(TEXT("Function '%s' is defined but never called"), *FunctionNameStr);
		}
		Issue.Severity = ESeverity::Medium; // 未引用的函数是中等严重度
		Issue.NodeGuid = FGuid(); // 函数图没有 NodeGuid，留空

		OutIssues.Add(Issue);
	}

	// ========== 检查未引用的宏 ==========
	// 收集所有被引用的宏
	TSet<FName> ReferencedMacros;
	for (const FAssetData& AssetData : AllBlueprintAssets)
	{
		UBlueprint* BP = Cast<UBlueprint>(AssetData.GetAsset());
		if (!BP)
		{
			continue;
		}

		TArray<UEdGraph*> AllGraphs = GetAllGraphs(BP);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph)
			{
				continue;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_MacroInstance* MacroInstance = Cast<UK2Node_MacroInstance>(Node))
				{
					// 获取被引用的宏图
					UEdGraph* ReferencedMacroGraph = MacroInstance->GetMacroGraph();
					if (ReferencedMacroGraph)
					{
						ReferencedMacros.Add(ReferencedMacroGraph->GetFName());
					}
				}
			}
		}
	}

	// 检查当前 Blueprint 的宏是否被引用
	for (UEdGraph* MacroGraph : Blueprint->MacroGraphs)
	{
		if (!MacroGraph)
		{
			continue;
		}

		FName MacroName = MacroGraph->GetFName();
		FString MacroNameStr = MacroName.ToString();

		// 跳过引擎自带的宏（通常以特定前缀开头）
		if (MacroNameStr.StartsWith(TEXT("K2_")) ||
			MacroNameStr.StartsWith(TEXT("Default__")))
		{
			continue;
		}

		// 检查宏是否被引用
		bool bIsMacroReferenced = ReferencedMacros.Contains(MacroName);

		// 也检查是否在当前 Blueprint 中被引用
		if (!bIsMacroReferenced)
		{
			TArray<UEdGraph*> AllGraphs = GetAllGraphs(Blueprint);
			for (UEdGraph* Graph : AllGraphs)
			{
				if (!Graph)
				{
					continue;
				}

				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (UK2Node_MacroInstance* MacroInstance = Cast<UK2Node_MacroInstance>(Node))
					{
						if (MacroInstance->GetMacroGraph() == MacroGraph)
						{
							bIsMacroReferenced = true;
							break;
						}
					}
				}
				if (bIsMacroReferenced)
				{
					break;
				}
			}
		}

		if (bIsMacroReferenced)
		{
			continue;  // 宏被引用，跳过
		}

		// 宏未被引用，报告问题
		FLintIssue Issue;
		Issue.Type = ELintIssueType::UnusedFunction;
		Issue.BlueprintPath = Blueprint->GetPathName();
		Issue.NodeName = MacroNameStr;
		if (FBlueprintProfilerLocalization::IsChinese())
		{
			Issue.Description = FString::Printf(TEXT("宏 '%s' 已定义但从未被使用"), *MacroNameStr);
		}
		else
		{
			Issue.Description = FString::Printf(TEXT("Macro '%s' is defined but never used"), *MacroNameStr);
		}
		Issue.Severity = ESeverity::Low; // 未引用的宏是低严重度
		Issue.NodeGuid = FGuid(); // 宏图没有 NodeGuid，留空

		OutIssues.Add(Issue);
	}
}

// Context analysis helpers
bool FStaticLinter::IsNodeInTickContext(UEdGraphNode* Node, TSet<UEdGraphNode*>& VisitedNodes) const
{
	if (!Node || VisitedNodes.Contains(Node))
	{
		return false;
	}

	VisitedNodes.Add(Node);

	// Check if this is a tick event node
	if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
	{
		FName EventName = EventNode->GetFunctionName();
		if (EventName == TEXT("ReceiveTick") || EventName == TEXT("Tick"))
		{
			return true;
		}
	}

	// Trace backwards through execution pins to find the source
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (LinkedPin && LinkedPin->GetOwningNode())
				{
					if (IsNodeInTickContext(LinkedPin->GetOwningNode(), VisitedNodes))
					{
						return true;
					}
				}
			}
		}
	}

	return false;
}

bool FStaticLinter::IsNodeInLoopContext(UEdGraphNode* Node, TSet<UEdGraphNode*>& VisitedNodes) const
{
	if (!Node || VisitedNodes.Contains(Node))
	{
		return false;
	}

	VisitedNodes.Add(Node);

	// Check if this is a loop node
	FString NodeClass = Node->GetClass()->GetName();
	if (NodeClass.Contains(TEXT("ForLoop")) || 
		NodeClass.Contains(TEXT("WhileLoop")) || 
		NodeClass.Contains(TEXT("ForEach")))
	{
		return true;
	}

	// Trace backwards through execution pins
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (LinkedPin && LinkedPin->GetOwningNode())
				{
					if (IsNodeInLoopContext(LinkedPin->GetOwningNode(), VisitedNodes))
					{
						return true;
					}
				}
			}
		}
	}

	return false;
}

bool FStaticLinter::IsNodeInFrequentlyCalledFunction(UEdGraphNode* Node, UEdGraph* Graph) const
{
	if (!Graph)
	{
		return false;
	}

	// Check if the graph is a function graph with certain naming patterns
	FString GraphName = Graph->GetName();
	
	// Common patterns for frequently called functions
	TArray<FString> FrequentPatterns = {
		TEXT("Update"),
		TEXT("Process"),
		TEXT("Calculate"),
		TEXT("Check"),
		TEXT("Validate"),
		TEXT("GetCurrent"),
		TEXT("IsValid")
	};

	for (const FString& Pattern : FrequentPatterns)
	{
		if (GraphName.Contains(Pattern))
		{
			return true;
		}
	}

	return false;
}

bool FStaticLinter::IsHardReferenceCast(UK2Node_DynamicCast* CastNode) const
{
	if (!CastNode)
	{
		return false;
	}

	// Check if the target class is a hard reference (not an interface)
	UClass* TargetClass = CastNode->TargetType;
	if (TargetClass && !TargetClass->HasAnyClassFlags(CLASS_Interface))
	{
		// Additional checks for expensive casts
		FString ClassName = TargetClass->GetName();
		
		// Actor casts are generally more expensive
		if (TargetClass->IsChildOf(AActor::StaticClass()))
		{
			return true;
		}
		
		// Component casts can also be expensive
		if (TargetClass->IsChildOf(UActorComponent::StaticClass()))
		{
			return true;
		}
	}

	return false;
}
