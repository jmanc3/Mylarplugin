cmake_minimum_required(VERSION 3.27)

project(Mylardesktop
    DESCRIPTION "Mylar desktop"
    VERSION 0.1
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 26)
set(CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Sources ---
file(GLOB_RECURSE HEAD "src/*.h")
file(GLOB_RECURSE SRC "src/*.cpp")

# --- Fetch Tracy profiler ---
include(FetchContent)
FetchContent_Declare(
    tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG master     # or a specific tag, e.g., v0.10
)
FetchContent_MakeAvailable(tracy)

# --- Create shared library ---
add_library(Mylardesktop SHARED
    ${HEAD}
    ${SRC}
    ${tracy_SOURCE_DIR}/public/TracyClient.cpp
)

# --- Dependencies ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    librsvg-2.0
    pixman-1
    libdrm
    hyprland
    pangocairo
    libinput
    libudev
    wayland-server
    wayland-client 
    wayland-cursor
    xkbcommon
    cairo
    dbus-1
    alsa
)

target_link_libraries(Mylardesktop PRIVATE rt PkgConfig::deps)

# --- Include directories ---
target_include_directories(Mylardesktop PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${tracy_SOURCE_DIR}/public
)

# --- Compiler & Tracy defines ---
target_compile_definitions(Mylardesktop PRIVATE
    TRACY_ENABLE
    #TRACY_ON_DEMAND
    TRACY_NO_MEMORY   # avoid malloc/free hooks (important for plugins)
)

target_compile_options(Mylardesktop PRIVATE
    -fPIC
    --no-gnu-unique
)

install(TARGETS Mylardesktop)

