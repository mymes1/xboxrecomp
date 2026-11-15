# [android-port] New: Android glue for a recompiled title (SDL_main entry,
# logcat forwarding). An OBJECT library: nothing in libmain.so references
# SDL_main (SDL's Java side looks it up with dlsym), so a static archive
# member would be dropped by the linker.
if(NOT ANDROID)
    message(FATAL_ERROR "src/host/android is Android-only")
endif()

add_library(xboxrecomp_android OBJECT
    xbr_android_main.c
    xbr_android_log.c
    xbr_android_input.c
)
target_include_directories(xboxrecomp_android PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
# xbr_android_input.c: the input layer's overlay pad (xinput_xbox.h, which
# includes platform/xbox_winnt.h from src/).
target_include_directories(xboxrecomp_android PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../input
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)
target_link_libraries(xboxrecomp_android PUBLIC SDL2::SDL2 log android)
target_compile_options(xboxrecomp_android PRIVATE -Wall -Wextra -Wno-unused-parameter)
