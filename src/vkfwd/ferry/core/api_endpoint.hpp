#pragma once

#include "blob.hpp"

namespace vkfwd {

class ApiEndpoint {
public:
  virtual ~ApiEndpoint() = default;

  // Flush is the synchronous boundary for a per-thread stream of packed calls.
  // request_blob may contain multiple deferrable command chunks followed by the
  // non-deferrable command that forced the flush. Endpoint implementations own
  // transport, replay, handle mapping, and any shared channel coordination. The
  // returned blob contains the response chunk for the last API call in that
  // packed stream.
  virtual Blob flush(Blob& request_blob) = 0;
};

} // namespace vkfwd
