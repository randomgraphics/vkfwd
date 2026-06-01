#pragma once

#include "transport_session.hpp"

#include <memory>

namespace vkfwd::loopback {

// Creates a shared in-process transport session for simulator use. The session
// does not perform receiver replay; it returns a contiguous copy of the incoming
// request stream so callers can validate forwarder packing and stream routing.
std::shared_ptr<TransportSession> make_loopback_transport_session();

} // namespace vkfwd::loopback
