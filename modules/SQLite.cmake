
if(NOT TARGET sqlite)

  # Module library
  file(GLOB SOURCE_FILES "SQLite/sqlite3.c")
  add_library(sqlite ${SOURCE_FILES})
  if(MSVC)
    # C4244: 'conversion' conversion from 'type1' to 'type2', possible loss of data
    set_target_properties(sqlite PROPERTIES COMPILE_FLAGS "${PEDANTIC_COMPILE_FLAGS} /wd4244")
  else()
    set_target_properties(sqlite PROPERTIES COMPILE_FLAGS "${PEDANTIC_COMPILE_FLAGS}")
  endif()
  target_include_directories(sqlite PUBLIC "SQLite")

  # Module folder
  set_target_properties(sqlite PROPERTIES FOLDER "modules/SQLite")

endif()

# set (LIB_DIR_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/modules/SQLite")
# set(INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/modules/SQLite")
# 
# find_library(SQLite3 NAMES sqlite3 PATHS ${LIB_DIR_LOCATION})
# 
# target_link_libraries(sqlite SQLite3)
# target_include_directories(sqlite PRIVATE ${INCLUDE_DIRS})
