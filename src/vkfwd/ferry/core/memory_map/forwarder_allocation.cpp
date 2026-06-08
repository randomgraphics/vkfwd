#include "memory_map/forwarder_allocation.hpp"

// Translation unit exists so future virtual destructors / inline-defined
// methods have a single anchoring object file. The header has no out-of-line
// definitions today; this file intentionally has no content.

namespace vkfwd::memory_map {} // namespace vkfwd::memory_map
