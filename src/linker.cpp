// Modern C++17 SO Loader - Linker Implementation (arm64 only)

#include "linker.hpp"
#include "tls.hpp"
#include "backtrace.hpp"
#include "sleb128.hpp"
#include "log.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/auxv.h>
#include <cstring>
#include <dlfcn.h>
#include <set>

namespace soloader {

// arm64 重定位类型 - 使用 #ifndef 避免重定义
#ifndef R_AARCH64_NONE
#define R_AARCH64_NONE          0
#endif
#ifndef R_AARCH64_ABS64
#define R_AARCH64_ABS64         257
#endif
#ifndef R_AARCH64_GLOB_DAT
#define R_AARCH64_GLOB_DAT      1025
#endif
#ifndef R_AARCH64_JUMP_SLOT
#define R_AARCH64_JUMP_SLOT     1026
#endif
#ifndef R_AARCH64_RELATIVE
#define R_AARCH64_RELATIVE      1027
#endif
#ifndef R_AARCH64_IRELATIVE
#define R_AARCH64_IRELATIVE     1032
#endif
#ifndef R_AARCH64_TLS_DTPMOD
#define R_AARCH64_TLS_DTPMOD    1028
#endif
#ifndef R_AARCH64_TLS_DTPREL
#define R_AARCH64_TLS_DTPREL    1029
#endif
#ifndef R_AARCH64_TLS_TPREL
#define R_AARCH64_TLS_TPREL     1030
#endif
#ifndef R_AARCH64_TLSDESC
#define R_AARCH64_TLSDESC       1031
#endif
#ifndef R_AARCH64_COPY
#define R_AARCH64_COPY          1024
#endif

// Android 压缩重定位
#ifndef DT_ANDROID_RELA
#define DT_ANDROID_RELA     0x6000000f
#define DT_ANDROID_RELASZ   0x60000011
#define DT_ANDROID_REL      0x6000000d
#define DT_ANDROID_RELSZ    0x60000010
#endif

// Android RELR 变体
#ifndef DT_ANDROID_RELR
#define DT_ANDROID_RELR     0x6fffe000
#define DT_ANDROID_RELRSZ   0x6fffe001
#define DT_ANDROID_RELRENT  0x6fffe003
#endif

int g_argc = 0;
char** g_argv = nullptr;
char** g_envp = nullptr;

static size_t s_page_size = 0;

// TLSDESC resolver: 返回相对于 TLS block 基地址的偏移量（而非绝对地址）
static ElfAddr dynamic_tls_resolver(TlsIndex* ti) {
    void* addr = TlsManager::instance().getAddress(ti);
    void* block_base = TlsManager::instance().getAddress(nullptr);
    return reinterpret_cast<ElfAddr>(addr) - reinterpret_cast<ElfAddr>(block_base);
}

size_t pageSize() {
    if (s_page_size == 0) {
        long ps = sysconf(_SC_PAGESIZE);
        if (ps <= 0) {
            LOGF("Failed to get system page size");
        }
        s_page_size = static_cast<size_t>(ps);
    }
    return s_page_size;
}

uintptr_t pageStart(uintptr_t addr) {
    return addr & ~(pageSize() - 1);
}

uintptr_t pageEnd(uintptr_t addr) {
    return pageStart(addr + pageSize() - 1);
}

Linker::~Linker() {
    if (is_linked_) {
        destroy();
    }
}

bool Linker::init(std::unique_ptr<ElfImage> image) {
    main_image_ = std::move(image);
    is_linked_ = false;
    main_map_size_ = 0;
    deps_.clear();
    return true;
}

void Linker::destroy() {
    // 主库析构（主库依赖于依赖库，所以主库析构函数必须先执行）
    if (main_image_ && is_linked_) {
        BacktraceManager::instance().unregisterEhFrame(main_image_.get());
        BacktraceManager::instance().unregisterLibrary(main_image_.get());
        callDestructors(main_image_.get());
    }

    // 逆序调用依赖的析构函数
    for (auto it = deps_.rbegin(); it != deps_.rend(); ++it) {
        if (it->image && it->is_manual_load) {
            BacktraceManager::instance().unregisterEhFrame(it->image.get());
            BacktraceManager::instance().unregisterLibrary(it->image.get());
            callDestructors(it->image.get());
        }
    }

    // 释放 TLSDESC 分配的 TlsIndex
    for (auto* ti : tls_indices_) {
        delete ti;
    }
    tls_indices_.clear();

    // 注销 TLS 段
    for (auto it = deps_.rbegin(); it != deps_.rend(); ++it) {
        if (it->image) {
            TlsManager::instance().unregisterSegment(it->image.get());
        }
    }
    if (main_image_) {
        TlsManager::instance().unregisterSegment(main_image_.get());
    }

    // 释放依赖
    for (auto& dep : deps_) {
        if (dep.is_manual_load && dep.map_size > 0) {
            munmap(dep.map_base, dep.map_size);
        }
    }
    deps_.clear();

    // 释放主库
    if (main_map_size_ > 0 && main_image_) {
        munmap(main_image_->base(), main_map_size_);
    }
    main_image_.reset();

    is_linked_ = false;
    main_map_size_ = 0;
}

void Linker::abandon() {
    // 类似 destroy 但不调用析构函数
    for (auto& dep : deps_) {
        if (dep.image && dep.is_manual_load) {
            BacktraceManager::instance().unregisterEhFrame(dep.image.get());
            BacktraceManager::instance().unregisterLibrary(dep.image.get());
        }
    }

    if (main_image_ && is_linked_) {
        BacktraceManager::instance().unregisterEhFrame(main_image_.get());
        BacktraceManager::instance().unregisterLibrary(main_image_.get());
    }

    // 释放 TLSDESC 分配的 TlsIndex
    for (auto* ti : tls_indices_) {
        delete ti;
    }
    tls_indices_.clear();

    // 注销 TLS 段
    for (auto it = deps_.rbegin(); it != deps_.rend(); ++it) {
        if (it->image) {
            TlsManager::instance().unregisterSegment(it->image.get());
        }
    }
    if (main_image_) {
        TlsManager::instance().unregisterSegment(main_image_.get());
    }

    deps_.clear();
    main_image_.reset();
    is_linked_ = false;
    main_map_size_ = 0;
}

static size_t getLoadSize(const ElfPhdr* phdr, size_t count, ElfAddr* min_vaddr) {
    ElfAddr lo = UINTPTR_MAX, hi = 0;
    
    for (size_t i = 0; i < count; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;
        if (phdr[i].p_vaddr < lo) lo = phdr[i].p_vaddr;
        if (phdr[i].p_vaddr + phdr[i].p_memsz > hi) hi = phdr[i].p_vaddr + phdr[i].p_memsz;
    }
    
    lo = pageStart(lo);
    hi = pageEnd(hi);
    
    if (min_vaddr) *min_vaddr = lo;
    return hi - lo;
}

static int loadSegment(int fd, ElfPhdr* phdr, ElfAddr bias) {
    auto seg_start = phdr->p_vaddr + bias;
    auto seg_end = seg_start + phdr->p_memsz;
    auto file_end = seg_start + phdr->p_filesz;
    
    auto pg_start = pageStart(seg_start);
    auto pg_end = pageEnd(seg_end);
    auto file_page = pageStart(phdr->p_offset);
    auto file_len = pageEnd(phdr->p_offset + phdr->p_filesz) - file_page;
    
    int prot = 0;
    if (phdr->p_flags & PF_R) prot |= PROT_READ;
    if (phdr->p_flags & PF_W) prot |= PROT_WRITE;
    if (phdr->p_flags & PF_X) prot |= PROT_EXEC;
    
    // W+X 需要特殊处理
    bool needs_mprotect = (prot & PROT_WRITE) && (prot & PROT_EXEC);
    if (needs_mprotect) prot &= ~PROT_EXEC;
    
    // 映射文件内容
    if (file_len > 0) {
        if (mmap(reinterpret_cast<void*>(pg_start), file_len, prot,
                 MAP_FIXED | MAP_PRIVATE, fd, file_page) == MAP_FAILED) {
            PLOGE("mmap segment");
            return -1;
        }
    }
    
    // BSS 段
    if (pg_end > pg_start + file_len) {
        auto bss_addr = reinterpret_cast<void*>(pg_start + file_len);
        auto bss_size = pg_end - (pg_start + file_len);
        
        if (mmap(bss_addr, bss_size, prot,
                 MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) {
            PLOGE("mmap BSS");
            return -1;
        }
        memset(bss_addr, 0, bss_size);
    }
    
    // 清零文件末尾到页边界
    if ((phdr->p_flags & PF_W) && file_end < seg_end) {
        auto zero_start = file_end;
        auto zero_len = std::min(pageEnd(file_end) - file_end, seg_end - file_end);
        memset(reinterpret_cast<void*>(zero_start), 0, zero_len);
    }
    
    // 恢复 EXEC
    if (needs_mprotect) {
        mprotect(reinterpret_cast<void*>(pg_start), pg_end - pg_start, prot | PROT_EXEC);
    }
    
    return 0;
}

void* Linker::loadLibraryManually(std::string_view path, LoadedDep& dep) {
    pageSize(); // 确保初始化
    
    int fd = open(std::string(path).c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        PLOGE("open %.*s", static_cast<int>(path.size()), path.data());
        return nullptr;
    }
    
    ElfEhdr eh;
    if (pread(fd, &eh, sizeof(eh), 0) != sizeof(eh)) {
        LOGE("Failed to read ELF header");
        close(fd);
        return nullptr;
    }
    
    size_t phdr_size = eh.e_phnum * sizeof(ElfPhdr);
    auto phdr = std::make_unique<ElfPhdr[]>(eh.e_phnum);
    
    if (pread(fd, phdr.get(), phdr_size, eh.e_phoff) != static_cast<ssize_t>(phdr_size)) {
        LOGE("Failed to read program headers");
        close(fd);
        return nullptr;
    }
    
    ElfAddr min_vaddr;
    dep.map_size = getLoadSize(phdr.get(), eh.e_phnum, &min_vaddr);
    if (dep.map_size == 0) {
        LOGE("No loadable segments");
        close(fd);
        return nullptr;
    }
    
    // 预留地址空间
    void* base = mmap(nullptr, dep.map_size, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        PLOGE("mmap reserve");
        close(fd);
        return nullptr;
    }
    
    ElfAddr bias = reinterpret_cast<ElfAddr>(base) - min_vaddr;
    
    // 加载各段
    for (int i = 0; i < eh.e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;
        if (loadSegment(fd, &phdr[i], bias) != 0) {
            munmap(base, dep.map_size);
            close(fd);
            return nullptr;
        }
    }
    
    close(fd);
    
    dep.is_manual_load = true;
    dep.map_base = base;
    
    return base;
}

bool Linker::findLibraryPath(std::string_view name, std::string& out) {
    // 如果是绝对路径，直接使用
    if (!name.empty() && name[0] == '/') {
        out = name;
        if (access(out.c_str(), F_OK) == 0) return true;
        LOGE("Library not found at absolute path: %s", out.c_str());
        return false;
    }
    
    // Android 库搜索路径（按优先级排序）
    static const char* search_paths[] = {
        // APEX 运行时库（Android 10+）
        "/apex/com.android.runtime/lib64/bionic/",
        "/apex/com.android.runtime/lib64/",
        "/apex/com.android.art/lib64/",
        // 系统库
        "/system/lib64/",
        "/system/lib64/vndk/",
        "/system/lib64/vndk-sp/",
        // 供应商库
        "/vendor/lib64/",
        "/vendor/lib64/vndk/",
        "/vendor/lib64/vndk-sp/",
        // ODM 库
        "/odm/lib64/",
        // 产品库
        "/product/lib64/",
        // 系统扩展库
        "/system_ext/lib64/",
        nullptr
    };
    
    // 特殊库名映射
    std::string actual_name(name);
    
    // libc++.so 可能有不同的位置
    if (name == "libc++.so") {
        // 优先检查 APEX
        out = "/apex/com.android.runtime/lib64/libc++.so";
        if (access(out.c_str(), F_OK) == 0) return true;
        
        out = "/system/lib64/libc++.so";
        if (access(out.c_str(), F_OK) == 0) return true;
    }
    
    // 搜索所有路径
    for (int i = 0; search_paths[i]; i++) {
        out = std::string(search_paths[i]) + actual_name;
        if (access(out.c_str(), F_OK) == 0) {
            LOGD("Found library: %s", out.c_str());
            return true;
        }
    }
    
    LOGE("Library not found: %.*s", static_cast<int>(name.size()), name.data());
    return false;
}

bool Linker::isLoaded(std::string_view path) {
    if (main_image_ && main_image_->path() == path) return true;
    for (auto& dep : deps_) {
        if (dep.image && dep.image->path() == path) return true;
    }
    return false;
}

SymbolLookup Linker::findSymbolCached(std::string_view name) {
    std::string name_str(name);
    
    // 检查缓存
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = symbol_cache_.find(name_str);
        if (it != symbol_cache_.end()) {
            if (it->second.found) {
                return {it->second.address, it->second.image};
            }
            return {};  // 缓存了"未找到"的结果
        }
    }
    
    // 未缓存，执行查找
    auto result = findSymbol(name);
    
    // 存入缓存
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        symbol_cache_[name_str] = {result.address, result.image, result.valid()};
    }
    
    return result;
}

SymbolLookup Linker::findSymbol(std::string_view name) {
    SymbolLookup result;
    SymbolLookup weak_result;  // 保存弱符号结果
    
    // 先在主库查找
    if (main_image_) {
        uint8_t sym_bind = 0;
        if (auto addr = main_image_->findSymbolAddress(name, &sym_bind)) {
            result = {reinterpret_cast<void*>(*addr), main_image_.get(), sym_bind, 0};
            
            // 如果是强符号（GLOBAL），直接返回
            if (sym_bind == STB_GLOBAL) {
                return result;
            }
            // 如果是弱符号，保存但继续查找
            if (sym_bind == STB_WEAK && !weak_result.valid()) {
                weak_result = result;
            }
        }
    }
    
    // 在依赖中查找
    for (auto& dep : deps_) {
        if (!dep.image) continue;
        
        uint8_t sym_bind = 0;
        if (auto addr = dep.image->findSymbolAddress(name, &sym_bind)) {
            result = {reinterpret_cast<void*>(*addr), dep.image.get(), sym_bind, 0};
            
            // 如果是强符号，直接返回
            if (sym_bind == STB_GLOBAL) {
                return result;
            }
            // 如果是弱符号，保存但继续查找
            if (sym_bind == STB_WEAK && !weak_result.valid()) {
                weak_result = result;
            }
        }
    }
    
    // 如果找到了强符号结果，返回它
    if (result.valid() && !result.isWeak()) {
        return result;
    }
    
    // 如果有弱符号结果，返回它
    if (weak_result.valid()) {
        LOGD("Using weak symbol for '%.*s'", static_cast<int>(name.size()), name.data());
        return weak_result;
    }
    
    // 尝试从系统库查找（通过 dlsym）
    void* sys_addr = dlsym(RTLD_DEFAULT, std::string(name).c_str());
    if (sys_addr) {
        LOGD("Found symbol '%.*s' in system libraries", 
             static_cast<int>(name.size()), name.data());
        return {sys_addr, nullptr, STB_GLOBAL, 0};
    }
    
    LOGE("Symbol not found: %.*s", static_cast<int>(name.size()), name.data());
    return {};
}

bool Linker::loadDependencies() {
    std::set<std::string> loaded_names;
    std::vector<std::string> to_load;

    // 辅助函数：从动态段获取字符串表和 DT_NEEDED 列表
    auto collectNeeded = [](ElfImage* img, ElfDyn* dyn,
                            std::set<std::string>& names,
                            std::vector<std::string>& out) {
        if (!dyn) return;

        // 优先使用 DT_STRTAB（运行时地址），回退到节区头 strtab
        const char* strtab = nullptr;
        for (auto* d = dyn; d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_STRTAB) {
                strtab = reinterpret_cast<const char*>(
                    reinterpret_cast<uintptr_t>(img->base()) + d->d_un.d_ptr - img->bias());
                break;
            }
        }
        if (!strtab) strtab = img->strtabStart();
        if (!strtab) return;

        for (auto* d = dyn; d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_NEEDED) {
                std::string name = strtab + d->d_un.d_val;
                if (names.insert(name).second) {
                    out.push_back(name);
                }
            }
        }
    };

    // 收集主库的依赖
    auto* header = main_image_->header();
    if (!header->e_phoff) return true;

    auto* phdr = reinterpret_cast<ElfPhdr*>(
        reinterpret_cast<uintptr_t>(header) + header->e_phoff);

    ElfDyn* dyn = nullptr;
    for (int i = 0; i < header->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<ElfDyn*>(
                reinterpret_cast<uintptr_t>(main_image_->base()) +
                phdr[i].p_vaddr - main_image_->bias());
            break;
        }
    }

    collectNeeded(main_image_.get(), dyn, loaded_names, to_load);

    // 递归加载依赖
    for (size_t i = 0; i < to_load.size(); i++) {
        std::string full_path;
        if (!findLibraryPath(to_load[i], full_path)) {
            LOGW("Skipping missing library: %s", to_load[i].c_str());
            continue;
        }

        if (isLoaded(full_path)) continue;

        LoadedDep dep;

        // 尝试使用系统已加载的库
        auto check = ElfImage::create(full_path, nullptr);
        if (check && check->base()) {
            dep.image = std::move(check);
            dep.is_manual_load = false;
        } else {
            // 手动加载
            void* base = loadLibraryManually(full_path, dep);
            if (!base) {
                LOGE("Failed to load: %s", full_path.c_str());
                return false;
            }
            dep.image = ElfImage::create(full_path, base);
            if (!dep.image) {
                munmap(base, dep.map_size);
                return false;
            }
            dep.is_manual_load = true;
        }

        // 收集该依赖的依赖
        if (dep.is_manual_load && dep.image->header()->e_phoff) {
            auto* dep_header = dep.image->header();
            auto* dep_phdr = reinterpret_cast<ElfPhdr*>(
                reinterpret_cast<uintptr_t>(dep_header) + dep_header->e_phoff);

            ElfDyn* dep_dyn = nullptr;
            for (int j = 0; j < dep_header->e_phnum; j++) {
                if (dep_phdr[j].p_type == PT_DYNAMIC) {
                    dep_dyn = reinterpret_cast<ElfDyn*>(
                        reinterpret_cast<uintptr_t>(dep.image->base()) +
                        dep_phdr[j].p_vaddr - dep.image->bias());
                    break;
                }
            }

            collectNeeded(dep.image.get(), dep_dyn, loaded_names, to_load);
        }

        deps_.push_back(std::move(dep));
    }

    return true;
}

void Linker::processRelocation(ElfImage* /*image*/, uint32_t sym_idx, uint32_t type,
                               ElfAddr offset, ElfAddr addend, ElfAddr load_bias,
                               ElfSym* dynsym, const char* dynstr, bool is_rela) {
    auto* target = reinterpret_cast<ElfAddr*>(load_bias + offset);

    switch (type) {
    case R_AARCH64_NONE:
        break;

    case R_AARCH64_COPY:
        LOGW("R_AARCH64_COPY relocation not supported");
        break;

    case R_AARCH64_RELATIVE:
        *target = load_bias + (is_rela ? addend : *target);
        break;

    case R_AARCH64_IRELATIVE: {
        auto resolver = load_bias + (is_rela ? addend : *target);

        struct IfuncArg { unsigned long size, hwcap, hwcap2; };
        IfuncArg arg{sizeof(IfuncArg), getauxval(AT_HWCAP), getauxval(AT_HWCAP2)};
        using Resolver = ElfAddr(*)(uint64_t, IfuncArg*);

        *target = reinterpret_cast<Resolver>(resolver)(arg.hwcap | (1ULL << 62), &arg);
        break;
    }

    case R_AARCH64_GLOB_DAT:
    case R_AARCH64_ABS64:
    case R_AARCH64_JUMP_SLOT:
    case R_AARCH64_TLS_DTPMOD:
    case R_AARCH64_TLS_DTPREL:
    case R_AARCH64_TLS_TPREL:
    case R_AARCH64_TLSDESC: {
        const char* sym_name = dynstr + dynsym[sym_idx].st_name;
        auto sym = findSymbolCached(sym_name);

        if (!sym.valid()) {
            LOGE("Undefined symbol: %s", sym_name);
            return;
        }

        // Hook dl_iterate_phdr 和 dladdr
        if (strcmp(sym_name, "dl_iterate_phdr") == 0) {
            *target = reinterpret_cast<ElfAddr>(&BacktraceManager::customDlIteratePhdr);
            return;
        }
        if (strcmp(sym_name, "dladdr") == 0) {
            *target = reinterpret_cast<ElfAddr>(&BacktraceManager::customDladdr);
            return;
        }

        switch (type) {
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT:
            *target = reinterpret_cast<ElfAddr>(sym.address);
            break;
        case R_AARCH64_ABS64:
            *target = reinterpret_cast<ElfAddr>(sym.address) + (is_rela ? addend : *target);
            break;
        case R_AARCH64_TLS_DTPMOD:
            // TLS 重定位需要有效的 image
            if (!sym.image) {
                LOGE("TLS_DTPMOD requires loaded image for symbol: %s", sym_name);
                *target = 0;
            } else {
                *target = sym.image->tlsSegment() ? sym.image->tlsModuleId() : 0;
            }
            break;
        case R_AARCH64_TLS_DTPREL:
            *target = dynsym[sym_idx].st_value + addend;
            break;
        case R_AARCH64_TLS_TPREL: {
            if (!sym.image) {
                LOGE("TLS_TPREL requires loaded image for symbol: %s", sym_name);
                *target = 0;
                break;
            }
            TlsIndex ti{sym.image->tlsModuleId(), static_cast<unsigned long>(dynsym[sym_idx].st_value + addend)};
            auto* block = TlsManager::instance().getAddress(&ti);
            if (block) {
                *target = reinterpret_cast<ElfAddr>(block) -
                          reinterpret_cast<ElfAddr>(TlsManager::instance().getAddress(nullptr));
            } else {
                LOGE("Failed to get TLS address for symbol: %s", sym_name);
                *target = 0;
            }
            break;
        }
        case R_AARCH64_TLSDESC: {
            if (!sym.image) {
                LOGE("TLSDESC requires loaded image for symbol: %s", sym_name);
                target[0] = 0;
                target[1] = 0;
                break;
            }
            auto* ti = TlsManager::instance().allocateIndex(
                sym.image, &dynsym[sym_idx], addend);
            target[0] = reinterpret_cast<ElfAddr>(&dynamic_tls_resolver);
            target[1] = reinterpret_cast<ElfAddr>(ti);
            tls_indices_.push_back(ti);
            break;
        }
        }
        break;
    }

    default:
        LOGE("Unsupported relocation type: %u", type);
    }
}

void Linker::processRelocations(ElfImage* image) {
    auto* header = image->header();
    auto* phdr = reinterpret_cast<ElfPhdr*>(
        reinterpret_cast<uintptr_t>(header) + header->e_phoff);
    
    ElfDyn* dyn = nullptr;
    for (int i = 0; i < header->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<ElfDyn*>(
                reinterpret_cast<uintptr_t>(image->base()) + 
                phdr[i].p_vaddr - image->bias());
            break;
        }
    }
    
    if (!dyn) return;
    
    ElfRela* rela = nullptr;
    size_t rela_sz = 0, rela_ent = 0;
    ElfRel* rel = nullptr;
    size_t rel_sz = 0, rel_ent = 0;
    ElfAddr* relr = nullptr;
    size_t relr_sz = 0;
    void* jmprel = nullptr;
    size_t jmprel_sz = 0;
    int pltrel_type = 0;
    
    ElfSym* dynsym = nullptr;
    const char* dynstr = nullptr;
    
    void* android_reloc = nullptr;
    size_t android_reloc_sz = 0;
    bool is_android_rela = false;
    
    ElfAddr load_bias = reinterpret_cast<ElfAddr>(image->base()) - image->bias();
    
    for (auto* d = dyn; d->d_tag != DT_NULL; d++) {
        auto ptr = reinterpret_cast<uintptr_t>(image->base()) + d->d_un.d_ptr - image->bias();
        
        switch (d->d_tag) {
        case DT_RELA:       rela = reinterpret_cast<ElfRela*>(ptr); break;
        case DT_RELASZ:     rela_sz = d->d_un.d_val; break;
        case DT_RELAENT:    rela_ent = d->d_un.d_val; break;
        case DT_REL:        rel = reinterpret_cast<ElfRel*>(ptr); break;
        case DT_RELSZ:      rel_sz = d->d_un.d_val; break;
        case DT_RELENT:     rel_ent = d->d_un.d_val; break;
        case DT_RELR:       relr = reinterpret_cast<ElfAddr*>(ptr); break;
        case DT_RELRSZ:     relr_sz = d->d_un.d_val; break;
        case DT_JMPREL:     jmprel = reinterpret_cast<void*>(ptr); break;
        case DT_PLTRELSZ:   jmprel_sz = d->d_un.d_val; break;
        case DT_PLTREL:     pltrel_type = d->d_un.d_val; break;
        case DT_SYMTAB:     dynsym = reinterpret_cast<ElfSym*>(ptr); break;
        case DT_STRTAB:     dynstr = reinterpret_cast<const char*>(ptr); break;
        case DT_ANDROID_RELA:
            android_reloc = reinterpret_cast<void*>(ptr);
            is_android_rela = true;
            break;
        case DT_ANDROID_RELASZ:
        case DT_ANDROID_RELSZ:
            android_reloc_sz = d->d_un.d_val;
            break;
        case DT_ANDROID_REL:
            android_reloc = reinterpret_cast<void*>(ptr);
            break;
        case DT_ANDROID_RELR:
            relr = reinterpret_cast<ElfAddr*>(ptr);
            break;
        case DT_ANDROID_RELRSZ:
            relr_sz = d->d_un.d_val;
            break;
        case DT_ANDROID_RELRENT:
            if (d->d_un.d_val != sizeof(ElfAddr)) {
                LOGE("Unsupported DT_ANDROID_RELRENT size %zu", static_cast<size_t>(d->d_un.d_val));
                return;
            }
            break;
        }
    }
    
    if (!dynsym || !dynstr) return;
    
    // RELR 处理
    if (relr && relr_sz) {
        size_t count = relr_sz / sizeof(ElfAddr);
        ElfAddr base_offset = 0;
        
        for (size_t i = 0; i < count; i++) {
            ElfAddr entry = relr[i];
            
            if ((entry & 1) == 0) {
                auto* target = reinterpret_cast<ElfAddr*>(load_bias + entry);
                *target += load_bias;
                base_offset = entry + sizeof(ElfAddr);
            } else {
                ElfAddr bitmap = entry >> 1;
                for (size_t bit = 0; bitmap && bit < 63; bit++, bitmap >>= 1) {
                    if (bitmap & 1) {
                        auto* target = reinterpret_cast<ElfAddr*>(
                            load_bias + base_offset + bit * sizeof(ElfAddr));
                        *target += load_bias;
                    }
                }
                base_offset += 63 * sizeof(ElfAddr);
            }
        }
    }
    
    // RELA 处理
    if (rela && rela_sz) {
        if (!rela_ent) rela_ent = sizeof(ElfRela);
        for (size_t i = 0; i < rela_sz / rela_ent; i++) {
            processRelocation(image, ELF_R_SYM(rela[i].r_info), ELF_R_TYPE(rela[i].r_info),
                            rela[i].r_offset, rela[i].r_addend, load_bias, dynsym, dynstr, true);
        }
    }
    
    // REL 处理
    if (rel && rel_sz) {
        if (!rel_ent) rel_ent = sizeof(ElfRel);
        for (size_t i = 0; i < rel_sz / rel_ent; i++) {
            processRelocation(image, ELF_R_SYM(rel[i].r_info), ELF_R_TYPE(rel[i].r_info),
                            rel[i].r_offset, 0, load_bias, dynsym, dynstr, false);
        }
    }
    
    // Android 压缩重定位
    if (android_reloc && android_reloc_sz > 4) {
        if (memcmp(android_reloc, "APS2", 4) != 0) {
            LOGE("Invalid Android relocation magic");
            return;
        }
        
        Sleb128Decoder dec(static_cast<const uint8_t*>(android_reloc) + 4, android_reloc_sz - 4);
        
        uint64_t num_relocs = dec.decodeUnsigned();
        ElfAddr r_offset = dec.decode();
        
        for (uint64_t i = 0; i < num_relocs; ) {
            uint64_t group_size = dec.decodeUnsigned();
            uint64_t group_flags = dec.decodeUnsigned();
            
            constexpr uint64_t GROUPED_BY_INFO = 1;
            constexpr uint64_t GROUPED_BY_OFFSET_DELTA = 2;
            constexpr uint64_t GROUPED_BY_ADDEND = 4;
            constexpr uint64_t HAS_ADDEND = 8;
            
            ElfAddr group_offset_delta = 0;
            uint32_t sym_idx = 0, type = 0;
            ElfAddr addend = 0;
            
            if (group_flags & GROUPED_BY_OFFSET_DELTA)
                group_offset_delta = dec.decode();
            
            if (group_flags & GROUPED_BY_INFO) {
                uint64_t r_info = dec.decodeUnsigned();
                sym_idx = ELF_R_SYM(r_info);
                type = ELF_R_TYPE(r_info);
            }
            
            if (is_android_rela && (group_flags & HAS_ADDEND) && (group_flags & GROUPED_BY_ADDEND))
                addend += dec.decode();
            else if (!is_android_rela && (group_flags & HAS_ADDEND))
                LOGE("REL relocations should not have addends");
            
            for (uint64_t j = 0; j < group_size; j++) {
                if (group_flags & GROUPED_BY_OFFSET_DELTA)
                    r_offset += group_offset_delta;
                else
                    r_offset += dec.decode();
                
                if (!(group_flags & GROUPED_BY_INFO)) {
                    uint64_t r_info = dec.decodeUnsigned();
                    sym_idx = ELF_R_SYM(r_info);
                    type = ELF_R_TYPE(r_info);
                }
                
                if (is_android_rela && (group_flags & HAS_ADDEND) && !(group_flags & GROUPED_BY_ADDEND))
                    addend += dec.decode();
                
                processRelocation(image, sym_idx, type, r_offset, addend, load_bias, dynsym, dynstr, is_android_rela);
            }
            
            i += group_size;
        }
    }
    
    // PLT 重定位
    if (jmprel && jmprel_sz) {
        if (pltrel_type == DT_RELA) {
            auto* r = static_cast<ElfRela*>(jmprel);
            for (size_t i = 0; i < jmprel_sz / sizeof(ElfRela); i++) {
                processRelocation(image, ELF_R_SYM(r[i].r_info), ELF_R_TYPE(r[i].r_info),
                                r[i].r_offset, r[i].r_addend, load_bias, dynsym, dynstr, true);
            }
        } else {
            auto* r = static_cast<ElfRel*>(jmprel);
            for (size_t i = 0; i < jmprel_sz / sizeof(ElfRel); i++) {
                processRelocation(image, ELF_R_SYM(r[i].r_info), ELF_R_TYPE(r[i].r_info),
                                r[i].r_offset, 0, load_bias, dynsym, dynstr, false);
            }
        }
    }
}

void Linker::restoreProtections(ElfImage* image) {
    auto* header = image->header();
    auto* phdr = reinterpret_cast<ElfPhdr*>(
        reinterpret_cast<uintptr_t>(header) + header->e_phoff);

    // 找到所有可加载段的最小和最大地址
    uintptr_t min_addr = UINTPTR_MAX;
    uintptr_t max_addr = 0;
    for (int i = 0; i < header->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;

        uintptr_t seg_start = reinterpret_cast<uintptr_t>(image->base()) +
                              phdr[i].p_vaddr - image->bias();
        uintptr_t seg_end = seg_start + phdr[i].p_memsz;

        if (seg_start < min_addr) min_addr = seg_start;
        if (seg_end > max_addr) max_addr = seg_end;
    }

    if (min_addr >= max_addr) return;

    uintptr_t start_page = pageStart(min_addr);
    uintptr_t end_page = pageEnd(max_addr);
    size_t pg_size = pageSize();
    size_t num_pages = (end_page - start_page) / pg_size;

    if (num_pages == 0) return;

    // 分配逐页保护位图
    auto page_prots = std::make_unique<int[]>(num_pages);
    memset(page_prots.get(), 0, num_pages * sizeof(int));

    // 对每个段，将其保护位 OR 到覆盖的所有页上
    for (int i = 0; i < header->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;

        int prot = 0;
        if (phdr[i].p_flags & PF_R) prot |= PROT_READ;
        if (phdr[i].p_flags & PF_W) prot |= PROT_WRITE;
        if (phdr[i].p_flags & PF_X) prot |= PROT_EXEC;

        uintptr_t seg_start = reinterpret_cast<uintptr_t>(image->base()) +
                              phdr[i].p_vaddr - image->bias();
        uintptr_t seg_end = seg_start + phdr[i].p_memsz;
        uintptr_t cur_page = pageStart(seg_start);

        while (cur_page < pageEnd(seg_end)) {
            size_t idx = (cur_page - start_page) / pg_size;
            if (idx < num_pages) {
                page_prots[idx] |= prot;
            }
            cur_page += pg_size;
        }
    }

    // 逐页恢复保护
    for (size_t i = 0; i < num_pages; i++) {
        int prot = page_prots[i];
        if (prot == 0) continue;

        uintptr_t page_addr = start_page + i * pg_size;
        mprotect(reinterpret_cast<void*>(page_addr), pg_size, prot);

        if (prot & PROT_EXEC) {
            __builtin___clear_cache(reinterpret_cast<char*>(page_addr),
                                   reinterpret_cast<char*>(page_addr + pg_size));
        }
    }
}

void Linker::callConstructors(ElfImage* image) {
    if (image->initFunc()) {
        LOGD("Calling .init for %s", image->path().c_str());
        image->initFunc()();
    }
    
    if (image->initArray()) {
        LOGD("Calling .init_array for %s", image->path().c_str());
        for (size_t i = 0; i < image->initArrayCount(); i++) {
            image->initArray()[i](g_argc, g_argv, g_envp);
        }
    }
}

void Linker::callDestructors(ElfImage* image) {
    if (image->finiArray()) {
        for (size_t i = image->finiArrayCount(); i > 0; i--) {
            image->finiArray()[i - 1]();
        }
    }
    
    if (image->finiFunc()) {
        image->finiFunc()();
    }
}

bool Linker::link() {
    // 1. 加载依赖
    if (!loadDependencies()) {
        LOGE("Failed to load dependencies");
        return false;
    }
    
    // 2. 注册 TLS
    TlsManager::instance().registerSegment(main_image_.get());
    for (auto& dep : deps_) {
        TlsManager::instance().registerSegment(dep.image.get());
    }
    TlsManager::instance().bumpGeneration();
    
    // 3. 设置内存可写
    auto makeWritable = [](ElfImage* img) {
        auto* header = img->header();
        auto* phdr = reinterpret_cast<ElfPhdr*>(
            reinterpret_cast<uintptr_t>(header) + header->e_phoff);
        
        for (int i = 0; i < header->e_phnum; i++) {
            if (phdr[i].p_type != PT_LOAD) continue;
            if (phdr[i].p_flags & PF_W) continue;
            
            auto start = pageStart(reinterpret_cast<uintptr_t>(img->base()) + 
                                  phdr[i].p_vaddr - img->bias());
            auto len = pageEnd(phdr[i].p_vaddr + phdr[i].p_memsz) - pageStart(phdr[i].p_vaddr);
            
            int prot = PROT_READ | PROT_WRITE;
            if (phdr[i].p_flags & PF_X) prot |= PROT_EXEC;
            
            mprotect(reinterpret_cast<void*>(start), len, prot);
        }
    };
    
    makeWritable(main_image_.get());
    for (auto& dep : deps_) {
        if (dep.is_manual_load) makeWritable(dep.image.get());
    }
    
    // 4. 处理重定位
    processRelocations(main_image_.get());
    for (auto& dep : deps_) {
        if (dep.is_manual_load) processRelocations(dep.image.get());
    }
    
    // 5. 恢复内存保护
    restoreProtections(main_image_.get());
    for (auto& dep : deps_) {
        if (dep.is_manual_load) restoreProtections(dep.image.get());
    }
    
    // 6. 注册回溯支持
    BacktraceManager::instance().registerLibrary(main_image_.get());
    BacktraceManager::instance().registerEhFrame(main_image_.get());
    
    for (auto& dep : deps_) {
        if (dep.is_manual_load) {
            BacktraceManager::instance().registerLibrary(dep.image.get());
            BacktraceManager::instance().registerEhFrame(dep.image.get());
        }
    }
    
    // 7. 调用构造函数（先依赖后主库）
    for (auto& dep : deps_) {
        if (dep.is_manual_load) callConstructors(dep.image.get());
    }
    callConstructors(main_image_.get());
    
    is_linked_ = true;
    return true;
}

} // namespace soloader
