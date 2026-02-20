// Modern C++17 SO Loader - Linker (arm64 only)
#pragma once

#include "elf_image.hpp"
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>

namespace soloader {

struct TlsIndex;

struct LoadedDep {
    std::unique_ptr<ElfImage> image;
    bool is_manual_load = false;
    void* map_base = nullptr;
    size_t map_size = 0;
};

struct SymbolLookup {
    void* address = nullptr;
    ElfImage* image = nullptr;
    uint8_t bind = 0;      // STB_LOCAL, STB_GLOBAL, STB_WEAK
    uint8_t type = 0;      // STT_FUNC, STT_OBJECT, etc.
    bool valid() const { return address != nullptr; }
    bool isWeak() const { return bind == 2; }  // STB_WEAK
};

// 符号缓存条目
struct SymbolCacheEntry {
    void* address = nullptr;
    ElfImage* image = nullptr;
    bool found = false;
};

class Linker {
public:
    Linker() = default;
    ~Linker();
    
    // 不可复制和移动（因为包含 mutex）
    Linker(const Linker&) = delete;
    Linker& operator=(const Linker&) = delete;
    Linker(Linker&&) = delete;
    Linker& operator=(Linker&&) = delete;
    
    bool init(std::unique_ptr<ElfImage> image);
    bool link();
    void destroy();
    void abandon();
    
    ElfImage* mainImage() const { return main_image_.get(); }
    void setMainMapSize(size_t size) { main_map_size_ = size; }
    bool isLinked() const { return is_linked_; }
    
    // 获取加载的依赖数量
    size_t dependencyCount() const { return deps_.size(); }
    
    // 清除符号缓存
    void clearSymbolCache() { 
        std::lock_guard<std::mutex> lock(cache_mutex_);
        symbol_cache_.clear(); 
    }

    static void* loadLibraryManually(std::string_view path, LoadedDep& dep);

private:
    bool loadDependencies();
    void processRelocations(ElfImage* image);
    void processRelocation(ElfImage* image, uint32_t sym_idx, uint32_t type,
                          ElfAddr offset, ElfAddr addend, ElfAddr load_bias,
                          ElfSym* dynsym, const char* dynstr, bool is_rela);
    
    SymbolLookup findSymbol(std::string_view name);
    bool findLibraryPath(std::string_view name, std::string& out);
    bool isLoaded(std::string_view path);
    void restoreProtections(ElfImage* image);
    void callConstructors(ElfImage* image);
    void callDestructors(ElfImage* image);
    
    // 符号缓存查找
    SymbolLookup findSymbolCached(std::string_view name);
    
    std::unique_ptr<ElfImage> main_image_;
    std::vector<LoadedDep> deps_;
    size_t main_map_size_ = 0;
    bool is_linked_ = false;
    
    // 符号缓存
    mutable std::mutex cache_mutex_;
    std::unordered_map<std::string, SymbolCacheEntry> symbol_cache_;

    // TLSDESC 分配的 TlsIndex 指针（需要在 destroy 时释放）
    std::vector<TlsIndex*> tls_indices_;
};

// 全局参数
extern int g_argc;
extern char** g_argv;
extern char** g_envp;

size_t pageSize();
uintptr_t pageStart(uintptr_t addr);
uintptr_t pageEnd(uintptr_t addr);

} // namespace soloader
