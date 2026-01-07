// Modern C++17 SO Loader - Main Interface (arm64 only)
#pragma once

#include "elf_image.hpp"
#include "linker.hpp"
#include <string_view>

namespace soloader {

class SoLoader {
public:
    SoLoader() = default;
    ~SoLoader();
    
    // 不可复制和移动（因为 Linker 包含 mutex）
    SoLoader(const SoLoader&) = delete;
    SoLoader& operator=(const SoLoader&) = delete;
    SoLoader(SoLoader&&) = delete;
    SoLoader& operator=(SoLoader&&) = delete;

    // 加载库
    bool load(std::string_view lib_path);
    
    // 卸载库
    bool unload();
    
    // 放弃（不调用析构函数）
    bool abandon();
    
    // 获取符号地址
    void* getSymbol(std::string_view name);
    
    template<typename T>
    T getSymbol(std::string_view name) {
        return reinterpret_cast<T>(getSymbol(name));
    }
    
    // 是否已加载
    bool isLoaded() const { return image_ != nullptr; }
    
    // 获取库路径
    const std::string& path() const { return lib_path_; }

private:
    std::string lib_path_;
    ElfImage* image_ = nullptr;
    Linker linker_;
};

} // namespace soloader
