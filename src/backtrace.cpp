// Modern C++17 SO Loader - Backtrace Support (arm64 only)

#include "backtrace.hpp"
#include "log.hpp"
#include <cstring>
#include <mutex>
#include <dlfcn.h>

namespace soloader {

// GCC 运行时函数
extern "C" {
    void __register_frame(void*) __attribute__((weak));
    void __deregister_frame(void*) __attribute__((weak));
}

// 缓存原始 dl 函数指针
static int (*s_orig_dl_iterate_phdr)(int(*)(dl_phdr_info*, size_t, void*), void*) = nullptr;
static int (*s_orig_dladdr)(const void*, Dl_info*) = nullptr;
static bool s_dl_funcs_initialized = false;
static std::mutex s_dl_init_mutex;

static void initDlFunctions() {
    std::lock_guard<std::mutex> lock(s_dl_init_mutex);
    if (s_dl_funcs_initialized) return;
    
    // 直接使用 dlsym 获取原始函数
    s_orig_dl_iterate_phdr = reinterpret_cast<decltype(s_orig_dl_iterate_phdr)>(
        dlsym(RTLD_DEFAULT, "dl_iterate_phdr"));
    s_orig_dladdr = reinterpret_cast<decltype(s_orig_dladdr)>(
        dlsym(RTLD_DEFAULT, "dladdr"));
    
    s_dl_funcs_initialized = true;
    
    if (!s_orig_dl_iterate_phdr) {
        LOGW("Failed to find dl_iterate_phdr");
    }
    if (!s_orig_dladdr) {
        LOGW("Failed to find dladdr");
    }
}

// DWARF 编码类型
enum DwarfEncoding : uint8_t {
    DW_EH_PE_absptr   = 0x00,
    DW_EH_PE_uleb128  = 0x01,
    DW_EH_PE_udata2   = 0x02,
    DW_EH_PE_udata4   = 0x03,
    DW_EH_PE_udata8   = 0x04,
    DW_EH_PE_sleb128  = 0x09,
    DW_EH_PE_sdata2   = 0x0a,
    DW_EH_PE_sdata4   = 0x0b,
    DW_EH_PE_sdata8   = 0x0c,
    DW_EH_PE_pcrel    = 0x10,
    DW_EH_PE_textrel  = 0x20,
    DW_EH_PE_datarel  = 0x30,
    DW_EH_PE_funcrel  = 0x40,
    DW_EH_PE_aligned  = 0x50,
    DW_EH_PE_omit     = 0xff
};

// 解码 DWARF 指针
static const uint8_t* decodePointer(const uint8_t* p, uint8_t encoding, 
                                     uintptr_t base, uintptr_t* result) {
    if (encoding == DW_EH_PE_omit) {
        *result = 0;
        return p;
    }
    
    uintptr_t value = 0;
    const uint8_t* start = p;
    
    // 解码值
    switch (encoding & 0x0f) {
    case DW_EH_PE_absptr:
        memcpy(&value, p, sizeof(uintptr_t));
        p += sizeof(uintptr_t);
        break;
    case DW_EH_PE_udata2:
        value = *reinterpret_cast<const uint16_t*>(p);
        p += 2;
        break;
    case DW_EH_PE_udata4:
        value = *reinterpret_cast<const uint32_t*>(p);
        p += 4;
        break;
    case DW_EH_PE_udata8:
        value = *reinterpret_cast<const uint64_t*>(p);
        p += 8;
        break;
    case DW_EH_PE_sdata2:
        value = static_cast<uintptr_t>(*reinterpret_cast<const int16_t*>(p));
        p += 2;
        break;
    case DW_EH_PE_sdata4:
        value = static_cast<uintptr_t>(*reinterpret_cast<const int32_t*>(p));
        p += 4;
        break;
    case DW_EH_PE_sdata8:
        value = static_cast<uintptr_t>(*reinterpret_cast<const int64_t*>(p));
        p += 8;
        break;
    default:
        LOGW("Unsupported DWARF encoding: 0x%02x", encoding);
        *result = 0;
        return p;
    }
    
    // 应用相对偏移
    if (value != 0) {
        switch (encoding & 0x70) {
        case DW_EH_PE_pcrel:
            value += reinterpret_cast<uintptr_t>(start);
            break;
        case DW_EH_PE_datarel:
            value += base;
            break;
        }
    }
    
    *result = value;
    return p;
}

// 从 eh_frame_hdr 解析 eh_frame 地址
static const uint8_t* parseEhFrameHdr(const uint8_t* hdr, size_t hdr_size, uintptr_t base) {
    if (!hdr || hdr_size < 4) return nullptr;
    
    // eh_frame_hdr 格式:
    // version (1 byte) - 必须是 1
    // eh_frame_ptr_enc (1 byte)
    // fde_count_enc (1 byte)
    // table_enc (1 byte)
    // eh_frame_ptr (encoded)
    // fde_count (encoded)
    // binary search table...
    
    if (hdr[0] != 1) {
        LOGW("Unsupported eh_frame_hdr version: %u", hdr[0]);
        return nullptr;
    }
    
    uint8_t eh_frame_ptr_enc = hdr[1];
    // uint8_t fde_count_enc = hdr[2];
    // uint8_t table_enc = hdr[3];
    
    if (eh_frame_ptr_enc == DW_EH_PE_omit) {
        return nullptr;
    }
    
    const uint8_t* p = hdr + 4;
    uintptr_t eh_frame_ptr = 0;
    
    decodePointer(p, eh_frame_ptr_enc, base, &eh_frame_ptr);
    
    return reinterpret_cast<const uint8_t*>(eh_frame_ptr);
}

BacktraceManager& BacktraceManager::instance() {
    static BacktraceManager inst;
    return inst;
}

bool BacktraceManager::registerLibrary(ElfImage* image) {
    std::lock_guard lock(mutex_);
    
    size_t slot = MAX_CUSTOM_LIBS;
    for (size_t i = 0; i < MAX_CUSTOM_LIBS; i++) {
        if (!libs_[i].in_use) {
            slot = i;
            break;
        }
    }
    
    if (slot >= MAX_CUSTOM_LIBS) {
        LOGE("No slots for library registration");
        return false;
    }
    
    auto& lib = libs_[slot];
    auto* header = image->header();
    
    // 复制程序头
    size_t phdr_size = header->e_phnum * sizeof(ElfPhdr);
    lib.phdr_copy = static_cast<ElfPhdr*>(malloc(phdr_size));
    if (!lib.phdr_copy) {
        LOGE("Failed to allocate phdr copy");
        return false;
    }
    
    auto* orig_phdr = reinterpret_cast<ElfPhdr*>(
        reinterpret_cast<uintptr_t>(header) + header->e_phoff);
    memcpy(lib.phdr_copy, orig_phdr, phdr_size);
    
    lib.phdr_info.dlpi_addr = reinterpret_cast<ElfW(Addr)>(image->base()) - image->bias();
    lib.phdr_info.dlpi_name = image->path().c_str();
    lib.phdr_info.dlpi_phdr = reinterpret_cast<const ElfW(Phdr)*>(lib.phdr_copy);
    lib.phdr_info.dlpi_phnum = header->e_phnum;
    lib.phdr_info.dlpi_adds = 1;
    lib.phdr_info.dlpi_subs = 0;
    
    if (image->tlsSegment()) {
        lib.phdr_info.dlpi_tls_modid = image->tlsModuleId();
    }
    
    lib.image = image;
    lib.in_use = true;
    lib.eh_frame_registered = nullptr;
    
    LOGD("Registered library for backtrace: %s", image->path().c_str());
    return true;
}

bool BacktraceManager::unregisterLibrary(ElfImage* image) {
    std::lock_guard lock(mutex_);
    
    for (size_t i = 0; i < MAX_CUSTOM_LIBS; i++) {
        if (libs_[i].in_use && libs_[i].image == image) {
            if (libs_[i].eh_frame_registered && __deregister_frame) {
                __deregister_frame(libs_[i].eh_frame_registered);
            }
            
            free(libs_[i].phdr_copy);
            libs_[i] = {};
            
            LOGD("Unregistered library: %s", image->path().c_str());
            return true;
        }
    }
    
    return false;
}

void BacktraceManager::registerEhFrame(ElfImage* image) {
    if (!__register_frame) return;
    
    const uint8_t* eh_frame = image->ehFrame();
    
    if (!eh_frame) {
        // 尝试从 eh_frame_hdr 解析
        auto* hdr = image->ehFrameHdr();
        if (hdr) {
            uintptr_t base = reinterpret_cast<uintptr_t>(image->base()) - image->bias();
            eh_frame = parseEhFrameHdr(hdr, image->ehFrameHdrSize(), base);
        }
    }
    
    if (!eh_frame) {
        LOGD("No eh_frame found for %s", image->path().c_str());
        return;
    }
    
    __register_frame(const_cast<uint8_t*>(eh_frame));
    
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < MAX_CUSTOM_LIBS; i++) {
        if (libs_[i].in_use && libs_[i].image == image) {
            libs_[i].eh_frame_registered = const_cast<uint8_t*>(eh_frame);
            break;
        }
    }
    
    LOGD("Registered eh_frame for %s at %p", image->path().c_str(), eh_frame);
}

void BacktraceManager::unregisterEhFrame(ElfImage* image) {
    if (!__deregister_frame) return;
    
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < MAX_CUSTOM_LIBS; i++) {
        if (libs_[i].in_use && libs_[i].image == image && libs_[i].eh_frame_registered) {
            __deregister_frame(libs_[i].eh_frame_registered);
            libs_[i].eh_frame_registered = nullptr;
            break;
        }
    }
}

int BacktraceManager::customDlIteratePhdr(int (*callback)(dl_phdr_info*, size_t, void*), void* data) {
    // 确保 dl 函数已初始化
    initDlFunctions();
    
    int result = 0;
    
    // 调用原始函数
    if (s_orig_dl_iterate_phdr) {
        result = s_orig_dl_iterate_phdr(callback, data);
        if (result != 0) return result;
    }
    
    // 遍历自定义库
    auto& mgr = instance();
    std::lock_guard lock(mgr.mutex_);
    
    for (size_t i = 0; i < MAX_CUSTOM_LIBS; i++) {
        if (!mgr.libs_[i].in_use) continue;
        
        result = callback(&mgr.libs_[i].phdr_info, sizeof(dl_phdr_info), data);
        if (result != 0) break;
    }
    
    return result;
}

int BacktraceManager::customDladdr(const void* addr, Dl_info* info) {
    // 确保 dl 函数已初始化
    initDlFunctions();
    
    // 先调用原始函数
    if (s_orig_dladdr && s_orig_dladdr(addr, info)) {
        return 1;
    }
    
    // 在自定义库中查找
    auto& mgr = instance();
    std::lock_guard lock(mgr.mutex_);
    
    for (size_t i = 0; i < MAX_CUSTOM_LIBS; i++) {
        if (!mgr.libs_[i].in_use) continue;
        
        auto& lib = mgr.libs_[i];
        
        // 检查地址是否在库范围内
        for (size_t j = 0; j < lib.phdr_info.dlpi_phnum; j++) {
            auto* phdr = &lib.phdr_info.dlpi_phdr[j];
            if (phdr->p_type != PT_LOAD) continue;
            
            auto start = lib.phdr_info.dlpi_addr + phdr->p_vaddr;
            auto end = start + phdr->p_memsz;
            
            if (reinterpret_cast<uintptr_t>(addr) >= start &&
                reinterpret_cast<uintptr_t>(addr) < end) {
                
                info->dli_fname = lib.phdr_info.dlpi_name;
                info->dli_fbase = reinterpret_cast<void*>(lib.phdr_info.dlpi_addr);
                
                auto sym = lib.image->getSymbolAt(reinterpret_cast<uintptr_t>(addr));
                if (sym.valid()) {
                    info->dli_sname = sym.name.data();
                    info->dli_saddr = reinterpret_cast<void*>(sym.address);
                } else {
                    info->dli_sname = nullptr;
                    info->dli_saddr = nullptr;
                }
                
                return 1;
            }
        }
    }
    
    return 0;
}

} // namespace soloader
