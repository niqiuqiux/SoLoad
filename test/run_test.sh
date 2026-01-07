#!/bin/bash
# Android 设备测试脚本

# 配置
DEVICE_DIR="/data/local/tmp/soloader_test"
BUILD_DIR="../build"

echo "=== SoLoader 测试脚本 ==="

# 检查 adb
if ! command -v adb &> /dev/null; then
    echo "ERROR: adb not found in PATH"
    exit 1
fi

# 检查设备连接
if ! adb devices | grep -q "device$"; then
    echo "ERROR: No Android device connected"
    exit 1
fi

echo "Device connected"

# 创建设备目录
echo "Creating device directory..."
adb shell "mkdir -p $DEVICE_DIR"

# 推送文件
echo "Pushing files to device..."
adb push "$BUILD_DIR/soloader_test" "$DEVICE_DIR/"
adb push "$BUILD_DIR/test/libtest_lib.so" "$DEVICE_DIR/"

# 设置权限
echo "Setting permissions..."
adb shell "chmod +x $DEVICE_DIR/soloader_test"

# 运行测试
echo ""
echo "=== Running tests ==="
echo ""
adb shell "$DEVICE_DIR/soloader_test $DEVICE_DIR/libtest_lib.so"

echo ""
echo "=== Test completed ==="
