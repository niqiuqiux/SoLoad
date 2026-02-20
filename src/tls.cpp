// Modern C++17 SO Loader - TLS Implementation (arm64 only)

#include "tls.hpp"
#include "linker.hpp"
#include "log.hpp"
#include <pthread.h>
#include <cstring>
#include <cstdlib>
#include <atomic>

namespace soloader {

static pthread_key_t g_tls_key;
static pthread_once_t g_tls_once = PTHREAD_ONCE_INIT;
static std::atomic<size_t> g_tls_block_count{0};

// TLS 块析构函数 - 线程退出时调用
static void tlsBlockDestructor(void* block) {
    if (block) {
        free(block);
        g_tls_block_count.fetch_sub(1, std::memory_order_relaxed);
        LOGD("TLS block freed, remaining: %zu", g_tls_block_count.load());
    }
}

static void tlsKeyInit() {
    int ret = pthread_key_create(&g_tls_key, tlsBlockDestructor);
    if (ret != 0) {
        LOGE("Failed to create TLS key: %d", ret);
    }
}

TlsManager& TlsManager::instance() {
    static TlsManager inst;
    return inst;
}

TlsManager::TlsManager() {
    pthread_once(&g_tls_once, tlsKeyInit);
}

bool TlsManager::registerSegment(ElfImage* image) {
    if (!image->tlsSegment()) return true;
    
    size_t mod_id = 0;
    for (size_t i = 1; i < MAX_TLS_MODULES; i++) {
        if (modules_[i].module_id == 0) {
            mod_id = i;
            break;
        }
    }
    
    if (mod_id == 0) {
        LOGE("TLS module overflow");
        return false;
    }
    
    auto* seg = image->tlsSegment();
    auto& m = modules_[mod_id];
    
    m.module_id = mod_id;
    m.align = seg->p_align ? seg->p_align : 1;
    m.memsz = seg->p_memsz;
    m.filesz = seg->p_filesz;
    m.init_image = reinterpret_cast<const void*>(
        reinterpret_cast<uintptr_t>(image->base()) + seg->p_vaddr - image->bias());
    m.owner = image;
    
    // 计算偏移
    static_size_ = (static_size_ + m.align - 1) & ~(m.align - 1);
    m.offset = static_size_;
    static_size_ += m.memsz;
    
    if (m.align > static_align_max_) static_align_max_ = m.align;
    
    image->setTlsModuleId(mod_id);
    
    LOGD("Registered TLS module %zu for %s", mod_id, image->path().c_str());
    return true;
}

void TlsManager::unregisterSegment(ElfImage* image) {
    for (size_t i = 1; i < MAX_TLS_MODULES; i++) {
        if (modules_[i].owner == image) {
            modules_[i] = {};
            break;
        }
    }
}

void* TlsManager::allocateBlock() {
    size_t align = static_align_max_ ? static_align_max_ : sizeof(void*);
    // 限制对齐值不超过页大小（与原始实现一致）
    size_t pg_size = pageSize();
    if (align > pg_size) align = pg_size;
    size_t total = static_size_ + align;
    
    if (total == 0) {
        LOGW("TLS block size is 0, using minimum");
        total = sizeof(void*);
    }
    
    void* block = nullptr;
    if (posix_memalign(&block, align, total) != 0) {
        LOGE("Failed to allocate TLS block of %zu bytes", total);
        return nullptr;
    }
    
    memset(block, 0, total);
    
    // 复制初始化数据
    for (size_t i = 1; i < MAX_TLS_MODULES; i++) {
        auto& m = modules_[i];
        if (m.owner && m.init_image && m.filesz > 0) {
            if (m.offset + m.filesz <= total) {
                memcpy(static_cast<char*>(block) + m.offset, m.init_image, m.filesz);
            } else {
                LOGE("TLS module %zu offset out of bounds", i);
            }
        }
    }
    
    pthread_setspecific(g_tls_key, block);
    g_tls_block_count.fetch_add(1, std::memory_order_relaxed);
    
    LOGD("Allocated TLS block: %p, size: %zu, total blocks: %zu", 
         block, total, g_tls_block_count.load());
    
    return block;
}

void* TlsManager::getBlockForThread() {
    pthread_once(&g_tls_once, tlsKeyInit);
    
    void* block = pthread_getspecific(g_tls_key);
    if (!block) {
        block = allocateBlock();
    }
    return block;
}

void* TlsManager::getAddress(TlsIndex* ti) {
    void* block = getBlockForThread();
    if (!block) return nullptr;
    
    if (!ti) return block;
    
    size_t mod_id = ti->module;
    if (mod_id == 0 || mod_id >= MAX_TLS_MODULES) {
        LOGE("TLS module ID out of range: %zu (max: %zu)", mod_id, MAX_TLS_MODULES - 1);
        return nullptr;
    }
    
    if (modules_[mod_id].module_id == 0) {
        LOGE("TLS module %zu not registered", mod_id);
        return nullptr;
    }
    
    // 边界检查
    size_t offset = modules_[mod_id].offset + ti->offset;
    if (offset >= static_size_) {
        LOGE("TLS offset out of bounds: %zu >= %zu", offset, static_size_);
        return nullptr;
    }
    
    return static_cast<uint8_t*>(block) + offset;
}

TlsIndex* TlsManager::allocateIndex(ElfImage* image, ElfSym* sym, ElfAddr addend) {
    auto* ti = new TlsIndex;
    ti->module = image->tlsModuleId();
    ti->offset = sym->st_value + addend;
    return ti;
}

extern "C" void* __tls_get_addr(TlsIndex* ti) {
    return TlsManager::instance().getAddress(ti);
}

} // namespace soloader
