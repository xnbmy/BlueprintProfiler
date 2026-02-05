# 需求文档

## 介绍

虚幻引擎蓝图性能分析插件是一个虚幻5.6版本编辑器插件，旨在帮助开发者诊断和优化蓝图性能问题。该插件通过运行时监控、静态代码分析和内存引用分析，为开发者提供全面的蓝图性能洞察。

## 术语表

- **Runtime_Profiler**: 运行时分析器，在PIE模式下监控蓝图节点执行性能
- **Static_Linter**: 静态扫描器，分析蓝图资产中的代码质量问题
- **Memory_Analyzer**: 内存分析器，分析蓝图的内存占用和引用关系
- **Dashboard_UI**: 仪表盘界面，统一展示所有分析结果的用户界面
- **PIE**: Play In Editor，虚幻引擎的编辑器内游戏运行模式
- **Hot_Node**: 热点节点，执行频率超过阈值的蓝图节点
- **Blueprint_Asset**: 蓝图资产，虚幻引擎中的可视化脚本文件

## 需求

### 需求 1: 运行时性能监控

**用户故事:** 作为蓝图开发者，我希望能够实时监控蓝图节点的执行性能，以便识别和解决逻辑卡顿问题。

#### 验收标准

1. WHEN 用户在PIE模式下启动录制 THEN Runtime_Profiler SHALL 开始收集所有蓝图节点的执行数据
2. WHEN 蓝图节点被执行 THEN Runtime_Profiler SHALL 记录该节点的执行次数和耗时
3. WHEN 节点每秒执行次数超过1000次 THEN Runtime_Profiler SHALL 将其标记为Hot_Node
4. WHEN 用户停止录制 THEN Runtime_Profiler SHALL 停止数据收集并保存分析结果
5. WHEN 检测到Event Tick节点且逻辑复杂 THEN Runtime_Profiler SHALL 将其标记为Tick滥用

### 需求 2: 静态代码质量分析

**用户故事:** 作为项目维护者，我希望能够扫描项目中的蓝图资产，以便发现和清理代码质量问题。

#### 验收标准

1. WHEN 用户启动静态扫描 THEN Static_Linter SHALL 遍历指定范围内的所有Blueprint_Asset
2. WHEN 发现未被引用的变量或函数 THEN Static_Linter SHALL 将其标记为无效节点
3. WHEN 发现未连接执行引脚的纯计算节点 THEN Static_Linter SHALL 将其标记为孤岛节点
4. WHEN 在循环或Tick中发现硬引用Cast操作 THEN Static_Linter SHALL 将其标记为Cast滥用
5. WHEN 扫描完成 THEN Static_Linter SHALL 生成包含所有问题的详细报告

### 需求 3: 内存和引用分析

**用户故事:** 作为性能优化工程师，我希望能够分析蓝图的内存占用和引用关系，以便解决加载卡顿和内存爆炸问题。

#### 验收标准

1. WHEN 用户选择蓝图进行内存分析 THEN Memory_Analyzer SHALL 计算该蓝图的包含大小
2. WHEN 分析蓝图引用 THEN Memory_Analyzer SHALL 追踪所有硬引用链并计算引用深度
3. WHEN 发现引用超过2048x2048贴图的变量 THEN Memory_Analyzer SHALL 将其标记为大资源警报
4. WHEN 分析完成 THEN Memory_Analyzer SHALL 提供引用关系的可视化展示
5. WHEN 发现异常大的资源引用 THEN Memory_Analyzer SHALL 指出具体的引用变量

### 需求 4: 统一数据展示界面

**用户故事:** 作为蓝图开发者，我希望有一个统一的界面来查看所有分析结果，以便高效地定位和解决问题。

#### 验收标准

1. WHEN 用户打开Dashboard_UI THEN 系统 SHALL 显示所有分析模块的结果汇总
2. WHEN 用户查看分析结果 THEN Dashboard_UI SHALL 按类别展示节点名、所属蓝图、计数和严重程度
3. WHEN 用户对结果进行排序 THEN Dashboard_UI SHALL 支持按消耗、名称、类型等维度排序
4. WHEN 用户使用搜索功能 THEN Dashboard_UI SHALL 实时过滤显示匹配的结果
5. WHEN 用户双击某个结果项 THEN Dashboard_UI SHALL 自动打开对应蓝图并聚焦到具体节点

### 需求 5: 数据导出和报告

**用户故事:** 作为项目经理，我希望能够导出分析报告，以便与团队分享性能分析结果。

#### 验收标准

1. WHEN 用户选择导出功能 THEN 系统 SHALL 支持CSV和JSON两种格式
2. WHEN 导出CSV格式 THEN 系统 SHALL 包含所有分析数据的结构化表格
3. WHEN 导出JSON格式 THEN 系统 SHALL 包含完整的分析结果和元数据
4. WHEN 导出完成 THEN 系统 SHALL 提供文件保存位置的确认信息
5. WHEN 导出的文件被打开 THEN 数据 SHALL 保持完整性和可读性

### 需求 6: 录制控制和状态管理

**用户故事:** 作为蓝图调试者，我希望能够精确控制性能数据的录制过程，以便针对特定场景进行分析。

#### 验收标准

1. WHEN 用户点击开始录制按钮 THEN Runtime_Profiler SHALL 开始收集性能数据
2. WHEN 用户点击停止录制按钮 THEN Runtime_Profiler SHALL 停止数据收集并保存当前结果
3. WHEN 用户点击重置数据按钮 THEN Runtime_Profiler SHALL 清空所有已收集的数据
4. WHEN PIE模式结束 THEN Runtime_Profiler SHALL 自动停止录制
5. WHEN PIE模式开始 THEN Runtime_Profiler SHALL 根据用户设置决定是否自动开始录制

### 需求 7: 批量处理和进度反馈

**用户故事:** 作为大型项目的开发者，我希望能够批量处理多个蓝图文件，并实时了解处理进度。

#### 验收标准

1. WHEN 用户选择批量扫描 THEN Static_Linter SHALL 支持全项目扫描或选定文件夹扫描
2. WHEN 批量处理进行中 THEN 系统 SHALL 显示当前处理进度和剩余时间估算
3. WHEN 处理大量文件 THEN 系统 SHALL 使用多线程技术加速扫描过程
4. WHEN 用户取消批量操作 THEN 系统 SHALL 安全停止处理并保存已完成的结果
5. WHEN 批量处理完成 THEN 系统 SHALL 提供处理摘要和结果统计

### 需求 8: 节点跳转和编辑器集成

**用户故事:** 作为蓝图编辑者，我希望能够从分析结果直接跳转到问题节点，以便快速进行修复。

#### 验收标准

1. WHEN 用户双击分析结果中的节点 THEN 系统 SHALL 自动打开对应的蓝图编辑器
2. WHEN 蓝图编辑器打开 THEN 系统 SHALL 自动聚焦到问题节点的位置
3. WHEN 目标蓝图已经在编辑器中打开 THEN 系统 SHALL 直接跳转到节点位置
4. WHEN 目标节点不存在 THEN 系统 SHALL 显示友好的错误提示信息
5. WHEN 跳转完成 THEN 系统 SHALL 高亮显示目标节点以便用户识别