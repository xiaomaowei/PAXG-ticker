#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <images/GreenArrow32.h>
#include <images/Japan24.h>
#include <images/RedArrow32.h>
#include <images/UK24.h>
#include <images/US24.h>
#include <images/moon24.h>
#include <modules/app_state.h>
#include <modules/constants.h>
#include <modules/modes/paxg_mode.h>
#include <modules/sprites.h>
#include <modules/variables.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{

constexpr int CHART_STEP = 15;
constexpr int MAX_CHART_POINTS = 10;
constexpr int CHART_GRID_HORIZONTAL_LINES = 6;
constexpr int CHART_GRID_VERTICAL_LINES = 10;
constexpr int CHART_GRID_Y_SPACING = 15;
constexpr int CHART_GRID_X_SPACING = 15;
constexpr int CHART_GRID_Y_START = 8;
constexpr int CHART_GRID_X_START = 20;
constexpr int CHART_GRID_LEFT_X = 10;
constexpr int CHART_GRID_RIGHT_X = 165;
constexpr int CHART_GRID_TOP_Y = 0;
constexpr int CHART_GRID_BOTTOM_Y = 91;
constexpr int CHART_FIRST_POINT_X = 155;
constexpr int CHART_FIRST_POINT_Y = 8;
constexpr int CHART_Y_SCALE = 75;
constexpr int CHART_SPRITE_X = 115;
constexpr int CHART_SPRITE_Y = 70;

constexpr int PRICE_SPRITE_X = 118;
constexpr int PRICE_SPRITE_Y = 15;
constexpr int PRICE_TEXT_BASELINE_Y = 32;

constexpr int PERCENT_TEXT_CENTER_X = 57;
constexpr int PERCENT_TEXT_BASELINE_Y = 28;
constexpr int PERCENT_ARROW_X = 46;
constexpr int PERCENT_ARROW_Y = 42;
constexpr int PERCENT_ARROW_WIDTH = 23;
constexpr int PERCENT_ARROW_HEIGHT = 32;
constexpr int PERCENT_SPRITE_X = 0;
constexpr int PERCENT_SPRITE_Y = 85;

constexpr int BADGE_CENTER_X = 36;
constexpr int BADGE_CENTER_Y = 36;
constexpr int BADGE_OUTER_RADIUS = 35;
constexpr int BADGE_INNER_RADIUS = 31;
constexpr int BADGE_TEXT_CENTER_X = 36;
constexpr int BADGE_TEXT_BASELINE_Y = 44;

constexpr int ROTATION_BACK_CIRCLE_X = 37;
constexpr int ROTATION_BACK_CIRCLE_Y = 37;
constexpr int ROTATION_BACK_CIRCLE_RADIUS = 34;
constexpr int ROTATION_SPRITE_X = 20;
constexpr int ROTATION_SPRITE_Y = 10;

constexpr int SESSIONS_MARKET_ICON_X = 4;
constexpr int SESSIONS_US_ICON_Y = 0;
constexpr int SESSIONS_UK_ICON_Y = 24;
constexpr int SESSIONS_JAPAN_ICON_Y = 48;
constexpr int SESSIONS_MOON_ICON_Y = 72;
constexpr int SESSIONS_ICON_WIDTH = 24;
constexpr int SESSIONS_ICON_HEIGHT = 24;
constexpr int SESSIONS_SPRITE_X = 288;
constexpr int SESSIONS_SPRITE_Y = 70;

constexpr int WEEKDAY_MON = 1;
constexpr int WEEKDAY_FRI = 5;
constexpr int UK_OPEN_MINUTE = 540;
constexpr int UK_CLOSE_MINUTE = 1049;
constexpr int US_OPEN_MINUTE = 930;
constexpr int US_CLOSE_MINUTE = 1319;
constexpr int JP_OPEN_MINUTE = 60;
constexpr int JP_CLOSE_MINUTE = 479;
constexpr int HTTP_TIMEOUT_MS = 8000;
constexpr unsigned long WIFI_RECOVERY_INTERVAL_MS = 30000UL;
constexpr int MAX_FAILED_FETCHES_BEFORE_RECOVERY = 3;
constexpr const char API_PAXG_PRICE[] = "https://api.binance.com/api/v3/ticker/24hr?symbol=PAXGUSDT";

int consecutivePriceFetchFailures = 0;
unsigned long lastWiFiRecoveryAttempt = 0;

void mark_price_update_failed()
{
    priceFreshSample = false;
    consecutivePriceFetchFailures++;

    if (consecutivePriceFetchFailures < MAX_FAILED_FETCHES_BEFORE_RECOVERY)
    {
        return;
    }

    if (currentMillis - lastWiFiRecoveryAttempt < WIFI_RECOVERY_INTERVAL_MS)
    {
        return;
    }

    // Throttle reconnect attempts to avoid blocking loops on unstable links.
    lastWiFiRecoveryAttempt = currentMillis;
    Serial.println("Price fetch repeatedly failing, attempting WiFi recovery");

    if (WiFi.status() != WL_CONNECTED)
    {
        WiFi.reconnect();
    }
}

void mark_price_update_succeeded()
{
    priceFreshSample = true;
    consecutivePriceFetchFailures = 0;
}

void chart_background()
{
    chart.fillSprite(TFT_BLACK);

    for (int i = 0; i < CHART_GRID_HORIZONTAL_LINES; i++)
    {
        chart.drawLine(CHART_GRID_LEFT_X, i * CHART_GRID_Y_SPACING + CHART_GRID_Y_START,
                       CHART_GRID_RIGHT_X, i * CHART_GRID_Y_SPACING + CHART_GRID_Y_START,
                       TFT_LIGHTGREY);
    }

    for (int i = 0; i < CHART_GRID_VERTICAL_LINES; i++)
    {
        chart.drawLine(i * CHART_GRID_X_SPACING + CHART_GRID_X_START, CHART_GRID_TOP_Y,
                       i * CHART_GRID_X_SPACING + CHART_GRID_X_START, CHART_GRID_BOTTOM_Y,
                       TFT_LIGHTGREY);
    }
}

void display_price_chart()
{
    if (readings.empty())
    {
        return;
    }

    auto minmax = std::minmax_element(readings.begin(), readings.end());
    double curveMin = *minmax.first;
    double curveMax = *minmax.second;

    double curveRange = curveMax - curveMin;
    if (curveRange <= 0.0001)
    {
        curveRange = 1.0;
    }

    int previousX = 0;
    int previousY = 0;

    for (size_t i = 0; i < readings.size(); ++i)
    {
        double priceDeviation = curveMax - readings[i];
        double relativeDeviation = priceDeviation / curveRange;
        int currentX = CHART_FIRST_POINT_X -
                       ((static_cast<int>(readings.size()) - static_cast<int>(i) - 1) * CHART_STEP);
        int currentY = static_cast<int>(CHART_FIRST_POINT_Y + relativeDeviation * CHART_Y_SCALE);

        chart.fillCircle(currentX, currentY, pointRadius, curveColor);
        if (i > 0)
        {
            chart.drawLine(previousX, previousY, currentX, currentY, curveColor);
            chart.drawLine(previousX - 1, previousY, currentX - 1, currentY, curveColor);
            chart.drawLine(previousX + 1, previousY, currentX + 1, currentY, curveColor);
        }

        previousX = currentX;
        previousY = currentY;
    }
}

void display_price()
{
    char priceBuffer[32];
    std::snprintf(priceBuffer, sizeof(priceBuffer), "$%.2f", price);

    paxg_price.fillSprite(TFT_BLACK);
    paxg_price.setTextSize(1);
    paxg_price.setFreeFont(&FreeSansBold18pt7b);
    paxg_price.setTextColor(TFT_WHITE);

    int textW = paxg_price.textWidth(priceBuffer);
    int cursorX = 0;
    if (textW < 205)
    {
        cursorX = (205 - textW) / 2;
    }

    paxg_price.setCursor(cursorX, PRICE_TEXT_BASELINE_Y);
    paxg_price.print(priceBuffer);
    paxg_price.pushSprite(PRICE_SPRITE_X, PRICE_SPRITE_Y);
}

void display_percent_change()
{
    String output = String(percentChange, 2) + "%";

    paxg_percents.fillSprite(TFT_BLACK);
    paxg_percents.setTextSize(1);
    paxg_percents.setFreeFont(&FreeMonoBold12pt7b);
    paxg_percents.setTextColor(TFT_WHITE);
    paxg_percents.setCursor(PERCENT_TEXT_CENTER_X - (paxg_percents.textWidth(output) / 2),
                            PERCENT_TEXT_BASELINE_Y);
    paxg_percents.print(output);

    if (percentChange > 0)
    {
        paxg_percents.pushImage(PERCENT_ARROW_X, PERCENT_ARROW_Y, PERCENT_ARROW_WIDTH,
                                PERCENT_ARROW_HEIGHT, GreenArrow);
    }
    else
    {
        paxg_percents.pushImage(PERCENT_ARROW_X, PERCENT_ARROW_Y, PERCENT_ARROW_WIDTH,
                                PERCENT_ARROW_HEIGHT, RedArrow);
    }

    paxg_percents.pushSprite(PERCENT_SPRITE_X, PERCENT_SPRITE_Y);
}

void display_paxg_badge()
{
    paxg_badge.fillSprite(TFT_BLACK);
    paxg_badge.fillCircle(BADGE_CENTER_X, BADGE_CENTER_Y, BADGE_OUTER_RADIUS, TFT_GOLD);
    paxg_badge.drawCircle(BADGE_CENTER_X, BADGE_CENTER_Y, BADGE_OUTER_RADIUS, TFT_YELLOW);
    paxg_badge.drawCircle(BADGE_CENTER_X, BADGE_CENTER_Y, BADGE_INNER_RADIUS, TFT_BLACK);

    paxg_badge.setFreeFont(&FreeSansBold12pt7b);
    paxg_badge.setTextSize(1);
    paxg_badge.setTextColor(TFT_BLACK);
    const char* label = "PAXG";
    int labelWidth = paxg_badge.textWidth(label);
    paxg_badge.setCursor(BADGE_TEXT_CENTER_X - (labelWidth / 2), BADGE_TEXT_BASELINE_Y);
    paxg_badge.print(label);

    tft.setPivot(BADGE_CENTER_X, BADGE_CENTER_Y);
}

void sessions_panel()
{
    int minutes = globalHours * 60 + globalMinutes;
    sessions.fillSprite(TFT_BLACK);

    if (globalDay >= WEEKDAY_MON && globalDay <= WEEKDAY_FRI)
    {
        if (minutes >= UK_OPEN_MINUTE && minutes <= UK_CLOSE_MINUTE)
        {
            sessions.pushImage(SESSIONS_MARKET_ICON_X, SESSIONS_UK_ICON_Y, SESSIONS_ICON_WIDTH,
                               SESSIONS_ICON_HEIGHT, UK);
        }
        if (minutes >= US_OPEN_MINUTE && minutes <= US_CLOSE_MINUTE)
        {
            sessions.pushImage(SESSIONS_MARKET_ICON_X, SESSIONS_US_ICON_Y, SESSIONS_ICON_WIDTH,
                               SESSIONS_ICON_HEIGHT, US);
        }
        if (minutes >= JP_OPEN_MINUTE && minutes <= JP_CLOSE_MINUTE)
        {
            sessions.pushImage(SESSIONS_MARKET_ICON_X, SESSIONS_JAPAN_ICON_Y, SESSIONS_ICON_WIDTH,
                               SESSIONS_ICON_HEIGHT, Japan);
        }
        if (minutes > US_CLOSE_MINUTE || minutes < JP_OPEN_MINUTE)
        {
            sessions.pushImage(SESSIONS_MARKET_ICON_X, SESSIONS_MOON_ICON_Y, SESSIONS_ICON_WIDTH,
                               SESSIONS_ICON_HEIGHT, moon);
        }
    }
    else
    {
        sessions.pushImage(SESSIONS_MARKET_ICON_X, SESSIONS_MOON_ICON_Y, SESSIONS_ICON_WIDTH,
                           SESSIONS_ICON_HEIGHT, moon);
    }

    sessions.pushSprite(SESSIONS_SPRITE_X, SESSIONS_SPRITE_Y);
}

void update_price_chart()
{
    if (readings.size() >= MAX_CHART_POINTS)
    {
        readings.erase(readings.begin());
    }

    readings.push_back(price);
}

}  // namespace

void paxg_update_price()
{
    lastPriceUpdate = currentMillis;

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("WiFi disconnected, skipping PAXG fetch");
        mark_price_update_failed();
        return;
    }

    HTTPClient http;
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.begin(API_PAXG_PRICE);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK)
    {
        String payload = http.getString();
        DynamicJsonDocument doc(1024);

        DeserializationError error = deserializeJson(doc, payload);
        if (error)
        {
            Serial.print("Failed to parse JSON. Error: ");
            Serial.println(error.c_str());
            mark_price_update_failed();
        }
        else
        {
            price = doc["lastPrice"].as<double>();
            percentChange = doc["priceChangePercent"].as<double>();
            mark_price_update_succeeded();

            char msg[80];
            std::snprintf(msg, sizeof(msg), "PAXG price updated: $%.2f, %.2f%%", price, percentChange);
            Serial.println(msg);
        }
    }
    else
    {
        Serial.print("HTTP GET failed. Code: ");
        Serial.println(httpCode);
        mark_price_update_failed();
    }

    http.end();
}

void paxg_sync_chart()
{
    if (chartTimeChange == chartTime)
    {
        return;
    }

    chartTimeChange = chartTime;

    // Append to chart only when we actually received a fresh sample.
    if (priceFreshSample)
    {
        update_price_chart();
        priceFreshSample = false;
    }
}

void paxg_render()
{
    chart_background();

    if (readings.size() > 1)
    {
        display_price_chart();
    }

    chart.pushSprite(CHART_SPRITE_X, CHART_SPRITE_Y);
    display_price();
    display_percent_change();
    sessions_panel();
}

void paxg_logo_rotation()
{
    const unsigned long now = millis();

    if (changed_mode)
    {
        display_paxg_badge();
    }

    if (now - lastRotationUpdate >= ROTATION_INTERVAL)
    {
        back_logo.fillCircle(ROTATION_BACK_CIRCLE_X, ROTATION_BACK_CIRCLE_Y,
                             ROTATION_BACK_CIRCLE_RADIUS, TFT_GOLD);
        paxg_badge.pushRotated(&back_logo, angle, TFT_BLACK);
        back_logo.pushSprite(ROTATION_SPRITE_X, ROTATION_SPRITE_Y);

        if (reversal)
        {
            angle++;
            if (angle == angleMax)
            {
                reversal = false;
            }
        }
        else
        {
            angle--;
            if (angle == angleMin)
            {
                reversal = true;
            }
        }

        lastRotationUpdate = now;
    }
}
