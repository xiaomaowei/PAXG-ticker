#include "ota_runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#include <esp_system.h>
#endif

namespace paxg {
namespace ota {

namespace {
constexpr uint32_t kRestartDelayMs = 2000;
}

bool OtaRuntime::requestMaintenance(const char* reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (progress_.state != OtaState::Idle && progress_.state != OtaState::Failed) return false;
    progress_ = {};
    progress_.state = OtaState::MaintenanceRequested;
    std::snprintf(progress_.statusMessage, sizeof(progress_.statusMessage), "%s",
                  (reason && reason[0]) ? reason : "MAINT_REQUESTED");
    releaseReportSent_ = false;
    return true;
}

void OtaRuntime::cancelMaintenanceRequest() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (progress_.state == OtaState::MaintenanceRequested) {
        progress_.state = OtaState::Idle;
        progress_.statusMessage[0] = '\0';
    }
}

bool OtaRuntime::activateMaintenance(uint32_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (progress_.state != OtaState::MaintenanceRequested) return false;
    progress_.state = OtaState::MaintenanceActive;
    progress_.lastUpdateMs = nowMs;
    std::snprintf(progress_.statusMessage, sizeof(progress_.statusMessage), "MAINTENANCE_ACTIVE");
    return true;
}

void OtaRuntime::updateProgress(size_t written, size_t total, uint32_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (progress_.state == OtaState::PendingReboot || progress_.state == OtaState::Failed) return;
    progress_.state = OtaState::Writing;
    progress_.bytesWritten = written;
    progress_.totalBytes = total;
    progress_.percent = total ? static_cast<uint8_t>(std::min<size_t>(100, written * 100 / total)) : 0;
    progress_.lastUpdateMs = nowMs;
    std::snprintf(progress_.statusMessage, sizeof(progress_.statusMessage), "WRITING_%u%%",
                  static_cast<unsigned>(progress_.percent));
}

void OtaRuntime::leaveMaintenance(bool success, uint32_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (progress_.state == OtaState::PendingReboot) return;
    progress_.lastUpdateMs = nowMs;
    if (success) {
        progress_.state = OtaState::Verifying;
        std::snprintf(progress_.statusMessage, sizeof(progress_.statusMessage), "IMAGE_VERIFIED");
    } else {
        progress_.state = OtaState::Failed;
        std::snprintf(progress_.statusMessage, sizeof(progress_.statusMessage), "FAILED");
        if (!progress_.failureReason[0]) {
            std::snprintf(progress_.failureReason, sizeof(progress_.failureReason), "UPLOAD_ABORTED");
        }
    }
}

void OtaRuntime::requestRestart(uint32_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    progress_.state = OtaState::PendingReboot;
    progress_.percent = 100;
    progress_.lastUpdateMs = nowMs;
    restartAtMs_ = nowMs + kRestartDelayMs;
    std::snprintf(progress_.statusMessage, sizeof(progress_.statusMessage), "RESTART_SCHEDULED");
}

bool OtaRuntime::restartDue(uint32_t nowMs) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_.state == OtaState::PendingReboot &&
           static_cast<int32_t>(nowMs - restartAtMs_) >= 0;
}

void OtaRuntime::fail(const char* reason, uint32_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    progress_.state = OtaState::Failed;
    progress_.lastUpdateMs = nowMs;
    std::snprintf(progress_.statusMessage, sizeof(progress_.statusMessage), "FAILED");
    std::snprintf(progress_.failureReason, sizeof(progress_.failureReason), "%s",
                  (reason && reason[0]) ? reason : "UNKNOWN_FAILURE");
}

void OtaRuntime::exitMaintenance(uint32_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (progress_.state == OtaState::PendingReboot) return;
    const char failure[sizeof(progress_.failureReason)] = "";
    progress_.state = OtaState::Idle;
    progress_.bytesWritten = 0;
    progress_.totalBytes = 0;
    progress_.percent = 0;
    progress_.lastUpdateMs = nowMs;
    std::memcpy(progress_.failureReason, failure, sizeof(failure));
    std::snprintf(progress_.statusMessage, sizeof(progress_.statusMessage), "DISARMED_READY");
    releaseReportSent_ = false;
}

OtaState OtaRuntime::getState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_.state;
}

bool OtaRuntime::isMaintenanceActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_.state == OtaState::MaintenanceRequested ||
           progress_.state == OtaState::MaintenanceActive ||
           progress_.state == OtaState::Writing ||
           progress_.state == OtaState::Verifying ||
           progress_.state == OtaState::PendingReboot;
}

bool OtaRuntime::isUacAnalysisPaused() const { return isMaintenanceActive(); }

bool OtaRuntime::hasFailed() const { return getState() == OtaState::Failed; }

OtaProgress OtaRuntime::getProgress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_;
}

MemoryMetrics OtaRuntime::getMemoryMetrics() const {
    MemoryMetrics metrics{};
#if defined(ARDUINO) || defined(ESP_PLATFORM)
    metrics.freeInternalHeap = esp_get_free_internal_heap_size();
    metrics.minFreeInternalHeap = esp_get_minimum_free_heap_size();
    metrics.psramTotal = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    metrics.psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    metrics.psramAvailable = metrics.psramTotal > 0;
#else
    metrics.freeInternalHeap = 260000;
    metrics.minFreeInternalHeap = 210000;
    metrics.psramAvailable = true;
    metrics.psramTotal = 8388608;
    metrics.psramFree = 8388608;
#endif
    return metrics;
}

void OtaRuntime::pollOwnerLoop(uint32_t nowMs,
                               paxg::fishing::LiveFishingController& controller,
                               paxg::fishing::IHidKeyboard& hid,
                               bool usbActuallyMounted) {
    const OtaState state = getState();
    if (state == OtaState::MaintenanceRequested) {
        controller.requestStop(paxg::fishing::StopReason::UserStopped);
        controller.disarm();
        controller.update(false, false);
        controller.updateKeypress(nowMs);
        if (usbActuallyMounted && controller.getKeyState() != paxg::fishing::KeyState::Idle) return;

        if (!usbActuallyMounted) {
            activateMaintenance(nowMs);
            return;
        }
        if (!hid.isReady()) return; // Suspended or endpoint busy: bounded begin hook will reject safely.
        if (!releaseReportSent_) {
            if (!hid.release()) return;
            releaseReportSent_ = true;
            return;
        }
        if (hid.isReady()) activateMaintenance(nowMs); // Endpoint has accepted keys-up and is free again.
    } else if (state == OtaState::Failed) {
        controller.requestStop(paxg::fishing::StopReason::UserStopped);
        controller.disarm();
        controller.update(false, false);
        controller.updateKeypress(nowMs);
    }
}

void OtaRuntime::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    progress_ = {};
    releaseReportSent_ = false;
    restartAtMs_ = 0;
}

OtaRuntime& getOtaRuntime() {
    static OtaRuntime instance;
    return instance;
}

} // namespace ota
} // namespace paxg
