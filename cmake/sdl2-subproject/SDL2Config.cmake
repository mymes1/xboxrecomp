# [android-port] find_package(SDL2) shim for builds that compile SDL2 from
# source in the same tree (the Android game build, templates/xbr-game). The
# toolkit's libraries ask for SDL2::SDL2; SDL's own CMakeLists defines it.
if(TARGET SDL2::SDL2)
    set(SDL2_FOUND TRUE)
elseif(TARGET SDL2)
    add_library(SDL2::SDL2 ALIAS SDL2)
    set(SDL2_FOUND TRUE)
else()
    set(SDL2_FOUND FALSE)
    set(SDL2_NOT_FOUND_MESSAGE "add_subdirectory(<SDL2 source>) must come before the toolkit")
endif()
