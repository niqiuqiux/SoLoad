# NewSoLoader

基于 CSOLoader 的 C++17 现代化重写版本，一个独立于系统链接器的 Android ARM64 共享库加载器。

## 特性

### 核心功能
- **独立加载** - 不依赖 `dlopen`，完全自主实现 ELF 加载和链接
- **C++17 标准** - 使用现代 C++ 特性，代码简洁高效
- **仅 ARM64** - 专为 `arm64-v8a` 架构优化

### ELF 支持
- 完整的 ELF64 头验证（魔数、架构、字节序）
- GNU Hash 和 ELF Hash 符号查找
- RELA、REL、RELR 重定位处理
- Android 压缩重定位（APS2 格式）
- 弱符号（Weak Symbol）支持
- IFUNC 解析

### 运行时支持
- **TLS** - 线程本地存储，支持多线程
- **异常处理** - eh_frame 注册，支持 C++ 异常
- **回溯支持** - 自定义 `dl_iterate_phdr` / `dladdr` 实现
- **构造/析构函数** - 正确调用 `.init`、`.init_array`、`.fini`、`.fini_array`

### 性能优化
- 符号查找缓存
- 延迟 TLS 块分配
- 高效的 SLEB128 解码

## 构建

### 环境要求
- Android NDK r25+ (推荐 r27c)
- CMake 3.18+
- 目标：Android API 26+, arm64-v8a

### CMake 构建

```bash
# 创建构建目录
mkdir build && cd build

# 配置（指定 NDK 路径）
cmake -DCMAKE_ANDROID_NDK=/path/to/android-ndk ..

# 或者设置环境变量
export ANDROID_NDK=/path/to/android-ndk
cmake ..

# 编译
cmake --build .
```

### 构建产物
- `libnewsoloader.a` - 静态库，用于集成到其他项目
- `soloader_test` - 独立测试程序
- `test/libtest_lib.so` - 测试用共享库

## 使用方法

### 基础用法

```cpp
#include "soloader.hpp"

int main(int argc, char** argv, char** envp) {
    // 设置全局参数（用于构造函数）
    soloader::g_argc = argc;
    soloader::g_argv = argv;
    soloader::g_envp = envp;
    
    // 创建加载器
    soloader::SoLoader loader;
    
    // 加载共享库
    if (!loader.load("/data/local/tmp/mylib.so")) {
        // 加载失败
        return 1;
    }
    
    // 获取函数指针
    auto my_func = loader.getSymbol<int(*)(int, int)>("my_function");
    if (my_func) {
        int result = my_func(10, 20);
    }
    
    // 获取变量地址
    auto my_var = loader.getSymbol<int*>("my_global_var");
    if (my_var) {
        *my_var = 100;
    }
    
    // 卸载（自动调用析构函数）
    loader.unload();
    
    return 0;
}
```

### API 参考

#### SoLoader 类

```cpp
class SoLoader {
public:
    // 加载共享库
    // 返回: 成功返回 true
    bool load(std::string_view lib_path);
    
    // 卸载库（调用 .fini_array 和 .fini）
    bool unload();
    
    // 放弃库（不调用析构函数，用于特殊场景）
    bool abandon();
    
    // 获取符号地址
    void* getSymbol(std::string_view name);
    
    // 获取类型化符号（模板版本）
    template<typename T>
    T getSymbol(std::string_view name);
    
    // 检查是否已加载
    bool isLoaded() const;
    
    // 获取库路径
    const std::string& path() const;
};
```

#### 全局变量

```cpp
namespace soloader {
    extern int g_argc;      // 命令行参数数量
    extern char** g_argv;   // 命令行参数
    extern char** g_envp;   // 环境变量
}
```

## 测试

### 运行测试

```bash
# 1. 编译
cd build && cmake --build .

# 2. 推送到设备
adb push soloader_test /data/local/tmp/
adb push test/libtest_lib.so /data/local/tmp/
adb shell chmod +x /data/local/tmp/soloader_test

# 3. 运行测试
adb shell /data/local/tmp/soloader_test /data/local/tmp/libtest_lib.so
```

### 使用测试脚本

```bash
# Linux/macOS
cd test && ./run_test.sh

# Windows
cd test && run_test.bat
```

### 测试覆盖
- 基础函数调用
- 参数传递和返回值
- 结构体参数
- 回调函数
- 数组和浮点运算
- 内存分配
- C++ 对象（构造/析构）
- TLS 多线程
- C++ 异常处理

## 项目结构

```
newSoLoad/
├── include/
│   ├── soloader.hpp      # 主接口
│   ├── elf_image.hpp     # ELF 解析和符号查找
│   ├── linker.hpp        # 链接器（重定位、依赖加载）
│   ├── tls.hpp           # TLS 管理
│   ├── backtrace.hpp     # 回溯支持
│   ├── sleb128.hpp       # SLEB128/ULEB128 解码
│   └── log.hpp           # 日志宏
├── src/
│   ├── soloader.cpp      # SoLoader 实现
│   ├── elf_image.cpp     # ELF 解析实现
│   ├── linker.cpp        # 链接器实现
│   ├── tls.cpp           # TLS 实现
│   └── backtrace.cpp     # 回溯实现
├── test/
│   ├── test_lib.cpp      # 测试库源码
│   ├── CMakeLists.txt    # 测试构建配置
│   ├── run_test.sh       # Linux 测试脚本
│   └── run_test.bat      # Windows 测试脚本
├── CMakeLists.txt        # 主构建配置
└── README.md
```

## 技术细节

### 库搜索路径
按以下顺序搜索依赖库：
1. `/apex/com.android.runtime/lib64/` (Android 10+)
2. `/apex/com.android.art/lib64/`
3. `/system/lib64/`
4. `/vendor/lib64/`
5. `/odm/lib64/`
6. `/product/lib64/`

### 支持的重定位类型
| 类型 | 说明 |
|------|------|
| R_AARCH64_NONE | 无操作 |
| R_AARCH64_ABS64 | 绝对地址 |
| R_AARCH64_GLOB_DAT | 全局数据 |
| R_AARCH64_JUMP_SLOT | PLT 跳转槽 |
| R_AARCH64_RELATIVE | 相对重定位 |
| R_AARCH64_IRELATIVE | 间接函数 |
| R_AARCH64_TLS_DTPMOD | TLS 模块 ID |
| R_AARCH64_TLS_DTPREL | TLS 偏移 |
| R_AARCH64_TLS_TPREL | TLS 静态偏移 |
| R_AARCH64_TLSDESC | TLS 描述符 |

### 注意事项

1. **异常处理** - 跨 SO 边界的 C++ 异常类型匹配受 RTTI 限制，建议使用 `catch(...)` 捕获
2. **TLS** - 每个线程首次访问 TLS 时会分配独立的 TLS 块
3. **内存管理** - `unload()` 会释放所有映射的内存，`abandon()` 则保留内存映射

## 调试

启用调试日志：
```cmake
target_compile_definitions(your_target PRIVATE SOLOADER_DEBUG)
```

日志输出到 Android logcat，tag 为 `soloader`。

## 许可证

与原 CSOLoader 项目相同。
