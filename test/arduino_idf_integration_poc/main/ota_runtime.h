#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

#include "live_fishing_controller.h"

namespace paxg {
namespace ota {

enum class OtaState : uint8_t {
    Idle = 0,
    MaintenanceRequested,
    MaintenanceActive,
    Writing,
    Verifying,
    PendingReboot,
    Failed
};

inline const char* getOtaStateName(OtaState state) {
    switch (state) {
        case OtaState::Idle: return "IDLE";
        case OtaState::MaintenanceRequested: return "MAINT_REQUESTED";
        case OtaState::MaintenanceActive: return "MAINT_ACTIVE";
        case OtaState::Writing: return "WRITING";
        case OtaState::Verifying: return "VERIFYING";
        case OtaState::PendingReboot: return "PENDING_REBOOT";
        case OtaState::Failed: return "FAILED";
        default: return "UNKNOWN";
    }
}

struct OtaProgress {
    OtaState state = OtaState::Idle;
    size_t bytesWritten = 0;
    size_t totalBytes = 0;
    uint8_t percent = 0;
    char statusMessage[48] = {};
    char failureReason[48] = {};
    uint32_t lastUpdateMs = 0;
};

struct MemoryMetrics {
    size_t freeInternalHeap = 0;
    size_t minFreeInternalHeap = 0;
    size_t psramTotal = 0;
    size_t psramFree = 0;
    bool psramAvailable = false;
};

class OtaRuntime {
public:
    bool requestMaintenance(const char* reason = "HTTP_OTA_REQUEST");
    void cancelMaintenanceRequest();
    bool activateMaintenance(uint32_t nowMs);
    void updateProgress(size_t written, size_t total, uint32_t nowMs);
    void leaveMaintenance(bool success, uint32_t nowMs);
    void requestRestart(uint32_t nowMs);
    bool restartDue(uint32_t nowMs) const;
    void fail(const char* reason, uint32_t nowMs);
    void exitMaintenance(uint32_t nowMs);

    OtaState getState() const;
    bool isMaintenanceActive() const;
    bool isUacAnalysisPaused() const;
    bool hasFailed() const;
    OtaProgress getProgress() const;
    MemoryMetrics getMemoryMetrics() const;

    void pollOwnerLoop(uint32_t nowMs,
                       paxg::fishing::LiveFishingController& controller,
                       paxg::fishing::IHidKeyboard& hid,
                       bool usbActuallyMounted);
    void reset();

private:
    mutable std::mutex mutex_;
    OtaProgress progress_{};
    bool releaseReportSent_ = false;
    uint32_t restartAtMs_ = 0;
};

OtaRuntime& getOtaRuntime();

} // namespace ota
} // namespace paxg
