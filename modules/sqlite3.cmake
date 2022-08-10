
if(NOT TARGET sqlite3)

  # Module library
  file(GLOB SOURCE_FILES "sqlite3/sqlite3.c")
  add_library(sqlite3 ${SOURCE_FILES})
  target_include_directories(sqlite3 PUBLIC "sqlite3")

  # Module folder
  set_target_properties(sqlite3 PROPERTIES FOLDER "modules/sqlite3")

endif()