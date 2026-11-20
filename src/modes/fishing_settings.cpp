#include <Preferences.h>
#include <modules/fishing_settings.h>

namespace paxg {
namespace fishing {

static const char* PREFS_NAMESPACE = "paxg_fishing";
static const char* KEY_THRESH = "thresh";
static const char* KEY_TIMEOUT = "timeout";
static const char* KEY_COOLDOWN = "cooldown";
static const char* KEY_CAST = "cast_k";
static const char* KEY_INTERACT = "act_k";

void loadPersistentSettings(FishingSettingsData& settings) {
    Preferences prefs;
    if (!prefs.begin(PREFS_NAMESPACE, true)) {  // read-only mode
        settings = FishingSettingsData();
        settings.clamp();
        return;
    }

    settings.absoluteThreshold = prefs.getInt(KEY_THRESH, DEFAULT_THRESHOLD);
    settings.waitTimeoutMs = prefs.getUInt(KEY_TIMEOUT, DEFAULT_WAIT_TIMEOUT_MS);
    settings.cooldownMs = prefs.getUInt(KEY_COOLDOWN, DEFAULT_COOLDOWN_MS);
    settings.castKey = static_cast<uint8_t>(prefs.getUChar(KEY_CAST, DEFAULT_CAST_KEY));
    settings.interactKey = static_cast<uint8_t>(prefs.getUChar(KEY_INTERACT, DEFAULT_INTERACT_KEY));

    prefs.end();
    settings.clamp();
}

void savePersistentSettings(const FishingSettingsData& settings) {
    FishingSettingsData copy = settings;
    copy.clamp();

    Preferences prefs;
    if (!prefs.begin(PREFS_NAMESPACE, false)) {  // read-write mode
        return;
    }

    prefs.putInt(KEY_THRESH, copy.absoluteThreshold);
    prefs.putUInt(KEY_TIMEOUT, copy.waitTimeoutMs);
    prefs.putUInt(KEY_COOLDOWN, copy.cooldownMs);
    prefs.putUChar(KEY_CAST, copy.castKey);
    prefs.putUChar(KEY_INTERACT, copy.interactKey);

    prefs.end();
}

void resetPersistentSettings(FishingSettingsData& settings) {
    settings = FishingSettingsData();
    settings.clamp();

    Preferences prefs;
    if (prefs.begin(PREFS_NAMESPACE, false)) {
        prefs.clear();
        prefs.end();
    }
}

}  // namespace fishing
}  // namespace paxg
