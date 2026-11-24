#pragma once

#include <cstdint>
#include <cstddef>
#include "live_fishing_controller.h"
#include "bag_full_detector.h"
#include "uac_audio_bridge.h"

namespace paxg {
namespace web {

/**
 * @brief Discrete command types sent from the network/web interface to the controller owner loop.
 */
enum class CommandType : uint8_t {
    None = 0,
    Stop,         // Priority Stop: immediate halt & disarm (highest priority)
    Start,        // Deliberate start fishing (safety-gated)
    ArmToggle,    // Deliberate arm/disarm toggle
    ResetBagFull  // Explicit bag-full latch and pulse reset
};

/**
 * @brief Result of command execution returned synchronously to caller.
 */
struct CommandOutcome {
    bool executed = false;
    bool success = false;
    paxg::fishing::FishingState state = paxg::fishing::FishingState::Idle;
    paxg::fishing::StopReason stopReason = paxg::fishing::StopReason::None;
    bool armed = false;
    char reason[32] = {0};
};

/**
 * @brief Thread-safe telemetry snapshot for web UI rendering and API serialization.
 */
struct WebServerStatus {
    bool armed = false;
    paxg::fishing::FishingState fsmState = paxg::fishing::FishingState::Idle;
    paxg::fishing::KeyState keyState = paxg::fishing::KeyState::Idle;
    paxg::fishing::StopReason stopReason = paxg::fishing::StopReason::None;
    uint32_t waitRemainingMs = 0;
    uint32_t totalCasts = 0;
    uint32_t totalBites = 0;
    uint32_t totalReels = 0;
    uint32_t totalTimeouts = 0;
    bool usbMounted = false;
    uint64_t audioPackets = 0;
    float audioRms = 0.0f;
    bool bagFull = false;
    bool bellContinuing = false;
    uint32_t bagIntervalMs = 0;
    uint32_t bagPulses = 0;
    uint32_t bagMatches = 0;
    char localIp[32] = {0};
};

/**
 * @brief Enqueue a command to the bounded thread-safe queue.
 *        Stop commands are placed in a dedicated priority slot and purge pending Start commands.
 */
bool enqueueCommand(CommandType type, uint32_t* outCmdId = nullptr);

/**
 * @brief Retrieve outcome of an executed command by its command ID.
 */
bool getCommandOutcome(uint32_t cmdId, CommandOutcome* outOutcome);

/**
 * @brief Synchronously submit a command from the HTTP server task to the controller owner loop.
 *        Blocks until the owner loop processes the command and returns the actual outcome.
 */
CommandOutcome submitCommandSync(CommandType type, uint32_t timeoutMs = 300);

/**
 * @brief Process pending commands from the queue in the controller owner loop.
 *        Strictly preserves Stop priority: Stop commands preempt any queued start/arm commands.
 */
void processPendingCommands(paxg::fishing::LiveFishingController* controller,
                            paxg::audio::BagFullDetector* bagFullDetector,
                            uint32_t nowMs);

/** Close start/arm admission and purge queued commands during OTA maintenance. */
void setMaintenanceActive(bool active);

/**
 * @brief Publish a thread-safe synchronized status snapshot from the controller owner loop.
 */
void publishStatusSnapshot(const paxg::fishing::LiveFishingController* controller,
                           const paxg::audio::BagFullDetector* bagFullDetector,
                           const paxg::uac::UacAudioBridge* bridge,
                           const char* ipOverride = nullptr);

/**
 * @brief Read the current published status snapshot (fully thread-safe, zero access to controller).
 */
WebServerStatus getPublishedStatusSnapshot();

/**
 * @brief Collects a coherent snapshot directly (backward compatibility / tests).
 */
WebServerStatus collectServerStatus(const paxg::fishing::LiveFishingController* controller,
                                    const paxg::audio::BagFullDetector* bagFullDetector,
                                    const paxg::uac::UacAudioBridge* bridge,
                                    const char* ipOverride = nullptr);

/**
 * @brief Serializes status snapshot to JSON string with bounded length.
 */
size_t buildStatusJson(const WebServerStatus& status, char* outBuf, size_t maxLen);

/**
 * @brief Returns the responsive HTML Web UI with status cards and deliberate start/stop controls.
 */
const char* getIndexHtml();

/**
 * @brief Returns the dedicated Wi-Fi Settings & Recovery HTML page.
 */
const char* getWifiHtml();

/**
 * @brief Start the local LAN HTTP web server on FreeRTOS task Core 0, priority 1.
 *        Guaranteed never to block or starve higher-priority USB audio tasks.
 */
bool startFishingWebServer(paxg::fishing::LiveFishingController* controller = nullptr,
                           paxg::audio::BagFullDetector* bagFullDetector = nullptr,
                           paxg::uac::UacAudioBridge* bridge = nullptr,
                           uint16_t port = 80);

/**
 * @brief Stop the web server and release resources.
 */
void stopFishingWebServer();

/**
 * @brief Check if web server is currently running.
 */
bool isFishingWebServerRunning();

} // namespace web
} // namespace paxg
