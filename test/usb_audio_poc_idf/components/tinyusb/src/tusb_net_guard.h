/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "tusb_option.h"

/*
 * Compile-time guard for TinyUSB networking class drivers.
 * In this vendor distribution, optional networking sources (lib/networking/rndis_reports.c)
 * and definitions are not present.
 * If actual TinyUSB preprocessor class macros CFG_TUD_ECM_RNDIS or CFG_TUD_NCM are enabled
 * without the required networking implementation files, compilation fails explicitly.
 */
#if (defined(CFG_TUD_ECM_RNDIS) && CFG_TUD_ECM_RNDIS) || (defined(CFG_TUD_NCM) && CFG_TUD_NCM)
#error "TinyUSB network driver enabled (CFG_TUD_ECM_RNDIS or CFG_TUD_NCM), but required networking sources (lib/networking/rndis_reports.c) are missing from the vendor tree."
#endif
