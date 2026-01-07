@echo off
REM Windows 测试脚本

set DEVICE_DIR=/data/local/tmp/soloader_test
set BUILD_DIR=..\build

echo === SoLoader 测试脚本 ===

REM 检查设备连接
adb devices | findstr "device" > nul
if errorlevel 1 (
    echo ERROR: No Android device connected
    exit /b 1
)

echo Device connected

REM 创建设备目录
echo Creating device directory...
adb shell "mkdir -p %DEVICE_DIR%"

REM 推送文件
echo Pushing files to device...
adb push "%BUILD_DIR%\soloader_test" "%DEVICE_DIR%/"
adb push "%BUILD_DIR%\test\libtest_lib.so" "%DEVICE_DIR%/"

REM 设置权限
echo Setting permissions...
adb shell "chmod +x %DEVICE_DIR%/soloader_test"

REM 运行测试
echo.
echo === Running tests ===
echo.
adb shell "%DEVICE_DIR%/soloader_test %DEVICE_DIR%/libtest_lib.so"

echo.
echo === Test completed ===
pause
