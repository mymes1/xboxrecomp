# [android-port] The NV2A render back end on Vulkan (nv2a_vk.h).
#
# Needs only the Vulkan headers: the loader is opened at run time (dlopen),
# so nothing links against libvulkan and a binary without a Vulkan driver
# still runs on the CPU rasteriser. The shaders are compiled SPIR-V in
# nv2a_vk_spv.h (python3 -m tools.android.vk_shaders), so no shader compiler
# either.
#
# XBOXRECOMP_VULKAN_HEADERS_DIR: a directory holding vulkan/vulkan.h. The
# default looks at the pinned Vulkan-Headers fetch_deps puts in
# build/android-deps, then the toolchain's own (the NDK sysroot has them).
# Without headers the target is not defined and the caller carries on
# without the back end (check `if(TARGET xboxrecomp_vk)`).

set(XBOXRECOMP_VULKAN_HEADERS_DIR "" CACHE PATH
    "Directory containing vulkan/vulkan.h (default: build/android-deps, then the toolchain)")

get_filename_component(_xbr_vk_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
find_path(XBR_VULKAN_INCLUDE_DIR vulkan/vulkan.h
    HINTS ${XBOXRECOMP_VULKAN_HEADERS_DIR}
          "${_xbr_vk_root}/build/android-deps/src/Vulkan-Headers/include"
    NO_CMAKE_FIND_ROOT_PATH)
if(NOT XBR_VULKAN_INCLUDE_DIR)
    find_path(XBR_VULKAN_INCLUDE_DIR vulkan/vulkan.h)
endif()
if(NOT XBR_VULKAN_INCLUDE_DIR)
    message(STATUS "xboxrecomp_vk: no vulkan/vulkan.h (set XBOXRECOMP_VULKAN_HEADERS_DIR); "
                   "the Vulkan back end is not built")
    return()
endif()
message(STATUS "xboxrecomp_vk: Vulkan headers in ${XBR_VULKAN_INCLUDE_DIR}")

add_library(xboxrecomp_vk STATIC nv2a_vk.c)
target_include_directories(xboxrecomp_vk
    PUBLIC  "${CMAKE_CURRENT_LIST_DIR}"
    PRIVATE "${XBR_VULKAN_INCLUDE_DIR}"
            "${_xbr_vk_root}/src/kernel"
            "${_xbr_vk_root}/src")
target_compile_definitions(xboxrecomp_vk PUBLIC XBR_HAVE_VULKAN=1)
if(NOT MSVC)
    target_compile_options(xboxrecomp_vk PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
target_link_libraries(xboxrecomp_vk PUBLIC ${CMAKE_DL_LIBS})
