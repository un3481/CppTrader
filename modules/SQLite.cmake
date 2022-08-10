
cmake_minimum_required (VERSION 3.20)
project (Tutorial)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

add_executable(tutorial new.cpp)
find_package (SQLite3)

if (SQLITE3_FOUND)
  include_directories(${SQLITE3_INCLUDE_DIRS})
  target_link_libraries (tutorial ${SQLITE3_LIBRARIES})
endif (SQLITE3_FOUND)