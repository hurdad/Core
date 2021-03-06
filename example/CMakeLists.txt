project(examples CXX)
cmake_minimum_required(VERSION 3.0)

set (CMAKE_CXX_STANDARD 14)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_COLOR_MAKEFILE ON)
set (CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE)
    message(STATUS "Default build type 'Debug'")
    set(CMAKE_BUILD_TYPE DEBUG CACHE STRING "" FORCE )
else()
    string(TOUPPER ${CMAKE_BUILD_TYPE} CMAKE_BUILD_TYPE)
    message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
endif()

set(PROJECT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(PROJECT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY  ${PROJECT_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY  ${PROJECT_DIR}/bin)
set(CMAKE_BINARY_DIR ${PROJECT_DIR}/bin)
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_DIR}/bin)
set(PROJECT_3RD_LOC ${CMAKE_CURRENT_SOURCE_DIR}/../Third_Party)

include_directories(. .. ${PROJECT_DIR} ${PROJECT_3RD_LOC}/include)
link_directories(${PROJECT_DIR}/../bin)
add_executable(example example.cpp)
if(WIN32)
	set(TO_LINK_LIBS Core)
else()
	set(TO_LINK_LIBS Core pthread)
endif()
target_link_libraries(example ${TO_LINK_LIBS})
