# 测试库和测试程序的 CMake 配置

# 测试用 SO 库
add_library(test_lib SHARED test_lib.cpp)

target_compile_options(test_lib PRIVATE
    -Wall
    -Wextra
    -fPIC
    -O2
)

# 设置输出名称
set_target_properties(test_lib PROPERTIES
    OUTPUT_NAME "test_lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test"
)
