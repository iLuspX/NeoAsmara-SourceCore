cmake_minimum_required(VERSION 3.20)
project(KarosCppCore VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ROSH_NATIVE_WINDOWS "Build native Windows services instead of the original offline laboratory" ${WIN32})
if(ROSH_NATIVE_WINDOWS)
  include(cmake/NativeWindows.cmake)
  return()
endif()

find_package(SQLite3 REQUIRED)

add_library(karos_core
    src/database.cpp
    src/gm.cpp
    src/gameplay.cpp
)
target_include_directories(karos_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(karos_core PUBLIC SQLite::SQLite3)
if(MSVC)
  target_compile_options(karos_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(karos_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

add_executable(karos_admin src/main.cpp)
target_link_libraries(karos_admin PRIVATE karos_core)

add_executable(karos_core_tests tests/self_test.cpp)
target_link_libraries(karos_core_tests PRIVATE karos_core)

enable_testing()
add_test(NAME karos_core_tests COMMAND karos_core_tests)
