cmake_minimum_required(VERSION 3.20)

project(
    GammaChanger
    VERSION 1.4.4
    DESCRIPTION "Native Windows hardware gamma-ramp utility"
    LANGUAGES CXX)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CTest)

add_library(gamma_math STATIC
    src/gamma_math.cpp
    src/gamma_math.h)
target_include_directories(gamma_math PUBLIC src)
target_compile_features(gamma_math PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(gamma_math PRIVATE /W4 /permissive-)
else()
    target_compile_options(gamma_math PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    enable_language(RC)
    add_executable(GammaChanger WIN32
        src/main.cpp
        src/auto_gamma.h
        src/gamma_toggle.h
        src/gpu_capture.h
        src/resource.h
        src/single_key.h
        src/single_key_win32.h
        src/assets/flashbang.ico
        src/GammaChanger.rc)
    set_source_files_properties(src/GammaChanger.rc PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/flashbang.ico;${CMAKE_CURRENT_SOURCE_DIR}/src/GammaChanger.manifest")
    target_link_libraries(GammaChanger PRIVATE
        gamma_math
        advapi32
        comctl32
        gdi32
        shell32
        user32
        wtsapi32)
    target_compile_definitions(GammaChanger PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00)
    target_include_directories(GammaChanger PRIVATE src)

    if(MSVC)
        target_compile_options(GammaChanger PRIVATE /W4 /permissive- /utf-8
            $<$<CONFIG:Release>:/Gy /Gw>)
        # GammaChanger.rc already embeds the complete application manifest.
        target_link_options(GammaChanger PRIVATE /DYNAMICBASE /NXCOMPAT /MANIFEST:NO
            $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
    elseif(MINGW)
        target_compile_options(GammaChanger PRIVATE -Wall -Wextra -Wpedantic
            $<$<CONFIG:Release>:-ffunction-sections -fdata-sections>)
        target_link_options(GammaChanger PRIVATE
            $<$<CONFIG:Release>:-Wl,--gc-sections -s>
            -municode
            -static
            -static-libgcc
            -static-libstdc++)
    endif()

    include(CheckIPOSupported)
    check_ipo_supported(RESULT gammachanger_ipo OUTPUT gammachanger_ipo_log)
    if(gammachanger_ipo)
        set_property(TARGET GammaChanger PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    endif()

    install(TARGETS GammaChanger RUNTIME DESTINATION .)
endif()

if(BUILD_TESTING)
    add_executable(gamma_toggle_tests tests/gamma_toggle_tests.cpp)
    target_link_libraries(gamma_toggle_tests PRIVATE gamma_math)
    if(MSVC)
        target_compile_options(gamma_toggle_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(gamma_toggle_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME gamma_toggle_tests COMMAND gamma_toggle_tests)

    add_executable(gamma_math_tests tests/gamma_math_tests.cpp)
    target_link_libraries(gamma_math_tests PRIVATE gamma_math)
    if(MSVC)
        target_compile_options(gamma_math_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(gamma_math_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME gamma_math_tests COMMAND gamma_math_tests)

    add_executable(auto_gamma_tests tests/auto_gamma_tests.cpp)
    target_include_directories(auto_gamma_tests PRIVATE src)
    target_compile_features(auto_gamma_tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(auto_gamma_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(auto_gamma_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME auto_gamma_tests COMMAND auto_gamma_tests)

    add_executable(single_key_tests tests/single_key_tests.cpp)
    target_include_directories(single_key_tests PRIVATE src)
    target_compile_features(single_key_tests PRIVATE cxx_std_17)
    if(WIN32)
        target_link_libraries(single_key_tests PRIVATE user32)
        target_compile_definitions(single_key_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    if(MSVC)
        target_compile_options(single_key_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(single_key_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME single_key_tests COMMAND single_key_tests)
endif()
