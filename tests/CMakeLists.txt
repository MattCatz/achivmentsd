add_executable(achd_unit unit.c)
target_link_libraries(achd_unit PRIVATE achd_core)
add_test(NAME unit COMMAND achd_unit)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME integration
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/integration.py $<TARGET_FILE:achivmentsd>)
endif()
