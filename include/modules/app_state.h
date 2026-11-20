#pragma once

#include <Arduino.h>
#include <modules/fishing_types.h>

#include <vector>

enum class DisplayMode : int
{
    PAXG = 0,
    Clock = 1,
    Weather = 2,
    Fishing = 3,
};

using FishingState = paxg::fishing::FishingState;
using FishingPage = paxg::fishing::FishingPage;

extern DisplayMode mode;
extern bool changed_mode;

extern FishingState fishingState;
extern FishingPage fishingCurrentPage;
extern unsigned long fishingStateStartTime;
extern unsigned long fishingSessionStartTime;
extern unsigned long fishingTotalUptimeMs;
extern unsigned int fishingSimReelCmds;
extern unsigned int fishingSimTimeouts;
extern float fishingSimAudioLevel;
extern float fishingSimAudioThreshold;
extern bool fishingAudioDetected;
extern unsigned long fishingNextBiteDelayMs;
extern unsigned long fishingWaitMaxMs;
extern unsigned long fishingCooldownMs;
extern bool fishingArmed;
extern unsigned int fishingLiveKeySentCount;
extern uint8_t fishingCastKey;
extern uint8_t fishingInteractKey;
extern char fishingErrorMessage[32];

extern int angle;
extern int angleMax;
extern int angleMin;
extern bool reversal;

extern byte currentBrightness;

extern int pointRadius;
extern int curveColor;
extern double price;
extern int chartTime;
extern double percentChange;
extern int chartTimeChange;
extern bool priceFreshSample;
extern std::vector<double> readings;

extern int globalMinutes;
extern int globalHours;
extern int globalDay;

extern int weatherCode;
extern int sunriseHours;
extern int sunriseMinutes;
extern int sunsetHours;
extern int sunsetMinutes;
extern float currentTemperature;
extern std::vector<int> precipitationProbability;
