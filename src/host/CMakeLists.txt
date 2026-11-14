# [android-port] New: the portable POSIX host layer (boot sequence, crash
# reporting, default game hooks) and, on Android, the platform glue
# (SDL_main entry, logcat, storage paths, Vulkan presenter).
#
# Not added by the top-level CMakeLists: a game project adds it after the
# toolkit, as templates/xbr-game/native/CMakeLists.txt does.

if(WIN32)
    message(FATAL_ERROR "src/host is the POSIX host layer; Windows titles use templates/new-game")
endif()

add_library(xboxrecomp_host STATIC
    xbr_boot.c
    xbr_game_glue.c
    xbr_stats.c
)
# xbr_stats.c sees flips and the scanout surface by wrapping two presenter
# entry points at link time (no change to the presenter or the executor).
# ld64 has no --wrap: on macOS the status line reports no flips.
if(APPLE)
    target_compile_definitions(xboxrecomp_host PRIVATE XBR_NO_WRAP=1)
else()
    target_link_options(xboxrecomp_host INTERFACE
        "LINKER:--wrap=xbox_FramebufferWindowFrameStats"
        "LINKER:--wrap=xbox_FramebufferWindowSet")
endif()
target_include_directories(xboxrecomp_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../kernel
)
target_link_libraries(xboxrecomp_host PUBLIC xboxrecomp ${CMAKE_DL_LIBS})
target_compile_options(xboxrecomp_host PRIVATE -Wall -Wextra -Wno-unused-parameter)
