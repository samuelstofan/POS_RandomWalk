cmake_minimum_required(VERSION 3.16)
project(RandomWalk C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(SDL2_TTF REQUIRED SDL2_ttf)

add_executable(server src/server.c)
target_include_directories(server PRIVATE src ${SDL2_INCLUDE_DIRS} ${SDL2_TTF_INCLUDE_DIRS})
target_link_libraries(server PRIVATE ${SDL2_LIBRARIES} ${SDL2_TTF_LIBRARIES} pthread)
target_compile_options(server PRIVATE ${SDL2_CFLAGS_OTHER} ${SDL2_TTF_CFLAGS_OTHER})

add_executable(client src/client.c)
target_include_directories(client PRIVATE src ${SDL2_INCLUDE_DIRS} ${SDL2_TTF_INCLUDE_DIRS})
target_link_libraries(client PRIVATE ${SDL2_LIBRARIES} ${SDL2_TTF_LIBRARIES} pthread m)
target_compile_options(client PRIVATE ${SDL2_CFLAGS_OTHER} ${SDL2_TTF_CFLAGS_OTHER})
