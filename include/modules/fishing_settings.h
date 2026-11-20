#pragma once

#include "fishing_types.h"

namespace paxg {
namespace fishing {

void loadPersistentSettings(FishingSettingsData& settings);
void savePersistentSettings(const FishingSettingsData& settings);
void resetPersistentSettings(FishingSettingsData& settings);

}  // namespace fishing
}  // namespace paxg
