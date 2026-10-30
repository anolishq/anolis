#pragma once

#include <functional>
#include <string>

namespace anolis {
namespace state {
class StateCache;
}
namespace control {
class CallRouter;
}
namespace provider {
class ProviderRegistry;
}
namespace automation {

class ParameterManager;

// Typed blackboard payload consumed by all custom BT nodes.
struct BTServiceContext {
    state::StateCache* state_cache = nullptr;
    control::CallRouter* call_router = nullptr;
    provider::ProviderRegistry* provider_registry = nullptr;
    ParameterManager* parameter_manager = nullptr;
    /// Told each device call's outcome as `(device_handle, function, success,
    /// error)`. A non-empty return is a fault: the call has failed too often in
    /// a row, and the node throws it so the engine-fault path enters FAULT (#678).
    std::function<std::string(const std::string&, const std::string&, bool, const std::string&)> on_call_result;
};

inline constexpr const char* kBTServiceContextKey = "anolis.bt_service_context";

}  // namespace automation
}  // namespace anolis
