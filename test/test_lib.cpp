// 测试用 SO 库 - 用于验证 SoLoader 功能
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <stdexcept>
#include <exception>

// ============== 全局变量测试 ==============
static int g_init_count = 0;
static int g_call_count = 0;
static const char* g_lib_name = "test_lib.so";

// ============== TLS 变量测试 ==============
static __thread int tls_counter = 0;
static __thread char tls_buffer[64] = {0};

// ============== 构造/析构函数测试 ==============
__attribute__((constructor))
static void lib_init() {
    g_init_count++;
    printf("[test_lib] Constructor called (init_count=%d)\n", g_init_count);
}

__attribute__((destructor))
static void lib_fini() {
    printf("[test_lib] Destructor called (call_count=%d)\n", g_call_count);
}

// ============== 导出函数 ==============
extern "C" {

// 基础函数测试
void shared_function() {
    g_call_count++;
    printf("[test_lib] shared_function called (count=%d)\n", g_call_count);
}

// 带参数和返回值的函数
int add_numbers(int a, int b) {
    int result = a + b;
    printf("[test_lib] add_numbers(%d, %d) = %d\n", a, b, result);
    return result;
}

// 字符串处理函数
const char* get_greeting(const char* name) {
    static char buffer[128];
    snprintf(buffer, sizeof(buffer), "Hello, %s! From %s", name, g_lib_name);
    printf("[test_lib] get_greeting: %s\n", buffer);
    return buffer;
}

// 结构体参数测试
struct TestData {
    int id;
    float value;
    char name[32];
};

void process_data(TestData* data) {
    printf("[test_lib] process_data: id=%d, value=%.2f, name=%s\n",
           data->id, data->value, data->name);
    data->value *= 2.0f;
    data->id += 100;
}

// 回调函数测试
typedef void (*Callback)(int value, void* user_data);

void register_callback(Callback cb, void* user_data) {
    printf("[test_lib] register_callback: invoking callback...\n");
    if (cb) {
        cb(42, user_data);
        cb(100, user_data);
    }
}

// TLS 测试函数
int tls_increment() {
    tls_counter++;
    printf("[test_lib] tls_increment: tls_counter=%d (thread=%lu)\n", 
           tls_counter, pthread_self());
    return tls_counter;
}

void tls_set_buffer(const char* str) {
    strncpy(tls_buffer, str, sizeof(tls_buffer) - 1);
    printf("[test_lib] tls_set_buffer: '%s' (thread=%lu)\n", 
           tls_buffer, pthread_self());
}

const char* tls_get_buffer() {
    return tls_buffer;
}

// 获取库信息
const char* get_lib_info() {
    static char info[256];
    snprintf(info, sizeof(info), 
             "Library: %s\n"
             "Init count: %d\n"
             "Call count: %d\n"
             "TLS counter: %d",
             g_lib_name, g_init_count, g_call_count, tls_counter);
    return info;
}

// 内存分配测试
void* allocate_buffer(size_t size) {
    void* ptr = malloc(size);
    printf("[test_lib] allocate_buffer: %zu bytes at %p\n", size, ptr);
    return ptr;
}

void free_buffer(void* ptr) {
    printf("[test_lib] free_buffer: %p\n", ptr);
    free(ptr);
}

// 数组处理测试
int sum_array(const int* arr, int count) {
    int sum = 0;
    for (int i = 0; i < count; i++) {
        sum += arr[i];
    }
    printf("[test_lib] sum_array: sum of %d elements = %d\n", count, sum);
    return sum;
}

// 浮点运算测试
double compute_average(const double* values, int count) {
    if (count <= 0) return 0.0;
    double sum = 0.0;
    for (int i = 0; i < count; i++) {
        sum += values[i];
    }
    double avg = sum / count;
    printf("[test_lib] compute_average: %.4f\n", avg);
    return avg;
}

// ============== 异常测试 ==============

// 抛出 C++ 异常
void throw_exception() {
    printf("[test_lib] throw_exception: throwing std::runtime_error\n");
    throw std::runtime_error("Test exception from shared library");
}

// 抛出整数异常
void throw_int_exception(int code) {
    printf("[test_lib] throw_int_exception: throwing int %d\n", code);
    throw code;
}

// 抛出自定义异常
class CustomException : public std::exception {
public:
    CustomException(int code, const char* msg) : code_(code) {
        snprintf(message_, sizeof(message_), "CustomException[%d]: %s", code, msg);
    }
    const char* what() const noexcept override { return message_; }
    int code() const { return code_; }
private:
    int code_;
    char message_[256];
};

void throw_custom_exception(int code, const char* msg) {
    printf("[test_lib] throw_custom_exception: code=%d, msg=%s\n", code, msg);
    throw CustomException(code, msg);
}

// 嵌套异常测试
void nested_throw() {
    try {
        throw std::runtime_error("Inner exception");
    } catch (...) {
        printf("[test_lib] nested_throw: caught inner, rethrowing with nested\n");
        std::throw_with_nested(std::runtime_error("Outer exception"));
    }
}

// 带返回值的异常函数
int may_throw(int value) {
    printf("[test_lib] may_throw: value=%d\n", value);
    if (value < 0) {
        throw std::invalid_argument("Value must be non-negative");
    }
    if (value == 0) {
        throw std::runtime_error("Value cannot be zero");
    }
    return value * 2;
}

// 异常安全的函数（noexcept）
int safe_function(int a, int b) noexcept {
    printf("[test_lib] safe_function: %d + %d = %d\n", a, b, a + b);
    return a + b;
}

// 捕获并处理异常
int catch_and_return(int value) {
    try {
        if (value < 0) {
            throw std::runtime_error("Negative value");
        }
        return value;
    } catch (const std::exception& e) {
        printf("[test_lib] catch_and_return: caught '%s', returning -1\n", e.what());
        return -1;
    }
}

// 回调中抛出异常
typedef int (*ThrowingCallback)(int);

int call_throwing_callback(ThrowingCallback cb, int value) {
    printf("[test_lib] call_throwing_callback: calling callback with %d\n", value);
    return cb(value);  // 异常会传播回调用者
}

// 析构函数中的异常处理测试类
class ExceptionTestClass {
public:
    ExceptionTestClass(bool throw_in_dtor = false) 
        : throw_in_dtor_(throw_in_dtor) {
        printf("[test_lib] ExceptionTestClass constructed\n");
    }
    
    ~ExceptionTestClass() {
        printf("[test_lib] ExceptionTestClass destructor\n");
        if (throw_in_dtor_) {
            // 注意：析构函数中抛异常是危险的，这里仅用于测试
            printf("[test_lib] WARNING: Exception in destructor (caught internally)\n");
        }
    }
    
    void do_work(bool should_throw) {
        printf("[test_lib] ExceptionTestClass::do_work (should_throw=%d)\n", should_throw);
        if (should_throw) {
            throw std::runtime_error("Exception in do_work");
        }
    }

private:
    bool throw_in_dtor_;
};

void* create_exception_test_object(bool throw_in_dtor) {
    return new ExceptionTestClass(throw_in_dtor);
}

void destroy_exception_test_object(void* obj) {
    delete static_cast<ExceptionTestClass*>(obj);
}

void exception_test_do_work(void* obj, bool should_throw) {
    static_cast<ExceptionTestClass*>(obj)->do_work(should_throw);
}

// RAII 异常安全测试
int raii_exception_test(bool should_throw) {
    printf("[test_lib] raii_exception_test: creating objects\n");
    
    ExceptionTestClass obj1;
    ExceptionTestClass obj2;
    
    if (should_throw) {
        printf("[test_lib] raii_exception_test: throwing exception\n");
        throw std::runtime_error("RAII test exception");
    }
    
    printf("[test_lib] raii_exception_test: normal return\n");
    return 0;
}

} // extern "C"

// ============== C++ 类测试 ==============
class TestClass {
public:
    TestClass(int val) : value_(val) {
        printf("[test_lib] TestClass::TestClass(%d)\n", val);
    }
    
    ~TestClass() {
        printf("[test_lib] TestClass::~TestClass() value=%d\n", value_);
    }
    
    int getValue() const { return value_; }
    void setValue(int val) { value_ = val; }
    
    void print() const {
        printf("[test_lib] TestClass::print() value=%d\n", value_);
    }

private:
    int value_;
};

extern "C" {

void* create_test_object(int value) {
    return new TestClass(value);
}

void destroy_test_object(void* obj) {
    delete static_cast<TestClass*>(obj);
}

int get_object_value(void* obj) {
    return static_cast<TestClass*>(obj)->getValue();
}

void set_object_value(void* obj, int value) {
    static_cast<TestClass*>(obj)->setValue(value);
}

void print_object(void* obj) {
    static_cast<TestClass*>(obj)->print();
}

} // extern "C"
