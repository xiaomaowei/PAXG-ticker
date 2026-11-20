#pragma once

#include <Arduino.h>

#include <vector>

enum class DisplayMode : int
{
    PAXG = 0,
    Clock = 1,
    Weather = 2,
    Fishing = 3,
};

enum class FishingState : int
{
    Stopped = 0,
    Casting = 1,
    WaitingBite = 2,
    Reeling = 3,
    Paused = 4,
};

extern DisplayMode mode;
extern bool changed_mode;

extern FishingState fishingState;
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
extern bool fishingArmed;
extern unsigned int fishingLiveKeySentCount;

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
