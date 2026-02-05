@echo off
chcp 65001 >nul
setlocal EnableDelayedExpansion

rem ========================================
rem AI Blueprint Node Copy-Paste Plugin 安装脚本
rem 功能：清理项目目录并安装插件
rem 使用方法：将 .uproject 文件拖拽到本脚本上
rem ========================================

set "PLUGIN_NAME=BlueprintProfiler"
set "SCRIPT_DIR=%~dp0"
set "PLUGIN_PATH=%SCRIPT_DIR%%PLUGIN_NAME%"

rem 检查是否拖拽了文件
if "%~1"=="" (
    echo ========================================
    echo 错误：未找到项目文件
    echo.
    echo 请将项目的 .uproject 文件拖拽到本脚本上
    echo ========================================
    pause
    exit /b 1
)

set "PROJECT_FILE=%~1"
set "PROJECT_DIR=%~dp1"
set "PROJECT_NAME=%~n1"

rem 检查是否是 .uproject 文件
if /i not "%~x1"==".uproject" (
    echo ========================================
    echo 错误：文件格式不正确
    echo.
    echo 请选择 .uproject 文件，当前文件：%~nx1
    echo ========================================
    pause
    exit /b 1
)

rem 检查插件是否存在
if not exist "%PLUGIN_PATH%" (
    echo ========================================
    echo 错误：插件目录不存在
    echo.
    echo 插件路径：%PLUGIN_PATH%
    echo ========================================
    pause
    exit /b 1
)

echo ========================================
echo AI 插件快速安装工具
echo ========================================
echo.
echo 项目名称：%PROJECT_NAME%
echo 项目路径：%PROJECT_DIR%
echo 插件路径：%PLUGIN_PATH%
echo.
echo 即将执行以下操作：
echo   1. 删除项目中的 %PLUGIN_NAME% 插件
echo   2. 删除项目中的 Saved 文件夹
echo   3. 删除项目中的 Intermediate 文件夹
echo   4. 删除项目中的 Binaries 文件夹
echo   5. 复制插件到项目的 Plugins 文件夹
echo.
echo ========================================

echo.
echo 开始清理...
echo ----------------------------------------

rem 删除特定插件文件夹
if exist "%PROJECT_DIR%Plugins\%PLUGIN_NAME%" (
    echo [1/4] 正在删除 %PLUGIN_NAME% 插件...
    rmdir /s /q "%PROJECT_DIR%Plugins\%PLUGIN_NAME%" 2>nul
    if exist "%PROJECT_DIR%Plugins\%PLUGIN_NAME%" (
        echo       警告：插件删除失败，尝试强制删除...
        rd /s /q "%PROJECT_DIR%Plugins\%PLUGIN_NAME%" 2>nul
    )
) else (
    echo [1/4] %PLUGIN_NAME% 插件不存在，跳过
)

rem 删除 Saved 文件夹
if exist "%PROJECT_DIR%Saved" (
    echo [2/4] 正在删除 Saved 文件夹...
    rmdir /s /q "%PROJECT_DIR%Saved" 2>nul
) else (
    echo [2/4] Saved 文件夹不存在，跳过
)

rem 删除 Intermediate 文件夹
if exist "%PROJECT_DIR%Intermediate" (
    echo [3/4] 正在删除 Intermediate 文件夹...
    rmdir /s /q "%PROJECT_DIR%Intermediate" 2>nul
) else (
    echo [3/4] Intermediate 文件夹不存在，跳过
)

rem 删除 Binaries 文件夹
if exist "%PROJECT_DIR%Binaries" (
    echo [4/4] 正在删除 Binaries 文件夹...
    rmdir /s /q "%PROJECT_DIR%Binaries" 2>nul
) else (
    echo [4/4] Binaries 文件夹不存在，跳过
)

echo.
echo ----------------------------------------
echo 清理完成！
echo.
echo 开始安装插件...
echo ----------------------------------------

rem 创建 Plugins 文件夹
if not exist "%PROJECT_DIR%Plugins" (
    echo 正在创建 Plugins 文件夹...
    mkdir "%PROJECT_DIR%Plugins"
)

rem 复制插件
echo 正在复制插件到项目...
xcopy "%PLUGIN_PATH%" "%PROJECT_DIR%Plugins\%PLUGIN_NAME%\" /E /I /Y /Q

if errorlevel 1 (
    echo.
    echo ========================================
    echo 错误：插件复制失败
    echo ========================================
    pause
    exit /b 1
)

echo.
echo ========================================
echo 安装完成！
echo ========================================
echo.
echo 项目路径：%PROJECT_DIR%
echo 插件位置：%PROJECT_DIR%Plugins\%PLUGIN_NAME%
echo.
echo 请在虚幻编辑器中启用插件：
echo   编辑 ↑- 插件 ↑- 项目 ↑- AI Blueprint Node Copy-Paste
echo.
echo ========================================

pause
