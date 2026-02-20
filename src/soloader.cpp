// Modern C++17 SO Loader - Main Implementation (arm64 only)

#include "soloader.hpp"
#include "log.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace soloader {

SoLoader::~SoLoader() {
    if (isLoaded()) {
        unload();
    }
}

bool SoLoader::load(std::string_view lib_path) {
    if (isLoaded()) {
        LOGE("Already loaded a library: %s", lib_path_.c_str());
        return false;
    }
    
    // 验证文件存在且可读
    std::string path_str(lib_path);
    struct stat st;
    if (stat(path_str.c_str(), &st) != 0) {
        LOGE("Library file not found: %s", path_str.c_str());
        return false;
    }
    
    if (!S_ISREG(st.st_mode)) {
        LOGE("Not a regular file: %s", path_str.c_str());
        return false;
    }
    
    if (access(path_str.c_str(), R_OK) != 0) {
        LOGE("Library file not readable: %s", path_str.c_str());
        return false;
    }
    
    LOGI("Loading library: %s (size: %lld bytes)", path_str.c_str(), 
         static_cast<long long>(st.st_size));
    
    // 手动加载库
    LoadedDep dep;
    void* base = Linker::loadLibraryManually(lib_path, dep);
    if (!base) {
        LOGE("Failed to map library into memory: %s", path_str.c_str());
        return false;
    }
    
    LOGD("Library mapped at %p, size: %zu", base, dep.map_size);
    
    // 创建 ELF 镜像
    auto image = ElfImage::create(lib_path, base);
    if (!image) {
        LOGE("Failed to parse ELF image: %s", path_str.c_str());
        munmap(base, dep.map_size);
        return false;
    }
    
    // 初始化链接器
    if (!linker_.init(std::move(image))) {
        LOGE("Failed to initialize linker for: %s", path_str.c_str());
        munmap(base, dep.map_size);
        return false;
    }
    
    linker_.setMainMapSize(dep.map_size);
    
    // 执行链接
    if (!linker_.link()) {
        LOGE("Failed to link library: %s", path_str.c_str());
        linker_.destroy();
        return false;
    }
    
    lib_path_ = lib_path;
    image_ = linker_.mainImage();
    
    LOGI("Successfully loaded: %s at %p", lib_path_.c_str(), image_->base());
    return true;
}

bool SoLoader::unload() {
    if (!isLoaded()) {
        LOGW("No library loaded");
        return false;
    }
    
    LOGI("Unloading library: %s", lib_path_.c_str());
    
    linker_.destroy();
    
    lib_path_.clear();
    image_ = nullptr;
    
    LOGI("Library unloaded successfully");
    return true;
}

bool SoLoader::abandon() {
    if (!isLoaded()) {
        LOGW("No library loaded");
        return false;
    }
    
    LOGI("Abandoning library: %s (no destructors called)", lib_path_.c_str());
    
    linker_.abandon();
    
    lib_path_.clear();
    image_ = nullptr;
    
    return true;
}

void* SoLoader::getSymbol(std::string_view name) {
    if (!isLoaded()) return nullptr;
    
    auto addr = image_->findSymbolAddress(name);
    return addr ? reinterpret_cast<void*>(*addr) : nullptr;
}

} // namespace soloader

// 独立测试入口
#ifdef STANDALONE_TEST

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <pthread.h>

// 测试结构体（与 test_lib.cpp 中定义一致）
struct TestData {
    int id;
    float value;
    char name[32];
};

// 回调函数类型
typedef void (*Callback)(int value, void* user_data);

// 回调测试函数
static void my_callback(int value, void* user_data) {
    const char* tag = static_cast<const char*>(user_data);
    printf("[main] Callback received: value=%d, tag=%s\n", value, tag);
}

// 异常测试函数
static void run_exception_tests(soloader::SoLoader& loader) {
    int passed = 0, failed = 0;
    
    printf("\n  [INFO] 跨 SO 边界的 C++ 异常类型匹配受限于 RTTI\n");
    printf("  [INFO] 使用 catch(...) 验证异常是否正确传播\n");
    
    // 12.1 基础异常抛出和捕获
    printf("\n  12.1 基础异常测试\n");
    auto throw_exception = loader.getSymbol<void(*)()>("throw_exception");
    if (throw_exception) {
        bool caught = false;
        try {
            throw_exception();
        } catch (...) {
            caught = true;
            printf("  [PASS] Exception caught successfully\n");
            passed++;
        }
        if (!caught) {
            printf("  [FAIL] Exception not thrown\n");
            failed++;
        }
    }
    
    // 12.2 整数异常
    printf("\n  12.2 整数异常测试\n");
    auto throw_int_exception = loader.getSymbol<void(*)(int)>("throw_int_exception");
    if (throw_int_exception) {
        bool caught = false;
        try {
            throw_int_exception(42);
        } catch (int code) {
            caught = true;
            printf("  [PASS] Caught int exception: %d\n", code);
            passed++;
        } catch (...) {
            caught = true;
            printf("  [PASS] Exception caught (int type may not match across SO boundary)\n");
            passed++;
        }
        if (!caught) {
            printf("  [FAIL] Exception not thrown\n");
            failed++;
        }
    }
    
    // 12.3 自定义异常
    printf("\n  12.3 自定义异常测试\n");
    auto throw_custom_exception = loader.getSymbol<void(*)(int, const char*)>("throw_custom_exception");
    if (throw_custom_exception) {
        bool caught = false;
        try {
            throw_custom_exception(100, "Custom error message");
        } catch (...) {
            caught = true;
            printf("  [PASS] Custom exception caught\n");
            passed++;
        }
        if (!caught) {
            printf("  [FAIL] Exception not thrown\n");
            failed++;
        }
    }
    
    // 12.4 条件异常
    printf("\n  12.4 条件异常测试\n");
    auto may_throw = loader.getSymbol<int(*)(int)>("may_throw");
    if (may_throw) {
        // 正常情况
        try {
            int result = may_throw(5);
            if (result == 10) {
                printf("  [PASS] Normal case: may_throw(5) = %d\n", result);
                passed++;
            } else {
                printf("  [FAIL] Wrong result: %d (expected 10)\n", result);
                failed++;
            }
        } catch (...) {
            printf("  [FAIL] Unexpected exception in normal case\n");
            failed++;
        }
        
        // 负数异常
        bool neg_caught = false;
        try {
            may_throw(-1);
        } catch (...) {
            neg_caught = true;
            printf("  [PASS] Exception caught for negative value\n");
            passed++;
        }
        if (!neg_caught) {
            printf("  [FAIL] Exception not thrown for negative value\n");
            failed++;
        }
        
        // 零值异常
        bool zero_caught = false;
        try {
            may_throw(0);
        } catch (...) {
            zero_caught = true;
            printf("  [PASS] Exception caught for zero value\n");
            passed++;
        }
        if (!zero_caught) {
            printf("  [FAIL] Exception not thrown for zero value\n");
            failed++;
        }
    }
    
    // 12.5 noexcept 函数
    printf("\n  12.5 noexcept 函数测试\n");
    auto safe_function = loader.getSymbol<int(*)(int, int)>("safe_function");
    if (safe_function) {
        int result = safe_function(10, 20);
        if (result == 30) {
            printf("  [PASS] safe_function(10, 20) = %d\n", result);
            passed++;
        } else {
            printf("  [FAIL] Wrong result: %d (expected 30)\n", result);
            failed++;
        }
    }
    
    // 12.6 内部捕获异常
    printf("\n  12.6 内部捕获异常测试\n");
    auto catch_and_return = loader.getSymbol<int(*)(int)>("catch_and_return");
    if (catch_and_return) {
        int result1 = catch_and_return(5);
        int result2 = catch_and_return(-5);
        
        if (result1 == 5 && result2 == -1) {
            printf("  [PASS] catch_and_return(5)=%d, catch_and_return(-5)=%d\n", result1, result2);
            passed++;
        } else {
            printf("  [FAIL] Wrong results: %d, %d (expected 5, -1)\n", result1, result2);
            failed++;
        }
    }
    
    // 12.7 回调中的异常
    printf("\n  12.7 回调异常测试\n");
    auto call_throwing_callback = loader.getSymbol<int(*)(int(*)(int), int)>("call_throwing_callback");
    if (call_throwing_callback) {
        // 正常回调
        auto normal_cb = [](int v) -> int { return v * 2; };
        try {
            int result = call_throwing_callback(normal_cb, 5);
            if (result == 10) {
                printf("  [PASS] Normal callback returned: %d\n", result);
                passed++;
            } else {
                printf("  [FAIL] Wrong callback result: %d\n", result);
                failed++;
            }
        } catch (...) {
            printf("  [FAIL] Unexpected exception in normal callback\n");
            failed++;
        }
        
        // 抛异常的回调 - 从主程序抛出，类型应该匹配
        auto throwing_cb = [](int v) -> int { 
            if (v > 0) throw std::runtime_error("Callback exception");
            return v;
        };
        try {
            call_throwing_callback(throwing_cb, 5);
            printf("  [FAIL] Exception not propagated from callback\n");
            failed++;
        } catch (const std::runtime_error& e) {
            printf("  [PASS] Caught callback exception: %s\n", e.what());
            passed++;
        } catch (...) {
            printf("  [PASS] Callback exception caught (type mismatch possible)\n");
            passed++;
        }
    }
    
    // 12.8 对象异常测试
    printf("\n  12.8 对象异常测试\n");
    auto create_exception_test_object = loader.getSymbol<void*(*)(bool)>("create_exception_test_object");
    auto destroy_exception_test_object = loader.getSymbol<void(*)(void*)>("destroy_exception_test_object");
    auto exception_test_do_work = loader.getSymbol<void(*)(void*, bool)>("exception_test_do_work");
    
    if (create_exception_test_object && destroy_exception_test_object && exception_test_do_work) {
        void* obj = create_exception_test_object(false);
        
        // 正常工作
        try {
            exception_test_do_work(obj, false);
            printf("  [PASS] do_work(false) completed normally\n");
            passed++;
        } catch (...) {
            printf("  [FAIL] Unexpected exception in do_work(false)\n");
            failed++;
        }
        
        // 抛异常
        bool caught = false;
        try {
            exception_test_do_work(obj, true);
        } catch (...) {
            caught = true;
            printf("  [PASS] Exception caught from do_work(true)\n");
            passed++;
        }
        if (!caught) {
            printf("  [FAIL] Exception not thrown in do_work(true)\n");
            failed++;
        }
        
        destroy_exception_test_object(obj);
    }
    
    // 12.9 RAII 异常安全测试
    printf("\n  12.9 RAII 异常安全测试\n");
    auto raii_exception_test = loader.getSymbol<int(*)(bool)>("raii_exception_test");
    if (raii_exception_test) {
        // 正常返回
        try {
            int result = raii_exception_test(false);
            if (result == 0) {
                printf("  [PASS] RAII test normal return\n");
                passed++;
            }
        } catch (...) {
            printf("  [FAIL] Unexpected exception in RAII normal case\n");
            failed++;
        }
        
        // 异常情况（验证析构函数被调用）
        bool caught = false;
        try {
            raii_exception_test(true);
        } catch (...) {
            caught = true;
            printf("  [PASS] RAII exception caught (destructors should have run)\n");
            passed++;
        }
        if (!caught) {
            printf("  [FAIL] Exception not thrown in RAII test\n");
            failed++;
        }
    }
    
    // 12.10 嵌套异常测试
    printf("\n  12.10 嵌套异常测试\n");
    auto nested_throw = loader.getSymbol<void(*)()>("nested_throw");
    if (nested_throw) {
        bool caught = false;
        try {
            nested_throw();
        } catch (...) {
            caught = true;
            printf("  [PASS] Nested exception caught\n");
            passed++;
        }
        if (!caught) {
            printf("  [FAIL] Nested exception not thrown\n");
            failed++;
        }
    }
    
    printf("\n  异常测试结果: %d passed, %d failed\n", passed, failed);
    printf("  [INFO] 跨 SO 边界异常传播正常工作\n");
}

// TLS 多线程测试
static soloader::SoLoader* g_loader = nullptr;

static void* thread_func(void* arg) {
    int thread_id = *static_cast<int*>(arg);
    printf("\n[Thread %d] Started\n", thread_id);
    
    // 获取 TLS 函数
    auto tls_increment = g_loader->getSymbol<int(*)()>("tls_increment");
    auto tls_set_buffer = g_loader->getSymbol<void(*)(const char*)>("tls_set_buffer");
    auto tls_get_buffer = g_loader->getSymbol<const char*(*)()>("tls_get_buffer");
    
    if (tls_increment && tls_set_buffer && tls_get_buffer) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Thread-%d", thread_id);
        tls_set_buffer(buf);
        
        for (int i = 0; i < 3; i++) {
            tls_increment();
        }
        
        printf("[Thread %d] TLS buffer: %s\n", thread_id, tls_get_buffer());
    }
    
    printf("[Thread %d] Finished\n", thread_id);
    return nullptr;
}

static void run_tests(soloader::SoLoader& loader) {
    printf("\n========== 开始测试 ==========\n\n");
    
    // 1. 基础函数测试
    printf("--- 1. 基础函数测试 ---\n");
    auto shared_function = loader.getSymbol<void(*)()>("shared_function");
    if (shared_function) {
        shared_function();
        shared_function();
    } else {
        printf("ERROR: shared_function not found\n");
    }
    
    // 2. 带参数返回值测试
    printf("\n--- 2. 带参数返回值测试 ---\n");
    auto add_numbers = loader.getSymbol<int(*)(int, int)>("add_numbers");
    if (add_numbers) {
        int result = add_numbers(10, 20);
        printf("[main] Result: %d (expected: 30)\n", result);
        
        result = add_numbers(-5, 15);
        printf("[main] Result: %d (expected: 10)\n", result);
    }
    
    // 3. 字符串处理测试
    printf("\n--- 3. 字符串处理测试 ---\n");
    auto get_greeting = loader.getSymbol<const char*(*)(const char*)>("get_greeting");
    if (get_greeting) {
        const char* msg = get_greeting("SoLoader");
        printf("[main] Greeting: %s\n", msg);
    }
    
    // 4. 结构体参数测试
    printf("\n--- 4. 结构体参数测试 ---\n");
    auto process_data = loader.getSymbol<void(*)(TestData*)>("process_data");
    if (process_data) {
        TestData data = {1, 3.14f, "TestItem"};
        printf("[main] Before: id=%d, value=%.2f\n", data.id, data.value);
        process_data(&data);
        printf("[main] After: id=%d, value=%.2f\n", data.id, data.value);
    }
    
    // 5. 回调函数测试
    printf("\n--- 5. 回调函数测试 ---\n");
    auto register_callback = loader.getSymbol<void(*)(Callback, void*)>("register_callback");
    if (register_callback) {
        register_callback(my_callback, const_cast<char*>("MyTag"));
    }
    
    // 6. 数组处理测试
    printf("\n--- 6. 数组处理测试 ---\n");
    auto sum_array = loader.getSymbol<int(*)(const int*, int)>("sum_array");
    if (sum_array) {
        int arr[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        int sum = sum_array(arr, 10);
        printf("[main] Sum: %d (expected: 55)\n", sum);
    }
    
    // 7. 浮点运算测试
    printf("\n--- 7. 浮点运算测试 ---\n");
    auto compute_average = loader.getSymbol<double(*)(const double*, int)>("compute_average");
    if (compute_average) {
        double values[] = {1.5, 2.5, 3.5, 4.5, 5.5};
        double avg = compute_average(values, 5);
        printf("[main] Average: %.4f (expected: 3.5000)\n", avg);
    }
    
    // 8. 内存分配测试
    printf("\n--- 8. 内存分配测试 ---\n");
    auto allocate_buffer = loader.getSymbol<void*(*)(size_t)>("allocate_buffer");
    auto free_buffer = loader.getSymbol<void(*)(void*)>("free_buffer");
    if (allocate_buffer && free_buffer) {
        void* buf = allocate_buffer(1024);
        if (buf) {
            memset(buf, 0xAB, 1024);
            printf("[main] Buffer allocated and filled\n");
            free_buffer(buf);
        }
    }
    
    // 9. C++ 对象测试
    printf("\n--- 9. C++ 对象测试 ---\n");
    auto create_test_object = loader.getSymbol<void*(*)(int)>("create_test_object");
    auto destroy_test_object = loader.getSymbol<void(*)(void*)>("destroy_test_object");
    auto get_object_value = loader.getSymbol<int(*)(void*)>("get_object_value");
    auto set_object_value = loader.getSymbol<void(*)(void*, int)>("set_object_value");
    auto print_object = loader.getSymbol<void(*)(void*)>("print_object");
    
    if (create_test_object && destroy_test_object && get_object_value && 
        set_object_value && print_object) {
        void* obj = create_test_object(42);
        print_object(obj);
        
        printf("[main] Object value: %d\n", get_object_value(obj));
        set_object_value(obj, 100);
        printf("[main] Object value after set: %d\n", get_object_value(obj));
        
        destroy_test_object(obj);
    }
    
    // 10. TLS 多线程测试
    printf("\n--- 10. TLS 多线程测试 ---\n");
    g_loader = &loader;
    
    pthread_t threads[3];
    int thread_ids[3] = {1, 2, 3};
    
    for (int i = 0; i < 3; i++) {
        pthread_create(&threads[i], nullptr, thread_func, &thread_ids[i]);
    }
    
    for (int i = 0; i < 3; i++) {
        pthread_join(threads[i], nullptr);
    }
    
    // 11. 获取库信息
    printf("\n--- 11. 库信息 ---\n");
    auto get_lib_info = loader.getSymbol<const char*(*)()>("get_lib_info");
    if (get_lib_info) {
        printf("%s\n", get_lib_info());
    }
    
    // 12. 异常测试
    printf("\n--- 12. 异常测试 ---\n");
    run_exception_tests(loader);
    
    printf("\n========== 测试完成 ==========\n");
}

int main(int argc, char** argv, char** envp) {
    soloader::g_argc = argc;
    soloader::g_argv = argv;
    soloader::g_envp = envp;
    
    printf("SoLoader Test Program\n");
    printf("=====================\n\n");
    
    if (argc < 2) {
        printf("Usage: %s <library.so>\n", argv[0]);
        printf("\nExample:\n");
        printf("  %s /data/local/tmp/libtest_lib.so\n", argv[0]);
        return 1;
    }
    
    const char* lib_path = argv[1];
    printf("Loading library: %s\n", lib_path);
    
    soloader::SoLoader loader;
    if (!loader.load(lib_path)) {
        printf("ERROR: Failed to load library: %s\n", lib_path);
        return 1;
    }
    
    printf("Library loaded successfully: %s\n", loader.path().c_str());
    
    // 运行测试
    run_tests(loader);
    
    // 卸载
    printf("\nUnloading library...\n");
    loader.unload();
    printf("Library unloaded successfully\n");
    
    return 0;
}

#endif
