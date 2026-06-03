#pragma once

#include "forwarder.hpp"
#include "logging.hpp"
#include "receiver_session.hpp"
#include "receiver.hpp"
#include "transport_session.hpp"

#if defined(_WIN32)
    #include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
    #include <dlfcn.h>
#endif

#include <memory>
#include <stdexcept>
#include <utility>

namespace vkfwd {

struct HostLoader {
    void *                    library = nullptr;
    PFN_vkGetInstanceProcAddr gipa    = nullptr;

    HostLoader()                               = default;
    HostLoader(const HostLoader &)             = delete;
    HostLoader & operator=(const HostLoader &) = delete;

    HostLoader(HostLoader && other) noexcept: library(other.library), gipa(other.gipa) {
        other.library = nullptr;
        other.gipa    = nullptr;
    }

    HostLoader & operator=(HostLoader && other) noexcept {
        if (this == &other) { return *this; }
#if defined(_WIN32)
        if (library) { FreeLibrary(static_cast<HMODULE>(library)); }
#elif defined(__unix__) || defined(__APPLE__)
        if (library) { dlclose(library); }
#endif
        library       = other.library;
        gipa          = other.gipa;
        other.library = nullptr;
        other.gipa    = nullptr;
        return *this;
    }

    ~HostLoader() {
#if defined(_WIN32)
        if (library) { FreeLibrary(static_cast<HMODULE>(library)); }
#elif defined(__unix__) || defined(__APPLE__)
        if (library) { dlclose(library); }
#endif
    }
};

// Load the platform Vulkan loader for receiver-side replay. The forwarder
// intentionally uses vkfwd entry points, while replay needs the host loader's
// vkGetInstanceProcAddr to resolve destination driver commands.
inline HostLoader load_host_loader() {
    HostLoader loader;
#if defined(__linux__)
    loader.library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!loader.library) { loader.library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL); }
    if (loader.library) { loader.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader.library, "vkGetInstanceProcAddr")); }
#elif defined(__APPLE__)
    loader.library = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!loader.library) { loader.library = dlopen("libvulkan.dylib", RTLD_NOW | RTLD_LOCAL); }
    if (loader.library) { loader.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader.library, "vkGetInstanceProcAddr")); }
#elif defined(_WIN32)
    loader.library = LoadLibraryA("vulkan-1.dll");
    if (loader.library) {
        loader.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(static_cast<HMODULE>(loader.library), "vkGetInstanceProcAddr"));
    }
#endif
    return loader;
}

struct LoopbackSession {
    std::shared_ptr<TransportSession> transport;
    std::unique_ptr<ReceiverSession>  receiver;

    // Creates paired in-process transport and receiver sessions. The transport
    // flattens each source-thread request stream before handing it to the
    // receiver, preserving the real forwarding lifetime boundary without
    // requiring a socket, process, or platform IPC backend.
    static LoopbackSession create();
};

namespace sample::detail {

class LoopbackReceiverSession final : public ReceiverSession {
public:
    void register_api_responder_factory(ApiResponderFactory factory) override { factory_ = std::move(factory); }

    CommandStream receive_accumulated_api_calls(const CommandStream & request_stream) {
        if (!responder_) {
            if (!factory_) {
                VKFWD_LOG_ERROR("vkfwd loopback receiver has no API responder factory");
                return {};
            }
            responder_ = factory_();
        }
        if (!responder_) {
            VKFWD_LOG_ERROR("vkfwd loopback receiver factory returned no API responder");
            return {};
        }
        return responder_->receive_accumulated_api_calls(request_stream);
    }

private:
    ApiResponderFactory           factory_;
    std::unique_ptr<ApiResponder> responder_;
};

class LoopbackTransportSession final : public TransportSession {
public:
    explicit LoopbackTransportSession(LoopbackReceiverSession & receiver): receiver_(receiver) {}

    CommandStream send_accumulated_api_calls(CommandStream & request) override {
        // Loopback is still a transport boundary: both directions should expose
        // contiguous serialized bytes, not either side's grow-only arena chunks.
        CommandStream flattened_request = request.flatten();
        CommandStream receiver_response = receiver_.receive_accumulated_api_calls(flattened_request);
        return receiver_response.flatten();
    }

private:
    LoopbackReceiverSession & receiver_;
};

} // namespace sample::detail

inline LoopbackSession LoopbackSession::create() {
    LoopbackSession session;
    auto            receiver  = std::make_unique<sample::detail::LoopbackReceiverSession>();
    auto            transport = std::make_shared<sample::detail::LoopbackTransportSession>(*receiver);
    session.receiver          = std::move(receiver);
    session.transport         = std::move(transport);
    return session;
}

} // namespace vkfwd

namespace vkfwd::sample {

struct VkfwdLoopbackRuntime {
    HostLoader                         host_loader;
    receiver::ReplayContext            replay_context;
    LoopbackSession                    loopback;
    std::unique_ptr<::vkfwd::Receiver> receiver;

    explicit VkfwdLoopbackRuntime(PFN_vkGetInstanceProcAddr replay_get_instance_proc_addr = nullptr) {
        if (!replay_get_instance_proc_addr) {
            host_loader                   = load_host_loader();
            replay_get_instance_proc_addr = host_loader.gipa;
        }
        if (!replay_get_instance_proc_addr) { throw std::runtime_error("Vulkan loader is not available for vkfwd receiver replay"); }

        // Receiver replay calls the host loader while the source application
        // talks only to vkfwd entry points. Handle remapping and broader Vulkan
        // coverage are still below the receiver boundary, so this runtime is a
        // loopback forwarding smoke path rather than complete Vulkan replay.
        replay_context.dispatch.global.init(replay_get_instance_proc_addr);
        loopback = LoopbackSession::create();
        receiver = std::make_unique<::vkfwd::Receiver>(*loopback.receiver, replay_context);

        // The lambda pins this in-process transport to the receiver above so
        // the forwarder exercises serialization and replay without depending
        // on an external transport backend.
        Forwarder::set_transport_creator([transport = loopback.transport] { return transport; });
        Forwarder::instance().reset_request_stream();
    }
};

} // namespace vkfwd::sample
