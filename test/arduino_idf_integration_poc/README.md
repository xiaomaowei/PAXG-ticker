# ESP32-S3 隔離整合概念驗證：Arduino + TFT_eSPI (Parallel 8-bit) + Wi-Fi + ESP-IDF 5.x USB Composite (UAC 2.0 + HID)

本目錄（`test/arduino_idf_integration_poc`）為獨立隔離的建置可行性驗證專案，目的在於評估並實證：**現有生產環境之 Arduino 應用層、`TFT_eSPI` 平行 8-bit 螢幕驅動與 `WiFi` 協定棧，能否在 ESP-IDF 5.x 體系下與 USB 複合裝置（UAC 2.0 音訊揚聲器 + HID 鍵盤）並存並成功編譯連結**。

---

## 一、 核心實證結論與範圍界定 (Scope & Findings)

### 1. 靜態編譯與符號鏈結可行性 (Static Build & Link Feasibility: PASS)
- **實證結果**：在 **ESP-IDF 5.1.2 + GCC 12.2.0 + Arduino-ESP32 3.0.7** 特定版本組合下，生產環境所採用的 **`TFT_eSPI` 平行 8-bit ST7789 驅動**、**Arduino `WiFi`** 以及 **USB 複合裝置 (UAC 2.0 Speaker + HID Keyboard)** 成功於同一二進位韌體內編譯並完成靜態符號鏈結。
- 產出之 `firmware.elf` 與 `firmware.bin` 均經由符號表（Symbol Table）檢驗，所有關鍵符號（包含 `TFT_eSPI::begin`、`TFT_eSprite::createSprite`、`WiFiSTAClass::begin`、`usb_composite_init`、`tud_audio_n_read`、`usb_composite_hid_keyboard_press` 等）全數成功解析與鏈結。

### 2. 邊界與局限說明 (Important Boundary: Build/Link vs. Live Operation)
> [!IMPORTANT]
> **靜態編譯成功不等於實體硬體全功能運作**：
> 本 PoC 的成功證明了 API 簽章、標頭檔宣告、結構體定義、暫存器巨集與符號位址在該工具鏈下的相容性與無衝突。然而，這**並不代表**在實體硬體與真實主機環境下已經過完整動態驗證。後續仍需實體硬體測試下列項目：
> 1. **平行 8-bit 匯流排時序與中斷搶佔**：`TFT_eSPI` 平行 8-bit 採用暫存器位元翻轉操作（Direct GPIO Bit-Banging），無硬體 DMA 支援。在高頻 Wi-Fi 封包中斷與 FreeRTOS 1000Hz 滴答搶佔下，寫入時序是否產生螢幕抖動或撕裂需示波器與實體螢幕驗證。
> 2. **UAC 2.0 音訊資料流即時吞吐與抖動 (Jitter)**：在 Wi-Fi 高負載傳輸與螢幕更新並發時，TinyUSB 音訊端點環形緩衝區是否會發生 Underrun/Overrun 破音。
> 3. **主機端列舉與驅動相容性**：不同作業系統（Windows / macOS / Linux）對複合 UAC 2.0 + HID 描述符之列舉延遲與驅動載入行為。
> 4. **FreeRTOS 核心任務優先權分配**：TinyUSB 裝置任務、Wi-Fi 事件任務與 Arduino 主迴圈間的 CPU 資源分配調優。

### 3. 安全與防護：移除無條件定時發鍵 (No Automatic Keystrokes)
- 本韌體**完全移除**任何開機或連線後自動定時發送 HID 按鍵（如每秒發送按鍵 '1'）之行為，杜絕設備插入開發機時對使用者游標或輸入介面造成意外干擾。
- 為保留鏈結符號證據，改採顯式手動觸發函式 `manual_trigger_keystroke_demo(modifier, keycode)`，並透過全域揮發性函式指標（`volatile g_hid_demo_ref`）強制編譯器與鏈結器保留符號，主迴圈 `loop()` 保持被動閒置。

### 4. Wi-Fi 憑證為純編譯佔位符 (Mock Placeholders)
- `main.cpp` 中之 `MOCK_WIFI_SSID` 與 `MOCK_WIFI_PASS` 僅為促使鏈結器拉入 Wi-Fi 協定棧符號之靜態常數佔位符，**不代表**可用連線憑證，嚴禁於版本庫中寫入真實金鑰。

---

## 二、 `TFT_eSPI` 平行 8-bit 技術分析與原生阻礙

### 1. 平行 8-bit 運作原理
- 社群普遍存在「TFT_eSPI 不相容 ESP-IDF 5.x」的推測，原因多為 SPI 模式驅動廢棄舊版 `driver/spi_master.h` 介面。
- **實證事實**：針對 LilyGo T-Display-S3 的平行 8-bit 模式（`TFT_PARALLEL_8_BIT=1`），`TFT_eSPI`（`Processors/TFT_eSPI_ESP32_S3.c` 及 `.h`）**完全不使用 `esp_lcd`，亦不使用任何專用硬體控制器**，而是直接透過巨集向 GPIO 暫存器寫入（Direct Register Writes：`GPIO.out1_w1tc.val` 與 `GPIO.out1_w1ts.val`）。

### 2. 官方原生組態之真實阻礙 (The Real Blocker)
- 官方 `TFT_eSPI/Kconfig` 中，對平行資料腳位 `TFT_D0`～`TFT_D7` 設定了 `range -1 31` 限制。
- LilyGo T-Display-S3 的硬體平行 8-bit 資料線配置在 GPIO 39, 40, 41, 42, 45, 46, 47, 48（均大於 31）。這導致 Kconfig 驗證失敗將腳位歸零為 -1，觸發 `#error "Invalid Data 0 pin"`。

### 3. 解法：隔離轉接與依賴自動獲取 (Bootstrap)
- 建立隔離轉接組件 [components/TFT_eSPI](file:///C:/Users/020183/Documents/prog/PAXG-ticker/test/arduino_idf_integration_poc/components/TFT_eSPI/)，透過專屬 [include/TFT_config.h](file:///C:/Users/020183/Documents/prog/PAXG-ticker/test/arduino_idf_integration_poc/components/TFT_eSPI/include/TFT_config.h) 直接注入正確腳位定義，徹底繞過 Kconfig 限制。
- 提供 [bootstrap_tft.py](file:///C:/Users/020183/Documents/prog/PAXG-ticker/test/arduino_idf_integration_poc/components/TFT_eSPI/bootstrap_tft.py)，在全新 git clone 環境下自動下載並校驗鎖定版本 `V2.5.43`（SHA256: `a303b432cbdb647673ce9efbb94139667a30e2739d2aeba741e3e7db8f64c8b0`），解壓至 `upstream/`，且透過 `.gitignore` 排除，避免將 32MB 之範例與工具重複提交進版本庫。

---

## 三、 Arduino 組件出處、授權與補丁記錄 (Provenance & Patches)

詳細記錄請參閱 [components/arduino/PROVENANCE.md](file:///C:/Users/020183/Documents/prog/PAXG-ticker/test/arduino_idf_integration_poc/components/arduino/PROVENANCE.md)。

| 項目 | 內容 |
| :--- | :--- |
| **上游倉庫** | `https://github.com/espressif/arduino-esp32` |
| **鎖定標籤** | `3.0.7` (專為 ESP-IDF v5.1.x 開發) |
| **主要授權** | GNU Lesser General Public License v2.1 ([LICENSE.txt](file:///C:/Users/020183/Documents/prog/PAXG-ticker/test/arduino_idf_integration_poc/components/arduino/LICENSE.txt)) |
| **在庫大小** | 修剪後 347 個檔案，合計 1.87 MB（直接納入版本控制，確保全新檢出無需聯網即可離線編譯） |

### 套用之修改與補丁 (Patches)
1. **庫修剪 (`CMakeLists.txt`)**：移除了 29 個未引用重型庫（如 BLE, Zigbee, RainMaker, TFLite 等），解決 Windows NTFS 大小寫不敏感檔案碰撞（`ppp.c` vs `PPP.cpp`）與多層未解析組件依賴。
2. **網路配置組件映射 (`CMakeLists.txt`)**：將 `espressif__network_provisioning` 對齊為 IDF 5.1 原生 `wifi_provisioning`。
3. **配置標頭檔轉接 (`cores/esp32/network_provisioning/`)**：新增 `network_config.h` 與 `manager.h` 包裝檔。
4. **GCC 12 Switch 警告修復 (`cores/esp32/chip-debug-report.cpp`)**：修正未列舉分割區型態警告。
5. **FTM API 版本相容 (`libraries/WiFi/src/WiFiGeneric.cpp`)**：移除 IDF 5.1.4+ 專屬之 `.use_get_report_api` 欄位以相容 IDF 5.1.2。
6. **晶片變體修剪 (`variants/`)**：僅保留 `esp32s3`。

---

## 四、 軟硬體環境與版本規格

| 項目 | 版本 / 規格 |
| :--- | :--- |
| **硬體目標** | LilyGo T-Display-S3 (ESP32-S3R8, 8MB Octal PSRAM, 16MB Flash) |
| **螢幕驅動** | ST7789 平行 8-bit (WR:8, RD:9, CS:6, DC:7, RST:5, BL:38, D0-D7: 39,40,41,42,45,46,47,48) |
| **建置框架** | PlatformIO 6.1.19 / espressif32 @ 6.5.0 (`framework = espidf`) |
| **ESP-IDF** | `framework-espidf @ 3.50102.240122 (5.1.2)` |
| **編譯工具鏈** | `toolchain-xtensa-esp32s3 @ 12.2.0+20230208` (GCC 12.2.0) |
| **Arduino 核心** | `arduino-esp32 @ 3.0.7` (In-tree 隔離修剪組件, LGPL-2.1) |
| **TFT 驅動庫** | `TFT_eSPI @ V2.5.43` (Bodmer, FreeBSD 授權, 隔離轉接) |
| **USB 複合組件** | TinyUSB 0.15.0 + `usb_device_composite` (UAC 2.0 揚聲器 + HID 鍵盤) |

---

## 五、 資源佔用與二進位產物

`pio run -d test/arduino_idf_integration_poc` 建置輸出數據：
```
RAM:   [=         ]  12.5% (used 40884 bytes from 327680 bytes)
Flash: [========= ]  88.0% (used 923149 bytes from 1048576 bytes)
```
- **RAM 佔用**：靜態配置 40,884 bytes，ESP32-S3 原生具備 320KB 內部 SRAM 及 8MB 外部 PSRAM，餘裕充足。
- **Flash 佔用**：約 902 KB，佔 PlatformIO 預設 1MB 應用分割區之 88.0%。LilyGo T-Display-S3 實體 Flash 為 16MB，後續生產遷移僅需配置 3MB 或 4MB APP 分割區即可納入完整業務邏輯與字型資產。

---

## 六、 獨立重現與自動化驗證指令

### 1. 自動化驗證腳本 (`run_validation.py`)
腳本會自動檢查依賴、觸發 `bootstrap_tft.py`（若無快取）、執行編譯並由 ELF 檢查上述 8 項核心符號：

```powershell
# 1. 執行完整建置與符號表驗證
python test/arduino_idf_integration_poc/run_validation.py

# 2. 執行驗證並於成功後自動清理產物 (.pio 與 sdkconfig.*)
python test/arduino_idf_integration_poc/run_validation.py --clean-after

# 3. 僅清理建置產物
python test/arduino_idf_integration_poc/run_validation.py --clean-only

# 4. 同時驗證根目錄生產環境未受干擾
python test/arduino_idf_integration_poc/run_validation.py --check-production
```

### 2. 全新 Checkout 手動逐步構建
在全新的 git checkout 環境下：
```powershell
# 1. 自動下載鎖定之 TFT_eSPI V2.5.43 (具備 SHA256 校驗)
python test/arduino_idf_integration_poc/components/TFT_eSPI/bootstrap_tft.py

# 2. 執行 PlatformIO 建置
pio run -d test/arduino_idf_integration_poc

# 3. 檢查關鍵符號
$nm = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32s3\bin\xtensa-esp32s3-elf-nm.exe"
& $nm -C test/arduino_idf_integration_poc/.pio/build/esp32s3_arduino_idf_poc/firmware.elf | Select-String "TFT_eSPI::begin|WiFiSTAClass::begin|usb_composite_init|tud_audio_n_read|manual_trigger_keystroke_demo"

# 4. 清理產物
python test/arduino_idf_integration_poc/run_validation.py --clean-only
```
