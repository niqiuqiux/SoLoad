// Modern C++17 SO Loader - ELF Image Implementation (arm64 only)

#include "elf_image.hpp"
#include "log.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/auxv.h>
#include <cstring>

namespace soloader {

// SHT_GNU_HASH 可能已在系统头文件中定义
#ifndef SHT_GNU_HASH
#define SHT_GNU_HASH 0x6ffffff6
#endif

// ELF 类型验证
static bool validateElfHeader(const ElfEhdr* header, size_t file_size) {
    // 验证魔数
    if (memcmp(header->e_ident, ELFMAG, SELFMAG) != 0) {
        LOGE("Invalid ELF magic");
        return false;
    }
    
    // 验证类型 (64-bit)
    if (header->e_ident[EI_CLASS] != ELFCLASS64) {
        LOGE("Not a 64-bit ELF file");
        return false;
    }
    
    // 验证字节序 (小端)
    if (header->e_ident[EI_DATA] != ELFDATA2LSB) {
        LOGE("Not a little-endian ELF file");
        return false;
    }
    
    // 验证机器类型 (ARM64)
    if (header->e_machine != EM_AARCH64) {
        LOGE("Not an ARM64 ELF file (machine=%u)", header->e_machine);
        return false;
    }
    
    // 验证类型 (共享库或可执行文件)
    if (header->e_type != ET_DYN && header->e_type != ET_EXEC) {
        LOGE("Not a shared library or executable (type=%u)", header->e_type);
        return false;
    }
    
    // 验证程序头表
    if (header->e_phoff == 0 || header->e_phnum == 0) {
        LOGE("No program headers");
        return false;
    }
    
    if (header->e_phoff + header->e_phnum * sizeof(ElfPhdr) > file_size) {
        LOGE("Program header table out of bounds");
        return false;
    }
    
    // 验证节区头表（可选）
    if (header->e_shoff != 0) {
        if (header->e_shoff + header->e_shnum * sizeof(ElfShdr) > file_size) {
            LOGW("Section header table out of bounds, ignoring sections");
        }
    }
    
    return true;
}

uint32_t elfHash(std::string_view name) {
    uint32_t h = 0;
    for (char c : name) {
        h = (h << 4) + static_cast<uint8_t>(c);
        uint32_t g = h & 0xf0000000;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

uint32_t gnuHash(std::string_view name) {
    uint32_t h = 5381;
    for (char c : name) {
        h = (h << 5) + h + static_cast<uint8_t>(c);
    }
    return h;
}

ElfImage::~ElfImage() {
    if (header_) {
        free(header_);
        header_ = nullptr;
    }
}

ElfImage::ElfImage(ElfImage&& other) noexcept {
    *this = std::move(other);
}

ElfImage& ElfImage::operator=(ElfImage&& other) noexcept {
    if (this != &other) {
        // 释放当前资源
        if (header_) {
            free(header_);
            header_ = nullptr;
        }
        
        // 移动所有成员
        path_ = std::move(other.path_);
        base_ = other.base_;
        header_ = other.header_;
        file_size_ = other.file_size_;
        bias_ = other.bias_;
        section_header_ = other.section_header_;
        dynsym_shdr_ = other.dynsym_shdr_;
        dynsym_start_ = other.dynsym_start_;
        strtab_shdr_ = other.strtab_shdr_;
        strtab_start_ = other.strtab_start_;
        nbucket_ = other.nbucket_;
        bucket_ = other.bucket_;
        chain_ = other.chain_;
        gnu_nbucket_ = other.gnu_nbucket_;
        gnu_symndx_ = other.gnu_symndx_;
        gnu_bloom_size_ = other.gnu_bloom_size_;
        gnu_shift2_ = other.gnu_shift2_;
        gnu_bloom_filter_ = other.gnu_bloom_filter_;
        gnu_bucket_ = other.gnu_bucket_;
        gnu_chain_ = other.gnu_chain_;
        symtab_start_ = other.symtab_start_;
        symtab_count_ = other.symtab_count_;
        symtab_strtab_ = other.symtab_strtab_;
        tls_segment_ = other.tls_segment_;
        tls_mod_id_ = other.tls_mod_id_;
        init_array_ = other.init_array_;
        init_array_count_ = other.init_array_count_;
        fini_array_ = other.fini_array_;
        fini_array_count_ = other.fini_array_count_;
        init_func_ = other.init_func_;
        fini_func_ = other.fini_func_;
        eh_frame_ = other.eh_frame_;
        eh_frame_size_ = other.eh_frame_size_;
        eh_frame_hdr_ = other.eh_frame_hdr_;
        eh_frame_hdr_size_ = other.eh_frame_hdr_size_;
        
        // 清空源对象
        other.header_ = nullptr;
        other.base_ = nullptr;
        other.file_size_ = 0;
    }
    return *this;
}

std::unique_ptr<ElfImage> ElfImage::create(std::string_view path, void* base) {
    auto img = std::unique_ptr<ElfImage>(new ElfImage());
    if (!img->init(path, base)) {
        return nullptr;
    }
    return img;
}

bool ElfImage::init(std::string_view path, void* base) {
    path_ = path;
    
    if (base) {
        base_ = base;
        LOGD("Using provided base %p for %s", base, path_.c_str());
    } else {
        // 尝试通过 dl_iterate_phdr 查找
        dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
            auto* self = static_cast<ElfImage*>(data);
            if (info->dlpi_name && strstr(info->dlpi_name, self->path_.c_str())) {
                self->base_ = reinterpret_cast<void*>(info->dlpi_addr);
                self->path_ = info->dlpi_name;
                return 1;
            }
            return 0;
        }, this);
        
        if (!base_) {
            LOGE("Failed to find base for %s", path_.c_str());
            return false;
        }
    }
    
    // 打开并读取文件
    int fd = open(path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        PLOGE("open %s", path_.c_str());
        return false;
    }
    
    struct stat st;
    if (fstat(fd, &st) != 0) {
        PLOGE("fstat %s", path_.c_str());
        close(fd);
        return false;
    }
    
    file_size_ = st.st_size;
    if (file_size_ <= sizeof(ElfEhdr)) {
        LOGE("File too small: %s", path_.c_str());
        close(fd);
        return false;
    }
    
    header_ = static_cast<ElfEhdr*>(malloc(file_size_));
    if (!header_) {
        LOGE("Failed to allocate %zu bytes", file_size_);
        close(fd);
        return false;
    }
    
    size_t total = 0;
    while (total < file_size_) {
        ssize_t n = read(fd, reinterpret_cast<char*>(header_) + total, file_size_ - total);
        if (n <= 0) {
            PLOGE("read %s", path_.c_str());
            close(fd);
            return false;
        }
        total += n;
    }
    close(fd);
    
    // 完整验证 ELF 头
    if (!validateElfHeader(header_, file_size_)) {
        LOGE("ELF validation failed: %s", path_.c_str());
        return false;
    }
    
    return parseHeaders() && parseDynamic();
}

bool ElfImage::parseHeaders() {
    // 节区头表
    if (header_->e_shoff && header_->e_shnum) {
        section_header_ = reinterpret_cast<ElfShdr*>(
            reinterpret_cast<uintptr_t>(header_) + header_->e_shoff);
    }
    
    // 节区名字符串表
    char* section_str = nullptr;
    if (section_header_ && header_->e_shstrndx < header_->e_shnum) {
        auto* shstrtab = section_header_ + header_->e_shstrndx;
        section_str = reinterpret_cast<char*>(
            reinterpret_cast<uintptr_t>(header_) + shstrtab->sh_offset);
    }
    
    // 遍历节区
    if (section_header_) {
        for (int i = 0; i < header_->e_shnum; i++) {
            auto* sh = section_header_ + i;
            const char* name = section_str ? section_str + sh->sh_name : "";
            
            switch (sh->sh_type) {
            case SHT_DYNSYM:
                dynsym_shdr_ = sh;
                dynsym_start_ = reinterpret_cast<ElfSym*>(
                    reinterpret_cast<uintptr_t>(header_) + sh->sh_offset);
                break;
                
            case SHT_SYMTAB:
                if (strcmp(name, ".symtab") == 0) {
                    symtab_start_ = reinterpret_cast<ElfSym*>(
                        reinterpret_cast<uintptr_t>(header_) + sh->sh_offset);
                    symtab_count_ = sh->sh_entsize ? sh->sh_size / sh->sh_entsize : 0;
                    
                    if (sh->sh_link < header_->e_shnum) {
                        auto* linked = section_header_ + sh->sh_link;
                        symtab_strtab_ = reinterpret_cast<const char*>(
                            reinterpret_cast<uintptr_t>(header_) + linked->sh_offset);
                    }
                }
                break;
                
            case SHT_HASH: {
                auto* d = reinterpret_cast<uint32_t*>(
                    reinterpret_cast<uintptr_t>(header_) + sh->sh_offset);
                if (sh->sh_size >= 2 * sizeof(uint32_t)) {
                    nbucket_ = d[0];
                    bucket_ = d + 2;
                    chain_ = bucket_ + nbucket_;
                }
                break;
            }
            
            case SHT_GNU_HASH: {
                auto* d = reinterpret_cast<uint32_t*>(
                    reinterpret_cast<uintptr_t>(header_) + sh->sh_offset);
                if (sh->sh_size >= 4 * sizeof(uint32_t)) {
                    gnu_nbucket_ = d[0];
                    gnu_symndx_ = d[1];
                    gnu_bloom_size_ = d[2];
                    gnu_shift2_ = d[3];
                    gnu_bloom_filter_ = reinterpret_cast<uintptr_t*>(d + 4);
                    gnu_bucket_ = reinterpret_cast<uint32_t*>(gnu_bloom_filter_ + gnu_bloom_size_);
                    gnu_chain_ = gnu_bucket_ + gnu_nbucket_;
                }
                break;
            }
            }
        }
    }
    
    // 链接 dynsym 的字符串表
    if (dynsym_shdr_ && dynsym_shdr_->sh_link < header_->e_shnum) {
        strtab_shdr_ = section_header_ + dynsym_shdr_->sh_link;
        strtab_start_ = reinterpret_cast<const char*>(
            reinterpret_cast<uintptr_t>(header_) + strtab_shdr_->sh_offset);
    }
    
    return true;
}

bool ElfImage::parseDynamic() {
    if (!header_->e_phoff || !header_->e_phnum) return true;
    
    auto* phdr = reinterpret_cast<ElfPhdr*>(
        reinterpret_cast<uintptr_t>(header_) + header_->e_phoff);
    
    ElfDyn* dyn = nullptr;
    
    // 计算 bias 并找到动态段
    for (int i = 0; i < header_->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD && phdr[i].p_offset == 0) {
            bias_ = phdr[i].p_vaddr;
        }
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<ElfDyn*>(
                reinterpret_cast<uintptr_t>(base_) + phdr[i].p_vaddr - bias_);
        }
        if (phdr[i].p_type == PT_TLS) {
            tls_segment_ = &phdr[i];
        }
        if (phdr[i].p_type == PT_GNU_EH_FRAME) {
            eh_frame_hdr_ = reinterpret_cast<const uint8_t*>(
                reinterpret_cast<uintptr_t>(base_) + phdr[i].p_vaddr - bias_);
            eh_frame_hdr_size_ = phdr[i].p_memsz;
        }
    }
    
    // 如果没找到 bias，用第一个 PT_LOAD
    if (bias_ == 0) {
        for (int i = 0; i < header_->e_phnum; i++) {
            if (phdr[i].p_type == PT_LOAD) {
                bias_ = phdr[i].p_vaddr - phdr[i].p_offset;
                break;
            }
        }
    }
    
    // 解析动态段
    if (dyn) {
        for (auto* d = dyn; d->d_tag != DT_NULL; d++) {
            auto ptr = reinterpret_cast<uintptr_t>(base_) + d->d_un.d_ptr - bias_;
            
            switch (d->d_tag) {
            case DT_INIT:
                init_func_ = reinterpret_cast<InitFunc>(ptr);
                break;
            case DT_FINI:
                fini_func_ = reinterpret_cast<InitFunc>(ptr);
                break;
            case DT_INIT_ARRAY:
                init_array_ = reinterpret_cast<CtorFunc*>(ptr);
                break;
            case DT_INIT_ARRAYSZ:
                init_array_count_ = d->d_un.d_val / sizeof(ElfAddr);
                break;
            case DT_FINI_ARRAY:
                fini_array_ = reinterpret_cast<DtorFunc*>(ptr);
                break;
            case DT_FINI_ARRAYSZ:
                fini_array_count_ = d->d_un.d_val / sizeof(ElfAddr);
                break;
            }
        }
    }
    
    // 从节区头查找 eh_frame
    if (section_header_ && header_->e_shstrndx < header_->e_shnum) {
        auto* shstrtab = section_header_ + header_->e_shstrndx;
        auto* names = reinterpret_cast<const char*>(
            reinterpret_cast<uintptr_t>(header_) + shstrtab->sh_offset);
        
        for (int i = 0; i < header_->e_shnum; i++) {
            auto* sh = section_header_ + i;
            const char* name = names + sh->sh_name;
            
            if (strcmp(name, ".eh_frame") == 0) {
                eh_frame_ = reinterpret_cast<const uint8_t*>(
                    reinterpret_cast<uintptr_t>(base_) + sh->sh_addr - bias_);
                eh_frame_size_ = sh->sh_size;
            }
        }
    }
    
    return true;
}

std::optional<ElfAddr> ElfImage::gnuHashLookup(std::string_view name, uint32_t hash, uint8_t* type, uint8_t* bind) const {
    if (!gnu_nbucket_ || !gnu_bloom_filter_ || !gnu_bucket_ || !gnu_chain_ || !dynsym_start_ || !strtab_start_)
        return std::nullopt;
    
    constexpr size_t BLOOM_BITS = sizeof(uintptr_t) * 8;
    
    // Bloom filter 检查
    size_t bloom_idx = (hash / BLOOM_BITS) % gnu_bloom_size_;
    uintptr_t bloom_word = gnu_bloom_filter_[bloom_idx];
    uintptr_t mask = (uintptr_t(1) << (hash % BLOOM_BITS)) |
                     (uintptr_t(1) << ((hash >> gnu_shift2_) % BLOOM_BITS));
    
    if ((bloom_word & mask) != mask) return std::nullopt;
    
    uint32_t sym_idx = gnu_bucket_[hash % gnu_nbucket_];
    if (sym_idx < gnu_symndx_) return std::nullopt;
    
    uint32_t dynsym_count = dynsym_shdr_->sh_size / dynsym_shdr_->sh_entsize;
    
    while (true) {
        if (sym_idx >= dynsym_count) return std::nullopt;
        
        uint32_t chain_val = gnu_chain_[sym_idx - gnu_symndx_];
        auto* sym = dynsym_start_ + sym_idx;
        
        if (((chain_val ^ hash) >> 1) == 0 &&
            name == (strtab_start_ + sym->st_name) &&
            sym->st_shndx != SHN_UNDEF) {
            if (type) *type = elf_st_type(sym->st_info);
            if (bind) *bind = elf_st_bind(sym->st_info);
            return sym->st_value;
        }
        
        if (chain_val & 1) break;
        sym_idx++;
    }
    
    return std::nullopt;
}

std::optional<ElfAddr> ElfImage::elfHashLookup(std::string_view name, uint32_t hash, uint8_t* type, uint8_t* bind) const {
    if (!nbucket_ || !bucket_ || !chain_ || !dynsym_start_ || !strtab_start_)
        return std::nullopt;
    
    for (uint32_t n = bucket_[hash % nbucket_]; n != STN_UNDEF; n = chain_[n]) {
        auto* sym = dynsym_start_ + n;
        if (name == (strtab_start_ + sym->st_name) && sym->st_shndx != SHN_UNDEF) {
            if (type) *type = elf_st_type(sym->st_info);
            if (bind) *bind = elf_st_bind(sym->st_info);
            return sym->st_value;
        }
    }
    
    return std::nullopt;
}

std::optional<ElfAddr> ElfImage::linearLookup(std::string_view name, uint8_t* type, uint8_t* bind) const {
    if (!symtab_start_ || !symtab_strtab_ || !symtab_count_)
        return std::nullopt;
    
    for (size_t i = 0; i < symtab_count_; i++) {
        auto* sym = symtab_start_ + i;
        uint8_t st = elf_st_type(sym->st_info);
        
        if ((st == STT_FUNC || st == STT_OBJECT) &&
            sym->st_size > 0 && sym->st_shndx != SHN_UNDEF &&
            name == (symtab_strtab_ + sym->st_name)) {
            if (type) *type = st;
            if (bind) *bind = elf_st_bind(sym->st_info);
            return sym->st_value;
        }
    }
    
    return std::nullopt;
}

std::optional<ElfAddr> ElfImage::findSymbolOffset(std::string_view name, uint8_t* type, uint8_t* bind) const {
    if (auto addr = gnuHashLookup(name, gnuHash(name), type, bind)) return addr;
    if (auto addr = elfHashLookup(name, elfHash(name), type, bind)) return addr;
    if (auto addr = linearLookup(name, type, bind)) return addr;
    return std::nullopt;
}

std::optional<ElfAddr> ElfImage::findSymbolAddress(std::string_view name, uint8_t* bind) const {
    uint8_t sym_type = 0;
    auto offset = findSymbolOffset(name, &sym_type, bind);
    if (!offset || !base_) return std::nullopt;
    
    auto addr = reinterpret_cast<uintptr_t>(base_) + *offset - bias_;
    
    // 处理 IFUNC
    if (sym_type == STT_GNU_IFUNC) {
        LOGD("Resolving IFUNC: %.*s", static_cast<int>(name.size()), name.data());
        
        // arm64 IFUNC 解析
        struct IfuncArg {
            unsigned long size;
            unsigned long hwcap;
            unsigned long hwcap2;
        };
        
        IfuncArg arg{sizeof(IfuncArg), getauxval(AT_HWCAP), getauxval(AT_HWCAP2)};
        using Resolver = ElfAddr(*)(uint64_t, IfuncArg*);
        
        return reinterpret_cast<Resolver>(addr)(arg.hwcap | (1ULL << 62), &arg);
    }
    
    return addr;
}

SymbolInfo ElfImage::getSymbolAt(uintptr_t addr) const {
    if (!symtab_start_ || !symtab_strtab_) return {};
    
    for (size_t i = 0; i < symtab_count_; i++) {
        auto* sym = symtab_start_ + i;
        if (sym->st_value == 0 || sym->st_size == 0) continue;
        
        auto start = reinterpret_cast<uintptr_t>(base_) + sym->st_value - bias_;
        auto end = start + sym->st_size;
        
        if (addr >= start && addr < end) {
            return {symtab_strtab_ + sym->st_name, start};
        }
    }
    
    return {};
}

} // namespace soloader
