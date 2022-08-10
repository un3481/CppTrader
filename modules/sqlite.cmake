
if(NOT TARGET sqlite)

  # Module library
  file(GLOB SOURCE_FILES "sqlite/sqlite3.c")
  add_library(sqlite ${SOURCE_FILES})
  target_include_directories(sqlite PUBLIC "sqlite")

  # Module folder
  set_target_properties(sqlite PROPERTIES FOLDER "modules/sqlite")

endif()