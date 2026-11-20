#pragma once
#include "wifi_provisioning/manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETWORK_PROV_EVENT WIFI_PROV_EVENT

typedef enum {
    NETWORK_PROV_INIT = WIFI_PROV_INIT,
    NETWORK_PROV_DEINIT = WIFI_PROV_DEINIT,
    NETWORK_PROV_START = WIFI_PROV_START,
    NETWORK_PROV_END = WIFI_PROV_END,
    NETWORK_PROV_WIFI_CRED_RECV = WIFI_PROV_CRED_RECV,
    NETWORK_PROV_WIFI_CRED_FAIL = WIFI_PROV_CRED_FAIL,
    NETWORK_PROV_WIFI_CRED_SUCCESS = WIFI_PROV_CRED_SUCCESS,
} network_prov_cb_event_t;

typedef wifi_prov_sta_fail_reason_t network_prov_wifi_sta_fail_reason_t;
#define NETWORK_PROV_WIFI_STA_AUTH_ERROR WIFI_PROV_STA_AUTH_ERROR

static inline void network_prov_mgr_deinit(void) {
    wifi_prov_mgr_deinit();
}

#ifdef __cplusplus
}
#endif
