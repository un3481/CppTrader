
set (LIB_DIR_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/modules/SQLite")
set(INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/modules/SQLite")

find_library(SQLite3 NAMES sqlite3 PATHS ${LIB_DIR_LOCATION})

target_link_libraries(cpptrader SQLite3)
target_include_directories(cpptrader PRIVATE ${INCLUDE_DIRS})
