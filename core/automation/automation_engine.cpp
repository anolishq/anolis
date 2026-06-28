#include "automation/automation_engine.hpp"

namespace anolis {
namespace automation {

const char* to_string(AutomationStatus status) {
    switch (status) {
        case AutomationStatus::Idle:
            return "idle";
        case AutomationStatus::Running:
            return "running";
        case AutomationStatus::Blocked:
            return "blocked";
        case AutomationStatus::Failed:
            return "failed";
        case AutomationStatus::Completed:
            return "completed";
        case AutomationStatus::Unknown:
            return "unknown";
    }
    return "unknown";
}

}  // namespace automation
}  // namespace anolis
