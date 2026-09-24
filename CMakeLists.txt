cmake_minimum_required(VERSION 3.16)
project(aether_chess LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AETHER_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(AETHER_ASPIRATION "Experiment: use aspiration windows in iterative deepening" OFF)
option(AETHER_HISTORY_AGING "Experiment: age quiet-move history between searches" OFF)
add_library(aether_build_options INTERFACE)
if(MSVC)
    target_compile_options(aether_build_options INTERFACE /W4)
else()
    target_compile_options(aether_build_options INTERFACE -Wall -Wextra -Wpedantic)
endif()
if(AETHER_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "AETHER_SANITIZERS requires GCC or Clang")
    endif()
    target_compile_options(aether_build_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(aether_build_options INTERFACE -fsanitize=address,undefined)
endif()
find_package(Threads REQUIRED)

add_library(aether_core
    src/position.cpp
    src/evaluation.cpp
    src/search.cpp
    src/notation.cpp
)
target_include_directories(aether_core PUBLIC src)
target_link_libraries(aether_core PUBLIC aether_build_options)
target_compile_definitions(aether_core PRIVATE
    AETHER_ASPIRATION=$<BOOL:${AETHER_ASPIRATION}>
    AETHER_HISTORY_AGING=$<BOOL:${AETHER_HISTORY_AGING}>)

add_executable(aether src/main.cpp)
target_link_libraries(aether PRIVATE aether_core Threads::Threads)

add_executable(perft tests/perft.cpp)
target_link_libraries(perft PRIVATE aether_core)

add_executable(core_tests tests/core.cpp)
target_link_libraries(core_tests PRIVATE aether_core)

enable_testing()
add_test(NAME perft COMMAND perft)
add_test(NAME core COMMAND core_tests)

foreach(suite position search evaluation)
    add_executable(${suite}_tests tests/${suite}_regressions.cpp)
    target_link_libraries(${suite}_tests PRIVATE aether_core)
    add_test(NAME ${suite}_regressions COMMAND ${suite}_tests)
    set_tests_properties(${suite}_regressions PROPERTIES TIMEOUT 120)
endforeach()
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME uci_regressions COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/uci_regressions.py $<TARGET_FILE:aether>)
    set_tests_properties(uci_regressions PROPERTIES TIMEOUT 40)
endif()
