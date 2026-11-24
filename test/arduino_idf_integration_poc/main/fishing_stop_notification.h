#pragma once

namespace paxg {
namespace notification {

/**
 * @brief Initialize the MacroDroid stop notification worker.
 *        Called once in setup; stays dormant if MACRODROID_WEBHOOK_URL is not defined or empty.
 */
void initStopNotification();

/**
 * @brief Specific callback invoked on active-to-idle transition due to BAG_FULL.
 *        Sends a counting FreeRTOS task notification to wake the background worker.
 */
void onActiveBagFullStop();

}  // namespace notification
}  // namespace paxg
