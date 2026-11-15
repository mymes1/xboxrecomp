# [android-port] The Vulkan back end against the CPU rasteriser
# (nv2a_vk_backend_test.c). Needs the Vulkan headers to build and a Vulkan
# device to run; without a device the test exits 77 and ctest reports it
# skipped. On a machine without a GPU, Mesa's lavapipe or SwiftShader is a
# Vulkan device (see docs/android/README.md, "Testing the Vulkan back end").
cmake_minimum_required(VERSION 3.20)
project(nv2a_vk_backend_regression C)
enable_testing()

add_subdirectory(../../src/vk vk)
if(NOT TARGET xboxrecomp_vk)
    message(FATAL_ERROR "nv2a_vk_backend: the Vulkan back end needs vulkan/vulkan.h "
                        "(python3 -m tools.android.fetch_deps --only git, or set "
                        "XBOXRECOMP_VULKAN_HEADERS_DIR)")
endif()

add_executable(nv2a-vk-backend-test
    nv2a_vk_backend_test.c
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c
    ../../src/kernel/nv2a_flip_hold.c)
target_include_directories(nv2a-vk-backend-test PRIVATE ../../src/kernel ../../src)
target_link_libraries(nv2a-vk-backend-test PRIVATE xboxrecomp_vk m pthread)

add_test(NAME nv2a-vk-backend COMMAND nv2a-vk-backend-test)
set_tests_properties(nv2a-vk-backend PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)

# The window presenter on a headless surface (VK_EXT_headless_surface):
# swapchain, upload, letterboxing, suspend/resume, the renderer sharing the
# presenter's device (nv2a_vk_present_test.c).
add_executable(nv2a-vk-present-test
    nv2a_vk_present_test.c
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c
    ../../src/kernel/nv2a_flip_hold.c)
target_include_directories(nv2a-vk-present-test PRIVATE ../../src/kernel ../../src
    "${XBR_VULKAN_INCLUDE_DIR}")
target_link_libraries(nv2a-vk-present-test PRIVATE xboxrecomp_vk m pthread ${CMAKE_DL_LIBS})

add_test(NAME nv2a-vk-present COMMAND nv2a-vk-present-test)
set_tests_properties(nv2a-vk-present PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
