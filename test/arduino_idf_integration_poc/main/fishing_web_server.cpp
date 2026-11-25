#include "fishing_web_server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <chrono>

#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <esp_http_server.h>
#include <WiFi.h>
#include <esp_log.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "usb_device_composite.h"
#endif

#include "fishing_wifi_settings.h"
#include "audio_match.h"
#include <esp_system.h>

namespace paxg {
namespace web {

namespace {

// ----------------------------------------------------------------------------
// Bounded Command Queue with Priority Stop Slot
// ----------------------------------------------------------------------------
struct QueuedCommand {
    CommandType type = CommandType::None;
    uint32_t id = 0;
};

static constexpr size_t CMD_QUEUE_CAPACITY = 8;

struct CommandQueueState {
    std::mutex mutex;
    std::condition_variable cvResult;

    // Dedicated high-priority Stop slot (never dropped, executed before all else)
    QueuedCommand priorityStop;

    // Bounded FIFO queue for other commands (Start, ArmToggle, ResetBagFull)
    QueuedCommand queue[CMD_QUEUE_CAPACITY];
    size_t head = 0;
    size_t tail = 0;
    size_t count = 0;
    bool maintenanceActive = false;

    uint32_t nextCommandId = 1;

    // Outcome of the most recently processed command
    uint32_t completedCommandId = 0;
    CommandOutcome lastOutcome;
};

static CommandQueueState s_cmdQueue;

#if defined(ARDUINO) || defined(ESP_PLATFORM)
static bool isMaintenanceActive() {
    std::lock_guard<std::mutex> lock(s_cmdQueue.mutex);
    return s_cmdQueue.maintenanceActive;
}
#endif

// ----------------------------------------------------------------------------
// Synchronized Status Snapshot
// ----------------------------------------------------------------------------
static std::mutex s_snapshotMutex;
static WebServerStatus s_publishedSnapshot;

// ----------------------------------------------------------------------------
// Web Server Management
// ----------------------------------------------------------------------------
static std::mutex s_serverMutex;

#if defined(ARDUINO) || defined(ESP_PLATFORM)
static httpd_handle_t s_httpdHandle = nullptr;
#else
static bool s_mockRunning = false;
#endif

// Static HTML UI string stored in Flash (.rodata)
static const char INDEX_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">\n"
"<title>PAXG ESP32-S3 Fishing Controller</title>\n"
"<style>\n"
"body{background:#121212;color:#eee;font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,sans-serif;margin:0;padding:16px;text-align:center}\n"
".container{max-width:540px;margin:0 auto;background:#1e1e1e;border-radius:12px;padding:20px;box-shadow:0 4px 12px rgba(0,0,0,0.5)}\n"
"h1{margin:0 0 10px;font-size:20px;color:#ffd700;letter-spacing:1px}\n"
".ip-tag{font-size:13px;color:#888;margin-bottom:15px}\n"
".badge{display:inline-block;padding:8px 18px;border-radius:20px;font-weight:bold;font-size:16px;margin-bottom:15px;letter-spacing:1px}\n"
".badge-armed{background:#00c853;color:#000}\n"
".badge-disarmed{background:#d50000;color:#fff}\n"
".grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:15px}\n"
".card{background:#2a2a2a;padding:12px;border-radius:8px;text-align:left}\n"
".card .label{font-size:11px;color:#aaa;text-transform:uppercase}\n"
".card .val{font-size:18px;font-weight:bold;margin-top:4px;color:#fff}\n"
".alert-box{background:#b71c1c;color:#fff;padding:10px;border-radius:8px;font-weight:bold;margin-bottom:15px;display:none}\n"
".btn-row{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:15px}\n"
"button{padding:14px;border:none;border-radius:8px;font-size:15px;font-weight:bold;cursor:pointer;transition:background 0.2s}\n"
"button:active{opacity:0.8}\n"
".btn-arm{background:#00e676;color:#000}\n"
".btn-disarm{background:#ff9100;color:#000}\n"
".btn-start{background:#00b0ff;color:#000}\n"
".btn-stop{background:#ff1744;color:#fff}\n"
".btn-reset{background:#757575;color:#fff;grid-column:span 2;margin-top:5px;padding:10px}\n"
".footer{margin-top:20px;font-size:11px;color:#666}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"container\">\n"
"<h1>ESP32-S3 FISHING CONTROLLER</h1>\n"
"<div class=\"ip-tag\">LAN Node: <span id=\"dev-ip\">Connecting...</span> | Port 80</div>\n"
"<div style=\"margin:8px 0 14px\"><a href=\"/wifi\" style=\"display:inline-block;padding:6px 14px;background:#2a2a2a;border:1px solid #00b0ff;color:#00b0ff;border-radius:6px;font-size:12px;font-weight:bold;text-decoration:none\">&rarr; WI-FI SETTINGS &amp; RECOVERY</a></div>\n"
"<div id=\"arm-badge\" class=\"badge badge-disarmed\">DISARMED</div>\n"
"<div id=\"bag-alert\" class=\"alert-box\">STOP: BAG_FULL DETECTED (HALTED & DISARMED)</div>\n"
"<div class=\"grid\">\n"
"<div class=\"card\"><div class=\"label\">FSM State</div><div id=\"fsm-state\" class=\"val\">IDLE</div></div>\n"
"<div class=\"card\"><div class=\"label\">Stop Reason</div><div id=\"stop-reason\" class=\"val\">NONE</div></div>\n"
"<div class=\"card\"><div class=\"label\">Wait Countdown</div><div id=\"wait-sec\" class=\"val\">0s</div></div>\n"
"<div class=\"card\"><div class=\"label\">Casts / Bites / Reels</div><div id=\"counts\" class=\"val\">0 / 0 / 0</div></div>\n"
"<div class=\"card\"><div class=\"label\">USB Audio RMS</div><div id=\"audio-rms\" class=\"val\">0.0</div></div>\n"
"<div class=\"card\"><div class=\"label\">Bag Pulse Interval</div><div id=\"bag-interval\" class=\"val\">-- ms</div></div>\n"
"</div>\n"
"<div class=\"btn-row\">\n"
"<button id=\"btn-arm-toggle\" class=\"btn-arm\" onclick=\"apiPost('/api/arm')\">DELIBERATE ARM</button>\n"
"<button class=\"btn-start\" onclick=\"apiPost('/api/start')\">START FISHING</button>\n"
"<button class=\"btn-stop\" style=\"grid-column:span 2\" onclick=\"apiPost('/api/stop')\">EMERGENCY STOP (DISARM)</button>\n"
"<button class=\"btn-reset\" onclick=\"apiPost('/api/bagfull/reset')\">CLEAR / RESET BAG-FULL LATCH</button>\n"
"</div>\n"
"<div class=\"footer\">Strict Fail-Closed | USB & Audio Safe Gating Active</div>\n"
"</div>\n"
R"AUDIO(
<details id="audio-settings" style="margin:14px 0">
<summary>Audio detection settings</summary>
<form id="audio-form">
<label>Detection Mode <select id="audio-mode"><option value="0">Legacy Amplitude</option><option value="1">Spectral Matching (monitor only)</option></select></label>
<label>Similarity Threshold (%) <input id="audio-threshold" type="number" min="1" max="100" required></label>
<label><input type="checkbox" id="audio-debug"> Audio Debug Mode</label>
<fieldset><legend>Reference templates</legend>
<label><input type="checkbox" id="audio-t1"> Template 1</label>
<label><input type="checkbox" id="audio-t2"> Template 2</label>
<label><input type="checkbox" id="audio-t3"> Template 3</label>
</fieldset>
<button type="submit">Save audio settings</button>
<button type="button" id="audio-reset">Restore Default Settings</button>
</form>
<output id="audio-message" role="status"></output>
<pre id="audio-metrics" style="white-space:pre-wrap"></pre>
</details>
<script>
(() => {
  const $ = id => document.getElementById('audio-' + id);
  let initialized = false, polling = false, saving = false;
  function update(d, controls) {
    if (controls) {
      $('mode').value = d.mode;
      $('threshold').value = d.threshold;
      $('debug').checked = !!d.debug;
      [1,2,3].forEach(i => $('t'+i).checked = !!(d.mask & (1 << (i-1))));
      initialized = true;
    }
    const lines = [
      'Current RMS / peak  ' + d.rms.toFixed(4) + ' / ' + d.peak.toFixed(4),
      ...d.scores.map((score,i) => 'Template '+(i+1)+' similarity  '+(d.mode ? score.toFixed(3) : '—')),
      'Best matching template  ' + (d.mode && d.best ? d.best : '—'),
      'Detection event count  ' + d.events,
      'Audio buffer usage  ' + d.queued + ' / 4096 PCM frames',
      'Dropped audio frames  ' + d.dropped,
      'DSP processing time  ' + d.dsp_us + ' µs',
      'Free / minimum free heap  ' + d.heap + ' / ' + d.min_heap + ' bytes'
    ];
    if (d.debug) lines.push('Maximum DSP time  '+d.max_dsp_us+' µs', 'DSP stack low-water mark  '+d.stack_free+' bytes');
    if (d.mode && !d.running) lines.push('DSP task unavailable');
    $('metrics').textContent = lines.join('\n');
  }
  async function request(query) {
    const response = await fetch('/api/audio' + (query ? '?' + query : ''), {method:query ? 'POST' : 'GET'});
    if (!response.ok) throw Error(await response.text());
    return response.json();
  }
  async function poll() {
    if (!$('settings').open || saving || polling) return;
    polling = true;
    try {
      const result = await request();
      if (!saving) update(result, !initialized);
    } catch (_) {
      $('metrics').textContent = 'Audio telemetry unavailable';
    } finally { polling = false; }
  }
  async function save(query) {
    if (saving) return;
    saving = true;
    try { update(await request(query), true); $('message').textContent = 'Saved'; }
    catch (err) { $('message').textContent = err.message; }
    finally { saving = false; }
  }
  $('form').onsubmit = event => {
    event.preventDefault();
    const mask = [1,2,3].reduce((sum,i) => sum + ($('t'+i).checked ? 1 << (i-1) : 0), 0);
    save(new URLSearchParams({mode:$('mode').value,threshold:$('threshold').value,mask,debug:+$('debug').checked}));
  };
  $('reset').onclick = () => save('restore=1');
  $('settings').ontoggle = poll;
  setInterval(poll, 1000);
})();
</script>
)AUDIO"
"<script>\n"
"let statusPending=false;\n"
"function update(){\n"
"if(statusPending)return;statusPending=true;\n"
"fetch('/api/status',{signal:AbortSignal.timeout(5000)}).then(r=>{if(!r.ok)throw Error(r.status);return r.json();}).then(d=>{\n"
"document.getElementById('dev-ip').innerText=d.ip||'Online';\n"
"const b=document.getElementById('arm-badge');\n"
"const btnArm=document.getElementById('btn-arm-toggle');\n"
"if(d.armed){\n"
"b.className='badge badge-armed';b.innerText='ARMED - ACTIVE';\n"
"btnArm.className='btn-disarm';btnArm.innerText='DISARM';\n"
"}else{\n"
"b.className='badge badge-disarmed';b.innerText='DISARMED - SAFE';\n"
"btnArm.className='btn-arm';btnArm.innerText='DELIBERATE ARM';\n"
"}\n"
"document.getElementById('fsm-state').innerText=d.fsm;\n"
"document.getElementById('stop-reason').innerText=d.stop_reason;\n"
"document.getElementById('wait-sec').innerText=Math.ceil(d.wait_remaining_ms/1000)+'s';\n"
"document.getElementById('counts').innerText=d.casts+' / '+d.bites+' / '+d.reels;\n"
"document.getElementById('audio-rms').innerText=d.audio_rms.toFixed(1);\n"
"document.getElementById('bag-interval').innerText=d.bag_interval_ms>0?(d.bag_interval_ms+' ms'):'-- ms';\n"
"const alert=document.getElementById('bag-alert');\n"
"if(d.bag_full){\n"
"alert.style.display='block';alert.innerText='STOP: BAG_FULL DETECTED (HALTED & DISARMED)';\n"
"}else if(d.bell_continuing){\n"
"alert.style.display='block';alert.innerText='ALERT: ADDON BELL CONTINUING (START BLOCKED)';\n"
"}else{\n"
"alert.style.display='none';\n"
"}\n"
" }).catch(()=>{document.getElementById('dev-ip').innerText='Status unavailable';}).finally(()=>{statusPending=false;});\n"
"}\n"
"setInterval(update,1000);update();\n"
"function apiPost(u){\n"
"fetch(u,{method:'POST'}).then(r=>r.json()).then(d=>{\n"
"if(!d.success&&d.reason){\n"
"alert('Action Rejected: '+d.reason);\n"
"}\n"
"update();\n"
"}).catch(e=>{alert('Network request failed: '+e)});\n"
"}\n"
"</script>\n"
"</body>\n"
"</html>\n";

} // namespace

// ----------------------------------------------------------------------------
// Command Queue Implementation
// ----------------------------------------------------------------------------
bool enqueueCommand(CommandType type, uint32_t* outCmdId) {
    std::lock_guard<std::mutex> lock(s_cmdQueue.mutex);
    if (s_cmdQueue.maintenanceActive && type != CommandType::Stop) {
        return false;
    }
    uint32_t cmdId = s_cmdQueue.nextCommandId++;
    if (cmdId == 0) cmdId = s_cmdQueue.nextCommandId++;
    if (outCmdId) *outCmdId = cmdId;

    if (type == CommandType::Stop) {
        // High-priority Stop: assigned to dedicated slot and purges any pending Start commands
        s_cmdQueue.priorityStop.type = CommandType::Stop;
        s_cmdQueue.priorityStop.id = cmdId;

        // Purge queued start commands to enforce strict stop priority
        size_t writeIdx = 0;
        for (size_t i = 0; i < s_cmdQueue.count; ++i) {
            size_t idx = (s_cmdQueue.head + i) % CMD_QUEUE_CAPACITY;
            if (s_cmdQueue.queue[idx].type != CommandType::Start) {
                s_cmdQueue.queue[writeIdx++] = s_cmdQueue.queue[idx];
            }
        }
        s_cmdQueue.head = 0;
        s_cmdQueue.tail = writeIdx % CMD_QUEUE_CAPACITY;
        s_cmdQueue.count = writeIdx;
        return true;
    }

    if (s_cmdQueue.count >= CMD_QUEUE_CAPACITY) {
        return false;
    }

    s_cmdQueue.queue[s_cmdQueue.tail].type = type;
    s_cmdQueue.queue[s_cmdQueue.tail].id = cmdId;
    s_cmdQueue.tail = (s_cmdQueue.tail + 1) % CMD_QUEUE_CAPACITY;
    s_cmdQueue.count++;
    return true;
}

bool getCommandOutcome(uint32_t cmdId, CommandOutcome* outOutcome) {
    std::lock_guard<std::mutex> lock(s_cmdQueue.mutex);
    if (s_cmdQueue.completedCommandId == cmdId) {
        if (outOutcome) *outOutcome = s_cmdQueue.lastOutcome;
        return true;
    }
    return false;
}

CommandOutcome submitCommandSync(CommandType type, uint32_t timeoutMs) {
    uint32_t cmdId = 0;
    if (!enqueueCommand(type, &cmdId)) {
        CommandOutcome rejected;
        rejected.executed = true;
        rejected.success = false;
        std::strncpy(rejected.reason, "QUEUE_FULL", sizeof(rejected.reason));
        return rejected;
    }

    std::unique_lock<std::mutex> lock(s_cmdQueue.mutex);
    bool signaled = s_cmdQueue.cvResult.wait_for(
        lock,
        std::chrono::milliseconds(timeoutMs),
        [cmdId]() {
            return s_cmdQueue.completedCommandId == cmdId;
        }
    );

    if (!signaled) {
        CommandOutcome to;
        to.executed = false;
        to.success = false;
        std::strncpy(to.reason, "TIMEOUT", sizeof(to.reason));
        return to;
    }
    return s_cmdQueue.lastOutcome;
}

void processPendingCommands(paxg::fishing::LiveFishingController* controller,
                            paxg::audio::BagFullDetector* bagFullDetector,
                            uint32_t nowMs) {
    (void)nowMs;
    if (!controller) return;

    std::unique_lock<std::mutex> lock(s_cmdQueue.mutex);

    // 1. Process Priority Stop FIRST
    if (s_cmdQueue.priorityStop.type == CommandType::Stop) {
        const uint32_t cmdId = s_cmdQueue.priorityStop.id;
        s_cmdQueue.priorityStop.type = CommandType::None;

        // Drain any remaining commands to ensure stop takes absolute priority
        s_cmdQueue.head = 0;
        s_cmdQueue.tail = 0;
        s_cmdQueue.count = 0;

        controller->requestStop(paxg::fishing::StopReason::UserStopped);
        controller->disarm();

        CommandOutcome outcome;
        outcome.executed = true;
        outcome.success = true;
        outcome.state = controller->getState();
        outcome.stopReason = controller->getLastStopReason();
        outcome.armed = controller->isArmed();
        std::strncpy(outcome.reason, "USER_STOP", sizeof(outcome.reason));

        s_cmdQueue.completedCommandId = cmdId;
        s_cmdQueue.lastOutcome = outcome;
        lock.unlock();
        s_cmdQueue.cvResult.notify_all();
        return;
    }

    // 2. Process next queued command (Start, ArmToggle, ResetBagFull)
    if (s_cmdQueue.count > 0) {
        QueuedCommand cmd = s_cmdQueue.queue[s_cmdQueue.head];
        s_cmdQueue.head = (s_cmdQueue.head + 1) % CMD_QUEUE_CAPACITY;
        s_cmdQueue.count--;

        CommandOutcome outcome;
        outcome.executed = true;

        // If OTA maintenance is active, safely reject and drain start/arm commands
        if (s_cmdQueue.maintenanceActive) {
            if (cmd.type == CommandType::Start) {
                outcome.success = false;
                outcome.state = controller->getState();
                outcome.stopReason = controller->getLastStopReason();
                outcome.armed = controller->isArmed();
                std::strncpy(outcome.reason, "OTA_MAINTENANCE", sizeof(outcome.reason));
                s_cmdQueue.completedCommandId = cmd.id;
                s_cmdQueue.lastOutcome = outcome;
                lock.unlock();
                s_cmdQueue.cvResult.notify_all();
                return;
            } else if (cmd.type == CommandType::ArmToggle) {
                controller->disarm();
                outcome.success = false;
                outcome.armed = false;
                outcome.state = controller->getState();
                outcome.stopReason = controller->getLastStopReason();
                std::strncpy(outcome.reason, "OTA_MAINTENANCE", sizeof(outcome.reason));
                s_cmdQueue.completedCommandId = cmd.id;
                s_cmdQueue.lastOutcome = outcome;
                lock.unlock();
                s_cmdQueue.cvResult.notify_all();
                return;
            }
        }

        switch (cmd.type) {
            case CommandType::Start: {
                bool ok = controller->requestStart();
                outcome.success = ok;
                outcome.state = controller->getState();
                outcome.stopReason = controller->getLastStopReason();
                outcome.armed = controller->isArmed();
                if (!ok) {
                    if (bagFullDetector && bagFullDetector->isBagFull()) {
                        std::strncpy(outcome.reason, "BAG_FULL", sizeof(outcome.reason));
                    } else if (!controller->isArmed()) {
                        std::strncpy(outcome.reason, "DISARMED", sizeof(outcome.reason));
                    } else {
                        std::strncpy(outcome.reason, "SAFETY_REJECTED", sizeof(outcome.reason));
                    }
                } else {
                    std::strncpy(outcome.reason, "STARTED", sizeof(outcome.reason));
                }
                break;
            }
            case CommandType::ArmToggle: {
                bool armed = controller->requestArmToggle();
                outcome.success = true;
                outcome.armed = armed;
                outcome.state = controller->getState();
                outcome.stopReason = controller->getLastStopReason();
                std::strncpy(outcome.reason, armed ? "ARMED" : "DISARMED", sizeof(outcome.reason));
                break;
            }
            case CommandType::ResetBagFull: {
                if (bagFullDetector) {
                    bagFullDetector->clearBagFull();
                }
                outcome.success = true;
                outcome.state = controller->getState();
                outcome.stopReason = controller->getLastStopReason();
                outcome.armed = controller->isArmed();
                std::strncpy(outcome.reason, "BAG_RESET", sizeof(outcome.reason));
                break;
            }
            default:
                outcome.success = false;
                std::strncpy(outcome.reason, "UNKNOWN_CMD", sizeof(outcome.reason));
                break;
        }

        s_cmdQueue.completedCommandId = cmd.id;
        s_cmdQueue.lastOutcome = outcome;
        lock.unlock();
        s_cmdQueue.cvResult.notify_all();
    }
}

void setMaintenanceActive(bool active) {
    {
        std::lock_guard<std::mutex> lock(s_cmdQueue.mutex);
        s_cmdQueue.maintenanceActive = active;
        s_cmdQueue.head = 0;
        s_cmdQueue.tail = 0;
        s_cmdQueue.count = 0;
        s_cmdQueue.priorityStop = {};
        s_cmdQueue.completedCommandId = 0;
        s_cmdQueue.lastOutcome = {};
    }
    s_cmdQueue.cvResult.notify_all();
}

// ----------------------------------------------------------------------------
// Synchronized Status Snapshot Implementation
// ----------------------------------------------------------------------------
void publishStatusSnapshot(const paxg::fishing::LiveFishingController* controller,
                           const paxg::audio::BagFullDetector* bagFullDetector,
                           const paxg::uac::UacAudioBridge* bridge,
                           const char* ipOverride) {
    WebServerStatus snap;
    if (controller) {
        snap.armed = controller->isArmed();
        snap.fsmState = controller->getState();
        snap.keyState = controller->getKeyState();
        snap.stopReason = controller->getLastStopReason();
        snap.waitRemainingMs = controller->getWaitRemainingMs();
        snap.totalCasts = controller->getCasts();
        snap.totalBites = controller->getBites();
        snap.totalReels = controller->getReels();
        snap.totalTimeouts = controller->getTimeouts();
    }
    if (bridge) {
        const auto bSnap = bridge->getMetricsSnapshot();
        snap.audioPackets = bSnap.packetsReceived;
        snap.audioRms = bSnap.detectorMetrics.rms;
        snap.audioPeak = bSnap.detectorMetrics.peak;
    }
    if (bagFullDetector) {
        const auto m = bagFullDetector->getMetrics();
        snap.bagFull = m.bagFullLatched;
        snap.bellContinuing = m.bellContinuing;
        snap.bagIntervalMs = m.lastIntervalMs;
        snap.bagPulses = m.totalPulsesDetected;
        snap.bagMatches = m.consecutiveMatches;
    }
#if defined(ARDUINO) || defined(ESP_PLATFORM)
    snap.usbMounted = usb_composite_is_mounted();
    usb_composite_audio_stats_t usbStats{};
    usb_composite_get_audio_stats(&usbStats);
    snap.usbQueueDrops = usbStats.packets_dropped;
    if (ipOverride && ipOverride[0] != '\0') {
        std::strncpy(snap.localIp, ipOverride, sizeof(snap.localIp) - 1);
    } else if (WiFi.isConnected()) {
        std::strncpy(snap.localIp, WiFi.localIP().toString().c_str(), sizeof(snap.localIp) - 1);
    }
#else
    snap.usbMounted = true;
    if (ipOverride) {
        std::strncpy(snap.localIp, ipOverride, sizeof(snap.localIp) - 1);
    }
#endif

    std::lock_guard<std::mutex> lock(s_snapshotMutex);
    s_publishedSnapshot = snap;
}

WebServerStatus getPublishedStatusSnapshot() {
    std::lock_guard<std::mutex> lock(s_snapshotMutex);
    return s_publishedSnapshot;
}

WebServerStatus collectServerStatus(const paxg::fishing::LiveFishingController* controller,
                                    const paxg::audio::BagFullDetector* bagFullDetector,
                                    const paxg::uac::UacAudioBridge* bridge,
                                    const char* ipOverride) {
    publishStatusSnapshot(controller, bagFullDetector, bridge, ipOverride);
    return getPublishedStatusSnapshot();
}

size_t buildStatusJson(const WebServerStatus& status, char* outBuf, size_t maxLen) {
    if (!outBuf || maxLen == 0) {
        return 0;
    }
    int written = std::snprintf(outBuf, maxLen,
        "{"
        "\"armed\":%s,"
        "\"fsm\":\"%s\","
        "\"key\":\"%s\","
        "\"stop_reason\":\"%s\","
        "\"wait_remaining_ms\":%u,"
        "\"casts\":%u,"
        "\"bites\":%u,"
        "\"reels\":%u,"
        "\"timeouts\":%u,"
        "\"usb_mounted\":%s,"
        "\"audio_packets\":%llu,"
        "\"audio_rms\":%.1f,"
        "\"usb_queue_drops\":%u,"
        "\"bag_full\":%s,"
        "\"bell_continuing\":%s,"
        "\"bag_interval_ms\":%u,"
        "\"bag_pulses\":%u,"
        "\"bag_matches\":%u,"
        "\"ip\":\"%s\""
        "}",
        status.armed ? "true" : "false",
        paxg::fishing::getFishingStateName(status.fsmState),
        paxg::fishing::getKeyStateName(status.keyState),
        paxg::fishing::getStopReasonName(status.stopReason),
        static_cast<unsigned int>(status.waitRemainingMs),
        static_cast<unsigned int>(status.totalCasts),
        static_cast<unsigned int>(status.totalBites),
        static_cast<unsigned int>(status.totalReels),
        static_cast<unsigned int>(status.totalTimeouts),
        status.usbMounted ? "true" : "false",
        static_cast<unsigned long long>(status.audioPackets),
        static_cast<double>(status.audioRms),
        static_cast<unsigned int>(status.usbQueueDrops),
        status.bagFull ? "true" : "false",
        status.bellContinuing ? "true" : "false",
        static_cast<unsigned int>(status.bagIntervalMs),
        static_cast<unsigned int>(status.bagPulses),
        static_cast<unsigned int>(status.bagMatches),
        status.localIp[0] ? status.localIp : "0.0.0.0"
    );

    if (written < 0) return 0;
    return (static_cast<size_t>(written) < maxLen) ? static_cast<size_t>(written) : (maxLen - 1);
}

const char* getIndexHtml() {
    return INDEX_HTML;
}

const char* getWifiHtml() {
    return paxg::wifi::WifiSettingsManager::getWifiHtml();
}

// ----------------------------------------------------------------------------
// Native HTTP URI Handlers (esp_http_server)
// ----------------------------------------------------------------------------
#if defined(ARDUINO) || defined(ESP_PLATFORM)

static bool isRequestFromSetupAp(httpd_req_t *req) {
    if (paxg::wifi::isSetupApActive()) {
        return true;
    }
#if defined(ARDUINO) || defined(ESP_PLATFORM)
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd >= 0) {
        struct sockaddr_in addr;
        socklen_t addr_size = sizeof(addr);
        if (getsockname(sockfd, (struct sockaddr *)&addr, &addr_size) == 0) {
            uint32_t ip = ntohl(addr.sin_addr.s_addr);
            if (ip == 0xC0A80401) { // 192.168.4.1 SoftAP local socket interface
                return true;
            }
        }
    }
#else
    (void)req;
#endif
    return false;
}

static esp_err_t get_index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t get_wifi_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, getWifiHtml(), HTTPD_RESP_USE_STRLEN);
}

static esp_err_t get_wifi_status_handler(httpd_req_t *req) {
    paxg::wifi::WifiStatus status = paxg::wifi::getWifiSettingsManager().getStatus();
    char jsonBuf[512];
    paxg::wifi::WifiSettingsManager::buildWifiStatusJson(status, jsonBuf, sizeof(jsonBuf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, jsonBuf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t post_wifi_apply_handler(httpd_req_t *req) {
    char hdrBuf[64] = {0};
    bool customHeader = false;
    if (httpd_req_get_hdr_value_str(req, "X-Requested-With", hdrBuf, sizeof(hdrBuf)) == ESP_OK) {
        customHeader = true;
    } else if (httpd_req_get_hdr_value_str(req, "X-Setup-Auth", hdrBuf, sizeof(hdrBuf)) == ESP_OK) {
        customHeader = true;
    }

    if (!customHeader) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"reason\":\"MISSING_CUSTOM_HEADER\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (req->content_len == 0 || req->content_len > 512) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"reason\":\"PAYLOAD_TOO_LARGE\"}", HTTPD_RESP_USE_STRLEN);
    }

    // Exact bounded read across chunks
    char bodyBuf[513] = {0};
    size_t totalReceived = 0;
    while (totalReceived < req->content_len) {
        int ret = httpd_req_recv(req, bodyBuf + totalReceived, req->content_len - totalReceived);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            httpd_resp_set_status(req, "500 Server Error");
            return httpd_resp_send(req, "{\"success\":false,\"reason\":\"READ_FAILED\"}", HTTPD_RESP_USE_STRLEN);
        }
        totalReceived += ret;
    }
    bodyBuf[totalReceived] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};
    char pin[9] = {0};
    if (!paxg::wifi::WifiSettingsManager::parseApplyJson(bodyBuf, ssid, sizeof(ssid), password, sizeof(password), pin, sizeof(pin))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"reason\":\"INVALID_JSON\"}", HTTPD_RESP_USE_STRLEN);
    }

    if (std::strlen(ssid) == 0 || std::strlen(ssid) > 32) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"reason\":\"INVALID_SSID\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (std::strlen(password) > 64) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"reason\":\"INVALID_PASSWORD\"}", HTTPD_RESP_USE_STRLEN);
    }

    // Validate authenticated PIN BEFORE affecting fishing controller
    if (!paxg::wifi::getWifiSettingsManager().verifyPin(pin)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"reason\":\"INVALID_PIN\"}", HTTPD_RESP_USE_STRLEN);
    }

    // Prevent Wi-Fi credential switching while OTA write is in progress
    if (isMaintenanceActive()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"reason\":\"BLOCKED_DURING_OTA_WRITE\"}", HTTPD_RESP_USE_STRLEN);
    }

    // Only apply when stop confirmed by owner with no intervening start
    CommandOutcome stopOutcome = submitCommandSync(CommandType::Stop, 500);
    if (!stopOutcome.success || stopOutcome.armed || stopOutcome.state != paxg::fishing::FishingState::Idle) {
        httpd_resp_set_status(req, "500 Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"reason\":\"STOP_FAILED\"}", HTTPD_RESP_USE_STRLEN);
    }

    // OTA may have started while the stop command was waiting in the owner queue.
    if (isMaintenanceActive()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"reason\":\"BLOCKED_DURING_OTA\"}", HTTPD_RESP_USE_STRLEN);
    }

    // Stage validated pending apply for owner loop
    if (!paxg::wifi::getWifiSettingsManager().stagePendingApply(ssid, password)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"reason\":\"STAGE_FAILED\"}", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"success\":true,\"message\":\"CONNECTING\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t get_status_handler(httpd_req_t *req) {
    WebServerStatus status = getPublishedStatusSnapshot();
    char jsonBuf[512];
    buildStatusJson(status, jsonBuf, sizeof(jsonBuf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, jsonBuf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t audioMonitorConflict(httpd_req_t* req) {
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"success\":false,\"reason\":\"SPECTRAL_MONITOR_ONLY\"}");
}

static esp_err_t get_audio_handler(httpd_req_t* req) {
    const auto config = paxg::audio::matchSettings();
    const auto m = paxg::audio::matchMetrics();
    const auto legacy = getPublishedStatusSnapshot();
    char json[512];
    std::snprintf(json, sizeof(json),
        "{\"mode\":%u,\"threshold\":%u,\"mask\":%u,\"debug\":%u,\"rms\":%.6f,\"peak\":%.6f,"
        "\"scores\":[%.4f,%.4f,%.4f],\"best\":%u,\"events\":%u,"
        "\"bag_similarity\":%.4f,\"queued\":%u,\"dropped\":%u,\"dsp_us\":%u,\"max_dsp_us\":%u,"
        "\"stack_free\":%u,\"heap\":%u,\"min_heap\":%u,\"running\":%s}",
        unsigned(config.spectral), unsigned(config.thresholdPercent), unsigned(config.templateMask), unsigned(config.debug),
        config.spectral ? m.rms : legacy.audioRms / 32768.0f,
        config.spectral ? m.peak : legacy.audioPeak / 32768.0f,
        m.scores[0], m.scores[1], m.scores[2], m.best, m.events, m.bagScore, m.queuedFrames, m.droppedFrames,
        m.dspUs, m.maxDspUs, m.stackFree, unsigned(esp_get_free_heap_size()), unsigned(esp_get_minimum_free_heap_size()),
        m.running ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t post_audio_handler(httpd_req_t* req) {
    if (isRequestFromSetupAp(req) || paxg::wifi::isSetupOrApplyActive() || isMaintenanceActive()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Audio settings unavailable during setup or OTA");
    }
    char query[96], value[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing audio settings");
    paxg::audio::MatchSettings config;
    auto number = [&](const char* key, uint32_t& field, unsigned min, unsigned max) {
        if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK ||
            value[0] == '\0' || std::strspn(value, "0123456789") != std::strlen(value)) return false;
        const auto parsed = std::strtoul(value, nullptr, 10);
        if (parsed < min || parsed > max) return false;
        field = parsed;
        return true;
    };
    if (std::strcmp(query, "restore=1") != 0 &&
        !(number("mode", config.spectral, 0, 1) && number("threshold", config.thresholdPercent, 1, 100) &&
          number("mask", config.templateMask, 0, 7) && number("debug", config.debug, 0, 1)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid audio settings");
    if (config.spectral && !paxg::audio::spectralMonitorSelected()) {
        const auto stop = submitCommandSync(CommandType::Stop, 500);
        if (!stop.success || stop.armed || stop.state != paxg::fishing::FishingState::Idle)
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Fishing stop failed");
    }
    if (isMaintenanceActive()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Audio settings unavailable during OTA");
    }
    if (paxg::audio::saveMatchSettings(config) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save audio settings");
    return get_audio_handler(req);
}



static esp_err_t post_start_handler(httpd_req_t *req) {
    if (paxg::audio::spectralMonitorSelected()) return audioMonitorConflict(req);
    if (isRequestFromSetupAp(req) || paxg::wifi::isSetupOrApplyActive() || isMaintenanceActive()) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"reason\":\"BLOCKED_DURING_SETUP_APPLY_OR_OTA\"}", HTTPD_RESP_USE_STRLEN);
    }

    CommandOutcome outcome = submitCommandSync(CommandType::Start, 300);
    char respBuf[160];
    if (outcome.success) {
        std::snprintf(respBuf, sizeof(respBuf),
                      "{\"success\":true,\"state\":\"%s\",\"armed\":%s}",
                      paxg::fishing::getFishingStateName(outcome.state),
                      outcome.armed ? "true" : "false");
    } else {
        std::snprintf(respBuf, sizeof(respBuf),
                      "{\"success\":false,\"reason\":\"%s\",\"armed\":%s,\"stop\":\"%s\"}",
                      outcome.reason[0] ? outcome.reason : "SAFETY_REJECTED",
                      outcome.armed ? "true" : "false",
                      paxg::fishing::getStopReasonName(outcome.stopReason));
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, respBuf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t post_stop_handler(httpd_req_t *req) {
    CommandOutcome outcome = submitCommandSync(CommandType::Stop, 300);
    char respBuf[128];
    std::snprintf(respBuf, sizeof(respBuf),
                  "{\"success\":true,\"state\":\"IDLE\",\"stop_reason\":\"%s\"}",
                  paxg::fishing::getStopReasonName(outcome.stopReason));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, respBuf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t post_arm_handler(httpd_req_t *req) {
    if (paxg::audio::spectralMonitorSelected()) return audioMonitorConflict(req);
    if (isRequestFromSetupAp(req) || paxg::wifi::isSetupOrApplyActive() || isMaintenanceActive()) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"success\":false,\"reason\":\"BLOCKED_DURING_SETUP_APPLY_OR_OTA\"}", HTTPD_RESP_USE_STRLEN);
    }

    CommandOutcome outcome = submitCommandSync(CommandType::ArmToggle, 300);
    char respBuf[64];
    std::snprintf(respBuf, sizeof(respBuf), "{\"success\":true,\"armed\":%s}", outcome.armed ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, respBuf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t post_bag_reset_handler(httpd_req_t *req) {
    CommandOutcome outcome = submitCommandSync(CommandType::ResetBagFull, 300);
    char respBuf[96];
    std::snprintf(respBuf, sizeof(respBuf), "{\"success\":%s,\"message\":\"Bag-full latch cleared\"}",
                  outcome.success ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, respBuf, HTTPD_RESP_USE_STRLEN);
}

#endif // defined(ARDUINO) || defined(ESP_PLATFORM)

bool startFishingWebServer(paxg::fishing::LiveFishingController* controller,
                           paxg::audio::BagFullDetector* bagFullDetector,
                           paxg::uac::UacAudioBridge* bridge,
                           uint16_t port) {
    std::lock_guard<std::mutex> lock(s_serverMutex);

    if (controller || bagFullDetector || bridge) {
        publishStatusSnapshot(controller, bagFullDetector, bridge, nullptr);
    }

#if defined(ARDUINO) || defined(ESP_PLATFORM)
    if (s_httpdHandle != nullptr) {
        return true; // Already started
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.task_priority = 1; // FreeRTOS priority 1: strictly below TinyUSB audio task (priority 5)
    config.core_id = 0;       // Core 0 pinned: eliminates thread contention with Core 1
    config.stack_size = 8192;
    config.max_uri_handlers = 12;

    esp_err_t ret = httpd_start(&s_httpdHandle, &config);
    if (ret != ESP_OK) {
        return false;
    }

    httpd_uri_t uri_get_index = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = get_index_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(s_httpdHandle, &uri_get_index);

    httpd_uri_t uri_get_wifi = {
        .uri = "/wifi",
        .method = HTTP_GET,
        .handler = get_wifi_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(s_httpdHandle, &uri_get_wifi);

    httpd_uri_t uri_get_wifi_status = {
        .uri = "/api/wifi/status",
        .method = HTTP_GET,
        .handler = get_wifi_status_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(s_httpdHandle, &uri_get_wifi_status);

    httpd_uri_t uri_post_wifi_apply = {
        .uri = "/api/wifi/apply",
        .method = HTTP_POST,
        .handler = post_wifi_apply_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(s_httpdHandle, &uri_post_wifi_apply);

    httpd_uri_t uri_get_status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = get_status_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(s_httpdHandle, &uri_get_status);

    httpd_uri_t uri_post_start = {
        .uri = "/api/start",
        .method = HTTP_POST,
        .handler = post_start_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(s_httpdHandle, &uri_post_start);

    httpd_uri_t uri_post_stop = {
        .uri = "/api/stop",
        .method = HTTP_POST,
        .handler = post_stop_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(s_httpdHandle, &uri_post_stop);

    httpd_uri_t uri_post_arm = {
        .uri = "/api/arm",
        .method = HTTP_POST,
        .handler = post_arm_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(s_httpdHandle, &uri_post_arm);

    httpd_uri_t uri_post_bag_reset = {
        .uri = "/api/bagfull/reset",
        .method = HTTP_POST,
        .handler = post_bag_reset_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(s_httpdHandle, &uri_post_bag_reset);
    const httpd_uri_t audioGet{"/api/audio", HTTP_GET, get_audio_handler, nullptr};
    const httpd_uri_t audioPost{"/api/audio", HTTP_POST, post_audio_handler, nullptr};
    httpd_register_uri_handler(s_httpdHandle, &audioGet);
    httpd_register_uri_handler(s_httpdHandle, &audioPost);

    return true;
#else
    s_mockRunning = true;
    (void)port;
    return true;
#endif
}

void stopFishingWebServer() {
    std::lock_guard<std::mutex> lock(s_serverMutex);
#if defined(ARDUINO) || defined(ESP_PLATFORM)
    if (s_httpdHandle != nullptr) {
        httpd_stop(s_httpdHandle);
        s_httpdHandle = nullptr;
    }
#else
    s_mockRunning = false;
#endif
}

bool isFishingWebServerRunning() {
    std::lock_guard<std::mutex> lock(s_serverMutex);
#if defined(ARDUINO) || defined(ESP_PLATFORM)
    return s_httpdHandle != nullptr;
#else
    return s_mockRunning;
#endif
}

} // namespace web
} // namespace paxg
