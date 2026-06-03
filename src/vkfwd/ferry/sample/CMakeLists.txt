add_executable(triangle triangle.cpp)
target_include_directories(triangle PRIVATE "${PROJECT_SOURCE_DIR}/src/third_party/rapid-vulkan/inc")
target_compile_definitions(triangle PRIVATE
    $<$<CONFIG:Debug>:RAPID_VULKAN_ENABLE_DEBUG_BUILD=1>
    RAPID_VULKAN_ENABLE_GLFW3=1)
target_link_libraries(triangle PRIVATE
    vkfwd_ferry_core
    vkfwd_ferry_receiver
    vkfwd_ferry_forwarder
    glfw)
