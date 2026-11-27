#pragma once

#include "live_fishing_controller.h"

namespace paxg {
namespace notification {

/**
 * @brief Initialize the MacroDroid stop notification worker.
 *        Called once in setup; stays dormant when neither webhook is configured.
 */
void initStopNotification();

/**
 * @brief Callback invoked when BAG_FULL or QUEUE_OVERFLOW stops active fishing.
 *        Queues the configured webhook for the background worker.
 */
void notifyFishingStop(fishing::StopReason reason, uint32_t stopId);
uint32_t takeDispatchedQueueOverflowStop();

}  // namespace notification
}  // namespace paxg
