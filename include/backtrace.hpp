// Modern C++17 SO Loader - Backtrace Support (arm64 only)
#pragma once

#include "elf_image.hpp"
#include <link.h>
#include <dlfcn.h>
#include <mutex>

namespace soloader {

constexpr size_t MAX_CUSTOM_LIBS = 64;

class BacktraceManager {
public:
    static BacktraceManager& instance();
    
    bool registerLibrary(ElfImage* image);
    bool unregisterLibrary(ElfImage* image);
    
    void registerEhFrame(ElfImage* image);
    void unregisterEhFrame(ElfImage* image);

    // 自定义实现
    static int customDlIteratePhdr(int (*callback)(dl_phdr_info*, size_t, void*), void* data);
    static int customDladdr(const void* addr, Dl_info* info);

private:
    BacktraceManager() = default;
    
    struct LibInfo {
        ElfImage* image = nullptr;
        dl_phdr_info phdr_info{};
        bool in_use = false;
        ElfPhdr* phdr_copy = nullptr;
        void* eh_frame_registered = nullptr;
    };
    
    LibInfo libs_[MAX_CUSTOM_LIBS]{};
    std::mutex mutex_;
};

} // namespace soloader
