#pragma once

#include "generated/dispatch_table.hpp"
#include "receiver_session.hpp"

namespace vkfwd {

class Receiver {
public:
    Receiver(ReceiverSession & session, const generated::DistributionTable & api_table);

private:
    ReceiverSession &                    session_;
    const generated::DistributionTable & api_table_;
};

} // namespace vkfwd
