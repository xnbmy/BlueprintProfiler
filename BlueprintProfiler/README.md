# Blueprint Profiler

A comprehensive performance analysis and code inspection tool for Unreal Engine Blueprints.

---

## Table of Contents

1. [Features](#features)
2. [Installation](#installation)
3. [Getting Started](#getting-started)
4. [Feature Documentation](#feature-documentation)
   - [Runtime Profiler](#runtime-profiler)
   - [Static Analysis (Linter)](#static-analysis-linter)
   - [Memory Analyzer](#memory-analyzer)
5. [User Interface](#user-interface)
6. [FAQ](#faq)
7. [Support](#support)
8. [Changelog](#changelog)

---

## Features

### 1. Runtime Profiler
- Real-time Blueprint node execution tracking
- Performance hotspot identification
- Execution frequency analysis
- Automatic PIE (Play In Editor) integration
- Session recording and replay

### 2. Static Analysis (Linter)
- Dead code detection
- Orphan node identification
- Unused variable and function detection
- Cast abuse detection
- Macro reference detection
- Timer function reference detection

### 3. Memory Analyzer
- Asset reference analysis
- Asset usage tracking
- Reference count visualization
- Large asset detection
- Reference chain analysis

### 4. Data Export
- CSV export support
- JSON export support
- Session save/load functionality

---

## Installation

### Prerequisites
- Unreal Engine 5.5 or later
- Windows 64-bit, Linux

### Method 1: Project Plugin (Recommended)

1. Copy the `BlueprintProfiler` folder to your project's `Plugins` directory:
   ```
   YourProject/
   ├── Content/
   ├── Source/
   ├── Plugins/
   │   └── BlueprintProfiler/     <-- Copy here
   └── YourProject.uproject
   ```

2. Right-click on your `.uproject` file and select **"Generate Visual Studio project files"**

3. Open your project in Unreal Engine

4. Enable the plugin:
   - Go to **Edit > Plugins**
   - Search for "Blueprint Profiler"
   - Check the **Enabled** checkbox
   - Restart the editor when prompted

### Method 2: Engine Plugin

1. Copy the `BlueprintProfiler` folder to the engine's plugins directory:
   ```
   UE_5.x/Engine/Plugins/Marketplace/
   ```

2. Restart Unreal Engine

---

## Getting Started

### Opening the Blueprint Profiler Window

1. In the Unreal Editor, go to **Window > Blueprint Profiler**
2. The Blueprint Profiler window will open as a dockable tab

### Basic Workflow

1. **Runtime Analysis**: Click "Start Recording" before running PIE to capture performance data
2. **Static Analysis**: Click "Start Scan" to analyze your blueprints for code issues
3. **Memory Analysis**: Click "Analyze Memory" to check asset references

---

## Feature Documentation

### Runtime Profiler

The Runtime Profiler tracks Blueprint node execution in real-time to identify performance bottlenecks.

#### How to Use

1. **Start Recording**:
   - Click the "Start Recording" button in the Runtime Profiler tab
   - Or enable "Auto-start with PIE" in settings

2. **Run Your Game**:
   - Click "Play" in the editor (PIE mode)
   - The profiler will automatically track node execution

3. **Stop Recording**:
   - Click "Stop Recording" or stop PIE
   - Data will be automatically saved to a session

4. **Analyze Results**:
   - View the "Hot Nodes" list to find performance bottlenecks
   - Sort by execution count or total execution time
   - Click on a node to highlight it in the blueprint

#### Understanding the Data

- **Execution Count**: How many times the node was executed
- **Total Execution Time**: Cumulative time spent in this node
- **Average Time**: Average execution time per call
- **Severity**: Color-coded indicator (Green/Yellow/Red) based on performance impact

#### Tips
- Focus on nodes with high "Executions Per Second" for optimization
- Check nodes marked in Red (High severity) first
- Use the filter to search for specific blueprints or nodes

---

### Static Analysis (Linter)

The Static Analysis feature scans your blueprints for code quality issues and potential bugs.

#### How to Use

1. **Configure Scan Settings**:
   - Select scan scope: Current Blueprint, Current Level, or Full Project
   - Choose which detectors to enable

2. **Start Scan**:
   - Click "Start Scan" button
   - Wait for the scan to complete (progress shown in status bar)

3. **Review Issues**:
   - Browse the issues list categorized by severity
   - Click on an issue to navigate to the affected node
   - Use filters to focus on specific issue types

#### Issue Types

| Issue Type | Description | Severity |
|------------|-------------|----------|
| Dead Node | Node not connected to execution flow | High |
| Orphan Node | Node with no input connections | Medium |
| Unused Variable | Variable defined but never used | Low |
| Unused Function | Function defined but never called | Medium |
| Unused Macro | Macro defined but never used | Low |
| Cast Abuse | Excessive use of Cast nodes | Medium |

#### Understanding Results

- **Severity Levels**:
  - 🔴 **High**: Critical issues that should be fixed immediately
  - 🟡 **Medium**: Issues that may cause problems
  - 🟢 **Low**: Minor issues or suggestions

- **Categories**:
  - **Runtime**: Performance-related issues
  - **Static**: Code quality issues
  - **Memory**: Asset reference issues

#### Tips
- Run static analysis regularly during development
- Fix High severity issues before committing code
- Use the export feature to share reports with your team

---

### Memory Analyzer

The Memory Analyzer helps you understand asset references and identify memory optimization opportunities.

#### How to Use

1. **Start Analysis**:
   - Click "Analyze Memory" in the Memory Analyzer tab
   - Select the scope: Current Level or Full Project

2. **Review Asset References**:
   - Browse the list of assets and their reference counts
   - Identify assets with high reference counts
   - Check for unused or rarely used assets

3. **Analyze Reference Chains**:
   - Click on an asset to see its reference chain
   - Understand why an asset is being loaded
   - Find opportunities to break unnecessary references

#### Understanding the Data

- **Reference Count**: How many objects reference this asset
- **Asset Size**: Memory footprint of the asset
- **Inclusive Size**: Total memory including all referenced assets
- **Reference Depth**: How deep in the reference chain this asset is

#### Tips
- Look for assets with high inclusive size but low usage
- Check for circular references
- Use the filter to find specific asset types

---

## User Interface

### Main Window Layout

```
┌─────────────────────────────────────────────────────────────┐
│  [Toolbar: Start/Stop Recording | Scan | Settings | Export]  │
├─────────────────────────────────────────────────────────────┤
│  [Tab: Runtime Profiler] [Tab: Static Analysis] [Tab: Memory]│
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  [Main Content Area - varies by selected tab]              │
│                                                             │
├─────────────────────────────────────────────────────────────┤
│  [Status Bar: Session Info | Progress | Issue Count]        │
└─────────────────────────────────────────────────────────────┘
```

### Toolbar Buttons

- **Start Recording**: Begin runtime profiling session
- **Stop Recording**: End current profiling session
- **Start Scan**: Run static analysis
- **Settings**: Configure plugin options
- **Export**: Export data to CSV or JSON

### Runtime Profiler Tab

```
┌─────────────────────────────────────────────────────────────┐
│  [Filters] [Search Box]                    [Sort Options]   │
├─────────────────────────────────────────────────────────────┤
│  Hot Nodes List                                             │
│  ┌─────────────┬──────────┬─────────────┬──────────┐       │
│  │ Node Name   │ Blueprint│ Exec Count  │ Severity │       │
│  ├─────────────┼──────────┼─────────────┼──────────┤       │
│  │ ...         │ ...      │ ...         │ ...      │       │
│  └─────────────┴──────────┴─────────────┴──────────┘       │
├─────────────────────────────────────────────────────────────┤
│  [Session List]                    [Performance Graph]      │
└─────────────────────────────────────────────────────────────┘
```

### Static Analysis Tab

```
┌─────────────────────────────────────────────────────────────┐
│  [Scan Scope] [Severity Filter] [Type Filter] [Search]      │
├─────────────────────────────────────────────────────────────┤
│  Issues List                                                │
│  ┌──────────┬─────────────┬──────────────┬──────────┐      │
│  │ Severity │ Type        │ Description  │ Location │      │
│  ├──────────┼─────────────┼──────────────┼──────────┤      │
│  │ ...      │ ...         │ ...          │ ...      │      │
│  └──────────┴─────────────┴──────────────┴──────────┘      │
├─────────────────────────────────────────────────────────────┤
│  [Issue Details]              [Quick Fix Suggestions]       │
└─────────────────────────────────────────────────────────────┘
```

---

## FAQ

### General Questions

**Q: Does this plugin work with packaged builds?**
A: No, this is an editor-only plugin designed for development workflow optimization.

**Q: Will this plugin slow down my editor?**
A: The plugin has minimal performance impact when not actively recording. Runtime profiling may cause slight performance degradation during PIE, but it's designed to be as lightweight as possible.

**Q: Can I use this with Blueprint Nativization?**
A: Yes, the profiler works with both standard and nativized blueprints.

### Runtime Profiler

**Q: Why are some nodes not showing up in the profiler?**
A: Only nodes that execute during the recording session will appear. Make sure to trigger the relevant gameplay during recording.

**Q: How accurate is the execution time measurement?**
A: The timing is accurate to microseconds, but keep in mind that the profiling overhead adds a small amount of latency.

**Q: Can I export profiling data?**
A: Yes, use the Export button to save data as CSV or JSON for further analysis in external tools.

### Static Analysis

**Q: Why is my function marked as unused when I'm calling it via SetTimer?**
A: The static analyzer now detects SetTimer references. If it's still marked as unused, make sure you're using the latest version of the plugin.

**Q: What does "Orphan Node" mean?**
A: An orphan node is a node that has no input execution connections. These are often leftover nodes from refactoring.

**Q: Can I customize which detectors are enabled?**
A: Yes, go to Settings > Static Analysis to enable/disable specific detectors.

### Memory Analyzer

**Q: Why does my asset show a high reference count?**
A: High reference counts are normal for widely-used assets like materials or blueprints. Focus on assets with unexpectedly high counts.

**Q: How do I break an asset reference?**
A: The analyzer shows you the reference chain. Navigate to the referencing object and remove the reference.

**Q: What's the difference between Asset Size and Inclusive Size?**
A: Asset Size is the memory used by the asset itself. Inclusive Size includes all assets that are referenced by this asset.

### Troubleshooting

**Q: The plugin doesn't appear in the Window menu**
A: Make sure the plugin is enabled in Edit > Plugins, then restart the editor.

**Q: I get compilation errors when building**
A: Ensure you're using a compatible Unreal Engine version (5.5 or later) and have the required modules enabled.

**Q: The profiler window is blank**
A: Try resetting the window layout (Window > Reset Layout) or restarting the editor.

---

## Support

For support, updates, and tutorials:

- **Video Tutorials**: [Bilibili](https://www.bilibili.com/video/BV1R1cuziExS)
- **Author Channel**: [Bilibili Space](https://space.bilibili.com/13578876)
- **GitHub**: https://github.com/xiaonan/BlueprintProfiler

---

## Changelog

### v1.1
- Fixed macro nodes being incorrectly identified as orphan nodes
- Added macro reference detection
- Added function name reference detection (SetTimer)
- Fixed version popup window issue
- Various bug fixes

### v1.0
- Initial release
- Runtime profiling
- Static analysis
- Memory analysis
- Data export functionality

---

---

---

# 蓝图分析器

一个全面的虚幻引擎蓝图性能分析和代码检查工具。

---

## 目录

1. [功能特性](#功能特性)
2. [安装](#安装)
3. [快速开始](#快速开始)
4. [功能文档](#功能文档)
   - [运行时分析器](#运行时分析器)
   - [静态分析（Linter）](#静态分析linter)
   - [内存分析器](#内存分析器)
5. [用户界面](#用户界面)
6. [常见问题](#常见问题)
7. [支持](#支持)
8. [更新日志](#更新日志)

---

## 功能特性

### 1. 运行时分析器
- 实时蓝图节点执行跟踪
- 性能热点识别
- 执行频率分析
- 自动 PIE（编辑器中播放）集成
- 会话录制和回放

### 2. 静态分析（Linter）
- 死代码检测
- 孤立节点识别
- 未使用的变量和函数检测
- 转换滥用检测
- 宏引用检测
- 定时器函数引用检测

### 3. 内存分析器
- 资产引用分析
- 资产使用跟踪
- 引用计数可视化
- 大资产检测
- 引用链分析

### 4. 数据导出
- CSV 导出支持
- JSON 导出支持
- 会话保存/加载功能

---

## 安装

### 前置要求
- 虚幻引擎 5.5 或更高版本
- Windows 64位、Linux

### 方法一：项目插件（推荐）

1. 将 `BlueprintProfiler` 文件夹复制到你项目的 `Plugins` 目录：
   ```
   YourProject/
   ├── Content/
   ├── Source/
   ├── Plugins/
   │   └── BlueprintProfiler/     <-- 复制到这里
   └── YourProject.uproject
   ```

2. 右键点击你的 `.uproject` 文件，选择 **"Generate Visual Studio project files"**

3. 在虚幻引擎中打开你的项目

4. 启用插件：
   - 进入 **编辑 > 插件**
   - 搜索 "Blueprint Profiler"
   - 勾选 **Enabled** 复选框
   - 按提示重启编辑器

### 方法二：引擎插件

1. 将 `BlueprintProfiler` 文件夹复制到引擎的插件目录：
   ```
   UE_5.x/Engine/Plugins/Marketplace/
   ```

2. 重启虚幻引擎

---

## 快速开始

### 打开蓝图分析器窗口

1. 在虚幻编辑器中，进入 **窗口 > Blueprint Profiler**
2. Blueprint Profiler 窗口将作为可停靠标签页打开

### 基本工作流程

1. **运行时分析**：在运行 PIE 之前点击"开始录制"以捕获性能数据
2. **静态分析**：点击"开始扫描"分析蓝图中的代码问题
3. **内存分析**：点击"分析内存"检查资产引用

---

## 功能文档

### 运行时分析器

运行时分析器实时跟踪蓝图节点执行，以识别性能瓶颈。

#### 使用方法

1. **开始录制**：
   - 在运行时分析器标签页中点击"开始录制"按钮
   - 或在设置中启用"PIE时自动启动"

2. **运行游戏**：
   - 点击编辑器中的"播放"（PIE模式）
   - 分析器将自动跟踪节点执行

3. **停止录制**：
   - 点击"停止录制"或停止 PIE
   - 数据将自动保存到会话

4. **分析结果**：
   - 查看"热点节点"列表以找到性能瓶颈
   - 按执行次数或总执行时间排序
   - 点击节点可在蓝图中高亮显示

#### 理解数据

- **执行次数**：节点执行了多少次
- **总执行时间**：在此节点中花费的累计时间
- **平均时间**：每次调用的平均执行时间
- **严重度**：基于性能影响的颜色指示器（绿/黄/红）

#### 提示
- 优先优化"每秒执行次数"高的节点
- 首先检查标记为红色（高严重度）的节点
- 使用过滤器搜索特定蓝图或节点

---

### 静态分析（Linter）

静态分析功能扫描你的蓝图以查找代码质量问题和潜在错误。

#### 使用方法

1. **配置扫描设置**：
   - 选择扫描范围：当前蓝图、当前关卡或完整项目
   - 选择要启用的检测器

2. **开始扫描**：
   - 点击"开始扫描"按钮
   - 等待扫描完成（状态栏显示进度）

3. **查看问题**：
   - 按严重度浏览问题列表
   - 点击问题可导航到受影响的节点
   - 使用过滤器聚焦特定问题类型

#### 问题类型

| 问题类型 | 描述 | 严重度 |
|----------|------|--------|
| 死节点 | 未连接到执行流的节点 | 高 |
| 孤立节点 | 没有输入连接的节点 | 中 |
| 未使用变量 | 已定义但从未使用的变量 | 低 |
| 未使用函数 | 已定义但从未调用的函数 | 中 |
| 未使用宏 | 已定义但从未使用的宏 | 低 |
| 转换滥用 | 过度使用 Cast 节点 | 中 |

#### 理解结果

- **严重度级别**：
  - 🔴 **高**：应立即修复的关键问题
  - 🟡 **中**：可能导致问题的问题
  - 🟢 **低**：轻微问题或建议

- **类别**：
  - **运行时**：与性能相关的问题
  - **静态**：代码质量问题
  - **内存**：资产引用问题

#### 提示
- 在开发过程中定期运行静态分析
- 在提交代码之前修复高严重度问题
- 使用导出功能与团队共享报告

---

### 内存分析器

内存分析器帮助你理解资产引用并识别内存优化机会。

#### 使用方法

1. **开始分析**：
   - 在内存分析器标签页中点击"分析内存"
   - 选择范围：当前关卡或完整项目

2. **查看资产引用**：
   - 浏览资产列表及其引用计数
   - 识别引用计数高的资产
   - 检查未使用或很少使用的资产

3. **分析引用链**：
   - 点击资产查看其引用链
   - 理解为什么加载某个资产
   - 找到打破不必要引用的机会

#### 理解数据

- **引用计数**：有多少对象引用此资产
- **资产大小**：资产的内存占用
- **包含大小**：包括所有引用资产的总内存
- **引用深度**：此资产在引用链中的深度

#### 提示
- 查找包含大小高但使用率低的资产
- 检查循环引用
- 使用过滤器查找特定资产类型

---

## 用户界面

### 主窗口布局

```
┌─────────────────────────────────────────────────────────────┐
│  [工具栏: 开始/停止录制 | 扫描 | 设置 | 导出]                  │
├─────────────────────────────────────────────────────────────┤
│  [标签: 运行时分析器] [标签: 静态分析] [标签: 内存]            │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  [主内容区域 - 根据所选标签变化]                              │
│                                                             │
├─────────────────────────────────────────────────────────────┤
│  [状态栏: 会话信息 | 进度 | 问题计数]                         │
└─────────────────────────────────────────────────────────────┘
```

### 工具栏按钮

- **开始录制**：开始运行时分析会话
- **停止录制**：结束当前分析会话
- **开始扫描**：运行静态分析
- **设置**：配置插件选项
- **导出**：将数据导出为 CSV 或 JSON

### 运行时分析器标签

```
┌─────────────────────────────────────────────────────────────┐
│  [过滤器] [搜索框]                          [排序选项]        │
├─────────────────────────────────────────────────────────────┤
│  热点节点列表                                                │
│  ┌─────────────┬──────────┬─────────────┬──────────┐       │
│  │ 节点名称     │ 蓝图     │ 执行次数     │ 严重度   │       │
│  ├─────────────┼──────────┼─────────────┼──────────┤       │
│  │ ...         │ ...      │ ...         │ ...      │       │
│  └─────────────┴──────────┴─────────────┴──────────┘       │
├─────────────────────────────────────────────────────────────┤
│  [会话列表]                          [性能图表]              │
└─────────────────────────────────────────────────────────────┘
```

### 静态分析标签

```
┌─────────────────────────────────────────────────────────────┐
│  [扫描范围] [严重度过滤器] [类型过滤器] [搜索]                 │
├─────────────────────────────────────────────────────────────┤
│  问题列表                                                    │
│  ┌──────────┬─────────────┬──────────────┬──────────┐      │
│  │ 严重度   │ 类型        │ 描述         │ 位置     │      │
│  ├──────────┼─────────────┼──────────────┼──────────┤      │
│  │ ...      │ ...         │ ...          │ ...      │      │
│  └──────────┴─────────────┴──────────────┴──────────┘      │
├─────────────────────────────────────────────────────────────┤
│  [问题详情]                       [快速修复建议]              │
└─────────────────────────────────────────────────────────────┘
```

---

## 常见问题

### 一般问题

**问：这个插件可以在打包构建中使用吗？**
答：不可以，这是一个仅编辑器使用的插件，专为开发工作流程优化设计。

**问：这个插件会减慢我的编辑器速度吗？**
答：当不主动录制时，插件对性能影响很小。运行时分析在 PIE 期间可能会导致轻微的性能下降，但设计得尽可能轻量。

**问：我可以将其与蓝图原生化一起使用吗？**
答：可以，分析器适用于标准蓝图和原生化蓝图。

### 运行时分析器

**问：为什么有些节点没有出现在分析器中？**
答：只有在录制会话期间执行的节点才会出现。确保在录制期间触发相关的游戏玩法。

**问：执行时间测量有多准确？**
答：时间精度达到微秒，但请记住分析开销会增加少量延迟。

**问：我可以导出分析数据吗？**
答：可以，使用导出按钮将数据保存为 CSV 或 JSON，以便在外部工具中进一步分析。

### 静态分析

**问：为什么我通过 SetTimer 调用的函数被标记为未使用？**
答：静态分析器现在可以检测 SetTimer 引用。如果仍然被标记为未使用，请确保你使用的是最新版本的插件。

**问："孤立节点"是什么意思？**
答：孤立节点是没有输入执行连接的节点。这些通常是重构遗留的节点。

**问：我可以自定义启用哪些检测器吗？**
答：可以，进入 设置 > 静态分析 以启用/禁用特定检测器。

### 内存分析器

**问：为什么我的资产显示高引用计数？**
答：对于广泛使用的资产（如材质或蓝图），高引用计数是正常的。关注计数异常高的资产。

**问：如何打破资产引用？**
答：分析器显示引用链。导航到引用对象并移除引用。

**问：资产大小和包含大小有什么区别？**
答：资产大小是资产本身使用的内存。包含大小包括此资产引用的所有资产。

### 故障排除

**问：插件没有出现在窗口菜单中**
答：确保在 编辑 > 插件 中启用了插件，然后重启编辑器。

**问：构建时出现编译错误**
答：确保你使用的是兼容的虚幻引擎版本（5.5或更高版本）并且已启用所需的模块。

**问：分析器窗口是空白的**
答：尝试重置窗口布局（窗口 > 重置布局）或重启编辑器。

---

## 支持

如需支持、更新和教程：

- **视频教程**：[Bilibili](https://www.bilibili.com/video/BV1R1cuziExS)
- **作者频道**：[Bilibili空间](https://space.bilibili.com/13578876)
- **GitHub**：https://github.com/xiaonan/BlueprintProfiler

---

## 更新日志

### v1.1
- 修复宏节点被错误识别为孤立节点的问题
- 添加宏引用检测
- 添加函数名引用检测（SetTimer）
- 修复版本弹出窗口问题
- 各种错误修复

### v1.0
- 初始发布
- 运行时分析
- 静态分析
- 内存分析
- 数据导出功能
