
if(NOT TARGET sqlite)

  # Module library
  file(GLOB SOURCE_FILES "SQLite/sqlite3.cpp")
  add_library(sqlite ${SOURCE_FILES})
  target_include_directories(sqlite PUBLIC "SQLite")

  # Module folder
  set_target_properties(sqlite PROPERTIES FOLDER "modules/SQLite")

endif()
