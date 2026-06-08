#include "command_id_range.hpp"
#include "command_stream.hpp"
#include "custom_command.hpp"
#include "manual_dispatch.hpp"
#include "replay_context.hpp"

#include <catch2/catch_test_macros.hpp>

namespace vkfwd::receiver::test {

TEST_CASE("dispatch_manual_command returns false for unknown manual ids") {
    ::vkfwd::receiver::ReplayContext context;
    CommandStream                    request;
    CommandStream                    response;
    const Range                      range {.offset = 0, .size = 0};
    // Pick a reserved-range id that is not one of the three known manual ids.
    CHECK_FALSE(::vkfwd::receiver::dispatch_manual_command(static_cast<::vkfwd::manual::CommandId>(::vkfwd::kReservedCommandIdBase + 0xfffe), request, range,
                                                           response, context));
}

} // namespace vkfwd::receiver::test
