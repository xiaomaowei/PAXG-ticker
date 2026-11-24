/**
 * @file ota_dashboard.h
 * @brief paxg::ota — Self-contained OTA dashboard HTML/CSS/JS (Traditional Chinese, Taiwan).
 *
 * Defines:
 *   namespace paxg { namespace ota {
 *     inline constexpr char kDashboardHtml[];
 *   }}
 *
 * Security model
 * --------------
 *  - Bearer token stored in JS variable only; never logged or put in innerHTML.
 *  - API response data rendered via textContent / DOM; no innerHTML from server data.
 *  - SHA-256 uses WebCrypto when available (HTTPS/localhost).
 *    On plain HTTP (private IP), falls back to a pure-JS FIPS 180-4 implementation
 *    whose K constants are derived at runtime from ∛prime fractional parts.
 *  - Firmware ELF digest extracted from binary bytes 176–207 (32 bytes).
 *  - Firmware version string extracted from bytes 48–79 (32 bytes, NUL-terminated).
 *
 * No CDN, no external assets.  Single-file, header-only.
 */

#pragma once

namespace paxg {
namespace ota {

inline constexpr char kDashboardHtml[] = R"PAXGHTML(<!DOCTYPE html>
<html lang="zh-TW">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PAXG OTA 韌體更新</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Helvetica Neue',Arial,'Noto Sans TC',sans-serif;background:#0f1117;color:#e2e8f0;min-height:100vh;display:flex;flex-direction:column}
header{background:#1a1d2e;border-bottom:1px solid #2d3148;padding:12px 20px;display:flex;align-items:center;gap:12px}
header h1{font-size:1.1rem;font-weight:700;color:#a78bfa;letter-spacing:.02em}
header .sub{font-size:.75rem;color:#64748b;margin-left:auto}
#app{flex:1;padding:16px;max-width:860px;margin:0 auto;width:100%}
.card{background:#1a1d2e;border:1px solid #2d3148;border-radius:10px;padding:16px;margin-bottom:14px}
.card-title{font-size:.8rem;font-weight:600;color:#94a3b8;text-transform:uppercase;letter-spacing:.06em;margin-bottom:10px}
/* login */
#login-card{max-width:380px;margin:60px auto 0}
#login-card label{display:block;font-size:.85rem;color:#94a3b8;margin-bottom:6px}
#login-card input{width:100%;background:#0f1117;border:1px solid #2d3148;border-radius:6px;color:#e2e8f0;padding:8px 10px;font-size:.9rem;font-family:monospace;outline:none}
#login-card input:focus{border-color:#a78bfa}
.btn{display:inline-flex;align-items:center;gap:6px;background:#4f46e5;color:#fff;border:none;border-radius:6px;padding:8px 16px;font-size:.85rem;cursor:pointer;transition:background .15s}
.btn:hover{background:#6366f1}
.btn:disabled{background:#374151;cursor:not-allowed;color:#6b7280}
.btn-sm{padding:5px 10px;font-size:.78rem}
.btn-danger{background:#7f1d1d;border:1px solid #991b1b}
.btn-danger:hover{background:#991b1b}
.btn-warn{background:#78350f;border:1px solid #92400e}
.btn-warn:hover{background:#92400e}
.btn-green{background:#14532d;border:1px solid #166534}
.btn-green:hover{background:#166534}
/* kv grid */
.kv{display:grid;grid-template-columns:max-content 1fr;gap:4px 12px;font-size:.82rem}
.kv .k{color:#64748b;white-space:nowrap}
.kv .v{color:#e2e8f0;word-break:break-all;font-family:monospace;font-size:.8rem}
/* status pill */
.pill{display:inline-block;padding:2px 8px;border-radius:999px;font-size:.72rem;font-weight:600}
.pill-green{background:#14532d;color:#4ade80}
.pill-red{background:#7f1d1d;color:#f87171}
.pill-yellow{background:#78350f;color:#fbbf24}
.pill-gray{background:#1f2937;color:#9ca3af}
/* log */
#log-box{background:#0a0d14;border:1px solid #1e2232;border-radius:6px;height:160px;overflow-y:auto;padding:8px;font-size:.75rem;font-family:monospace;color:#94a3b8}
#log-box .log-row{padding:1px 0;border-bottom:1px solid #111827}
#log-box .log-ts{color:#4b5563;margin-right:8px}
/* progress bar */
.prog-wrap{background:#0f1117;border-radius:4px;height:10px;overflow:hidden;margin-top:8px}
.prog-bar{height:100%;background:#4f46e5;border-radius:4px;transition:width .3s}
/* error banner */
#err-banner{display:none;background:#7f1d1d;border:1px solid #991b1b;border-radius:6px;padding:10px 14px;font-size:.82rem;color:#fca5a5;margin-bottom:10px;white-space:pre-wrap;word-break:break-word}
/* row util */
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
/* tabs */
.tabs{display:flex;gap:2px;margin-bottom:14px;border-bottom:1px solid #2d3148}
.tab{padding:8px 14px;font-size:.82rem;cursor:pointer;border-radius:6px 6px 0 0;color:#64748b;user-select:none}
.tab.active{background:#1a1d2e;color:#a78bfa;border:1px solid #2d3148;border-bottom:1px solid #1a1d2e;margin-bottom:-1px}
.tab:hover:not(.active){color:#94a3b8}
.tab-pane{display:none}.tab-pane.active{display:block}
/* upload zone */
#drop-zone{border:2px dashed #2d3148;border-radius:8px;padding:28px;text-align:center;cursor:pointer;color:#64748b;font-size:.85rem;transition:border-color .15s}
#drop-zone.over{border-color:#a78bfa;color:#a78bfa}
#file-info{font-size:.8rem;color:#94a3b8;margin-top:8px}
/* modal */
#modal-overlay{display:none;position:fixed;inset:0;background:rgba(0,0,0,.7);z-index:100;align-items:center;justify-content:center}
#modal-overlay.show{display:flex}
#modal-box{background:#1a1d2e;border:1px solid #2d3148;border-radius:10px;padding:20px;max-width:400px;width:90%}
#modal-box h3{font-size:.95rem;margin-bottom:10px;color:#f1f5f9}
#modal-box p{font-size:.83rem;color:#94a3b8;margin-bottom:16px;line-height:1.5}
/* spinner */
.spin{display:inline-block;width:14px;height:14px;border:2px solid #4b5563;border-top-color:#a78bfa;border-radius:50%;animation:sp .6s linear infinite}
@keyframes sp{to{transform:rotate(360deg)}}
/* responsive */
@media(max-width:500px){.kv{grid-template-columns:1fr;gap:2px 0}.kv .k{margin-top:6px}}
</style>
</head>
<body>
<header>
  <h1>⚙ PAXG OTA 控制台</h1>
  <span class="sub" id="hdr-device">—</span>
</header>

<div id="app">
  <!-- Login -->
  <div class="card" id="login-card">
    <div class="card-title">登入 — 輸入存取金鑰</div>
    <label for="tok-in">Bearer Token（64 位十六進制）</label>
    <input id="tok-in" type="password" autocomplete="off" spellcheck="false"
           placeholder="從裝置 LCD 取得金鑰後貼上" maxlength="64">
    <div style="margin-top:12px" class="row">
      <button class="btn" id="login-btn" onclick="doLogin()">確認登入</button>
      <span id="login-err" style="font-size:.8rem;color:#f87171"></span>
    </div>
  </div>

  <!-- Dashboard (hidden until login) -->
  <div id="dashboard" style="display:none">
    <div id="err-banner" id="global-err"></div>

    <!-- Tabs -->
    <div class="tabs">
      <div class="tab active" onclick="switchTab('status')">狀態</div>
      <div class="tab" onclick="switchTab('update')">韌體更新</div>
      <div class="tab" onclick="switchTab('logs')">事件紀錄</div>
    </div>

    <!-- ===== STATUS TAB ===== -->
    <div class="tab-pane active" id="tab-status">
      <div class="card">
        <div class="card-title">版本資訊 <span id="ver-refresh-spin" style="display:none"><span class="spin"></span></span></div>
        <div class="kv" id="ver-kv"></div>
      </div>
      <div class="card">
        <div class="card-title row" style="justify-content:space-between">
          <span>健康狀態 <span id="health-pill"></span></span>
          <span id="health-spin" style="display:none"><span class="spin"></span></span>
        </div>
        <div class="kv" id="health-kv"></div>
        <div class="row" style="margin-top:12px;gap:8px">
          <button class="btn btn-sm btn-warn" onclick="cmdReboot()" id="btn-reboot">重新啟動</button>
          <button class="btn btn-sm btn-danger" onclick="cmdRollback()" id="btn-rollback">回滾韌體</button>
        </div>
      </div>
    </div>

    <!-- ===== UPDATE TAB ===== -->
    <div class="tab-pane" id="tab-update">
      <div class="card">
        <div class="card-title">選擇韌體檔案</div>
        <div id="drop-zone" onclick="document.getElementById('fw-file').click()"
             ondragover="dzOver(event)" ondragleave="dzLeave()" ondrop="dzDrop(event)">
          拖曳 .bin 檔案至此，或點擊選擇
        </div>
        <input type="file" id="fw-file" accept=".bin,application/octet-stream"
               style="display:none" onchange="onFileChosen(this.files[0])">
        <div id="file-info"></div>
        <div id="fw-meta-kv" class="kv" style="margin-top:10px"></div>
      </div>
      <div class="card" id="upload-card" style="display:none">
        <div class="card-title">上傳進度</div>
        <div id="upload-status" style="font-size:.82rem;color:#94a3b8"></div>
        <div class="prog-wrap"><div class="prog-bar" id="prog-bar" style="width:0%"></div></div>
        <div id="upload-detail" style="font-size:.78rem;color:#64748b;margin-top:6px"></div>
      </div>
      <div class="row" style="gap:8px;margin-top:4px">
        <button class="btn btn-green" id="btn-upload" disabled onclick="startUpload()">開始上傳</button>
        <span id="upload-err" style="font-size:.8rem;color:#f87171"></span>
      </div>
    </div>

    <!-- ===== LOGS TAB ===== -->
    <div class="tab-pane" id="tab-logs">
      <div class="card">
        <div class="card-title row" style="justify-content:space-between">
          <span>裝置事件紀錄</span>
          <button class="btn btn-sm" onclick="fetchLogs()">重新整理</button>
        </div>
        <div id="log-box"></div>
      </div>
    </div>
  </div><!-- /dashboard -->
</div><!-- /app -->

<!-- Confirm modal -->
<div id="modal-overlay">
  <div id="modal-box">
    <h3 id="modal-title"></h3>
    <p id="modal-body"></p>
    <div class="row">
      <button class="btn btn-danger" id="modal-ok">確認</button>
      <button class="btn" onclick="closeModal()">取消</button>
    </div>
  </div>
</div>

<script>
"use strict";
// ─── State ───────────────────────────────────────────────────────────────────
let _tok = '';           // Bearer token (JS variable only, never logged)
let _busy = false;       // blocks concurrent commands
let _pollTimer = null;
let _dashTimer = null;
let _fwFile = null;
let _fwSha  = '';        // hex SHA-256 of selected firmware
let _fwElfDigest = '';   // hex of ELF SHA from firmware bytes 176-207
let _fwVersion   = '';   // version string from bytes 48-79
let _modalOkCb = null;

// ─── SHA-256 (FIPS 180-4) ────────────────────────────────────────────────────
// K constants derived at runtime: floor(cbrt(p) * 2^32) for first 64 primes.
// Works in both secure (HTTPS) and insecure (HTTP private IP) contexts.
const _sha256 = (() => {
  // Generate first 64 prime numbers
  function genPrimes(n) {
    const ps = [];
    for (let c = 2; ps.length < n; c++) {
      let ok = true;
      for (let i = 0; i < ps.length && ps[i] * ps[i] <= c; i++)
        if (c % ps[i] === 0) { ok = false; break; }
      if (ok) ps.push(c);
    }
    return ps;
  }
  // floor(frac(x^(1/3)) * 2^32) as unsigned 32-bit int
  function cbrtFrac32(p) {
    const r = Math.cbrt(p);
    return (r - Math.floor(r)) * 4294967296 >>> 0;
  }
  const primes64 = genPrimes(64);
  const K = new Uint32Array(primes64.map(cbrtFrac32));

  // Initial hash values: floor(frac(sqrt(first 8 primes)) * 2^32)
  const H0 = new Uint32Array(genPrimes(8).map(p => {
    const r = Math.sqrt(p);
    return (r - Math.floor(r)) * 4294967296 >>> 0;
  }));

  function rotr(x, n) { return (x >>> n) | (x << (32 - n)); }
  function ch(x, y, z)  { return (x & y) ^ (~x & z); }
  function maj(x, y, z) { return (x & y) ^ (x & z) ^ (y & z); }
  function S0(x) { return rotr(x,2) ^ rotr(x,13) ^ rotr(x,22); }
  function S1(x) { return rotr(x,6) ^ rotr(x,11) ^ rotr(x,25); }
  function g0(x) { return rotr(x,7) ^ rotr(x,18) ^ (x >>> 3); }
  function g1(x) { return rotr(x,17) ^ rotr(x,19) ^ (x >>> 10); }

  function hashBytes(data) {
    // data: Uint8Array
    const len = data.length;
    const bitLen = len * 8;
    // Pad: append 0x80, zeros, then 64-bit big-endian length
    // Easier: build padded buffer
    const pad = new Uint8Array(((len + 1 + 8 + 63) & ~63));
    pad.set(data);
    pad[len] = 0x80;
    // Write 64-bit big-endian bit length at the end
    // JS numbers: bitLen may exceed 32 bits for large files; handle hi/lo
    const hi = Math.floor(bitLen / 0x100000000);
    const lo = bitLen >>> 0;
    const dv = new DataView(pad.buffer);
    dv.setUint32(pad.length - 8, hi, false);
    dv.setUint32(pad.length - 4, lo, false);

    const H = H0.slice();
    const W = new Uint32Array(64);

    for (let off = 0; off < pad.length; off += 64) {
      const dv2 = new DataView(pad.buffer, off, 64);
      for (let i = 0; i < 16; i++) W[i] = dv2.getUint32(i * 4, false);
      for (let i = 16; i < 64; i++)
        W[i] = (g1(W[i-2]) + W[i-7] + g0(W[i-15]) + W[i-16]) >>> 0;

      let [a,b,c,d,e,f,g,h] = H;
      for (let i = 0; i < 64; i++) {
        const t1 = (h + S1(e) + ch(e,f,g) + K[i] + W[i]) >>> 0;
        const t2 = (S0(a) + maj(a,b,c)) >>> 0;
        h=g; g=f; f=e; e=(d+t1)>>>0;
        d=c; c=b; b=a; a=(t1+t2)>>>0;
      }
      H[0]=(H[0]+a)>>>0; H[1]=(H[1]+b)>>>0;
      H[2]=(H[2]+c)>>>0; H[3]=(H[3]+d)>>>0;
      H[4]=(H[4]+e)>>>0; H[5]=(H[5]+f)>>>0;
      H[6]=(H[6]+g)>>>0; H[7]=(H[7]+h)>>>0;
    }
    return H;
  }

  function toHex(h32) {
    return Array.from(h32).map(v => ('00000000' + v.toString(16)).slice(-8)).join('');
  }

  // Public: returns Promise<string> hex sha256 of ArrayBuffer or Uint8Array
  async function digest(input) {
    const data = input instanceof Uint8Array ? input : new Uint8Array(input);
    if (window.crypto && window.crypto.subtle) {
      try {
        const buf = await crypto.subtle.digest('SHA-256', data);
        return Array.from(new Uint8Array(buf), b => b.toString(16).padStart(2, '0')).join('');
      } catch(_) { /* fall through to pure-JS on insecure context */ }
    }
    return toHex(hashBytes(data));
  }

  // Self-test on known vectors (called once at startup)
  function selfTest() {
    const cases = [
      ['', 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'],
      ['abc', 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'],
      ['The quick brown fox jumps over the lazy dog',
       'd7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592'],
    ];
    for (const [msg, expected] of cases) {
      const enc = new TextEncoder().encode(msg);
      const got = toHex(hashBytes(enc));
      if (got !== expected) {
        console.error('SHA-256 self-test FAILED for:', msg);
        console.error('  expected:', expected);
        console.error('  got:     ', got);
      }
    }
  }

  selfTest();
  return { digest };
})();

// ─── Utility ─────────────────────────────────────────────────────────────────
function esc(s) {
  if (s === null || s === undefined) return '';
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}
function setText(id, v) {
  const el = document.getElementById(id);
  if (el) el.textContent = v;
}
function showErr(msg) {
  const b = document.getElementById('err-banner');
  if (!b) return;
  b.textContent = msg;
  b.style.display = msg ? 'block' : 'none';
}
function clearErr() { showErr(''); }
function fmtBytes(n) {
  if (n >= 1048576) return (n/1048576).toFixed(1)+' MB';
  if (n >= 1024)    return (n/1024).toFixed(1)+' KB';
  return n+' B';
}
function fmtUptime(s) {
  const h = Math.floor(s/3600), m = Math.floor((s%3600)/60), sec = s%60;
  return `${h}h ${m}m ${sec}s`;
}

// ─── API helpers ──────────────────────────────────────────────────────────────
// apiFetch with AbortController timeout (default 10 s, poll calls use shorter)
async function apiFetch(path, opts={}, timeoutMs=10000) {
  const ac = new AbortController();
  const tid = setTimeout(() => ac.abort(), timeoutMs);
  const headers = Object.assign({ 'Authorization': 'Bearer ' + _tok }, opts.headers||{});
  try {
    return await fetch(path, Object.assign({}, opts, { headers, signal: ac.signal }));
  } finally {
    clearTimeout(tid);
  }
}

async function apiJSON(path, timeoutMs=10000) {
  const r = await apiFetch(path, {}, timeoutMs);
  if (!r.ok) {
    let msg = r.status + ' ' + r.statusText;
    try { const j = await r.json(); msg = j.detail || j.error || msg; } catch(_) {}
    throw new Error(msg);
  }
  return r.json();
}

// ─── Login ────────────────────────────────────────────────────────────────────
function doLogin() {
  const val = document.getElementById('tok-in').value.trim();
  if (!/^[0-9a-fA-F]{64}$/.test(val)) {
    document.getElementById('login-err').textContent = '格式錯誤：需要 64 位十六進制字元';
    return;
  }
  _tok = val;
  document.getElementById('tok-in').value = '';  // clear field
  document.getElementById('login-err').textContent = '';
  verifyAndEnter();
}

async function verifyAndEnter() {
  document.getElementById('login-btn').disabled = true;
  document.getElementById('login-err').textContent = '驗證中…';
  try {
    const v = await apiJSON('/api/version');
    // Success — show dashboard
    document.getElementById('login-card').style.display = 'none';
    document.getElementById('dashboard').style.display = 'block';
    renderVersion(v);
    fetchHealth();
    fetchLogs();
    startDashTimer();
  } catch(e) {
    _tok = '';
    document.getElementById('login-err').textContent = '登入失敗：' + e.message;
  } finally {
    document.getElementById('login-btn').disabled = false;
  }
}

// ─── Tabs ─────────────────────────────────────────────────────────────────────
function switchTab(name) {
  document.querySelectorAll('.tab').forEach((t,i) => {
    const names = ['status','update','logs'];
    t.classList.toggle('active', names[i]===name);
  });
  document.querySelectorAll('.tab-pane').forEach(p => {
    p.classList.toggle('active', p.id==='tab-'+name);
  });
  if (name==='logs') fetchLogs();
}

// ─── KV rendering ─────────────────────────────────────────────────────────────
const KEY_LABELS_VER = {
  device_id:'裝置 ID', boot_id:'開機 ID', project_name:'專案名稱',
  version:'版本', git_commit:'Git Commit', elf_sha256:'ELF SHA-256',
  chip_id:'晶片 ID', active_partition:'使用分區', ota_max_size:'OTA 最大容量',
  build_time:'編譯時間', device_name:'裝置名稱'
};
const KEY_LABELS_HEALTH = {
  rssi:'Wi-Fi 訊號 (RSSI)', local_ip:'本機 IP', uptime_sec:'運行時間',
  free_heap:'可用 Heap', min_free_heap:'最小 Heap', free_psram:'可用 PSRAM',
  usb_mounted:'USB 掛載', wifi_mode:'Wi-Fi 模式', active_slot:'運行分區',
  boot_state:'開機狀態'
};

function renderKV(elId, obj, labelMap) {
  const el = document.getElementById(elId);
  if (!el) return;
  while (el.firstChild) el.removeChild(el.firstChild);
  for (const [k, v] of Object.entries(obj)) {
    const kEl = document.createElement('span');
    kEl.className = 'k';
    kEl.textContent = (labelMap && labelMap[k]) ? labelMap[k] : k;
    const vEl = document.createElement('span');
    vEl.className = 'v';
    // Safe value rendering — format known types
    if (k === 'ota_max_size' && typeof v === 'number') {
      vEl.textContent = fmtBytes(v);
    } else if (k === 'uptime_sec' && typeof v === 'number') {
      vEl.textContent = fmtUptime(v);
    } else if ((k === 'free_heap' || k === 'min_free_heap' || k === 'free_psram') && typeof v === 'number') {
      vEl.textContent = fmtBytes(v);
    } else if (k === 'rssi' && typeof v === 'number') {
      vEl.textContent = v + ' dBm';
    } else {
      vEl.textContent = (v === null || v === undefined) ? '—' : String(v);
    }
    el.appendChild(kEl);
    el.appendChild(vEl);
  }
}

// ─── Version ──────────────────────────────────────────────────────────────────
let _lastVersion = null;
function renderVersion(data) {
  _lastVersion = data;
  renderKV('ver-kv', data, KEY_LABELS_VER);
  const hdr = document.getElementById('hdr-device');
  if (hdr) hdr.textContent = (data.device_name || data.device_id || '') + '  |  ' + (data.version || '');
}

async function fetchVersion() {
  document.getElementById('ver-refresh-spin').style.display = '';
  try {
    const v = await apiJSON('/api/version');
    renderVersion(v);
  } catch(e) {
    showErr('版本查詢失敗：' + e.message);
  } finally {
    document.getElementById('ver-refresh-spin').style.display = 'none';
  }
}

// ─── Health ───────────────────────────────────────────────────────────────────
let _lastHealth = null;
function renderHealth(data) {
  _lastHealth = data;
  const pill = document.getElementById('health-pill');
  if (data.healthy) {
    pill.className = 'pill pill-green'; pill.textContent = '正常';
  } else {
    pill.className = 'pill pill-red'; pill.textContent = '異常';
  }
  const telem = data.telemetry || {};
  // Also show device_id / boot_id in health card if present
  const display = {};
  if (data.device_id) display.device_id = data.device_id;
  if (data.boot_id)   display.boot_id   = data.boot_id;
  for (const [k,v] of Object.entries(telem)) display[k] = v;
  renderKV('health-kv', display, KEY_LABELS_HEALTH);
}

async function fetchHealth() {
  document.getElementById('health-spin').style.display = '';
  try {
    const h = await apiJSON('/api/health');
    renderHealth(h);
  } catch(e) {
    showErr('健康狀態查詢失敗：' + e.message);
  } finally {
    document.getElementById('health-spin').style.display = 'none';
  }
}

// ─── Logs ─────────────────────────────────────────────────────────────────────
async function fetchLogs() {
  const box = document.getElementById('log-box');
  if (!box) return;
  try {
    const r = await apiFetch('/api/logs');
    let entries = [];
    try {
      // Tolerant: attempt JSON parse; fall back to raw text
      const text = await r.text();
      try { entries = JSON.parse(text); } catch(_) {
        // Raw text fallback: show as single entry
        const row = document.createElement('div');
        row.className = 'log-row';
        row.textContent = text;
        while (box.firstChild) box.removeChild(box.firstChild);
        box.appendChild(row);
        return;
      }
    } catch(_) {}
    while (box.firstChild) box.removeChild(box.firstChild);
    if (!Array.isArray(entries)) return;
    for (const e of entries) {
      const row = document.createElement('div');
      row.className = 'log-row';
      const ts = document.createElement('span');
      ts.className = 'log-ts';
      // e.ts is ms since boot
      if (typeof e.ts === 'number') {
        const s = (e.ts/1000).toFixed(1);
        ts.textContent = '[' + s + 's]';
      }
      const msg = document.createElement('span');
      // Safe: textContent never executes or injects HTML
      msg.textContent = (e.msg != null) ? String(e.msg) : '';
      row.appendChild(ts);
      row.appendChild(msg);
      box.appendChild(row);
    }
    box.scrollTop = box.scrollHeight;
  } catch(e) {
    const row = document.createElement('div');
    row.className = 'log-row';
    row.textContent = '載入失敗：' + e.message;
    while (box.firstChild) box.removeChild(box.firstChild);
    box.appendChild(row);
  }
}

// ─── Dashboard auto-refresh ───────────────────────────────────────────────────
function startDashTimer() {
  if (_dashTimer) clearInterval(_dashTimer);
  _dashTimer = setInterval(() => {
    if (!_busy) { fetchHealth(); fetchVersion(); }
  }, 8000);
}

// ─── Firmware file handling ───────────────────────────────────────────────────
function dzOver(e) { e.preventDefault(); document.getElementById('drop-zone').classList.add('over'); }
function dzLeave()  { document.getElementById('drop-zone').classList.remove('over'); }
function dzDrop(e)  { e.preventDefault(); dzLeave(); onFileChosen(e.dataTransfer.files[0]); }

async function onFileChosen(file) {
  if (!file) return;
  _fwFile = file;
  _fwSha = '';
  _fwElfDigest = '';
  _fwVersion = '';
  document.getElementById('btn-upload').disabled = true;
  document.getElementById('upload-err').textContent = '';

  const infoEl = document.getElementById('file-info');
  infoEl.textContent = file.name + ' — ' + fmtBytes(file.size) + ' — 計算 SHA-256…';

  // Render drop zone filename
  const dz = document.getElementById('drop-zone');
  dz.classList.remove('over');

  let buf;
  try {
    buf = await file.arrayBuffer();
  } catch(e) {
    infoEl.textContent = '讀取檔案失敗：' + e.message;
    return;
  }

  const bytes = new Uint8Array(buf);

  // Extract ELF digest from bytes 176..207 (32 bytes)
  if (bytes.length >= 208) {
    _fwElfDigest = Array.from(bytes.slice(176, 208))
      .map(b => b.toString(16).padStart(2,'0')).join('');
  }

  // Extract version string from bytes 48..79 (32 bytes, NUL-terminated)
  if (bytes.length >= 80) {
    const vb = bytes.slice(48, 80);
    const nul = vb.indexOf(0);
    _fwVersion = new TextDecoder('utf-8', {fatal:false}).decode(
      nul >= 0 ? vb.slice(0, nul) : vb
    );
  }

  // Compute full firmware SHA-256
  _fwSha = await _sha256.digest(bytes);

  // Render firmware metadata
  const meta = {};
  if (_fwVersion) meta['韌體版本'] = _fwVersion;
  if (_fwElfDigest) meta['ELF SHA-256（嵌入）'] = _fwElfDigest;
  meta['檔案 SHA-256'] = _fwSha;
  meta['大小'] = fmtBytes(file.size);
  renderKV('fw-meta-kv', meta, null);

  const validHeader = bytes.length >= 80 && bytes[0] === 0xE9 && _fwVersion.startsWith('ota1-');
  infoEl.textContent = validHeader
    ? file.name + ' — ' + fmtBytes(file.size) + ' — 就緒'
    : '韌體格式不符：需要有效 ESP32 映像與 ota1- 版本';
  document.getElementById('btn-upload').disabled = !validHeader || !_fwElfDigest;
}

// ─── Upload ───────────────────────────────────────────────────────────────────
async function startUpload() {
  if (!_fwFile || !_fwSha) return;
  if (_busy) { showErr('有其他操作正在進行，請稍候'); return; }

  clearErr();
  document.getElementById('upload-err').textContent = '';
  document.getElementById('upload-card').style.display = 'block';
  setUploadStatus('讀取裝置狀態…', 0);
  _busy = true;
  setBtnsDisabled(true);

  let preDevice = '', preBoot = '', preSlot = '', preElf = '';
  try {
    // Snapshot device state before upload
    const v = await apiJSON('/api/version');
    preDevice = v.device_id || '';
    preBoot   = v.boot_id   || '';
    preSlot   = v.active_partition || '';
    preElf    = v.elf_sha256 || '';
    if (!preDevice || !preBoot || !/^app[01]$/.test(preSlot))
      throw new Error('裝置分區資訊無效');
    if (!_fwVersion.startsWith('ota1-') || !_fwElfDigest || !_fwSha)
      throw new Error('韌體缺少有效 ota1 版本或 ELF SHA-256');
    if (!Number.isSafeInteger(v.ota_max_size) || _fwFile.size > v.ota_max_size)
      throw new Error('韌體超過裝置分區容量');

    setUploadStatus('上傳中…', 0);

    await new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/update', true);
      xhr.timeout = 120000;
      xhr.setRequestHeader('Authorization', 'Bearer ' + _tok);
      xhr.setRequestHeader('Content-Type', 'application/octet-stream');
      xhr.setRequestHeader('X-Firmware-SHA256', _fwSha);
      // Content-Length set automatically by browser for send(Blob)

      xhr.upload.addEventListener('progress', e => {
        if (e.lengthComputable) {
          const pct = Math.round(e.loaded / e.total * 100);
          setUploadStatus('上傳中… ' + pct + '%', pct);
          document.getElementById('upload-detail').textContent =
            fmtBytes(e.loaded) + ' / ' + fmtBytes(e.total);
        }
      });

      xhr.addEventListener('load', () => {
        if (xhr.status === 202) {
          resolve(JSON.parse(xhr.responseText));
        } else {
          let msg = xhr.status + ' ' + xhr.statusText;
          try { const j = JSON.parse(xhr.responseText); msg = j.detail||j.error||msg; } catch(_) {}
          reject(new Error(msg));
        }
      });
      xhr.addEventListener('error', () => reject(new Error('網路錯誤')));
      xhr.addEventListener('abort', () => reject(new Error('上傳已中止')));
      xhr.addEventListener('timeout', () => reject(new Error('上傳逾時')));

      xhr.send(_fwFile);
    });

    setUploadStatus('上傳完成，等待裝置重新啟動…', 100);
    // Poll for new boot: same device, different boot_id, different slot, matching ELF digest
    await pollForNewBoot({
      deviceId: preDevice,
      oldBootId: preBoot,
      oldSlot: preSlot,
      expectElf: _fwElfDigest,   // from firmware bytes 176-207
      expectVersion: _fwVersion,
      maxMs: 60000,
      statusFn: s => setUploadStatus(s, 100)
    });

    setUploadStatus('✓ 更新成功，新韌體已運行', 100);
    _fwFile = null; _fwSha = '';
    document.getElementById('btn-upload').disabled = true;
    document.getElementById('file-info').textContent = '';
    while(document.getElementById('fw-meta-kv').firstChild)
      document.getElementById('fw-meta-kv').removeChild(document.getElementById('fw-meta-kv').firstChild);

    fetchVersion(); fetchHealth();
  } catch(e) {
    setUploadStatus('上傳失敗：' + e.message, 0);
    document.getElementById('upload-err').textContent = e.message;
    showErr('上傳失敗：' + e.message);
  } finally {
    _busy = false;
    setBtnsDisabled(false);
  }
}

function setUploadStatus(msg, pct) {
  setText('upload-status', msg);
  document.getElementById('prog-bar').style.width = pct + '%';
}

// ─── Poll for new boot ────────────────────────────────────────────────────────
async function pollForNewBoot({deviceId, oldBootId, oldSlot, expectElf, expectVersion, maxMs, statusFn}) {
  if (oldSlot !== null && oldSlot !== undefined && !/^app[01]$/.test(oldSlot))
    throw new Error('原分區資訊無效');
  const start = Date.now();
  let attempt = 0;
  while (Date.now() - start < maxMs) {
    await sleep(2500);
    attempt++;
    if (statusFn) statusFn('等待新開機… (' + attempt + ')');
    try {
      const h = await apiJSON('/api/health');
      const v = await apiJSON('/api/version');
      if (h.healthy !== true) continue;
      if (!deviceId || !oldBootId || !v.device_id || !v.boot_id ||
          h.device_id !== deviceId || v.device_id !== deviceId ||
          h.boot_id !== v.boot_id) continue;
      if (v.boot_id === oldBootId)   continue;  // not rebooted yet
      if (oldSlot && v.active_partition === oldSlot) continue;
      if (!/^app[01]$/.test(v.active_partition || '')) continue;
      if (expectElf && v.elf_sha256 !== expectElf) continue;
      if (expectVersion && v.version !== expectVersion) continue;
      return; // success
    } catch(_) { /* device rebooting, not yet reachable */ }
  }
  throw new Error('等待超時：裝置未在 ' + (maxMs/1000) + ' 秒內完成更新');
}

// ─── Reboot ───────────────────────────────────────────────────────────────────
async function cmdReboot() {
  showConfirm('確認重新啟動', '裝置將立即重新啟動，連線會短暫中斷。', async () => {
    if (_busy) { showErr('有其他操作正在進行'); return; }
    _busy = true; setBtnsDisabled(true); clearErr();
    try {
      const v0 = await apiJSON('/api/version');
      const preDevice = v0.device_id||'';
      const preBoot   = v0.boot_id||'';
      const preSlot   = v0.active_partition||'';

      const r = await apiFetch('/api/reboot', {method:'POST'});
      if (r.status !== 202 && !r.ok) {
        const j = await r.json().catch(()=>({}));
        throw new Error(j.detail||j.error||r.status);
      }

      showErr('裝置重新啟動中，等待恢復…');
      // Poll: same device, different boot_id, same slot (reboot not OTA)
      await pollForNewBoot({
        deviceId: preDevice, oldBootId: preBoot, oldSlot: null,
        expectElf: '', expectVersion: '', maxMs: 45000,
        statusFn: s => showErr(s)
      }).catch(e => showErr('重啟等待超時：' + e.message));
      clearErr();
      fetchVersion(); fetchHealth();
    } catch(e) {
      showErr('重啟失敗：' + e.message);
    } finally {
      _busy = false; setBtnsDisabled(false);
    }
  });
}

// ─── Rollback ─────────────────────────────────────────────────────────────────
async function cmdRollback() {
  showConfirm('確認回滾韌體', '此操作將切換回前一個有效分區並重新啟動。請確認目標分區狀態有效後再繼續。', async () => {
    if (_busy) { showErr('有其他操作正在進行'); return; }
    _busy = true; setBtnsDisabled(true); clearErr();
    try {
      const v0 = await apiJSON('/api/version');
      const preDevice = v0.device_id||'';
      const preBoot   = v0.boot_id||'';
      const preSlot   = v0.active_partition||'';

      const r = await apiFetch('/api/rollback', {method:'POST'});
      if (r.status !== 202 && !r.ok) {
        const j = await r.json().catch(()=>({}));
        throw new Error(j.detail||j.error||r.status);
      }

      showErr('回滾中，等待裝置重啟…');
      // Poll: same device, different boot_id, different slot (rollback must switch)
      await pollForNewBoot({
        deviceId: preDevice, oldBootId: preBoot, oldSlot: preSlot,
        expectElf:'', expectVersion:'', maxMs: 45000,
        statusFn: s => showErr(s)
      });
      clearErr();
      fetchVersion(); fetchHealth();
    } catch(e) {
      showErr('回滾失敗：' + e.message);
    } finally {
      _busy = false; setBtnsDisabled(false);
    }
  });
}

// ─── Shared helpers ───────────────────────────────────────────────────────────
function setBtnsDisabled(v) {
  ['btn-reboot','btn-rollback','btn-upload','login-btn'].forEach(id => {
    const el = document.getElementById(id);
    if (el) el.disabled = v;
  });
}

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

function showConfirm(title, body, okCb) {
  document.getElementById('modal-title').textContent = title;
  document.getElementById('modal-body').textContent  = body;
  _modalOkCb = okCb;
  document.getElementById('modal-overlay').classList.add('show');
  document.getElementById('modal-ok').onclick = () => { closeModal(); if(_modalOkCb) _modalOkCb(); };
}
function closeModal() {
  document.getElementById('modal-overlay').classList.remove('show');
  _modalOkCb = null;
}

// Allow Enter key on token input
document.getElementById('tok-in').addEventListener('keydown', e => {
  if (e.key === 'Enter') doLogin();
});
</script>
</body>
</html>
)PAXGHTML";

} // namespace ota
} // namespace paxg
