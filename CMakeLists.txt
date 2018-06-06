project (uplayer1 C)
cmake_minimum_required(VERSION 3.5 FATAL_ERROR)
enable_language(C)

file(GLOB_RECURSE SOURCES "*.c" "*.cpp")
file(GLOB_RECURSE HEADERS "*.h" "*.hpp")

set(CMAKE_CXX_STANDARD 11)
set(sourcesDir ${CMAKE_CURRENT_SOURCE_DIR})
set(productsDir "${sourcesDir}_products")
#set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${productsDir})
set(CMAKE_C_FLAGS -Wno-deprecated-declarations)

include_directories(${sourcesDir}/include/)
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})
target_link_libraries(${PROJECT_NAME} avformat avcodec swscale)
