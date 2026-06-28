cmake_minimum_required(VERSION 3.21)
project(libmucom88 VERSION 0.1.0 LANGUAGES CXX)

# header-only INTERFACE target (for consumption by other CMake projects)
add_library(mucom88 INTERFACE)
add_library(mucom88::mucom88 ALIAS mucom88)
target_include_directories(mucom88 INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(mucom88 INTERFACE cxx_std_17)

# tools/tests only when libmucom88 is the top-level project (not via add_subdirectory)
option(LIBMUCOM88_BUILD_TOOLS "Build bundled CLI tools" ${PROJECT_IS_TOP_LEVEL})
option(LIBMUCOM88_BUILD_TESTS "Build header smoke tests" ${PROJECT_IS_TOP_LEVEL})

if(LIBMUCOM88_BUILD_TOOLS)
    add_executable(drumkit_gen tools/drumkit_gen.cpp)
    target_link_libraries(drumkit_gen PRIVATE mucom88::mucom88)
    if(MSVC)
        target_compile_options(drumkit_gen PRIVATE /W3 /O2)
    else()
        target_compile_options(drumkit_gen PRIVATE -Wall -Wextra -O2)
    endif()
endif()

if(LIBMUCOM88_BUILD_TESTS)
    enable_testing()
    add_executable(adpcm_a_roundtrip tests/adpcm_a_roundtrip.cpp)
    target_link_libraries(adpcm_a_roundtrip PRIVATE mucom88::mucom88)
    add_test(NAME adpcm_a_roundtrip COMMAND adpcm_a_roundtrip)

    add_executable(mml_render_mixed_limit tests/mml_render_mixed_limit.cpp)
    target_link_libraries(mml_render_mixed_limit PRIVATE mucom88::mucom88)
    add_test(NAME mml_render_mixed_limit COMMAND mml_render_mixed_limit)
endif()
