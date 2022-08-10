
if(NOT TARGET sqlite3)

  # Module library
  file(GLOB SOURCE_FILES "SQLite/sqlite3.c")
  add_library(sqlite3 ${SOURCE_FILES})

  set(INCLUDE_DIRS "SQLite")
  include_directories(${INCLUDE_DIRS})

endif()
