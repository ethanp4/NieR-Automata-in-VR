# Needs MSYS2 UCRT64 (GCC), the OpenXR SDK headers, CMake and Ninja:
#   pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-openxr-sdk mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
# Usually run through build.ps1. By hand, from the UCRT64 shell:
#   cmake -S . -B build -G Ninja -DCMAKE_INSTALL_PREFIX="C:/Program Files (x86)/Steam/steamapps/common/NieRAutomata"
#   cmake -S . -B build-clang -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_ASM_COMPILER=clang -DCMAKE_INSTALL_PREFIX="C:/Program Files (x86)/Steam/steamapps/common/NieRAutomata"
#   cmake --build build
#   cmake --install build
#   cmake --build build --target launch
#   ctest --test-dir build
cmake_minimum_required(VERSION 3.20)
project(NieRVR LANGUAGES C CXX ASM)

option(NIERVR_TESTS "Build the unit tests" ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # build/compile_commands.json, for clangd

# --- third-party code, downloaded at configure time ----------------------------------------------
# Pinned archives checked by SHA256: a different download fails the configure. They're unpacked in
# .deps/ (shared by every build folder, survives a clean build). Only the files are used; the
# libraries' own CMake projects are not (SOURCE_SUBDIR points nowhere).
include(FetchContent)
set(FETCHCONTENT_BASE_DIR ${CMAKE_SOURCE_DIR}/.deps)
FetchContent_Declare(minhook  # function hooking (upstream master, Sep 2026)
    URL https://github.com/TsudaKageyu/minhook/archive/8af6b4acae5a9388fd742b56fa79ece89d96f823.tar.gz
    URL_HASH SHA256=6c0334d09607da1c4a34a68b5febf0cf24c5810cae842eda13c9c24ea4196adb
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR none)
FetchContent_Declare(imgui    # Dear ImGui: the stats overlay (ui/overlay.cpp)
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
    URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR none)
FetchContent_MakeAvailable(minhook imgui)  # sets minhook_SOURCE_DIR, imgui_SOURCE_DIR

file(GLOB_RECURSE NIERVR_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.S)
list(FILTER NIERVR_SOURCES EXCLUDE REGEX "/src/gen/")  # thunks.S is listed below
# stand-ins for the C runtime from the -nostdlib days; the real one is linked now
list(FILTER NIERVR_SOURCES EXCLUDE REGEX "/src/core/(crt\\.cpp|chkstk\\.S)$")
add_library(nier_vr SHARED
    ${NIERVR_SOURCES}
    src/gen/thunks.S
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    src/gen/exports.def)  # the export table: every d3d11.dll export, forwarded

set_target_properties(nier_vr PROPERTIES
    OUTPUT_NAME d3d11
    PREFIX ""                                      # d3d11.dll, not libd3d11.dll
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}   # build/d3d11.dll
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

target_include_directories(nier_vr PRIVATE src ${minhook_SOURCE_DIR}/include ${imgui_SOURCE_DIR})
# ImGui only draws the overlay: no clipboard/IME/shell helpers (they'd import SHELL32 and more)
target_compile_definitions(nier_vr PRIVATE IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_WIN32_FUNCTIONS)
# -fno-threadsafe-statics: a guarded function-local static pulls all of libstdc++'s exception
# handling into the DLL (see build/d3d11.map); ImGui runs on one thread only.
target_compile_options(nier_vr PRIVATE
    -O2 -msse4.1 -fno-math-errno -fno-asynchronous-unwind-tables -fno-stack-protector
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-threadsafe-statics>)
# The C runtime is Windows' own (UCRT, part of Windows 10+); -static builds libgcc, libstdc++ and
# winpthread into the DLL, so nothing has to be shipped next to it.
target_link_options(nier_vr PRIVATE -static -Wl,--enable-stdcall-fixup -s
    -Wl,-Map=${CMAKE_BINARY_DIR}/d3d11.map)  # what the linker pulled in, and from which library
target_link_libraries(nier_vr PRIVATE kernel32 user32 advapi32)

# install target
install(TARGETS nier_vr RUNTIME DESTINATION .)   # just d3d11.dll, not the import library

# launch target will isntall
add_custom_target(launch
    COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR}
    COMMAND cmd /c start steam://rungameid/524220
    VERBATIM)
add_dependencies(launch nier_vr)

if(NIERVR_TESTS)
    enable_testing()
    file(GLOB NIERVR_TESTS_SOURCES CONFIGURE_DEPENDS tests/test_*.cpp)
    foreach(test_src ${NIERVR_TESTS_SOURCES})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        target_include_directories(${test_name} PRIVATE src)
        target_compile_options(${test_name} PRIVATE -msse4.1)
        set_target_properties(${test_name} PROPERTIES
            CXX_STANDARD 17
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
        add_test(NAME ${test_name} COMMAND ${test_name})
        set_tests_properties(${test_name} PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
    endforeach()
endif()
