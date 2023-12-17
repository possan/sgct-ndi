cmake_minimum_required(VERSION 3.25 FATAL_ERROR)
cmake_policy(VERSION 3.25)

option(BUILD_SHARED_LIBS "Build using shared libraries" OFF)

include(FetchContent)

FetchContent_Declare(
  mdns
  GIT_REPOSITORY https://github.com/mjansson/mdns.git
  GIT_TAG        1727be0602941a714cb6048a737f0584b1cebf3c
  SOURCE_DIR src/ext/mdns
)
FetchContent_MakeAvailable(mdns)

add_subdirectory(src/ext/sgct)
add_subdirectory(src/ext/libndi)
add_subdirectory(src/app)
