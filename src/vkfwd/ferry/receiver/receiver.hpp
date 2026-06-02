#pragma once

#include "receiver_session.hpp"
#include "replay_context.hpp"

namespace vkfwd {

class Receiver {
public:
    Receiver(ReceiverSession & session, receiver::ReplayContext & replay_context);

private:
    ReceiverSession &         session_;
    receiver::ReplayContext & replay_context_;
};

} // namespace vkfwd
