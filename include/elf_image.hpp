// Modern C++17 SO Loader - ELF Image (arm64 only)
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <memory>
#include <elf.h>
#include <link.h>

namespace soloader {

// arm64 ELF 类型
using ElfAddr = Elf64_Addr;
using ElfSym = Elf64_Sym;
using ElfShdr = Elf64_Shdr;
using ElfPhdr = Elf64_Phdr;
using ElfEhdr = Elf64_Ehdr;
using ElfDyn = Elf64_Dyn;
using ElfRel = Elf64_Rel;
using ElfRela = Elf64_Rela;

#define ELF_R_SYM ELF64_R_SYM
#define ELF_R_TYPE ELF64_R_TYPE

// 避免与系统宏冲突，直接使用内联函数
inline uint8_t elf_st_type(uint8_t info) { return info & 0xf; }
inline uint8_t elf_st_bind(uint8_t info) { return info >> 4; }

// 符号绑定类型 - 使用 #ifndef 避免与系统头文件冲突
#ifndef STB_LOCAL
#define STB_LOCAL 0
#endif
#ifndef STB_GLOBAL
#define STB_GLOBAL 1
#endif
#ifndef STB_WEAK
#define STB_WEAK 2
#endif

struct SymbolInfo {
    std::string_view name;
    ElfAddr address = 0;
    uint8_t type = 0;
    uint8_t bind = 0;
    bool valid() const { return address != 0; }
    bool isWeak() const { return bind == STB_WEAK; }
};

using InitFunc = void(*)();
using CtorFunc = void(*)(int, char**, char**);
using DtorFunc = void(*)();

class ElfImage {
public:
    ~ElfImage();
    ElfImage(ElfImage&&) noexcept;
    ElfImage& operator=(ElfImage&&) noexcept;
    
    static std::unique_ptr<ElfImage> create(std::string_view path, void* base = nullptr);
    
    // 符号查找
    std::optional<ElfAddr> findSymbolOffset(std::string_view name, uint8_t* type = nullptr, uint8_t* bind = nullptr) const;
    std::optional<ElfAddr> findSymbolAddress(std::string_view name, uint8_t* bind = nullptr) const;
    SymbolInfo getSymbolAt(uintptr_t addr) const;

    // Getters
    const std::string& path() const { return path_; }
    void* base() const { return base_; }
    ElfEhdr* header() const { return header_; }
    ptrdiff_t bias() const { return bias_; }
    
    ElfPhdr* tlsSegment() const { return tls_segment_; }
    size_t tlsModuleId() const { return tls_mod_id_; }
    void setTlsModuleId(size_t id) { tls_mod_id_ = id; }
    
    InitFunc initFunc() const { return init_func_; }
    InitFunc finiFunc() const { return fini_func_; }
    CtorFunc* initArray() const { return init_array_; }
    size_t initArrayCount() const { return init_array_count_; }
    DtorFunc* finiArray() const { return fini_array_; }
    size_t finiArrayCount() const { return fini_array_count_; }
    
    const uint8_t* ehFrame() const { return eh_frame_; }
    size_t ehFrameSize() const { return eh_frame_size_; }
    const uint8_t* ehFrameHdr() const { return eh_frame_hdr_; }
    size_t ehFrameHdrSize() const { return eh_frame_hdr_size_; }
    
    ElfSym* dynsymStart() const { return dynsym_start_; }
    const char* strtabStart() const { return strtab_start_; }
    ElfShdr* dynsymShdr() const { return dynsym_shdr_; }
    ElfShdr* strtabShdr() const { return strtab_shdr_; }

private:
    ElfImage() = default;
    bool init(std::string_view path, void* base);
    bool parseHeaders();
    bool parseDynamic();
    
    std::optional<ElfAddr> gnuHashLookup(std::string_view name, uint32_t hash, uint8_t* type, uint8_t* bind) const;
    std::optional<ElfAddr> elfHashLookup(std::string_view name, uint32_t hash, uint8_t* type, uint8_t* bind) const;
    std::optional<ElfAddr> linearLookup(std::string_view name, uint8_t* type, uint8_t* bind) const;

    std::string path_;
    void* base_ = nullptr;
    ElfEhdr* header_ = nullptr;
    size_t file_size_ = 0;
    ptrdiff_t bias_ = 0;
    
    ElfShdr* section_header_ = nullptr;
    ElfShdr* dynsym_shdr_ = nullptr;
    ElfSym* dynsym_start_ = nullptr;
    ElfShdr* strtab_shdr_ = nullptr;
    const char* strtab_start_ = nullptr;
    
    // ELF Hash
    uint32_t nbucket_ = 0;
    uint32_t* bucket_ = nullptr;
    uint32_t* chain_ = nullptr;
    
    // GNU Hash
    uint32_t gnu_nbucket_ = 0;
    uint32_t gnu_symndx_ = 0;
    uint32_t gnu_bloom_size_ = 0;
    uint32_t gnu_shift2_ = 0;
    uintptr_t* gnu_bloom_filter_ = nullptr;
    uint32_t* gnu_bucket_ = nullptr;
    uint32_t* gnu_chain_ = nullptr;
    
    // .symtab
    ElfSym* symtab_start_ = nullptr;
    size_t symtab_count_ = 0;
    const char* symtab_strtab_ = nullptr;
    
    ElfPhdr* tls_segment_ = nullptr;
    size_t tls_mod_id_ = 0;
    
    CtorFunc* init_array_ = nullptr;
    size_t init_array_count_ = 0;
    DtorFunc* fini_array_ = nullptr;
    size_t fini_array_count_ = 0;
    InitFunc init_func_ = nullptr;
    InitFunc fini_func_ = nullptr;
    
    const uint8_t* eh_frame_ = nullptr;
    size_t eh_frame_size_ = 0;
    const uint8_t* eh_frame_hdr_ = nullptr;
    size_t eh_frame_hdr_size_ = 0;
};

uint32_t elfHash(std::string_view name);
uint32_t gnuHash(std::string_view name);

} // namespace soloader
