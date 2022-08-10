
find_package (SQLite3)

if (SQLITE3_FOUND)
  include_directories(${SQLITE3_INCLUDE_DIRS})
  target_link_libraries (cpptrader ${SQLITE3_LIBRARIES})
endif (SQLITE3_FOUND)