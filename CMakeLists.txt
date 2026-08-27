cmake_minimum_required(VERSION 3.20)

project(
    GammaChanger
    VERSION 1.2.0
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
        src/GammaChanger.rc)
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
        target_compile_options(GammaChanger PRIVATE /W4 /permissive- /utf-8)
        target_link_options(GammaChanger PRIVATE /DYNAMICBASE /NXCOMPAT)
    elseif(MINGW)
        target_compile_options(GammaChanger PRIVATE -Wall -Wextra -Wpedantic)
        target_link_options(GammaChanger PRIVATE
            -municode
            -static
            -static-libgcc
            -static-libstdc++)
    endif()

    install(TARGETS GammaChanger RUNTIME DESTINATION .)
endif()

if(BUILD_TESTING)
    add_executable(gamma_math_tests tests/gamma_math_tests.cpp)
    target_link_libraries(gamma_math_tests PRIVATE gamma_math)
    if(MSVC)
        target_compile_options(gamma_math_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(gamma_math_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME gamma_math_tests COMMAND gamma_math_tests)
endif()
