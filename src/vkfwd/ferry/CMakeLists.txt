# Keep module target definitions beside their implementation contracts. Core
# comes first because both receiver and forwarder link against the shared ferry
# wire-format and serialization substrate.
add_subdirectory(core)
add_subdirectory(receiver)
add_subdirectory(forwarder)
