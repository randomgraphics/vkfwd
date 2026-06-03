#include "loopback_session.hpp"

#include "forwarder.hpp"
#include "receiver.hpp"

#include <rapid-vulkan/rapid-vulkan.h>

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
    #include <dlfcn.h>
#endif

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char * name);

namespace vkfwd::test {
namespace {

class LoopbackTransportCreator {
public:
    explicit LoopbackTransportCreator(std::shared_ptr<TransportSession> transport): transport_(std::move(transport)) {}

    std::shared_ptr<TransportSession> operator()() const {
        // The rapid-vulkan hooked path uses vkfwd's loader-facing getprocaddr
        // exclusively. This creator only binds those generated entry points to
        // the paired in-process receiver transport for this test.
        return transport_;
    }

private:
    std::shared_ptr<TransportSession> transport_;
};

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
#if defined(__unix__) || defined(__APPLE__)
        if (library) { dlclose(library); }
#endif
        library       = other.library;
        gipa          = other.gipa;
        other.library = nullptr;
        other.gipa    = nullptr;
        return *this;
    }

    ~HostLoader() {
#if defined(__unix__) || defined(__APPLE__)
        if (library) { dlclose(library); }
#endif
    }
};

HostLoader load_host_loader() {
    HostLoader loader;
#if defined(__linux__)
    loader.library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!loader.library) { loader.library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL); }
    if (!loader.library) { return loader; }
    loader.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader.library, "vkGetInstanceProcAddr"));
#elif defined(__APPLE__)
    loader.library = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!loader.library) { loader.library = dlopen("libvulkan.dylib", RTLD_NOW | RTLD_LOCAL); }
    if (!loader.library) { return loader; }
    loader.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader.library, "vkGetInstanceProcAddr"));
#endif
    return loader;
}

void create_and_destroy_rapid_vulkan_instance(PFN_vkGetInstanceProcAddr get_instance_proc_addr) {
    rapid_vulkan::Instance::ConstructParameters cp;
    cp.setValidation(rapid_vulkan::Instance::VALIDATION_DISABLED).setPrintVkInfo(rapid_vulkan::Device::SILENCE);
    cp.getInstanceProcAddr = get_instance_proc_addr;

    rapid_vulkan::Instance instance(cp);
    if (!instance.handle()) { throw std::runtime_error("rapid-vulkan created a null VkInstance"); }
}

} // namespace

TEST_CASE("rapid-vulkan instance smoke passes direct and with vkfwd loopback", "[loopback][rapid-vulkan][hardware]") {
    auto host_loader = load_host_loader();
    if (!host_loader.gipa) { SKIP("Vulkan loader is not available on this machine"); }

    try {
        create_and_destroy_rapid_vulkan_instance(nullptr);
    } catch (const std::exception & e) { SKIP(std::string("Direct rapid-vulkan instance creation is unavailable: ") + e.what()); }

    receiver::ReplayContext replay_context;
    replay_context.dispatch.global.init(host_loader.gipa);

    auto     loopback = LoopbackSession::create();
    Receiver receiver(*loopback.receiver, replay_context);

    Forwarder::set_transport_creator(LoopbackTransportCreator(loopback.transport));
    Forwarder::instance().reset_request_stream();

    create_and_destroy_rapid_vulkan_instance(::vkGetInstanceProcAddr);
}

} // namespace vkfwd::test
