# PAXG-ticker

**文件語言：** [English](README.md) · [繁體中文](README.zh-TW.md)

PAXG-ticker 是為 **LILYGO T-Display S3（ESP32-S3）** 開發的 Arduino 韌體，在內建螢幕顯示 PAX Gold（PAXG）行情、時間、天氣，以及 WoW Fishing 模式。

## 功能

- 從 Binance 公開 API 取得 PAXGUSDT 即時價格與 24 小時變化。
- PAXG 行情畫面、數位時鐘及天氣畫面。
- WoW Fishing 模式：非阻塞狀態機、合成 PCM 音訊偵測、監測儀表板及可保存設定。
- 標準 USB HID 鍵盤輸出，具有明確的 ARM 操作、USB 主機連線檢查及斷線自動解除 ARM。
- 透過裝置上的兩個按鈕切換模式及操作 Fishing 頁面。

## 硬體與開發環境

- LILYGO T-Display S3（ESP32-S3，320×170 螢幕）
- PlatformIO Core 或 VS Code 的 PlatformIO 擴充套件
- Arduino ESP32 framework；目標環境定義於 `platformio.ini`：`lilygo-t-display-s3`
- 專案函式庫：TFT_eSPI、WiFi、ArduinoJson、HTTPClient；USB HID 使用 Arduino ESP32 提供的標準鍵盤支援

## 設定與編譯

複製環境變數範本，填入時區及天氣地點；若要使用快速 Wi-Fi 連線，再填入 Wi-Fi 帳密：

```powershell
Copy-Item .env.example .env
```

`.env` 範例：

```env
WIFI_SSID=your_wifi_name
WIFI_PASSWORD=your_wifi_password
APP_TIMEZONE=CST-8
WEATHER_CITY=Taipei
WEATHER_COUNTRY=TW
```

`APP_TIMEZONE` 必須設定。PlatformIO 的預先建置腳本會讀取 `.env`，並依城市與國家產生天氣座標標頭檔。若未提供 Wi-Fi 帳密，韌體會改用裝置上的手動選網與輸入流程。`.env` 已列入忽略清單，請勿提交帳密。

```powershell
pio run
pio run -t upload
pio device monitor -b 115200
```

## 操作方式

兩個按鈕為 active-low：UP 在 GPIO 14，DOWN 在 GPIO 0。短按定義為少於 800 ms；Fishing 模式中的長按門檻為 800 ms。

| 操作 | 一般模式 | Fishing 模式 |
| --- | --- | --- |
| 短按 UP | 下一個模式 | Dashboard / Audio 頁切換；Settings 頁選下一列 |
| 短按 DOWN | 上一個模式 | Dashboard / Audio 頁返回 PAXG；Settings 頁調整選取值 |
| 長按 UP | 下一個模式 | 開始／暫停／繼續；Settings 頁返回 Dashboard |
| 長按 DOWN | 上一個模式 | 停止或重設工作階段計數；Settings 頁還原預設值 |
| Fishing 頁面同時長按兩鍵 | — | 切換 ARM 狀態；必須已連接 USB 主機 |

Fishing 的 Settings 可調整音訊門檻（500–5000）、等待逾時（5–30 秒）、冷卻時間（0.5–5 秒）以及 Cast/Interact 按鍵。設定會保存在 ESP32 NVS；可選按鍵為 `1`、`2`、`3`、`4`、`5`、`9`、`0` 和空白鍵。

## WoW Fishing 模式的範圍與安全

目前韌體的 Fishing 音訊輸入是**合成測試資料**，不是遊戲音訊擷取；畫面上的音訊觸發與 Reel 計數不能代表實際咬鉤偵測或漁獲資料。USB Audio（UAC）尚未整合至一般韌體建置。相關可行性調查與隔離 PoC 請見 [docs/usb_feasibility.md](docs/usb_feasibility.md) 及 `test/`。

HID 預設為解除 ARM。只有在 Fishing 模式、USB 主機已掛載、使用者明確 ARM，且狀態機正在執行時，韌體才會送出按鍵。退出 Fishing 或 USB 斷線時會解除 ARM、清除待送按鍵並釋放按鍵。按鍵輸出會送到目前取得焦點的 USB 主機應用程式；使用前請確認主機與目標應用程式。

## 專案目錄

- `src/core/`：主程式、模式控制、共享狀態及 USB HID 服務
- `src/modes/`：PAXG、時鐘、天氣、Fishing 模式與 Fishing 設定
- `src/hal/`：螢幕、Wi-Fi、亮度及其他硬體介面
- `src/audio/`、`include/modules/audio/`：PCM 音訊偵測器、環形緩衝區及合成資料
- `include/modules/`：模式、HAL 與共用標頭檔
- `include/images/`：嵌入式圖像資源
- `test/`：音訊偵測器測試及 USB Audio／ESP-IDF 可行性 PoC
- `docs/`：設計與可行性文件
- `scripts/`：建置前環境變數載入及天氣座標處理

## 測試

建置韌體：

```powershell
pio run
```

音訊偵測器的原生測試位於 `test/audio_detector/`；USB Audio PoC 位於獨立測試目錄，不屬於一般韌體建置。請依各測試目錄中的 README 執行對應測試。

## 授權

本專案採用 MIT License，詳見 [LICENSE](LICENSE)。
