cmake_minimum_required(VERSION 3.18)



# 如果未设置 NDK 路径，尝试从环境变量获取
if(NOT DEFINED CMAKE_ANDROID_NDK)

    set(CMAKE_ANDROID_NDK D:/ndkollvm/android-ndk-r27c-windows/android-ndk-r27c)
    
endif()

if(CMAKE_ANDROID_NDK)
    set(CMAKE_TOOLCHAIN_FILE ${CMAKE_ANDROID_NDK}/build/cmake/android.toolchain.cmake)
endif()

# Android NDK 设置
set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_SYSTEM_VERSION 28)
set(ANDROID_ABI arm64-v8a)
set(ANDROID_PLATFORM android-26)

project(newSoLoader CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)



# 源文件
set(SOURCES
    src/elf_image.cpp
    src/linker.cpp
    src/tls.cpp
    src/backtrace.cpp
    src/soloader.cpp
)

# 静态库
add_library(newsoloader STATIC ${SOURCES})

target_include_directories(newsoloader PUBLIC include)

target_compile_definitions(newsoloader PRIVATE
    $<$<CONFIG:Debug>:SOLOADER_DEBUG>
)

target_compile_options(newsoloader PRIVATE
    -Wall
    -Wextra
    -fstack-protector-strong
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Release>:-O3>
)

# 可执行测试程序
add_executable(soloader_test ${SOURCES})

target_include_directories(soloader_test PRIVATE include)

target_compile_definitions(soloader_test PRIVATE
    STANDALONE_TEST
    SOLOADER_DEBUG
)

target_compile_options(soloader_test PRIVATE
    -Wall
    -Wextra
    -g
    -O0
    -fstack-protector-strong
)

target_link_libraries(soloader_test PRIVATE log)

# 添加测试子目录
add_subdirectory(test)
