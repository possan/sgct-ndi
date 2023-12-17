cmake_minimum_required(VERSION 3.25 FATAL_ERROR)
cmake_policy(VERSION 3.25)

project(sgctndi)

include(../ext/sgct/support/cmake/set_compile_options.cmake)
 
# find_package(ndi REQUIRED)
# find_package(sgct REQUIRED)
find_package(libpng)
find_package(png)
# find_package(mdns REQUIRED)
# find_package(PkgConfig)
# pkg_check_modules (AVCODEC REQUIRED IMPORTED_TARGET libavcodec>=58.0)
# pkg_check_modules (AVFORMAT REQUIRED IMPORTED_TARGET libavformat>=58.0)
# pkg_check_modules (AVUTIL REQUIRED IMPORTED_TARGET libavutil>=58.0)
# pkg_check_modules (SWSCALE REQUIRED IMPORTED_TARGET libswscale>=7.0)
find_package(FFMPEG REQUIRED)

# include_directories(${AVCODEC_INCLUDE_DIRS})

add_executable(sgctndi main.cpp)
set_compile_options(sgctndi)

target_compile_features(sgctndi PRIVATE cxx_std_20)
target_include_directories(sgctndi PUBLIC ../ext/libndi/include)

target_link_libraries(sgctndi PRIVATE ndi)
target_link_libraries(sgctndi PRIVATE sgct)
target_link_libraries(sgctndi PRIVATE mdns)

target_include_directories(sgctndi PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(sgctndi PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(sgctndi PRIVATE ${FFMPEG_LIBRARIES})

#target_link_libraries(sgctndi PRIVATE ${AVCODEC_LIBRARIES})
#target_link_libraries(sgctndi PRIVATE ${AVFORMAT_LIBRARIES})
#target_link_libraries(sgctndi PRIVATE ${AVUTIL_LIBRARIES})
#target_link_libraries(sgctndi PRIVATE ${SWSCALE_LIBRARIES})
#target_compile_options(sgctndi PRIVATE ${AVCODEC_CFLAGS})
#target_compile_options(sgctndi PRIVATE ${AVFORMAT_CFLAGS})
#target_compile_options(sgctndi PRIVATE ${AVUTIL_CFLAGS})
#target_compile_options(sgctndi PRIVATE ${SWSCALE_CFLAGS})
#target_link_directories(sgctndi PRIVATE ${AVCODEC_LIBRARY_DIRS})
#target_link_directories(sgctndi PRIVATE ${AVFORMAT_LIBRARY_DIRS})
#target_link_directories(sgctndi PRIVATE ${AVUTIL_LIBRARY_DIRS})
#target_link_directories(sgctndi PRIVATE ${SWSCALE_LIBRARY_DIRS})

target_include_directories(sgctndi PRIVATE ${FETCHCONTENT_BASE_DIR}/mdns-src/) # FIXME

# FIXME...
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_link_libraries(sgctndi PRIVATE ${PNG_LIBRARY_DEBUG})
else()
  target_link_libraries(sgctndi PRIVATE ${PNG_LIBRARY_RELEASE})
endif()


add_custom_command(
  TARGET sgctndi POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${CMAKE_CURRENT_SOURCE_DIR}/empty_y.png"
  "${CMAKE_CURRENT_SOURCE_DIR}/empty_u.png"
  "${CMAKE_CURRENT_SOURCE_DIR}/empty_v.png"
  "${CMAKE_CURRENT_SOURCE_DIR}/single_node.xml"
  "${CMAKE_CURRENT_SOURCE_DIR}/two_nodes.xml"
  "${CMAKE_CURRENT_SOURCE_DIR}/two_nodes_stereo_dummy.xml"
  "${CMAKE_CURRENT_SOURCE_DIR}/multi_viewport.xml"
  $<TARGET_FILE_DIR:sgctndi>
)

# if (WIN32 AND $<TARGET_RUNTIME_DLLS:sgctndi>)
#   add_custom_command(TARGET sgctndi POST_BUILD
#     COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:sgctndi> $<TARGET_FILE_DIR:sgctndi>
#     COMMAND_EXPAND_LISTS
#   )
# endif ()
