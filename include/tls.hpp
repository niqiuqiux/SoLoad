// Modern C++17 SO Loader - TLS Support (arm64 only)
#pragma once

#include "elf_image.hpp"
#include <cstddef>

namespace soloader {

constexpr size_t MAX_TLS_MODULES = 128;

struct TlsModule {
    size_t module_id = 0;
    size_t align = 1;
    size_t memsz = 0;
    size_t filesz = 0;
    size_t offset = 0;
    const void* init_image = nullptr;
    ElfImage* owner = nullptr;
};

struct TlsIndex {
    unsigned long module;
    unsigned long offset;
};

class TlsManager {
public:
    static TlsManager& instance();
    
    bool registerSegment(ElfImage* image);
    void unregisterSegment(ElfImage* image);
    
    void* getAddress(TlsIndex* ti);
    TlsIndex* allocateIndex(ElfImage* image, ElfSym* sym, ElfAddr addend);
    
    void bumpGeneration() { generation_++; }

private:
    TlsManager();
    void* allocateBlock();
    void* getBlockForThread();
    
    TlsModule modules_[MAX_TLS_MODULES]{};
    size_t generation_ = 0;
    size_t static_size_ = 0;
    size_t static_align_max_ = 1;
};

// 供链接器调用
extern "C" void* __tls_get_addr(TlsIndex* ti);

} // namespace soloader
