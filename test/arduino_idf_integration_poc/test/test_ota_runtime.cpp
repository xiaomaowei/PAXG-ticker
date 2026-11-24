#include <cassert>
#include <cstdio>
#include <cstring>

#include "ota_runtime.h"

using namespace paxg::ota;
using namespace paxg::fishing;

class MockHid final : public IHidKeyboard {
public:
    bool mounted = true;
    bool ready = true;
    int releases = 0;
    bool isMounted() override { return mounted; }
    bool isReady() override { return ready; }
    bool press(uint8_t, uint8_t) override { return true; }
    bool release() override { ++releases; return true; }
};

class MockClock final : public IClock {
public:
    uint32_t now = 100;
    uint32_t nowMs() override { return now; }
};

class MockAudio final : public IAudioStream {
public:
    uint64_t getPacketsReceived() const override { return 0; }
    uint64_t getDiscontinuities() const override { return 0; }
    uint32_t getQueueDrops() const override { return 0; }
    bool isTriggered() const override { return false; }
    void clearTrigger() override {}
    float getRms() const override { return 0; }
};

static void testMaintenanceAndHidReleaseHandshake() {
    OtaRuntime runtime;
    MockHid hid;
    MockClock clock;
    MockAudio audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    assert(runtime.requestMaintenance());
    assert(runtime.getState() == OtaState::MaintenanceRequested);
    assert(runtime.isUacAnalysisPaused());

    runtime.pollOwnerLoop(clock.now, controller, hid, true);
    assert(runtime.getState() == OtaState::MaintenanceRequested);
    assert(hid.releases == 1); // Keys-up was submitted, wait another endpoint-ready tick.
    runtime.pollOwnerLoop(++clock.now, controller, hid, true);
    assert(runtime.getState() == OtaState::MaintenanceActive);
    assert(!controller.isArmed());
}

static void testDetachedHostCanEnterMaintenance() {
    OtaRuntime runtime;
    MockHid hid;
    hid.mounted = false;
    MockClock clock;
    MockAudio audio;
    LiveFishingController controller(&hid, &clock, &audio);
    controller.init();
    assert(runtime.requestMaintenance());
    runtime.pollOwnerLoop(clock.now, controller, hid, false);
    assert(runtime.getState() == OtaState::MaintenanceActive);
    assert(hid.releases == 0);
}

static void testProgressFailureAndRecoveryState() {
    OtaRuntime runtime;
    assert(runtime.requestMaintenance());
    assert(runtime.activateMaintenance(10));
    runtime.updateProgress(250, 1000, 20);
    const OtaProgress progress = runtime.getProgress();
    assert(progress.state == OtaState::Writing);
    assert(progress.bytesWritten == 250 && progress.totalBytes == 1000);
    assert(progress.percent == 25);

    runtime.leaveMaintenance(false, 30);
    assert(runtime.hasFailed());
    runtime.fail("UPLOAD_ABORTED", 31);
    assert(std::strcmp(runtime.getProgress().failureReason, "UPLOAD_ABORTED") == 0);
    runtime.exitMaintenance(40);
    assert(runtime.getState() == OtaState::Idle);
    assert(!runtime.isMaintenanceActive());
}

static void testDeferredRestartIsWrapSafe() {
    OtaRuntime runtime;
    runtime.requestRestart(0xFFFFFF00u);
    assert(!runtime.restartDue(0x00000600u));
    assert(runtime.restartDue(0x00000700u));
    assert(runtime.isMaintenanceActive()); // Hold remains until restart.
}

int main() {
    testMaintenanceAndHidReleaseHandshake();
    testDetachedHostCanEnterMaintenance();
    testProgressFailureAndRecoveryState();
    testDeferredRestartIsWrapSafe();
    std::puts("OTA runtime: 4/4 tests passed");
    return 0;
}
