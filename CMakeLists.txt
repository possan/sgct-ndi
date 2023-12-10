cmake_minimum_required(VERSION 3.25 FATAL_ERROR)
cmake_policy(VERSION 3.25)

project(sgctndi)

add_subdirectory(src/ext/sgct)
add_subdirectory(src/ext/libndi)

add_executable(sgctndi src/main.cpp)
set_compile_options(sgctndi)

target_link_libraries(sgctndi PRIVATE ndi)
target_link_libraries(sgctndi PRIVATE sgct::sgct)

# set_compile_options(sgctndi)

# target_link_libraries()
# add_library(
#   sgctndi
# 	avcodec
# 	avformat
# 	avutil
# )

# add_library(ndi
# 	src/recv.c
# 	src/scramble.c
# 	src/codec.c
# 	${NDI_SOURCES}
# )

find_package(PkgConfig)
pkg_check_modules (AVCODEC REQUIRED libavcodec>=58.0)
pkg_check_modules (AVFORMAT REQUIRED libavformat>=58.0)
pkg_check_modules (AVUTIL REQUIRED libavutil>=58.0)

target_include_directories(sgctndi PRIVATE ${AVCODEC_INCLUDE_DIRS})
target_include_directories(sgctndi PRIVATE ${AVFORMAT_INCLUDE_DIRS})
target_include_directories(sgctndi PRIVATE ${AVUTIL_INCLUDE_DIRS})

target_compile_options(sgctndi PRIVATE ${AVCODEC_CFLAGS_OTHER})
target_compile_options(sgctndi PRIVATE ${AVFORMAT_CFLAGS_OTHER})
target_compile_options(sgctndi PRIVATE ${AVUTIL_CFLAGS_OTHER})

target_link_libraries(sgctndi PRIVATE ${AVCODEC_LINK_LIBRARIES})
target_link_libraries(sgctndi PRIVATE ${AVFORMAT_LINK_LIBRARIES})
target_link_libraries(sgctndi PRIVATE ${AVUTIL_LINK_LIBRARIES})

target_include_directories(sgctndi PRIVATE src/ext/libndi/include)
target_include_directories(sgctndi PRIVATE src/ext/sgct/ext/glm)

# target_link_libraries(sgctndi PRIVATE ${NDI_LINK_LIBRARIES})

# add_custom_command(
#   TARGET sgctndi POST_BUILD
#   COMMAND ${CMAKE_COMMAND} -E copy_if_different
#   "${CMAKE_CURRENT_SOURCE_DIR}/two_nodes.xml"
#   "${CMAKE_CURRENT_SOURCE_DIR}/two_nodes_stereo_dummy.xml"

#   $<TARGET_FILE_DIR:sgctndi>
# )
# set_property(TARGET sgctndi PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:sgctndi>)
# set_target_properties(sgctndi PROPERTIES FOLDER "Examples")

# if (WIN32 AND $<TARGET_RUNTIME_DLLS:sgctndi>)
#   add_custom_command(TARGET sgctndi POST_BUILD
#     COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:sgctndi> $<TARGET_FILE_DIR:sgctndi>
#     COMMAND_EXPAND_LISTS
#   )
# endif ()
