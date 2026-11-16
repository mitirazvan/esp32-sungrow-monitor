/*
  Sungrow SH6.0RS monitor
  ESP32-C3 Super Mini + 16x2 I2C LCD + WiFi setup portal + web page

  Libraries (Library Manager):
    - "WiFiManager" by tzapu (2.0.17+)
    - "LiquidCrystal I2C" by Frank de Brabander
  Everything else (WebServer, ESPmDNS, Preferences) is part of the ESP32 core.

  Board: "ESP32C3 Dev Module", Tools -> USB CDC On Boot: Enabled

  What it does
    - Reads PV power, house load, grid power and battery level/direction from the
      inverter over Modbus TCP (port 502) every 5 s, in a background task.
    - Shows them on the LCD; the top-right icon shows the link state
      (check = data loaded, circular arrow = syncing, X = offline).
    - Serves a web page (http://<esp-ip>/ or http://pv.local), a JSON API (/api)
      and a settings page (/settings) to set or scan for the inverter IP.
    - First boot / no WiFi: opens the AP "PV-Setup" (captive portal) to enter WiFi
      and the inverter IP. If WiFi stays down for 60 s while running, it opens
      the portal again. Hold a button on GPIO3 at power-up to erase saved WiFi.
*/

#include <Wire.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <LiquidCrystal_I2C.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_timer.h>

// ============================================================================
// Configuration
// ============================================================================
#define FW_VERSION     "1.0.0"                // bump on every change you flash
#define FW_BUILD       __DATE__ " " __TIME__  // filled in by the compiler

#define SDA_PIN        8                     // ESP32-C3 Super Mini default I2C pins
#define SCL_PIN        9
#define RESET_PIN      3                      // button to GND: hold at power-up to erase WiFi

#define AP_NAME        "PV-Setup"
#define AP_PASS        "12345678"             // min 8 chars, or "" for an open AP
#define WIFI_LOST_MS   60000                  // WiFi down this long => reopen the setup AP

#define DEFAULT_INVERTER_IP "192.168.1.134"   // used until you set/discover another one
#define INVERTER_PORT  502
#define MODBUS_UNIT    1                      // slave id (1 for direct port 502 on SH hybrids)
#define POLL_MS        5000                   // inverter poll interval
#define OFFLINE_AFTER  3                      // failed polls before the link shows offline
#define RESCAN_AFTER   12                     // failed polls before auto-scanning the network
#define RESCAN_EVERY   600000UL               // ...and at most this often (ms)

// Input registers (function 0x04), zero-based = documented register - 1.
// If values look shifted/wrong, adjust REG_OFFSET by +1 / -1.
#define REG_OFFSET     0
#define REG_PV_POWER   (5016  + REG_OFFSET)   // U32 W    total DC (PV) power
#define REG_RUNSTATE   (13000 + REG_OFFSET)   // U16      bit1 = battery charging, bit2 = discharging
#define REG_LOAD       (13007 + REG_OFFSET)   // S32 W    house load; the next 2 regs are grid power
                                              //          (S32 W, + = export, - = import)
#define REG_BAT_POWER  (13021 + REG_OFFSET)   // U16 W    battery power; the next reg is SOC (0.1 %)

// ============================================================================
// Globals
// ============================================================================
WebServer   server(80);
WiFiManager wm;
Preferences prefs;
LiquidCrystal_I2C *lcd = nullptr;             // created in setup()
WiFiClient  modbusClient;                     // one connection, reused for all reads of a poll

String inverterIp;                            // from flash; "" = auto-discover
WiFiManagerParameter *portalInverterIp = nullptr;   // "Inverter IP" field in the setup portal

// Latest values from the inverter (written by the poll task, read by loop/web)
struct Readings {
  int32_t pv = 0, load = 0, grid = 0, batPower = 0;   // grid: + export, - import
  float   soc = 0;
  int8_t  batDir = 0;                                 // +1 charging, -1 discharging, 0 idle
} live;
volatile bool online = false;                 // true after a good poll, false after OFFLINE_AFTER failures
uint32_t lastOkMs = 0;                        // millis() of the last good poll
volatile uint8_t failedPolls = 0;

enum LinkState { LINK_OFF, LINK_OK, LINK_SYNC };
volatile LinkState linkState = LINK_OFF;

volatile bool needDraw = false;               // full redraw requested (set by the poll task)
volatile bool needIcon = false;               // only the link icon changed
volatile bool pausePoll = false, pollBusy = false;
bool     scanRequested = false;
uint32_t lastScan = 0;

// Watchdog heartbeats (see watchdogCheck): if loop or the poll task stops running,
// or no inverter data arrives for a long time, the ESP restarts itself.
volatile uint32_t loopBeat = 0, pollBeat = 0, lastProgressMs = 0;
volatile bool wdtPause = false;
String lastRestartWhy;                        // why the previous run restarted itself (shown on the web page)

// Serial log with an uptime stamp: [   123.456] message
void logf(const char *fmt, ...) {
  char b[220];
  va_list args;
  va_start(args, fmt);
  vsnprintf(b, sizeof(b), fmt, args);
  va_end(args);
  uint32_t t = millis();
  Serial.printf("[%6lu.%03lu] %s\n", (unsigned long)(t / 1000), (unsigned long)(t % 1000), b);
}
const char *failedAt = "";                    // which register read failed last (for the log)

// ============================================================================
// LCD
// ============================================================================
uint8_t lcdAddr = 0x27;                       // auto-detected in setup()

// Custom characters (5x8). Slot CH_LINK is shared by the three link icons.
const uint8_t ICON_SUN[8]   = {0b00100,0b10101,0b01110,0b11011,0b01110,0b10101,0b00100,0};
const uint8_t ICON_BAT[8]   = {0b01110,0b11011,0b10001,0b11111,0b11111,0b11111,0b11111,0b11111};
const uint8_t ICON_HOUSE[8] = {0b00100,0b01110,0b11111,0b10001,0b10101,0b10101,0b11111,0};
const uint8_t ICON_GRID[8]  = {0b00100,0b01110,0b00100,0b11111,0b00100,0b01010,0b01010,0b10001};
const uint8_t ICON_UP[8]    = {0b00100,0b01110,0b11111,0b00100,0b00100,0b00100,0b00100,0};
const uint8_t ICON_DOWN[8]  = {0b00100,0b00100,0b00100,0b00100,0b11111,0b01110,0b00100,0};
const uint8_t ICON_OK[8]    = {0,0b00001,0b00010,0b10100,0b01000,0,0,0};                // check = data loaded
const uint8_t ICON_OFF[8]   = {0b10001,0b01010,0b00100,0b01010,0b10001,0,0,0};          // X = offline
const uint8_t ICON_SYNC[8]  = {0,0b01101,0b10011,0b10111,0b10000,0b10001,0b01110,0};    // circular arrow
enum { CH_SUN, CH_BAT, CH_HOUSE, CH_GRID, CH_UP, CH_DOWN, CH_LINK };

// Find the first I2C device (LCD backpacks are 0x27 or 0x3F)
uint8_t findLcdAddress() {
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) return a;
  }
  return 0;
}

// Print text on a row, padded/cut to 16 characters
void lcdPrint(uint8_t row, const String &text) {
  String s = text;
  while (s.length() < 16) s += ' ';
  lcd->setCursor(0, row);
  lcd->print(s.substring(0, 16));
}

void loadLinkGlyph() {
  const uint8_t *g = linkState == LINK_OK ? ICON_OK : linkState == LINK_SYNC ? ICON_SYNC : ICON_OFF;
  lcd->createChar(CH_LINK, (uint8_t *)g);
}

// Re-sent on every redraw: recovers from LCD glitches caused by a loose wire
void loadIcons() {
  lcd->createChar(CH_SUN,   (uint8_t *)ICON_SUN);
  lcd->createChar(CH_BAT,   (uint8_t *)ICON_BAT);
  lcd->createChar(CH_HOUSE, (uint8_t *)ICON_HOUSE);
  lcd->createChar(CH_GRID,  (uint8_t *)ICON_GRID);
  lcd->createChar(CH_UP,    (uint8_t *)ICON_UP);
  lcd->createChar(CH_DOWN,  (uint8_t *)ICON_DOWN);
  loadLinkGlyph();
}

// Redraw only the link icon (top-right corner)
void showLinkIcon() {
  loadLinkGlyph();
  lcd->setCursor(15, 0);
  lcd->write((uint8_t)CH_LINK);
}

// Compact power text, max 6 chars: "357W", "5.23kW", "10.5kW"
String fmtCompact(int32_t w) {
  char b[10];
  int32_t a = abs(w);
  if (a < 1000)       snprintf(b, sizeof(b), "%ldW", (long)a);
  else if (a < 10000) snprintf(b, sizeof(b), "%.2fkW", a / 1000.0f);
  else                snprintf(b, sizeof(b), "%.1fkW", a / 1000.0f);
  return String(b);
}

// Screen buffer: whole rows are rewritten (no clear() => no flicker)
uint8_t scr[2][16];
void putStr(uint8_t col, uint8_t row, const String &s) {
  for (uint8_t i = 0; i < s.length() && col + i < 16; i++) scr[row][col + i] = s[i];
}
void putIcon(uint8_t col, uint8_t row, uint8_t ch) { scr[row][col] = ch; }

/*  Layout:  col: 0123456789012345
      row 0:      *5.23kW  [85%  ^L     * sun (PV)      [ battery  ^ charging, v discharging
      row 1:      H1.20kW  T0.44kW v    H house (load)  T grid     v import,   ^ export
      L = link state: check = data loaded, circular arrow = syncing, X = offline      */
void drawScreen() {
  memset(scr, ' ', sizeof(scr));
  putIcon(0, 0, CH_SUN);   putStr(1, 0, fmtCompact(live.pv));
  putIcon(8, 0, CH_BAT);   putStr(9, 0, String((int)(live.soc + 0.5f)) + "%");
  if (live.batDir) putIcon(14, 0, live.batDir > 0 ? CH_UP : CH_DOWN);
  putIcon(15, 0, CH_LINK);
  putIcon(0, 1, CH_HOUSE); putStr(1, 1, fmtCompact(live.load));
  putIcon(8, 1, CH_GRID);  putStr(9, 1, fmtCompact(live.grid));
  if (abs(live.grid) >= 20) putIcon(15, 1, live.grid < 0 ? CH_DOWN : CH_UP);

  loadIcons();
  for (uint8_t row = 0; row < 2; row++) {
    lcd->setCursor(0, row);
    for (uint8_t i = 0; i < 16; i++) lcd->write(scr[row][i]);
  }
}

// ============================================================================
// Modbus TCP (hand-written, no library)
// ============================================================================
// Read `count` input registers over modbusClient; returns true on success.
bool modbusRead(uint16_t addr, uint16_t count, uint16_t *out) {
  static uint16_t tid = 0;
  tid++;
  const uint8_t req[12] = {
    (uint8_t)(tid >> 8), (uint8_t)tid, 0, 0, 0, 6, MODBUS_UNIT, 0x04,
    (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(count >> 8), (uint8_t)count};
  modbusClient.write(req, sizeof(req));

  uint8_t hdr[9];                                   // MBAP header (7) + function + byte count
  bool good = modbusClient.readBytes(hdr, 9) == 9 && hdr[7] == 0x04 && hdr[8] == count * 2;
  for (uint16_t i = 0; good && i < count; i++) {
    uint8_t b[2];
    good = modbusClient.readBytes(b, 2) == 2;
    out[i] = (b[0] << 8) | b[1];
  }
  delay(150);                                       // be gentle with the dongle
  return good;
}

// Read every value into `r`; nothing is published unless all reads succeed.
bool readInverter(Readings &r) {
  uint16_t w[4];
  failedAt = "pv power";
  if (!modbusRead(REG_PV_POWER, 2, w)) return false;
  r.pv = ((uint32_t)w[1] << 16) | w[0];             // Sungrow: low word first
  failedAt = "load+grid";
  if (!modbusRead(REG_LOAD, 4, w)) return false;    // load (2 regs) + grid (2 regs)
  r.load = (int32_t)(((uint32_t)w[1] << 16) | w[0]);
  r.grid = (int32_t)(((uint32_t)w[3] << 16) | w[2]);
  failedAt = "run state";
  if (!modbusRead(REG_RUNSTATE, 1, w)) return false;
  uint16_t run = w[0];
  failedAt = "battery";
  if (!modbusRead(REG_BAT_POWER, 2, w)) return false;   // power + SOC are adjacent
  r.batPower = w[0];
  r.soc = w[1] / 10.0f;
  r.batDir = (run & 0x02) ? 1 : (run & 0x04) ? -1 : 0;
  return true;
}

// One connection per poll: the WiNet-S drops quickly repeated connections.
bool pollInverter() {
  modbusClient.stop();
  if (inverterIp.length() == 0) return false;
  modbusClient.setTimeout(3000);                    // milliseconds on ESP32
  if (!modbusClient.connect(inverterIp.c_str(), INVERTER_PORT, 3000)) {
    logf("poll: CONNECT to %s:%d failed (wifi=%d rssi=%d)", inverterIp.c_str(), INVERTER_PORT,
         (int)WiFi.status(), WiFi.RSSI());
    return false;
  }
  Readings r;
  bool ok = readInverter(r);
  modbusClient.stop();
  if (ok) live = r;
  else logf("poll: READ of %s failed", failedAt);
  return ok;
}

// Try twice before counting a failure (the dongle sometimes refuses a connection)
bool pollInverterRetry() {
  if (pollInverter()) return true;
  delay(1000);
  return pollInverter();
}

// Background task: polls every POLL_MS so the web server never blocks on it.
void pollTask(void *) {
  for (;;) {
    pollBeat = millis();
    if (!pausePoll && WiFi.status() == WL_CONNECTED && inverterIp.length() > 0) {
      pollBusy = true;
      linkState = LINK_SYNC;
      needIcon = true;
      uint32_t t0 = millis();
      if (pollInverterRetry()) {
        if (!online) logf("inverter ONLINE");
        failedPolls = 0;
        online = true;
        lastOkMs = lastProgressMs = millis();
        logf("poll ok in %lu ms: pv=%ldW load=%ldW grid=%ldW bat=%.1f%% dir=%d heap=%lu",
             (unsigned long)(millis() - t0), (long)live.pv, (long)live.load, (long)live.grid,
             live.soc, live.batDir, (unsigned long)ESP.getFreeHeap());
      } else {
        if (failedPolls < 250) failedPolls = failedPolls + 1;
        logf("poll FAILED after %lu ms (%u in a row) heap=%lu", (unsigned long)(millis() - t0),
             (unsigned)failedPolls, (unsigned long)ESP.getFreeHeap());
        if (failedPolls >= OFFLINE_AFTER && online) {
          online = false;                                   // last values stay on screen
          logf("inverter OFFLINE");
        }
      }
      linkState = online ? LINK_OK : LINK_OFF;
      needDraw = true;
      pollBusy = false;
    }
    vTaskDelay(POLL_MS / portTICK_PERIOD_MS);
  }
}

// ============================================================================
// Inverter IP: storage and discovery
// ============================================================================
void saveInverterIp(const String &ip) {
  inverterIp = ip;
  prefs.putString("inv", ip);
}

// Accepts a blank value (= auto-discover) or a valid IPv4 address
bool validInverterIp(String v) {
  v.trim();
  IPAddress t;
  return v.length() == 0 || t.fromString(v);
}

// Scan the local /24 for a host that answers a Sungrow Modbus request on port 502.
bool discoverInverter() {
  IPAddress me = WiFi.localIP();
  for (int i = 1; i < 255; i++) {
    if (i == me[3]) continue;
    char msg[20];
    snprintf(msg, sizeof(msg), "Scanning %3d/254", i);
    lcdPrint(1, msg);
    server.handleClient();
    loopBeat = millis();                            // a scan is slow but not a hang

    IPAddress ip(me[0], me[1], me[2], i);
    modbusClient.stop();
    modbusClient.setTimeout(1500);
    if (!modbusClient.connect(ip, INVERTER_PORT, 300)) continue;
    uint16_t r[2];
    bool ok = modbusRead(REG_PV_POWER, 2, r);
    modbusClient.stop();
    if (ok) {
      saveInverterIp(ip.toString());
      return true;
    }
  }
  return false;
}

void runDiscovery() {
  pausePoll = true;
  while (pollBusy) delay(50);                       // the poll task shares modbusClient
  lastScan = millis();
  logf("scan: looking for the inverter on %s/24", WiFi.localIP().toString().c_str());
  lcd->clear();
  lcdPrint(0, "Finding inverter");
  bool found = discoverInverter();
  logf("scan: %s %s", found ? "FOUND" : "not found", found ? inverterIp.c_str() : "");
  lcdPrint(0, found ? "Inverter found" : "Not found");
  lcdPrint(1, found ? inverterIp : String("set IP at /settings"));
  delay(2500);
  failedPolls = 0;
  pausePoll = false;
  needDraw = true;
}

// ============================================================================
// Web server
// ============================================================================
const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>PV Monitor</title>
<style>
:root{--bg:#0f1720;--card:#18232f;--tx:#e8eef4;--mu:#8aa0b3;--sun:#f5b301;--ok:#2ecc71;--bad:#ff5d5d;--bat:#4cc9f0}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--tx);font:16px system-ui,sans-serif;padding:16px;max-width:560px;margin:auto}
h1{font-size:20px;margin:4px 0 16px;display:flex;align-items:center;gap:10px}
#dot{width:12px;height:12px;border-radius:50%;background:var(--mu)}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.card{background:var(--card);border-radius:14px;padding:16px}
.card .l{color:var(--mu);font-size:13px;text-transform:uppercase;letter-spacing:.06em}
.card .v{font-size:30px;font-weight:700;margin:6px 0 2px}.card .s{font-size:13px;color:var(--mu);min-height:1.2em}
.bar{height:10px;background:#0b1118;border-radius:6px;overflow:hidden;margin-top:8px}
.bar i{display:block;height:100%;background:var(--bat);width:0;transition:width .5s}
#foot{color:var(--mu);font-size:12px;text-align:center;margin-top:16px}
</style></head><body>
<h1><span id="dot"></span>PV Monitor <small id="st" style="color:var(--mu);font-weight:400"></small></h1>
<div class="grid">
<div class="card"><div class="l">&#9728; Solar</div><div class="v" id="pv" style="color:var(--sun)">-</div></div>
<div class="card"><div class="l">&#127968; House load</div><div class="v" id="load">-</div></div>
<div class="card"><div class="l">&#128267; Battery</div><div class="v" id="soc" style="color:var(--bat)">-</div><div class="s" id="bs"></div><div class="bar"><i id="bar"></i></div></div>
<div class="card"><div class="l">&#9889; Grid</div><div class="v" id="grid">-</div><div class="s" id="gs"></div></div>
</div><div id="foot"></div>
<div style="text-align:center;margin-top:8px"><a href="/settings" style="color:var(--mu);font-size:12px">Settings</a>
<div id="ver" style="color:var(--mu);font-size:12px;margin-top:6px"></div></div>
<script>
const f=w=>{w=Math.abs(w);return w<1000?w+" W":(w/1000).toFixed(w<10000?2:1)+" kW"};
const $=i=>document.getElementById(i);
async function u(){try{const d=await (await fetch('/api')).json();
$('dot').style.background=d.ok?'var(--ok)':'var(--bad)';$('st').textContent=d.ok?'':'inverter offline (last values)';
$('pv').textContent=f(d.pv);$('load').textContent=f(d.load);
$('soc').textContent=Math.round(d.soc)+' %';$('bar').style.width=d.soc+'%';
$('bs').textContent=d.bat_dir>0?'Charging '+f(d.bat_power):d.bat_dir<0?'Discharging '+f(d.bat_power):'Idle';
const g=Math.abs(d.grid)<20?0:d.grid;$('grid').textContent=f(d.grid);
$('grid').style.color=g<0?'var(--bad)':g>0?'var(--ok)':'';
$('gs').textContent=g<0?'Importing from grid':g>0?'Exporting to grid':'Balanced';
$('ver').textContent='firmware v'+d.ver+' · built '+d.build;
$('foot').textContent=(d.ok?'updated '+d.age+' s ago':'no data')+' · inverter '+d.inv+' · WiFi '+d.rssi+' dBm · uptime '+Math.floor(d.up/60)+' min · heap '+Math.round(d.heap/1024)+'k (min '+Math.round(d.minheap/1024)+'k) · reset '+d.rst+(d.why?' ('+d.why+')':'');
}catch(e){$('dot').style.background='var(--bad)';$('st').textContent='no connection'}}
u();setInterval(u,5000);
</script></body></html>)HTML";

void handleApi() {
  char b[560];
  snprintf(b, sizeof(b),
           "{\"ok\":%s,\"pv\":%ld,\"load\":%ld,\"grid\":%ld,\"bat_power\":%ld,\"soc\":%.1f,"
           "\"bat_dir\":%d,\"age\":%lu,\"rssi\":%d,\"up\":%lu,\"inv\":\"%s\","
           "\"heap\":%lu,\"minheap\":%lu,\"rst\":%d,\"why\":\"%s\",\"ver\":\"%s\",\"build\":\"%s\"}",
           online ? "true" : "false", (long)live.pv, (long)live.load, (long)live.grid,
           (long)live.batPower, live.soc, live.batDir,
           lastOkMs ? (unsigned long)((millis() - lastOkMs) / 1000) : 0UL, WiFi.RSSI(),
           (unsigned long)(millis() / 1000), inverterIp.c_str(),
           (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
           (int)esp_reset_reason(), lastRestartWhy.c_str(), FW_VERSION, FW_BUILD);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", b);
}

void handleSettings() {
  String h = F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<title>Settings</title><style>body{background:#0f1720;color:#e8eef4;font:16px system-ui,sans-serif;max-width:480px;margin:auto;padding:16px}"
               "input,button,a.b{display:block;width:100%;padding:12px;margin:8px 0;border-radius:10px;border:0;font-size:16px;text-align:center;box-sizing:border-box}"
               "input{background:#18232f;color:#e8eef4}button,a.b{background:#4cc9f0;color:#06222d;text-decoration:none}</style></head><body>"
               "<h2>Settings</h2><form method='POST' action='/save'><label>Inverter IP (blank = auto-discover)</label>"
               "<input name='ip' placeholder='192.168.1.x' value='");
  h += inverterIp;
  h += F("'><button>Save</button></form><a class='b' href='/scan'>Scan network for inverter</a>"
         "<a class='b' href='/' style='background:#18232f;color:#e8eef4'>Back</a></body></html>");
  server.send(200, "text/html", h);
}

void handleSave() {
  String v = server.arg("ip");
  v.trim();
  if (validInverterIp(v)) {
    saveInverterIp(v);
    failedPolls = 0;
  }
  server.sendHeader("Location", "/settings");
  server.send(303);
}

void handleScan() {
  scanRequested = true;
  server.send(200, "text/html",
              F("<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<meta http-equiv='refresh' content='75;url=/settings'></head>"
                "<body style='background:#0f1720;color:#e8eef4;font:16px system-ui;padding:16px'>"
                "Scanning the network (up to ~75 s). Progress is on the LCD. This page returns to Settings automatically.</body></html>"));
}

void startWebServer() {
  server.on("/", []() {
    logf("web: page requested by %s", server.client().remoteIP().toString().c_str());
    server.send_P(200, "text/html", PAGE);
  });
  server.on("/api", handleApi);
  server.on("/settings", handleSettings);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/scan", handleScan);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });   // e.g. /favicon.ico
  server.begin();
  if (MDNS.begin("pv")) MDNS.addService("http", "tcp", 80);
  Serial.println("Web server: http://" + WiFi.localIP().toString() + "/  (or http://pv.local)");
}

// ============================================================================
// WiFi setup portal
// ============================================================================
// Called by WiFiManager when the portal (AP) starts
void onPortalStart(WiFiManager *) {
  lcd->clear();
  lcdPrint(0, String("AP: ") + AP_NAME);
  lcdPrint(1, WiFi.softAPIP().toString());          // 192.168.4.1
}

// Store the inverter IP typed in the portal
void applyPortalParams() {
  if (!portalInverterIp) return;
  String v = portalInverterIp->getValue();
  v.trim();
  if (validInverterIp(v) && v != inverterIp) saveInverterIp(v);
}

// WiFi has been gone too long: reopen the setup AP. Blocks until configured or
// the portal times out, then restarts.
void openSetupPortal() {
  logf("WiFi down for %d s -> opening setup AP", WIFI_LOST_MS / 1000);
  wdtPause = true;                                  // the portal blocks for up to 3 min on purpose
  server.stop();                                    // the portal needs port 80
  wm.setConfigPortalTimeout(180);
  wm.startConfigPortal(AP_NAME, AP_PASS);
  applyPortalParams();
  ESP.restart();
}

// ============================================================================
// Self-recovery watchdog (runs in its own esp_timer task, every 10 s)
// ============================================================================
void restartBecause(const char *why) {
  logf("WATCHDOG: restarting (%s) heap=%lu", why, (unsigned long)ESP.getFreeHeap());
  prefs.putString("why", why);                    // shown on the web page after the restart
  delay(100);
  ESP.restart();
}

void watchdogCheck(void *) {
  if (wdtPause) return;
  uint32_t now = millis();
  if (now - loopBeat > 60000)       restartBecause("loop stalled");
  else if (now - pollBeat > 120000) restartBecause("poll task stalled");
  else if (WiFi.status() == WL_CONNECTED && inverterIp.length() > 0 &&
           now - lastProgressMs > 1200000UL)        // 20 min without a single good reading
    restartBecause("no inverter data");
}

void startWatchdog() {
  loopBeat = pollBeat = lastProgressMs = millis();
  esp_timer_create_args_t args = {};
  args.callback = watchdogCheck;
  args.name = "wdt";
  esp_timer_handle_t timer;
  if (esp_timer_create(&args, &timer) == ESP_OK) esp_timer_start_periodic(timer, 10000000);   // 10 s
}

// ============================================================================
// setup / loop
// ============================================================================
// WiFi events -> Serial log (shows WHY the connection dropped)
void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      logf("WiFi: associated with the access point"); break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      logf("WiFi: got IP %s", WiFi.localIP().toString().c_str()); break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      logf("WiFi: LOST IP"); break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      logf("WiFi: DISCONNECTED, reason %d", (int)info.wifi_sta_disconnected.reason); break;
    default: break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  WiFi.onEvent(onWiFiEvent);
  logf("=== boot: firmware v%s (built %s), reset reason %d, heap %lu ===", FW_VERSION, FW_BUILD,
       (int)esp_reset_reason(), (unsigned long)ESP.getFreeHeap());

  Wire.begin(SDA_PIN, SCL_PIN);
  uint8_t found = findLcdAddress();
  if (found) lcdAddr = found;
  lcd = new LiquidCrystal_I2C(lcdAddr, 16, 2);
  lcd->init();
  lcd->backlight();
  lcdPrint(0, "SH6.0RS PV");
  lcdPrint(1, "v" FW_VERSION);

  pinMode(RESET_PIN, INPUT_PULLUP);

  // Inverter IP: stored in flash, editable in the setup portal and on /settings
  prefs.begin("pv", false);
  lastRestartWhy = prefs.getString("why", "");      // set if the watchdog restarted us last time
  prefs.remove("why");
  if (lastRestartWhy.length()) logf("previous run was restarted by the watchdog: %s", lastRestartWhy.c_str());
  inverterIp = prefs.isKey("inv") ? prefs.getString("inv") : String(DEFAULT_INVERTER_IP);
  portalInverterIp = new WiFiManagerParameter("inv", "Inverter IP (blank = auto-discover)",
                                              inverterIp.c_str(), 15);
  wm.addParameter(portalInverterIp);
  wm.setAPCallback(onPortalStart);
  wm.setConfigPortalTimeout(180);                   // portal closes after 3 min, device restarts

  if (digitalRead(RESET_PIN) == LOW) {
    lcdPrint(1, "Reset WiFi...");
    wm.resetSettings();
    delay(1000);
  }

  // Blocks: connects to the saved WiFi, otherwise runs the AP + portal.
  if (!wm.autoConnect(AP_NAME, AP_PASS)) {
    lcdPrint(0, "WiFi failed");
    lcdPrint(1, "Restarting...");
    delay(3000);
    ESP.restart();
  }

  applyPortalParams();
  WiFi.setSleep(false);                             // WiFi power-save makes phones time out
  WiFi.setAutoReconnect(true);
  Serial.println("Connected, IP: " + WiFi.localIP().toString());
  lcd->clear();
  lcdPrint(0, "WiFi connected");
  lcdPrint(1, WiFi.localIP().toString());

  startWebServer();
  delay(3000);
  startWatchdog();
  xTaskCreate(pollTask, "poll", 8192, nullptr, 1, nullptr);
  drawScreen();                                    // zeros + offline icon until the first poll
}

void loop() {
  static uint32_t lostSince = 0, lastMsg = 0;
  static bool wasLost = false;

  loopBeat = millis();
  server.handleClient();

  // Heartbeat every 30 s: if these lines stop, the whole ESP is hung
  static uint32_t lastBeatLog = 0;
  if (millis() - lastBeatLog >= 30000) {
    lastBeatLog = millis();
    logf("alive: wifi=%d rssi=%d ip=%s online=%d failedPolls=%u heap=%lu minHeap=%lu",
         (int)WiFi.status(), WiFi.RSSI(), WiFi.localIP().toString().c_str(), (int)online,
         (unsigned)failedPolls, (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
  }

  // WiFi down: retry, and after WIFI_LOST_MS reopen the setup AP
  if (WiFi.status() != WL_CONNECTED) {
    if (lostSince == 0) lostSince = millis();
    if (millis() - lostSince >= WIFI_LOST_MS) openSetupPortal();
    if (lastMsg == 0 || millis() - lastMsg >= POLL_MS) {
      lastMsg = millis();
      lcdPrint(0, "WiFi lost");
      lcdPrint(1, "Reconnecting...");
      WiFi.reconnect();
    }
    wasLost = true;
    delay(5);
    return;
  }
  lostSince = 0;
  if (wasLost) { wasLost = false; needDraw = true; }

  // Scan for the inverter: on request, when no IP is set, or after a long run of failures
  if (scanRequested || inverterIp.length() == 0 ||
      (failedPolls >= RESCAN_AFTER && (lastScan == 0 || millis() - lastScan > RESCAN_EVERY))) {
    scanRequested = false;
    runDiscovery();
  }

  if (needDraw)      { needDraw = false; needIcon = false; drawScreen(); }
  else if (needIcon) { needIcon = false; showLinkIcon(); }

  delay(2);
}
