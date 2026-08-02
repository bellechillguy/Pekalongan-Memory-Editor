cmake_minimum_required(VERSION 3.20)

project(pekalongan_memory_editor LANGUAGES CXX)

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Memory editor ini hanya mendukung Windows x64 dan Linux x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(memory_core
    src/scanner.cpp
    src/value.cpp
)

if(WIN32)
    target_sources(memory_core PRIVATE src/process_windows.cpp)
else()
    target_sources(memory_core PRIVATE src/process_linux.cpp)
endif()

target_include_directories(memory_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(memory_core PUBLIC Threads::Threads)
target_compile_options(memory_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->
)

add_executable(memory_editor src/main.cpp)
target_link_libraries(memory_editor PRIVATE memory_core)

add_executable(other_target demo/other_target.cpp)

include(CTest)
if(BUILD_TESTING)
    add_executable(scanner_tests tests/scanner_tests.cpp)
    target_link_libraries(scanner_tests PRIVATE memory_core)
    target_compile_options(scanner_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-UNDEBUG>
        $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
    )
    add_test(NAME scanner_tests COMMAND scanner_tests)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(integration_test tests/integration_test.cpp)
        target_link_libraries(integration_test PRIVATE memory_core)
        target_compile_options(integration_test PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-UNDEBUG>
        )
        add_test(NAME integration_test
                 COMMAND integration_test $<TARGET_FILE:other_target>)
    endif()
endif()
