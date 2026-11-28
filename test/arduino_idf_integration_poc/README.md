# ESP32-S3 隔離整合概念驗證：Arduino + TFT_eSPI (Parallel 8-bit) + Wi-Fi + ESP-IDF 5.x USB Composite (UAC 2.0 + HID) & 生產級音訊偵測管線

本目錄（`test/arduino_idf_integration_poc`）為獨立隔離的建置與架構驗證專案，目的在於實證：
1. **現有生產環境之 Arduino 應用層、`TFT_eSPI` 平行 8-bit 螢幕驅動與 `WiFi` 協定棧，能否在 ESP-IDF 5.x 體系下與 USB 複合裝置（UAC 2.0 音訊揚聲器 + HID 鍵盤）並存**。
2. **實作有界（Bounded）、零動態記憶體配置（Allocation-Free）的 UAC 2.0 48 kHz 雙聲道 PCM 串流橋接器，將實際 USB 音訊幀無損餵入既有生產級音訊偵測器（`paxg::audio::PcmAudioDetector`）**。
3. **整合生產級 PAXG 行情畫面（幣安 24h Ticker、即時價格、24h 漲跌、走勢圖、交易時區指示板與黃金徽章旋轉）與真實 USB UAC 2.0 / HID 連續自動釣魚畫面於同一二進位韌體**。
4. **實作有界非阻塞非同步網路工作任務（FreeRTOS Task, Core 0, Priority 1），並以 `WiFi.persistent(false)` 保護 Flash NVS，絕不在網路請求或 TLS 握手時阻塞或飢餓 USB 音訊接收與 HID 控制器**。
5. **嚴格區分行情新鮮度標記（`[LIVE MARKET QUOTE]`、`[STALE - CACHED QUOTE]`、`[OFFLINE - NO QUOTE]`），絕不將過期報價虛報為即時行情**。
6. **提供無縫雙畫面切換（短按實體 DOWN 鍵），切換時強制停機與解除武裝，杜絕任何意外鍵盤按鍵外洩**。

---

## 一、 核心實證結論與範圍界定 (Scope & Findings)

### 1. 靜態編譯與符號鏈結可行性 (Static Build & Link Feasibility: PASS)
- **實證結果**：在 **ESP-IDF 5.1.2 + GCC 12.2.0 + Arduino-ESP32 3.0.7** 特定版本組合下，生產環境所採用的 **`TFT_eSPI` 平行 8-bit ST7789 驅動**、**Arduino `WiFi`** 以及 **USB 複合裝置 (UAC 2.0 Speaker + HID Keyboard)** 成功於同一二進位韌體內編譯並完成靜態符號鏈結。
- 產出之 `firmware.elf` 與 `firmware.bin` 均經由符號表（Symbol Table）檢驗，所有關鍵符號（包含 `TFT_eSPI::begin`、`TFT_eSprite::createSprite`、`WiFiSTAClass::begin`、`usb_composite_init`、`tud_audio_n_read`、`usb_composite_hid_keyboard_press` 等）全數成功解析與鏈結。

### 2. 生產級音訊橋接管線驗證 (Production Audio Pipeline Ingestion: PASS)
- **直接編譯生產源碼**：直接引入並編譯 `src/audio/pcm_audio_detector.cpp`，完全不分叉（No fork）生產代碼。
- **無損格式轉換與封包邊界處理**：
  - **實際 USB 描述符格式**：UAC 2.0 等時輸出（Isochronous OUT），48,000 Hz，16 位元有符號小端序（Signed 16-bit LE PCM），雙聲道立體聲（Stereo 2 Channels），每幀 4 位元組（`[L_lsb, L_msb, R_lsb, R_msb]`）。
  - **混音運算**：採用 `(int32_t(left) + int32_t(right)) / 2` 進行對稱單聲道降混，避免整數溢位與動態範圍截斷。
  - **連續串流封包邊界殘留對齊**：透過 4 位元組有界殘留暫存區（`residual_[4]`），跨連續回呼邊界安全暫存 1 至 3 個未對齊位元組，杜絕位元組相位漂移（Phase Shift）或單聲道交錯顛倒。
  - **跨串流中斷殘留重設 API（Stream Break Discontinuity Handling: NOT YET CONNECTED）**：
    - 橋接器實作了顯式 API `resetResidualOnStreamBreak()`，供生命週期中斷發生時手動清除非成幀殘留位元組並記錄 `residualDiscontinuityCount`。
    - **精確邊界說明**：目前實際之 `usb_device_composite.c` 僅在主機發送靜音控制請求時調用 `set_mute_cb`，**並未**在 USB 串流關閉（stream close/alternate setting 0）、暫停（suspend）、斷開連線（disconnect）或驅動環形佇列丟包（queue drop）時發出回呼。
    - 此外，主機靜音控制請求可能在音訊資料流持續傳輸時下發，若在靜音時逕行重設殘留緩衝區，反將破壞進行中串流之雙聲道幀邊界。因此，目前 `main.cpp` 中**未連接**該函式，且明確指出關閉/重開/溢位等生命週期之斷流處理**尚未實裝**，需待 USB 複合驅動未來提供完整生命週期信號勾子；在此之前，**不宣稱**串流重啟之相位正確性。
- **48 kHz 偵測器幀組態**：
  - 設定 `frameSize = 384` 樣本（48 kHz 下正好為 8.0 ms 分析窗，與 16 kHz 下的 128 樣本維持完全一致的時域長度）。
  - 設定 `debounceFrames = 2` 幀（16.0 ms 去抖驗證）。
  - 設定 `cooldownFrames = 25` 幀（200.0 ms 強制冷卻閉鎖期）。
- **嚴謹執行緒安全與鎖定架構 (Thread Safety & Concurrency Model)**：
  - **統一內部互斥鎖 (`std::mutex mutex_`)**：所有內部狀態變異與讀取（包括 `consumeUacChunk`、`getMetricsSnapshot`、`clearTrigger`、`isTriggered`、`getConfig`、`updateConfig`、`reset` 與 `resetResidualOnStreamBreak`）均由內部互斥鎖保護，消除任何資料競爭。
  - **杜絕死鎖 (Deadlock Prevention)**：內部重設邏輯抽離為 `resetLocked()`，供 `init()` 與 `reset()` 安全調用，避免在不可遞迴鎖上發生重複上鎖死鎖。
  - **消除逃逸指標與並行破壞**：移除公開的 `getDetector()` 原型，禁止外部直接取得裸指標，改以執行緒安全的 `getConfig()` 與 `updateConfig()` 進行參數交互。
- **非秘密、限速即時遙測診斷**：
  - 每秒限速印出一行純數據遙測，可明確區分「完全無 USB 封包」（`pkts == 0`）與「傳輸數位靜音」（`pkts > 0, rms == 0.0`），並記錄斷流殘留計數 `disc`。
- **嚴格無自動化 / 無遊戲判定暗示**：
  - 偵測器觸發僅更新內部狀態計數器與遙測，**嚴禁**自動發送任何鍵盤按鍵。手動按鍵展示維持獨立被動函式，絕不與音訊邏輯串接；絕不宣稱此機制能自動判定遊戲上鉤。

### 3. 實體硬體有限驗證與局限說明 (Bounded Hardware Acceptance Evidence & Scope)

#### 3.1 2026-11-20 實體硬體有限驗證記錄 (ESP32-S3 COM3)
於 2026-11-20，使用者將本 PoC 韌體臨時燒錄至實體硬體 ESP32-S3 (COM3) 進行隔離驗證，取得以下具體證據：
1. **被動 UAC 音訊串流接收（使用者觀察）**：
   - Windows 系統成功枚舉 USB 複合裝置 VID `0x303A` PID `0x8002`，並掛載 `usb uac` 揚聲器。
   - 使用者將 World of Warcraft 遊戲音訊導向該 `usb uac` 裝置。
   - 設備螢幕即時回報：USB 顯示已連線、封包計數器持續遞增、RMS 隨遊戲音訊動態跳動，並於一次手動觀察到的魚咬鉤瞬間觀察到觸發計數（Trigs）遞增。
   - *邊界與局限*：此為單次手動遊玩過程之使用者觀察，絕非經過聲學校準的真陽性率（True Positive Rate）或假陽性率（False Positive Rate）統計度量；本次測試未擷取螢幕截圖亦無數值封包日誌。
2. **記事本獨立控制器按鍵驗證（使用者授權，僅限 Windows Notepad）**：
   - 使用者授權於 Windows 記事本（Notepad）中進行非遊戲音訊測試，並回報測試期間視窗焦點置於 Notepad。
   - 在武裝並啟動後且音訊持續流動時，觀察到記事本輸入單一空白鍵（Spacebar / HID 0x2c，此對應啟動時之起投 Cast 按鍵，並非咬鉤/音訊觸發起竿之證據），按下實體 DOWN 鍵後系統即刻返回 `DISARMED` 解除武裝狀態。
   - *本次工作未測遊戲內按鍵*：本次測試工作未進行任何遊戲內自動按鍵測試。
3. **單次受監督遊戲內單循環觀察與晚期咬鉤逾時觀測 (User In-Game Report & Timeout Diagnosis)**：
   - **單次受監督單循環驗證**：使用者授權進行單次受監督遊戲內測試，觀察到控制器成功執行一次投竿與咬鉤起竿循環（Cast 1 次、Reel 1 次後自動解除武裝回到 DISARMED）。
   - **晚期咬鉤逾時停機觀測**：在後續數次較晚咬鉤的測試案例中，使用者觀察到設備於投竿後約 20 秒亮紅燈進入 DISARMED 解除武裝。
   - **30 秒拋竿視窗與 Waiting 逾時修正**：
     - 原程式碼中 `WAIT_TIMEOUT_MS = 20000UL` 係於 1500ms Casting 結束後起算，若 20 秒內未偵測到咬鉤即觸發全閉停機 (`StopReason::Timeout`)。
     - 依據使用者回報之遊戲機制，魔獸世界一次拋竿持續時間達 30 秒，咬鉤可能發生在 30 秒內之任意時刻。此事實支持 20 秒逾時為晚期咬鉤停機之合理解釋（但不排除音訊/USB 瞬態中斷之可能性）。
     - 為此，將 Waiting 階段之逾時上限調增為 32 秒（`WAIT_TIMEOUT_MS = 32000UL`，自進入 Waiting 狀態起算），提供 32 秒之等待視窗（自拋竿起始合計 33.5 秒），完整覆蓋 30 秒拋竿時間並具備 2 秒容限。
     - 螢幕介面同步加入倒數計時 `WAITING (%2lus)` 與診斷停機原因顯示（如 `ARM: [DISARMED - BITE_TIMEOUT]`），停機後計時歸零 `( 0s)`，不虛報時間。
   - **局限說明**：上述內容均屬使用者口頭回報之單次操作與定性觀察，無實體錄音檔或數值日誌；自動釣魚之成功率、真陽性率及環境音假陽性率仍未經統計測量，音訊門檻亦未經標定。
4. **生產環境還原與現狀確認**：
   - 測試完成後已將原始生產環境 `app0` 二進位映像檔重新燒錄，並完成雜湊值驗證。
   - 使用者已確認生產環境 Home 金價首頁、Wi-Fi 連線與電腦遊戲音訊輸出完全恢復正常。
   - *生產環境狀態說明*：目前運作之生產韌體仍使用合成音訊（Synthetic Audio），尚未整合 UAC 音訊偵測管線；源碼中針對 SPACE 的標籤顯示變更亦尚未存在於目前已還原的生產二進位中。

#### 3.2 仍待深入驗證項目 (Remaining Unverified Scope)
1. **平行 8-bit 匯流排時序與高頻中斷**：本 PoC 驗證了螢幕驅動與音訊管線共存，但在高頻 Wi-Fi 封包中斷與 FreeRTOS 1000Hz 滴答搶佔下，寫入時序是否產生細微螢幕抖動需長期示波器與實體螢幕觀測。
2. **音訊串流長時間穩定度**：TinyUSB 音訊端點環形緩衝區在多小時高負載下是否發生極罕見的 Underrun/Overrun 破音。
3. **聲學能量門檻未經標定**：偵測器採用之能量門檻尚未經實體遊戲聲學標定（Uncalibrated），不代表真實辨識「遊戲咬鉤特徵音」；環境背景音、音樂或介面音效均可能引發假陽性觸發，真陽性/假陽性率未經統計測量。
4. **遊戲內連續自動化完全未測**：本 PoC 嚴格限制為單次單循環（$\le 2$ 次 Space 按鍵後自動解除武裝），嚴禁自動重投，未曾在遊戲內進行多循環自動化測試。

### 4. 安全與防護：移除無條件定時發鍵 (No Automatic Keystrokes)
- 本韌體**完全移除**任何開機或連線後自動定時發送 HID 按鍵（如每秒發送按鍵 '1'）之行為，杜絕設備插入開發機時對使用者游標或輸入介面造成意外干擾。
- 為保留鏈結符號證據，改採顯式手動觸發函式 `manual_trigger_keystroke_demo(modifier, keycode)`，並透過全域揮發性函式指標（`volatile g_hid_demo_ref`）強制編譯器與鏈結器保留符號，主迴圈 `loop()` 保持被動閒置。

### 5. Wi-Fi 堆疊休眠與 NVS 憑證保護 (Zero NVS Modification)
- 原先 `main.cpp` 中之 `WiFi.begin(...)` 雖使用佔位常數，但底層 `esp_wifi` 預設會將連線參數持久化寫入 Flash 的 `nvs` 分區（`nvs.net80211` 命名空間）。若於實體硬體執行，可能覆蓋使用者於生產韌體儲存之真實 Wi-Fi 金鑰。
- **保護措施**：已**完全移除**執行時期之 `WiFi.begin()` 與 `WiFi.mode()` 調用，改採全域揮發性函式指標錨點（`volatile g_wifi_anchor_ref` 指向 `dummy_wifi_symbol_retention_anchor`），確保編譯器與鏈結器保留 `WiFiSTAClass::begin` 與 `WiFiGenericClass::mode` 符號以通過架構驗證，但**絕不在執行時期啟動射頻或寫入任何 NVS 數據**。

### 6. 生產分區表完全對齊與快閃安全 (Partition Table Parity & Flashing Safety)
- 本 PoC 之 `partitions.csv` 完全對齊生產環境之 16MB Flash 佈局，產出之 `partitions.bin` 雜湊值（SHA256: `bd0f7954aca2ef7d925ee21aaa1f3dc8822d1d6ce5cbbd26a135e5886bfff6ce`）與生產環境完全一致：
  - `nvs`: `0x00009000` (28KB, `0x00007000`)
  - `otadata`: `0x0000e000` (8KB, `0x00002000`)
  - `app0` (ota_0): `0x00010000` (6400KB, `0x00640000`)
  - `app1` (ota_1): `0x00650000` (6400KB, `0x00640000`)
  - `spiffs`: `0x00c90000` (3456KB, `0x00360000`)
  - `coredump`: `0x00ff0000` (64KB, `0x00010000`)
- **安全燒錄建議（僅燒錄 `app0`，保留既有開機載入器與 NVS/OTA 數據）**：
  1. **燒錄前備份現有分區**：
     ```bash
     esptool.py --chip esp32s3 -p <PORT> read_flash 0x10000 0x640000 prod_app0_backup.bin
     esptool.py --chip esp32s3 -p <PORT> read_flash 0x9000 0x7000 prod_nvs_otadata_backup.bin
     ```
  2. **僅燒錄 PoC 至 `app0` (`0x10000`)**：
     ```bash
     esptool.py --chip esp32s3 -p <PORT> write_flash 0x10000 test/arduino_idf_integration_poc/.pio/build/esp32s3_arduino_idf_poc/firmware.bin
     ```
     *注意：切勿執行 `erase_flash` 或覆寫 `0x9000` (NVS) 與 `0xe000` (otadata)。*
  3. **測試完畢還原生產環境**：
     ```bash
     esptool.py --chip esp32s3 -p <PORT> write_flash 0x10000 .pio/build/lilygo-t-display-s3/firmware.bin
     ```

### 7. 隔離即時釣魚控制器與安全空白鍵 (Spacebar) HID 整合 (Live Fishing Controller & Safe HID: PASS)
- **單一按鍵 (SPACE = HID 0x2c) 投竿與起竿協同**：
  - 依據使用者之 WoW 釣魚插件規格，投竿（Cast）與起竿（Reel）均觸發單一空白鍵（Spacebar）。
  - 控制器實作非阻塞按鍵狀態機：按壓維持 80ms，隨後自動發送釋放；若主機端端點忙碌，自動於主迴圈非阻塞重試釋放直至確認或斷開。
- **連續多循環自動拋竿 (Continuous Multi-Cycle Operation & 2.0s Cooldown)**：
  - 依據使用者實地測試需求，起竿（Reel）動作成功完成且按鍵確認完全釋放後，控制器進入非阻塞 2.0 秒冷卻期（`COOLDOWN_DURATION_MS = 2000UL`）。
  - 冷卻期間持續進行安全檢查（USB 掛載、音訊串流在 1.0 秒內持續新鮮、零佇列丟包與斷流）。並在狀態轉換邊界（進入 Waiting、拋竿前、進入 Cooldown）徹底清除殘留音訊觸發旗標，防止拋竿水花聲學瞬態引發假咬鉤。
  - 冷卻時間屆滿後，若控制器仍處於武裝且狀態健康，自動發送下一次空白鍵拋竿進入 `Casting`，無須每一竿手動重新武裝。
  - **30 秒拋竿過期安全超時自動重投 (Safe Post-Cast-Expiry Timeout Auto-Recast)**：
    - 使用者插件投竿與起竿均使用單一空白鍵（Spacebar），若浮標未消失即發送按鍵將誤引發收竿。因此控制器設定 32 秒等待期限（`WAIT_TIMEOUT_MS = 32000UL`），完整覆蓋使用者回報之 30 秒遊戲拋竿視窗並具備 2 秒容限（即使發生按鍵重試亦確保過期）。
    - 滿 32 秒未咬鉤時，狀態機透過 `FishingEvent::BiteTimeout` 進入 `Timeout` 狀態，`totalTimeouts_` 嚴格遞增一次並清除殘留觸發。
    - **超時優先權機制**：到達或超過 32 秒截止線時，超時判定具有最高優先權，嚴格抑制任何遲到或瞬態聲學觸發，杜絕過期雜音在浮標消失後引發誤起竿。
    - 經非阻塞 1.5 秒超時顯示（`TIMEOUT_DURATION_MS = 1500UL`）與 2.0 秒冷卻（`COOLDOWN_DURATION_MS = 2000UL`）後，只要控制器仍處於武裝且 USB、音訊均健康，自動觸發下一次空白鍵拋竿（`Casting`），實現使用者要求之無咬鉤連續重投。
    - 實體按鍵 DOWN 在任何階段（包含 Timeout 與 Cooldown）均具最高硬體優先權，可隨時全閉停機並解除武裝。
- **審查修訂與安全契約強化 (Focused Security Corrections)**：
  1. **需正向封包進度與即時性 (Positive Packet Progress & Freshness Before Start/Press)**：
     - `init()` 時將 `lastPacketArrivalMs_ = 0`，未收過音訊封包前不認定串流新鮮。
     - `requestStart()` 檢查已武裝後之封包進度增量 (`currentPackets > armPacketsBaseline_`) 與新鮮度 (`(now - lastPacketArrivalMs_) <= STREAM_TIMEOUT_MS`)。零封包或過期音訊嚴禁啟動或發送按鍵。
  2. **非阻塞釋放重試與重新武裝鎖定 (Non-blocking Release Retry on Stop/Disarm & Rearm Lockout)**：
     - 在停機、解除武裝、斷流或錯誤時，若主機端 HID 端點回報忙碌 (`release()` 失敗)，控制器保持在 `KeyState::Releasing` 狀態並在後續 `update()` 滴答中持續重試釋放，直到主機接受或真正 unmount。
     - 在按鍵未完全釋放（`keyState_ != KeyState::Idle`）前，嚴禁重新武裝或重新啟動，避免卡鍵疊加。主機卸載（unmount）時安全重設本機狀態。
     - **實體 USB 匯流排斷線局限說明**：韌體於 unmount 時安全清理本機狀態，但若實體 USB 線纜在傳輸中途被拔除且主機端正處於按鍵按壓狀態，韌體無法控制作業系統主機端之按鍵鎖定狀態（此為 USB 實體匯流排之硬體物理限制）。
  3. **顯式確認發鍵與有界超時全閉 (Explicit Confirmed Keypress & Bounded Retry)**：
     - `triggerKey()` 進入 `KeyState::PendingPress`，在 500ms（`PRESS_RETRY_TIMEOUT_MS`）內非阻塞重試按鍵發送。
     - 若 500ms 內仍無法成功送出，立即以 `StopReason::PressFailure` 全閉停機並解除武裝。絕不靜默略過按鍵而推進狀態機或虛增 Cast/Reel 計數。
  4. **閒置狀態持續安全防護 (Continuous Safety Guards in Idle)**：
     - `checkSafetyGuards()` 在 `Idle` 狀態下亦嚴密監控 USB 掛載與連線狀態。若發生斷線或暫停，立即解除武裝（`armed_ = false`，`lastStopReason_ = StopReason::UsbUnmounted`）。重新連線時嚴禁自動重新武裝。
  5. **雙鍵武裝後放開判定 (Post-Arm Button Release Requirement)**：
     - 在雙鍵長按達 1.0s 武裝後，設定 `requireButtonsReleaseAfterArm_ = true`。必須檢測到使用者完全放開雙鍵（`!btnUp && !btnDown`），隨後發生的按鍵事件方可被識別為獨立啟動手勢，杜絕武裝後鬆開單鍵直接觸發啟動之誤操作。
  6. **空白鍵語意二義性防護與 32 秒安全過期視窗 (Single-Key Spacebar Timing & 32s Safe Expiry Window)**：
     - 由於使用者之 WoW 釣魚插件投竿與收竿均使用空白鍵（Spacebar），在浮標未消失前發送按鍵會誤引發收竿。為確保安全，控制器將 Waiting 階段之期限設為 32,000ms（WAIT_TIMEOUT_MS = 32000UL，自進入 Waiting 起算，自拋竿開始合計 33.5 秒），完整覆蓋 30 秒遊戲拋竿視窗並具備 2 秒以上安全容限（即便存在按鍵重試延遲亦能保證浮標自然消失）。
     - 超時到達時嚴格以超時為最高優先權，清除殘留聲學旗標，依序經由 Timeout 與 Cooldown 後在武裝且健康狀態下自動發送空白鍵重投，兼顧無咬鉤自動連續運作與浮標狀態同步。
  7. **無符號溢位安全算式 (Wrap-safe Unsigned Arithmetic)**：
     - 所有的時間與超時比對均嚴格遵循 `(now - start) >= duration` 形式，杜絕 32-bit 毫秒溢位造成之時序錯亂。
- **實體雙鍵顯式武裝機制 (Explicit Dual-Button Arming)**：
  - **預設開機安全狀態**：開機預設為 `DISARMED` 與 `IDLE`。在此狀態下，無論音訊信號或門檻觸發如何劇烈，**絕對禁止**發送任何自動化 HID 按鍵。
  - **雙鍵長按武裝**：使用者必須刻意同時長按 `BUTTON_UP` (GPIO 14) 與 `BUTTON_DOWN` (GPIO 0) 達 1.0 秒（1000ms），方可切換為 `ARMED` 狀態。
  - **開機按鍵保護 (Startup Held-Button Immunity)**：若開機/重置時按鍵處於被按住狀態，控制器自動忽略，直至使用者完全放開雙鍵後才重新接受手勢。
- **獨立啟動與最高優先權停止 (Separate Start/Stop)**：
  - **START**：短按 `BUTTON_UP`（僅在已武裝且處於 IDLE 時有效）啟動釣魚狀態機，進入 `Casting`。
  - **STOP**：任何時刻按下 `BUTTON_DOWN`，具最高硬體優先權，立即中斷當前動作、安全釋放按鍵並**強制解除武裝 (DISARM)**。
- **全閉安全保護 (Fail-Closed Safety Protections)**：
  - **USB 斷線/掛起**：偵測到 `!usb_composite_is_mounted()` 即刻停止並解除武裝。重新連線時**嚴禁**自動重新武裝（No Auto-Rearm）。
  - **音訊串流中斷 (Stream Dropout)**：運作中若超過 1.0 秒未接收到新音訊封包，立即視為音訊斷流，自動停止並解除武裝。
  - **佇列溢位與不連續事件 (Queue Drop / Discontinuity)**：底層驅動發生佇列丟包或串流不連續時，立即全閉停機並解除武裝。
- **聲學能量門檻局限與防誤觸警告 (Energy Threshold Disclaimer)**：
  - > [!WARNING]
  - > 偵測器採用之能量門檻尚未經實體遊戲聲學標定（Uncalibrated），不代表真實辨識「遊戲咬鉤特徵音」；環境背景音、音樂或介面音效均可能引發假陽性（False Positive）觸發。在已武裝且啟動狀態下，任何突波均會觸發起竿動作，使用者須了解自動化風險。

### 8. 清楚醒目之即時 LCD 遙測介面 (Prominently Legible UI Layout)
- 320x170 LCD 螢幕以高對比直接覆寫排版，兼顧測試數據、倒數計時與停機診斷：
  - **Line 1 (ARM 狀態與停機原因診斷)**：醒目顯示武裝狀態，在停機時精確區分原因而不遮蔽武裝標示：
    - 已武裝：綠底 `ARM: [ARMED - SPACEBAR ACTIVE]    `
    - 單循環完成：綠底 `ARM: [DONE - CYCLE COMPLETED]     `
    - 咬鉤逾時：橘底 `ARM: [DISARMED - BITE_TIMEOUT]    `
    - 音訊斷流：紅底 `ARM: [DISARMED - AUDIO_DROPOUT]   `
    - USB 斷線：紅底 `ARM: [DISARMED - USB_UNMOUNTED]   `
    - 佇列丟包：紅底 `ARM: [DISARMED - QUEUE_OVERFLOW]  `
    - 串流不連續：紅底 `ARM: [DISARMED - STREAM_DISC]     `
    - 預設安全：紅底 `ARM: [DISARMED - PASSIVE SAFE]    `
  - **Line 2 (FSM 狀態、倒數計時與按鍵)**：
    - Waiting 階段即時顯示 32 秒倒數：`State: WAITING (32s) | Key: IDLE   ` 下降至 `( 1s)`
    - 停機/閒置時顯示歸零與停機碼：`State: IDLE ( 0s) | Stop: BITE_TIMEOUT`，絕不於停機後虛報剩餘時間
  - **Line 3 (USB 與串流狀態)**：明確區分 `MOUNTED` / `UNMOUNT` 與串流活躍狀態。
  - **Line 4-6 (真實度量與數據)**：RMS、Peak、Effective Threshold、Casts、Bites、Reels、Timeouts、Packets、Queue Drops、Discontinuities。
  - **Line 7 (安全警示)**：`WARN: Energy threshold uncalibrated!`
  - **Line 8 (操作指南)**：`UP+DOWN (1s):ARM | UP:GO | DOWN:STOP`

---

## 二、 UAC 音訊橋接器與生產偵測器管線規格

### 1. 描述符與音訊資料規格
| 項目 | 規格值 | 說明 |
| :--- | :--- | :--- |
| **傳輸模式** | USB 2.0 High Speed 等時傳輸 (Isochronous OUT Endpoint 0x01) | TinyUSB UAC 2.0 Audio Streaming |
| **取樣率** | 48,000 Hz | 由 `CONFIG_UAC_SAMPLE_RATE` 宣告 |
| **聲道數** | 2 (Stereo: Left, Right) | 由 `CONFIG_UAC_SPEAKER_CHANNEL_NUM` 宣告 |
| **位元深度** | 16-bit signed PCM (Little-Endian) | 每樣本 2 位元組，範圍 [-32768, 32767] |
| **幀大小** | 4 Bytes / Frame | `[L_low, L_high, R_low, R_high]` |
| **典型封包** | 48 樣本/毫秒 (約 192 Bytes/ms) | 10ms 典型回呼為 1,920 位元組 |

### 2. 封包邊界、殘留位元組與中斷處理機制
USB 回呼函式 `output_cb(buf, len, ...)` 收到的位元組長度可能非 4 的整數倍（例如封包被拆分或非對齊）：
1. 若內部殘留緩衝區含有位元組（`residualLen_ > 0`），優先自傳入緩衝區取出補足至 4 位元組，完成前一幀並送出。
2. 批量處理傳入緩衝區中所有完整的 4 位元組幀，透過雙聲道混音算式 `(int32_t(left) + int32_t(right)) / 2` 填入內部有界 `monoScratch_[128]`。
3. 若緩衝區尾端剩下 1 至 3 個未完成幀的位元組，存入 `residual_`，等待下次回呼補足，絕不丟棄樣本亦不產生相位偏移。
4. **生命週期中斷與佇列丟包重設機制（已完整串接並通過驗證）**：橋接器提供 `resetResidualOnStreamBreak()`，並與複合驅動之 `discontinuity_cb` 串接。當串流生命週期變更（關閉、開啟、掛起、卸載）或佇列發生溢位丟包時，消費任務保證在下一批音訊餵入前觸發重設，徹底清除未對齊位元組殘留與偵測器未成幀樣本，消滅跨中斷樣本污染。

### 3. 即時遙測指標與 1 Hz TFT 螢幕儀表板（可區分靜音與無封包）
透過 `bridge.getMetricsSnapshot()` 與 `usb_composite_get_audio_stats()` 發布之指標：
- `packetsReceived`：累積 USB 回呼次數。
- `bytesReceived`：累積接收原始位元組數。
- `stereoFramesDecoded`：成功解碼之 4 位元組雙聲道立體聲幀數。
- `monoSamplesIngested`：注入偵測器之單聲道樣本數。
- `unalignedChunkCount`：尾端帶有殘留位元組之封包數。
- `residualDiscontinuityCount`：串流中斷時丟棄之殘留位元組事件數。
- `streamDiscontinuityCount`：總斷流信號觸發次數（包含串流生命週期重開與佇列溢位）。
- `detectorMetrics.rms`：最新幀 RMS 振幅（靜音串流為 `0.0`，無封包時幀數不前進）。
- `detectorMetrics.peak`：最新幀最大絕對峰值。
- `detectorMetrics.noiseFloor`：動態環境底噪 RMS 估計值。
- `detectorMetrics.effectiveThreshold`：動態觸發門檻值。
- `detectorMetrics.triggerCount`：有效信號突波觸發累積次數。
- `detectorMetrics.triggered`：觸發鎖定旗標（讀取後可手動清除）。

#### 320x170 TFT 實體螢幕即時儀表板 (ST7789 1 Hz Dashboard)
為支援實體硬體測試（尤其在 USB OTG 複合模式可能接管主機 COM3 序列埠時），於 `main.cpp` 的 `loop()` 中實作低頻（1 Hz）可讀畫面更新：
- **行 0 (y=2)**：`PAXG UAC2 Audio PoC (TEST ONLY)`（黃色，標記測試專用）。
- **行 1 (y=20)**：`MODE: Passive Audio (NO AUTO-HID)`（青色，明確聲明無自動按鍵）。
- **行 2 (y=38)**：`USB: MOUNTED` 或 `USB: UNMOUNTED`（綠/紅，清晰反應列舉狀態）。
- **行 3 (y=56)**：音訊狀態顯式三態區分：
  - `Stream: NO PACKETS (Idle / Wait)`（橙色，無封包等待中）
  - `Stream: DIGITAL SILENCE (0.0 RMS)`（青色，收到全零數位靜音封包）
  - `Stream: AUDIO ACTIVE (Streaming)`（綠色，實際聲音波形注入中）
- **行 4 (y=74)**：`Pkts: %llu | Q Drop: %lu`（白色，若有丟包以洋紅色警示）。
- **行 5 (y=92)**：`Frames: %llu | Disc: %llu`（白色，顯示已解碼立體聲幀與斷流計數）。
- **行 6 (y=110)**：`RMS: %.1f Pk: %u Th: %.1f`（白色，顯示當前幀能量、峰值與動態門檻）。
- **行 7 (y=128)**：`Trigs: %u | Latched: YES/NO`（觸發鎖定時以綠色醒目提示）。
- **行 8 (y=146)**：`Q Max: %lu/8 | Wi-Fi: Mock/Off`（深灰色，註記佇列高水位與 Wi-Fi 純佔位不聯網）。
- **繪圖設計特性**：
  - 僅由 Arduino `loop()` 以 1 秒間隔繪製，直接讀取執行緒安全快照，不使用可變共享緩衝區，杜絕中斷音訊任務（`usb_spk_task`）。
  - 採前景色＋背景色直接繪製與固定字元寬度填充，免除全螢幕重刷的閃爍問題，全過程零動態記憶體配置。

---

## 三、 本機單元測試與並行壓力驗證

在 `test/arduino_idf_integration_poc/test/test_uac_audio_bridge.cpp` 中實作了 10 項針對生產橋接器、生產偵測器與生產佇列之確定性單元測試與並行壓力測試：

1. **`test_bridge_init_and_empty_packet_handling`**：
   - 驗證初始指標為零。
   - 驗證 `nullptr` 與 `len == 0` 的極端情況安全攔截，累計 `nullOrEmptyPackets` 計數器且不崩潰。
   - 驗證 `reset()` 清理全部指標與殘留狀態。
2. **`test_arbitrary_chunk_sizes_and_residuals`**：
   - 將 12 幀立體聲音訊（48 Bytes）切分成 `[1, 2, 1, 5, 3, 7, 6, 2, 11, 10]` 等 10 個任意長度碎片依序餵入。
   - 嚴格斷言最終解碼幀數恰好為 12 幀，殘留緩衝區在結尾完全歸零，零樣本遺失且無相位顛倒。
3. **`test_stereo_mix_extremes`**：
   - **純左聲道**（L=4000, R=0）-> 降混 RMS=2000.0, Peak=2000。
   - **純右聲道**（L=0, R=6000）-> 降混 RMS=3000.0, Peak=3000。
   - **反相抵消**（L=+5000, R=-5000）-> 降混 RMS=0.0, Peak=0。
   - **正向滿幅**（L=+32767, R=+32767）-> 降混 RMS=32767.0, Peak=32767。
   - **負向滿幅**（L=-32768, R=-32768）-> 降混 RMS=32768.0, Peak=32768（驗證 `INT16_MIN` 無符號反轉未定義行為或截斷）。
4. **`test_silence_vs_signal_distinction`**：
   - 驗證「無 USB 串流」狀態下封包數為 0、幀數為 0。
   - 驗證「數位靜音串流」注入時，封包數與幀數正常前進，但 RMS 維持 `0.0`，不會誤觸發。
   - 驗證「有效信號突波」注入時，連續 2 幀超標後確認去抖並觸發，`clearTrigger()` 可安全解鎖。
5. **`test_timing_and_cooldown_behavior`**：
   - 驗證第一次觸發後進入 25 幀冷卻閉鎖期。
   - 驗證冷卻期間再度送入大振幅雜訊，`triggerCount` 被嚴格抑制不遞增。
   - 驗證 25 幀冷卻完全耗盡後，偵測器安全重新武裝（Re-arm），後續突波成功觸發使 `triggerCount` 遞增至 2。
6. **`test_stream_break_residual_reset`**：
   - 餵入 18 個位元組（4 完整幀 + 2 個殘留位元組）。
   - 呼叫 `resetResidualOnStreamBreak()` 模擬串流中斷，斷言殘留長度歸零且 `residualDiscontinuityCount` 遞增為 1。
   - 隨後餵入全新 4 位元組完整幀，斷言立即以正確相位解碼，證明無聲道交錯顛倒。
7. **`test_concurrent_audio_stream_and_control_access`**：
   - 啟動 3 個真實並行執行緒進行並發壓力測試：
     - **Producer 執行緒**（模擬 TinyUSB speaker callback）：持續推送 800 個非對齊音訊碎片。
     - **Reader 執行緒**（模擬 Arduino 主迴圈）：以 200 微秒頻率高頻獲取度量快照並清除觸發。
     - **Control 執行緒**：週期性讀取與更新門檻設定，並模擬串流中斷調用 `resetResidualOnStreamBreak()`。
   - 執行 800 個封包並發未發生死鎖、崩潰或記憶體未定義行為，所有幀與中斷度量皆維持一致性。

8. **`test_deterministic_discontinuity_sample_isolation`**：
   - 注入帶有 `USB_AUDIO_SLOT_FLAG_DISCONTINUITY` 的封包，斷言橋接器重設殘留並記錄中斷。
9. **`test_end_to_end_consumer_pipeline_ordering`**：
   - 驗證端到端音訊封包的時序傳遞與解碼順序。
10. **`test_multithreaded_pipeline_race_stress`**：
    - 多執行緒高壓並發注入與讀取，驗證資料完整性與零死鎖。

### 測試執行結果 (PASS: 73/73)
- **UAC PCM 串流橋接管線測試 (10/10 PASS)**：覆蓋封包切分邊界、任意碎片殘留處理、極限混音、數位靜音與有效信號辨別、冷卻閉鎖期、中斷重設、多執行緒高壓並發。
- **Live 釣魚控制器與安全 HID 測試 (28/28 PASS)**：覆蓋開機被動安全、正向封包進度檢驗、釋放重試與武裝鎖定、發鍵失敗全閉停機、閒置斷線自動解除武裝、雙鍵武裝後放開要求、32 秒咬鉤逾時全閉、29 秒晚期咬鉤起竿循環、連續多循環冷卻自動重投、30 秒拋竿過期安全超時自動重投、全階段 DOWN 鍵停機最高優先權。
- **PAXG 行情與雙畫面整合測試 (17/17 PASS)**：覆蓋嚴格遞迴下降 JSON 語法解析（拒絕嵌於字串值內之虛假鍵名、巢狀衝突幣種、語法錯誤結構、陣列、重複鍵與尾隨字串）、非數值/非有限浮點數/NaN/inf 與尾隨字元拒絕、行情即時性判定、網路失敗降級為 Stale、零初始化 `struct tm timeinfo` 與時間同步嚴格閘控（杜絕未定義行為與偽造狀態）、`APP_TIMEZONE` 本地時間時區基準校準、50ms 實體接點彈跳抑制、模式切換強制解除武裝不漏鍵、實體 DOWN 鍵「先停機再退出」(Stop-Then-Exit) 語意、雙鍵武裝後完全放開要求、重新進入釣魚畫面初始維持 Disarmed/Idle、長停機字串（`STREAM_DISCONTINUITY`）305px 完整清除無殘影、停機原因跨畫面保存、多執行緒快照並發讀寫安全、`FishingUiState::differsFrom` 狀態差異化評估（抑制穩態冗餘重繪，降低匯流排負載 90-95% 避免音訊任務飢餓）。
- **滿包音訊停機、指令佇列與區域網路 Web 伺服器測試 (19/19 PASS)**：覆蓋 3.0s 週期性鈴聲脈衝檢驗、單次孤立雜訊/非週期突波拒絕、500ms 鈴聲內部去抖動、動態校準更新、等待咬鉤/冷卻/閒置全階段安全檢驗、咬鉤優先權搶佔、孤立脈衝不阻擋啟動且等待咬鉤正常起竿、確認滿包停機解除武裝並鎖定重啟、控制器擁有者迴圈有界線程安全指令佇列、緊急停止（Stop）最高優先權搶佔與待處理啟動清除、命令實際結果同步回報、同步發布狀態快照隔離、清包後顯式重設（`clearBagFull`）、Web JSON 遙測序列化。

---

## 四、 整合目標編譯與符號驗證狀態 (Integrated Target Build & Symbol Status: PASS)

在 TinyUSB 依賴防護、USB 複合裝置音訊佇列、認證級 TLS、雙緩衝全螢幕 Sprite（`s_fishingSprite` 108,800 bytes / 106.3 KB，總記憶體佔用約 204.7 KB 於 >350 KB 可用內部 SRAM）、滿包音訊偵測器、有界執行緒安全指令佇列與非同步 Web 伺服器工作完成後，執行完整目標建置與符號表檢驗：

1. **整合建置成功 (Build Success)**：
   - 透過 PlatformIO ESP-IDF 5.1.2 建置。
   - `main/CMakeLists.txt` 編譯 `main.cpp`、`mode_navigator.cpp`、`paxg_async_quote.cpp`、`live_fishing_controller.cpp`、`uac_audio_bridge.cpp`、`bag_full_detector.cpp`、`fishing_web_server.cpp`，並鏈結 `usb_device_composite` 與 Arduino 組件。
   - 產出最終二進位檔案：`firmware.elf` 與 `firmware.bin` (RAM: 14.9%, Flash: 17.2%)。
2. **符號表驗證全數通過 (Symbols Verified: 23/23)**：
   - `TFT_eSPI::begin` (TFT_eSPI Display Driver)
   - `TFT_eSprite::createSprite` (TFT_eSprite Framebuffer Support)
   - `WiFiSTAClass::begin` (Arduino Wi-Fi STA)
   - `WiFiGenericClass::mode` (Arduino Wi-Fi Generic Mode)
   - `usb_composite_init` (Composite USB Device Init)
   - `tud_audio_n_read` (TinyUSB Audio Class 2.0 Speaker)
   - `usb_composite_hid_keyboard_press` (Composite USB HID Keyboard)
   - `desc_configuration` (TinyUSB Composite Configuration Descriptor)
   - `manual_trigger_keystroke_demo` (Manual Keystroke Trigger Function)
   - `g_hid_demo_ref` (Volatile HID Reference Pointer)
   - `paxg::uac::UacAudioBridge::consumeUacChunk` (UAC PCM Audio Bridge Consumer)
   - `paxg::audio::PcmAudioDetector::processSamples` (Production Audio Detector Consumer)
   - `usb_composite_get_audio_stats` (Composite Driver Audio Queue Telemetry)
   - `paxg::fishing::LiveFishingController::update` (Live Fishing Controller Main Update Loop)
   - `paxg::fishing::LiveFishingController::requestStart` (Live Fishing Controller Start Operation)
   - `parseBinanceTicker` (PAXG Binance 24h Ticker JSON Parser)
   - `getMarketQuoteSnapshot` (PAXG Thread-Safe Market Quote Reader)
   - `startPaxgQuoteWorker` (PAXG Bounded Async Network Worker Task)
   - `evaluateMarketSessions` (Market Sessions Evaluator)
   - `ModeNavigator::update` (Mode Navigator Loop Update)
   - `paxg::fishing::BagFullDetector::processRms` (Bag Full Audio Periodicity Detector)
   - `paxg::web::startFishingWebServer` (Deliberate Fail-Closed LAN Web Server)
   - `paxg::web::processPendingCommands` (Owner-Loop Thread-Safe Command Queue Processor)
3. **滿包即時音訊測量證據與指定信號辨識 (Live Hardware Audio Evidence)**：
   - 使用者確認：使用者確認此鐘聲/鈴聲專門且唯一配置給滿包警示，作為指定滿包信號。
   - 採樣環境：ESP32-S3 USB Audio Class 揚聲器裝置（VID 0x303A, PID 0x8002）。
   - 錄音分析：6.09 秒 WASAPI Loopback 實錄 PCM 音訊（2,338,560 位元組）。
   - 脈衝週期：$T_0 = 0.016s$ (RMS 4675.8), $T_1 = 2.968s$ (RMS 17030.2, $\Delta t = 2.952s$), $T_2 = 6.008s$ (RMS 20132.4, $\Delta t = 3.040s$)，平均間隔 2.996s。
   - 頻譜特徵：4.1 kHz 鐘聲泛音（強度 1225.6）伴隨 200–350 Hz 本體共鳴，環境底噪 RMS < 500。
   - 背包警告音相似度達 0.90 才計為一次脈衝。連續三次脈衝的兩段間隔都在 2650–3350 毫秒內，才鎖定 `BAG_FULL`。目前相似度可從首頁音訊設定或 `GET /api/audio` 查看。
   - 確認滿包嚴格阻擋（Confirmed Bag Full Protection）：僅在確認週期性脈衝滿足校準視窗時鎖定 `bagFullLatched`，控制器立即阻擋 `requestStart`，並全面停機解除武裝且鎖定狀態；單次孤立音訊脈衝（如咬鉤水花聲）不被誤判為滿包，確保起竿正常進行。
   - 顯式重設機制：當使用者在遊戲內清理背包後（鈴聲停止），可透過 Web 介面或 `/api/bagfull/reset` 顯式呼叫 `clearBagFull()`，清除鎖定與脈衝時序；若背包未清空，鈴聲再度響起將立即重新阻擋。
   - **未標定/目標驗證免責聲明**：雖然依據實錄音訊設定了基準參數，但在未經過目標環境全盤實地驗證前，不得假設實體偵測器已完全校準；韌體提供全閉保護與顯式配置介面。
4. **指令路由、停止優先權與狀態快照同步架構**：
   - HTTP 伺服器任務嚴格解耦：HTTP 處理函式禁止直接調用 `LiveFishingController` 的方法或讀取其未同步內部狀態。
   - 有界執行緒安全指令佇列：所有 Web 控制指令（Start、Stop、ArmToggle、ResetBagFull）皆進入有界佇列，由控制器擁有者迴圈（`main.cpp` 的 `loop()`）順序執行。
   - 停止優先權保證（Stop Priority）：緊急停止指令擁有專屬優先插槽，且在入列時立即清除所有待處理之 Start 指令，由擁有者迴圈第一優先搶佔執行，保證停止與解除武裝絕對不被阻塞。
   - 命令實際結果回報：`submitCommandSync` 透過條件變數等待擁有者迴圈執行完成，向 HTTP 客戶端精確回報實際執行結果（如成功狀態或 `BAG_FULL`、`DISARMED`、`SAFETY_REJECTED` 原因代碼）。
   - 釣魚首頁底部的 `Firmware` 欄位顯示裝置目前執行的韌體版本，開啟首頁即可查看。
   - 展開釣魚首頁的 `Bite detection settings` 可調整咬鉤 RMS 門檻（1–32768）。儲存後立即套用，重啟後保留。恢復預設值會設回 1500。
   - `BAG_FULL` 或 `QUEUE_OVERFLOW` 造成停釣時會觸發各自設定的 MacroDroid webhook。可在釣魚首頁的 `Stop Reason` 查看原因。短暫丟包後自動恢復時不會發通知。
   - `QUEUE_OVERFLOW` webhook 收到 HTTP 2xx 後，韌體等待 3 分鐘，再依原本的開釣條件嘗試續釣一次。這段期間手動停止或操作解鎖會取消續釣。`BAG_FULL` 停釣後須手動處理。
   - 同步狀態快照（Synchronized Status Snapshot）：擁有者迴圈在每輪主迴圈末端發布完整快照至互斥鎖保護之結構體，HTTP `/api/status` 端點僅讀取已發布之快照，完全杜絕資料競爭。
5. **可編輯 Wi-Fi 設定與斷線安全復原機制 (Editable Wi-Fi Settings & Setup AP Recovery)**：
   - **專屬 NVS 儲存與快閃保護**：Wi-Fi 憑證由 `paxg::wifi::WifiSettingsManager` 獨立管理，採用專屬 `paxg_wifi` NVS 命名空間，保持 `WiFi.persistent(false)`，絕不讓底層自動覆寫，且絕不改動 `.env`。
   - **憑證回退階層**：開機優先讀取專屬 NVS 記錄；若無則回退至編譯期常數憑證；若無法連線或無設定，自動切換至有時限之 WPA2 設定熱點（Setup AP）。
   - **有時限 WPA2 Setup AP 與實體 TFT 密碼**：嚴禁開放式 AP（No Open AP），密碼由設備隨機產生且至少 8 字元（`paxgXXXX`），與 SSID（`PAXG-Setup-XXXX`）、IP（`192.168.4.1`）及單次驗證 PIN 碼同步顯示於 TFT 螢幕。
   - **本機 PIN 授權與自訂標頭防護**：變更 Wi-Fi 設定必須輸入螢幕顯示之 4 位數 PIN，並由 POST 端點驗證自訂標頭（`X-Requested-With` / `X-Setup-Auth`）及有界輸入長度（SSID $\le 32$、Password $\le 64$），杜絕 CSRF 攻擊。密碼絕不回顯或記錄於日誌。
   - **Setup AP 禁止 HID 鍵盤指令**：當連線來自 Setup AP 子網或 Setup AP 處於活躍狀態時，`/api/start` 與 `/api/arm` 等控制端點立即拒絕（403 Forbidden），防範未授權之按鍵注入。
   - **安全連線驗證與失敗復原**：套用新設定時立即安全解除釣魚武裝（Disarm in owner context），並以 `WIFI_AP_STA` 模式在背景連線，保持 Setup AP 可連線；僅在 STA 連線成功且取得 DHCP IP 後才寫入 NVS；若連線超時（15秒）或失敗，絕不寫入 NVS，並安全復原至前次有效設定或回退憑證，保持 Setup AP 持續可用。
   - **非阻塞運作**：所有連線與重試均採非同步狀態機推進，絕不阻塞 Core 0 / Core 1，完全不干擾 USB UAC 2.0 音訊與 HID 控制。

6. **現有局限與執行環境聲明**：
   - 本韌體已完成靜態編譯、鏈結、85/85 本機單元測試與 25/25 符號檢驗。
   - 本次工作階段未進行實體設備燒錄亦未進行遊戲內自動按鍵測試。

---

## 五、 自動化驗證指令

```powershell
# 執行主機測試與生產跨編譯檢查（--host-only 避免觸碰其他並行作業中的共用元件）
python test/arduino_idf_integration_poc/run_validation.py --host-only

# 檢查生產程式庫相容性與符號
python test/arduino_idf_integration_poc/run_validation.py --check-production

# 執行驗證並自動清理專屬測試產物（不使用萬用字元，僅刪除 OWNED_TEST_ARTIFACTS）
python test/arduino_idf_integration_poc/run_validation.py --host-only --clean-after

# 僅清理自身測試產物
python test/arduino_idf_integration_poc/run_validation.py --clean-only
```

---

## 六、 安全復原與回滾指引 (Safe Restoration Requirements)

若需還原至本次管線實作前之狀態，僅需移除或還原以下檔案，其餘代碼庫（特別是 4 個包含使用者 WiFi 工作的髒檔案）完全不受任何影響：
1. 還原 `test/arduino_idf_integration_poc/main/main.cpp`
2. 還原 `test/arduino_idf_integration_poc/main/CMakeLists.txt`
3. 刪除 `test/arduino_idf_integration_poc/main/live_fishing_controller.h`
4. 刪除 `test/arduino_idf_integration_poc/main/live_fishing_controller.cpp`
5. 刪除 `test/arduino_idf_integration_poc/main/uac_audio_bridge.h`
6. 刪除 `test/arduino_idf_integration_poc/main/uac_audio_bridge.cpp`
7. 刪除 `test/arduino_idf_integration_poc/main/bag_full_detector.h`
8. 刪除 `test/arduino_idf_integration_poc/main/bag_full_detector.cpp`
9. 刪除 `test/arduino_idf_integration_poc/main/fishing_web_server.h`
10. 刪除 `test/arduino_idf_integration_poc/main/fishing_web_server.cpp`
11. 刪除 `test/arduino_idf_integration_poc/test/`
12. 還原 `test/arduino_idf_integration_poc/run_validation.py`
13. 還原 `test/arduino_idf_integration_poc/README.md`
