cmake_minimum_required(VERSION 3.10)
project(zenonet VERSION 0.2.0 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0601)
    set(PLATFORM_LIBS ws2_32)
else()
    set(PLATFORM_LIBS pthread)
endif()

# ONE C source file. that's it. no more fragmentation.
set(ZENONET_SOURCES
    src/zenonet.c
    src/zenonet_cpp.cpp
)

set(ZENONET_HEADERS include/zenonet.h)

add_library(zenonet STATIC ${ZENONET_SOURCES} ${ZENONET_HEADERS})
target_include_directories(zenonet PUBLIC include)
target_link_libraries(zenonet ${PLATFORM_LIBS})

if(MSVC)
    target_compile_options(zenonet PRIVATE /W3)
else()
    target_compile_options(zenonet PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# examples
add_executable(chat_example examples/chat_example.c)
target_link_libraries(chat_example zenonet)

add_executable(game_example examples/game_example.cpp)
target_link_libraries(game_example zenonet)

# tests
add_executable(test_basic tests/test_basic.c)
target_link_libraries(test_basic zenonet)

install(TARGETS zenonet DESTINATION lib)
install(FILES ${ZENONET_HEADERS} DESTINATION include)

