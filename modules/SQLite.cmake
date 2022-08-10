
# set (LIB_DIR_LOCATION ${NAME}/lib) 
# find_library(SQLite3 NAMES sqlite3 PATHS ${LIB_DIR_LOCATION}) 
# set(INCLUDE_DIRS ${NAME}/include)  
#      
# target_link_libraries(cpptrader SQLite3) 
# target_include_directories(cpptrader PRIVATE ${INCLUDE_DIRS}) 

find_package (SQLite3)

if (SQLITE3_FOUND)
  include_directories(${SQLITE3_INCLUDE_DIRS})
  target_link_libraries (cpptrader ${SQLITE3_LIBRARIES})
endif (SQLITE3_FOUND)