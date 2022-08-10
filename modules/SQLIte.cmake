if(NOT TARGET sqlite)

  # Module flag
  set(SQLITE_MODULE Y)

  # Module subdirectory
  add_subdirectory("SQLite")

  # Module folder
  set_target_properties(sqlite PROPERTIES FOLDER "modules/SQLite")

endif()