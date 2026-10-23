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

file(GLOB NIERVR_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.S)
add_library(nier_vr SHARED
    ${NIERVR_SOURCES}
    src/gen/thunks.S
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c
    src/gen/exports.def)  # the export table: every d3d11.dll export, forwarded

set_target_properties(nier_vr PROPERTIES
    OUTPUT_NAME d3d11
    PREFIX ""                                      # d3d11.dll, not libd3d11.dll
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}   # build/d3d11.dll
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

target_include_directories(nier_vr PRIVATE src third_party/minhook/include)
target_compile_options(nier_vr PRIVATE
    -O2 -msse4.1 -fno-math-errno -fno-asynchronous-unwind-tables -fno-stack-protector
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
target_link_options(nier_vr PRIVATE
    -nostdlib -nostartfiles -Wl,-e,DllMain -Wl,--enable-stdcall-fixup -s)
target_link_libraries(nier_vr PRIVATE kernel32 user32 advapi32 gcc)

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
