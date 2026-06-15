cmake_minimum_required(VERSION 3.15)
project(aipc VERSION 1.0.0 LANGUAGES C CXX)

# ========================================
# 全局编译设置
# ========================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# SDK 根目录默认是仓库父目录；toolchain 文件可覆盖编译器绝对路径。
get_filename_component(AIPC_DEFAULT_SDK_ROOT "${CMAKE_SOURCE_DIR}" DIRECTORY)
set(AIPC_SDK_ROOT "${AIPC_DEFAULT_SDK_ROOT}" CACHE PATH "AIPC SDK root directory")

# 当编译选项为Debug时，启用调试符号
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g")

# ========================================
# spdlog 编译时日志级别控制
# ========================================
# 设置此选项可在编译时完全移除低于该级别的日志代码（零开销）
# 定义日志等级常量
set(LOG_LEVEL_TRACE    0)
set(LOG_LEVEL_DEBUG    1)
set(LOG_LEVEL_INFO     2)
set(LOG_LEVEL_WARN     3)
set(LOG_LEVEL_ERROR    4)
set(LOG_LEVEL_CRITICAL 5)
set(LOG_LEVEL_OFF      6)

# 根据构建类型自动设置默认日志等级
# Debug 模式下默认 DEBUG，Release 模式下默认 INFO
if(NOT DEFINED SPDLOG_ACTIVE_LEVEL)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(SPDLOG_ACTIVE_LEVEL ${LOG_LEVEL_DEBUG} CACHE STRING "Compile-time log level")
    else()
        set(SPDLOG_ACTIVE_LEVEL ${LOG_LEVEL_INFO} CACHE STRING "Compile-time log level")
    endif()
endif()

add_compile_definitions(SPDLOG_ACTIVE_LEVEL=${SPDLOG_ACTIVE_LEVEL})

# 针对嵌入式环境的优化编译选项
add_compile_options(-ffunction-sections -fdata-sections)
add_link_options(-Wl,--gc-sections)

# ========================================
# 第三方库路径（全局可用）
# ========================================
set(LUCKFOX_MPI_PATH ${CMAKE_SOURCE_DIR}/3rdparty/luckfox_pico_rkmpi_example CACHE PATH "Luckfox MPI library path")
set(LUCKFOX_MPI_INCLUDE_DIR ${LUCKFOX_MPI_PATH}/include CACHE PATH "Luckfox MPI include path")
set(LUCKFOX_MPI_LIB_DIR ${LUCKFOX_MPI_PATH}/lib/uclibc CACHE PATH "Luckfox MPI library path")
set(SPDLOG_PATH ${CMAKE_SOURCE_DIR}/3rdparty/spdlog CACHE PATH "spdlog library path")
set(CPP_HTTPLIB_PATH ${CMAKE_SOURCE_DIR}/3rdparty/cpp-httplib CACHE PATH "cpp-httplib library path")
set(LIBDATACHANNEL_PATH ${CMAKE_SOURCE_DIR}/3rdparty/libdatachannel CACHE PATH "libdatachannel library path")
set(ASIO_PATH ${CMAKE_SOURCE_DIR}/3rdparty/asio CACHE PATH "Asio library path")

# ========================================
# OpenSSL 配置（用于交叉编译）
# ========================================
set(AIPC_DEFAULT_BUILDROOT_SYSROOT
    "${AIPC_SDK_ROOT}/../Luckfox/luckfox-pico/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot")
set(BUILDROOT_SYSROOT "${AIPC_DEFAULT_BUILDROOT_SYSROOT}" CACHE PATH
    "Buildroot target sysroot that provides OpenSSL for cross-compilation")

set(AIPC_OPENSSL_ROOT_DIR "${BUILDROOT_SYSROOT}/usr")
set(AIPC_OPENSSL_INCLUDE_DIR "${AIPC_OPENSSL_ROOT_DIR}/include")
set(AIPC_OPENSSL_SSL_LIBRARY "")
set(AIPC_OPENSSL_CRYPTO_LIBRARY "")

if(EXISTS "${AIPC_OPENSSL_ROOT_DIR}/lib/libssl.so")
    set(AIPC_OPENSSL_SSL_LIBRARY "${AIPC_OPENSSL_ROOT_DIR}/lib/libssl.so")
elseif(EXISTS "${AIPC_OPENSSL_ROOT_DIR}/lib/libssl.a")
    set(AIPC_OPENSSL_SSL_LIBRARY "${AIPC_OPENSSL_ROOT_DIR}/lib/libssl.a")
endif()

if(EXISTS "${AIPC_OPENSSL_ROOT_DIR}/lib/libcrypto.so")
    set(AIPC_OPENSSL_CRYPTO_LIBRARY "${AIPC_OPENSSL_ROOT_DIR}/lib/libcrypto.so")
elseif(EXISTS "${AIPC_OPENSSL_ROOT_DIR}/lib/libcrypto.a")
    set(AIPC_OPENSSL_CRYPTO_LIBRARY "${AIPC_OPENSSL_ROOT_DIR}/lib/libcrypto.a")
endif()

if(EXISTS "${AIPC_OPENSSL_INCLUDE_DIR}/openssl/ssl.h"
   AND AIPC_OPENSSL_SSL_LIBRARY
   AND AIPC_OPENSSL_CRYPTO_LIBRARY)
    set(OPENSSL_ROOT_DIR "${AIPC_OPENSSL_ROOT_DIR}" CACHE PATH "OpenSSL root directory" FORCE)
    set(OPENSSL_INCLUDE_DIR "${AIPC_OPENSSL_INCLUDE_DIR}" CACHE PATH "OpenSSL include directory" FORCE)
    set(OPENSSL_SSL_LIBRARY "${AIPC_OPENSSL_SSL_LIBRARY}" CACHE FILEPATH "OpenSSL SSL library" FORCE)
    set(OPENSSL_CRYPTO_LIBRARY "${AIPC_OPENSSL_CRYPTO_LIBRARY}" CACHE FILEPATH "OpenSSL crypto library" FORCE)
    set(OPENSSL_USE_STATIC_LIBS OFF CACHE BOOL "Prefer shared OpenSSL libraries" FORCE)

    find_package(OpenSSL QUIET)

    if(NOT TARGET OpenSSL::Crypto)
        add_library(OpenSSL::Crypto UNKNOWN IMPORTED GLOBAL)
        set_target_properties(OpenSSL::Crypto PROPERTIES
            IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
        )
    endif()

    if(NOT TARGET OpenSSL::SSL)
        add_library(OpenSSL::SSL UNKNOWN IMPORTED GLOBAL)
        set_target_properties(OpenSSL::SSL PROPERTIES
            IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES OpenSSL::Crypto
        )
    endif()
else()
    message(WARNING
        "OpenSSL was not found under BUILDROOT_SYSROOT='${BUILDROOT_SYSROOT}'. "
        "Expected headers in '${AIPC_OPENSSL_INCLUDE_DIR}' and libraries in "
        "'${AIPC_OPENSSL_ROOT_DIR}/lib'.")
endif()

# ========================================
# 添加第三方库
# ========================================

# spdlog (动态库模式)
set(SPDLOG_BUILD_SHARED ON CACHE BOOL "Build spdlog as shared library" FORCE)
add_subdirectory(${SPDLOG_PATH} ${CMAKE_BINARY_DIR}/spdlog)

# cpp-httplib (header-only)
add_library(httplib INTERFACE)
target_include_directories(httplib INTERFACE ${CPP_HTTPLIB_PATH})

# Asio (header-only, standalone mode)
add_library(asio INTERFACE)
target_include_directories(asio INTERFACE ${ASIO_PATH}/asio/include)
target_compile_definitions(asio INTERFACE
    ASIO_STANDALONE
    ASIO_NO_DEPRECATED
)
# Asio 需要 pthread
target_link_libraries(asio INTERFACE pthread)

# libdatachannel (WebRTC 支持)
# 配置选项
set(NO_WEBSOCKET OFF CACHE BOOL "Disable WebSocket support" FORCE)
set(NO_MEDIA OFF CACHE BOOL "Disable media support" FORCE)  # 需要启用媒体支持以使用 H264RtpPacketizer
set(NO_EXAMPLES ON CACHE BOOL "Disable examples" FORCE)
set(NO_TESTS ON CACHE BOOL "Disable tests" FORCE)
set(USE_NICE OFF CACHE BOOL "Use libnice for ICE" FORCE)
add_subdirectory(${LIBDATACHANNEL_PATH} ${CMAKE_BINARY_DIR}/libdatachannel)

# ========================================
# 添加源代码子目录
# ========================================
add_subdirectory(src)

# ========================================
# 安装配置
# ========================================
# 设置安装前缀（默认安装到 build/install）
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Install path prefix" FORCE)
endif()

# 安装目录设置
set(AIPC_INSTALL_BIN_DIR "bin")
set(AIPC_INSTALL_LIB_DIR "lib")
set(AIPC_INSTALL_ASSETS_DIR "assets")
set(AIPC_INSTALL_WWW_DIR "www")
# 安装可执行文件，设置 RPATH 让可执行文件能找到 ../lib 下的动态库
set_target_properties(aipc PROPERTIES
    INSTALL_RPATH "$ORIGIN/../lib"
    BUILD_WITH_INSTALL_RPATH OFF
)
install(TARGETS aipc
    RUNTIME DESTINATION ${AIPC_INSTALL_BIN_DIR}
)

# 安装 libdatachannel 动态库到项目 lib 目录
if(TARGET datachannel)
    install(TARGETS datachannel
        LIBRARY DESTINATION ${AIPC_INSTALL_LIB_DIR}
        ARCHIVE DESTINATION ${AIPC_INSTALL_LIB_DIR}
    )
endif()

# 安装 spdlog 动态库到项目 lib 目录
if(TARGET spdlog)
    install(TARGETS spdlog
        LIBRARY DESTINATION ${AIPC_INSTALL_LIB_DIR}
        ARCHIVE DESTINATION ${AIPC_INSTALL_LIB_DIR}
    )
endif()

# 安装启动/停止脚本
install(PROGRAMS
    ${CMAKE_SOURCE_DIR}/assets/start_app.sh
    ${CMAKE_SOURCE_DIR}/assets/stop_app.sh
    DESTINATION ${AIPC_INSTALL_BIN_DIR}
)

# 安装 Web 资源文件 (from www/dist)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/www/dist/
    DESTINATION ${AIPC_INSTALL_WWW_DIR}
    FILES_MATCHING
    PATTERN "*.html"
    PATTERN "*.js"
    PATTERN "*.css"
    PATTERN "*.ico"
    PATTERN "*.png"
    PATTERN "*.jpg"
    PATTERN "*.svg"
)

# 安装其他资源文件（图片等）
install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/images/
    DESTINATION ${AIPC_INSTALL_ASSETS_DIR}/images
    FILES_MATCHING
    PATTERN "*.png"
    PATTERN "*.jpg"
    PATTERN "*.bmp"
)

# 生成版本信息文件
file(WRITE ${CMAKE_BINARY_DIR}/version.txt
    "AIPC Version: ${PROJECT_VERSION}\n"
    "Build Type: ${CMAKE_BUILD_TYPE}\n"
    "Build Date: ${CMAKE_CURRENT_SOURCE_DIR}\n"
)
install(FILES ${CMAKE_BINARY_DIR}/version.txt DESTINATION ${AIPC_INSTALL_BIN_DIR})

# ========================================
# 输出配置信息
# ========================================
message(STATUS "========================================")
message(STATUS "Project: ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "spdlog compile-time log level: ${SPDLOG_ACTIVE_LEVEL} (0=TRACE, 2=INFO, 6=OFF)")
message(STATUS "========================================")
