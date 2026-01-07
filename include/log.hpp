// Modern C++17 SO Loader - Logging
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace soloader {

#define LOG_TAG "soloader"

#ifdef SOLOADER_DEBUG
    #ifdef __ANDROID__
        #define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
        #define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
        #define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
        #define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
        #define LOGF(...) do { __android_log_print(ANDROID_LOG_FATAL, LOG_TAG, __VA_ARGS__); abort(); } while(0)
        #define PLOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, fmt ": %s", ##__VA_ARGS__, strerror(errno))
    #else
        #define LOGD(...) do { fprintf(stderr, "[D] " LOG_TAG ": " __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
        #define LOGI(...) do { fprintf(stderr, "[I] " LOG_TAG ": " __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
        #define LOGW(...) do { fprintf(stderr, "[W] " LOG_TAG ": " __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
        #define LOGE(...) do { fprintf(stderr, "[E] " LOG_TAG ": " __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
        #define LOGF(...) do { fprintf(stderr, "[F] " LOG_TAG ": " __VA_ARGS__); fprintf(stderr, "\n"); abort(); } while(0)
        #define PLOGE(fmt, ...) do { fprintf(stderr, "[E] " LOG_TAG ": " fmt ": %s\n", ##__VA_ARGS__, strerror(errno)); } while(0)
    #endif
#else
    #define LOGD(...) do {} while(0)
    #define LOGI(...) do {} while(0)
    #define LOGW(...) do {} while(0)
    #define LOGE(...) do {} while(0)
    #define LOGF(...) do { abort(); } while(0)
    #define PLOGE(...) do {} while(0)
#endif

} // namespace soloader
