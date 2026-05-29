add_test(
  NAME vkfwd_structure
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/smoke_structure.sh ${PROJECT_SOURCE_DIR})

add_test(
  NAME vkfwd_generated_metadata
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/generated_metadata.sh ${PROJECT_SOURCE_DIR})
