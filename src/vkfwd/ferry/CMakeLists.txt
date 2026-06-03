# Keep module target definitions beside their implementation contracts. Core
# comes first because both receiver and forwarder link against the shared ferry
# wire-format and serialization substrate.
add_subdirectory(core)
add_subdirectory(receiver)
add_subdirectory(forwarder)

if(NOT ANDROID)
  # The sample currently exercises the desktop GLFW surface path. Keep it out
  # of Android builds until it has a native-window entry point instead of
  # pretending the desktop swapchain setup is portable.
  add_subdirectory(sample)
endif()
