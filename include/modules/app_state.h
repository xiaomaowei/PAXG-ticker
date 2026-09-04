#pragma once

#include <Arduino.h>

#include <vector>

enum class DisplayMode : int
{
    PAXG = 0,
    Clock = 1,
    Weather = 2,
};

extern DisplayMode mode;
extern bool changed_mode;

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
