add_library(uvc_lib STATIC
    uvc_producer.cpp
    uvc_producer.h
    uvc_config.h
)

target_include_directories(uvc_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(uvc_lib
    PUBLIC
        common
        spdlog::spdlog
)

target_compile_options(uvc_lib PRIVATE -Wall -Wextra)

set(UVC_FFMPEG_LIBS
    avcodec
    swresample
    avutil
    swscale
)

set(UVC_FFMPEG_SYS_DEPS
    iconv
    gnutls
    tasn1
    hogweed
    nettle
    gmp
    unistring
    intl
    bz2
    drm
    lzma
    z
    atomic
    m
)

add_library(uvc_h264_lib STATIC
    uvc_h264_producer.cpp
    uvc_h264_producer.h
    uvc_h264_config.h
)

target_include_directories(uvc_h264_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
        ${LUCKFOX_MPI_INCLUDE_DIR}
        ${BUILDROOT_SYSROOT}/usr/include
)

target_link_directories(uvc_h264_lib
    PUBLIC
        ${LUCKFOX_MPI_LIB_DIR}
        ${BUILDROOT_SYSROOT}/usr/lib
)

target_link_libraries(uvc_h264_lib
    PUBLIC
        uvc_lib
        common
        ${UVC_FFMPEG_LIBS}
        ${UVC_FFMPEG_SYS_DEPS}
        rockit
        rockchip_mpp
    PRIVATE
        spdlog::spdlog
)

target_compile_options(uvc_h264_lib PRIVATE -Wall -Wextra)

add_executable(uvc_capture_test uvc_capture_test.cpp)
target_link_libraries(uvc_capture_test PRIVATE uvc_lib)
target_compile_options(uvc_capture_test PRIVATE -Wall -Wextra)
set_target_properties(uvc_capture_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN/../lib"
)
