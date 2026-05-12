/**
 * ESP32-S3 USB HID Mouse & Keyboard Controller
 *
 * Čo robí:
 *   - ESP32-S3 sa pripojí k PC cez USB a tvári sa ako myš + klávesnica (USB HID)
 *   - Pri prvom spustení (alebo po dlhom stlačení BOOT tlačidla) vytvorí WiFi AP
 *     "ESP32-Mouse-Setup" kde nastavíš WiFi sieť cez captive portál
 *   - Každú hodinu skontroluje GitHub releases a ak je nová verzia firmware.bin,
 *     automaticky sa aktualizuje cez OTA a reštartuje
 *
 * Nastavenie Arduino IDE:
 *   Board           : ESP32S3 Dev Module
 *   USB Mode        : USB-OTG (TinyUSB)        ← DÔLEŽITÉ
 *   USB CDC on Boot : Disabled
 *   Upload Mode     : UART0 / Hardware CDC
 *   Partition       : Default 4MB with spiffs
 *
 * Potrebné knižnice (Library Manager):
 *   - WiFiManager  od tzapu / tablatronix  >= 2.0.17
 *   - ArduinoJson  od bblanchon            >= 7.0
 *   (USB, HTTPClient, Update – súčasť ESP32 Arduino core)
 *
 * Ako vydať novú verziu:
 *   1. Zmeň FIRMWARE_VERSION nižšie (napr. "1.0.1")
 *   2. Commitni a vytvor git tag rovnakej verzie: git tag v1.0.1 && git push origin v1.0.1
 *   3. GitHub Actions automaticky skompiluje a vytvorí Release s firmware.bin
 *   4. ESP si ho stiahne sám do hodiny (alebo reštartuj pre okamžitú kontrolu)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <USB.h>
#include <USBHIDMouse.h>
#include <USBHIDKeyboard.h>

// ═══════════════════════════════════════════════════════════════════════════════
// KONFIGURÁCIA
// ═══════════════════════════════════════════════════════════════════════════════

// Zvýš pri každom release pushnutom na GitHub (semver "major.minor.patch")
#define FIRMWARE_VERSION       "1.0.6"

// GitHub repo odkiaľ sa sťahujú aktualizácie
#define GITHUB_REPO            "matkoagh-hub/usbmouse"
#define FIRMWARE_BIN_NAME      "firmware.bin"

// Interval kontroly aktualizácií (1 hodina)
#define OTA_CHECK_INTERVAL_MS  (60UL * 60UL * 1000UL)

// Názov AP siete pri prvom spustení / po resete
#define WIFI_AP_NAME           "ESP32-Mouse-Setup"
#define WIFI_CONFIG_TIMEOUT    180  // sekundy

// BOOT tlačidlo (GPIO 0) – dlhé stlačenie vymaže WiFi a spustí config portál
#define WIFI_RESET_PIN         0
#define WIFI_RESET_HOLD_MS     3000

// ═══════════════════════════════════════════════════════════════════════════════
// USB HID – myš a klávesnica
// ═══════════════════════════════════════════════════════════════════════════════

USBHIDMouse    hidMouse;
USBHIDKeyboard hidKeyboard;

void hid_begin() {
    hidMouse.begin();
    hidKeyboard.begin();
    USB.begin();
}

// ── Myš ──────────────────────────────────────────────────────────────────────
inline void mouse_move(int8_t dx, int8_t dy)        { hidMouse.move(dx, dy, 0); }
inline void mouse_scroll(int8_t delta)              { hidMouse.move(0, 0, delta); }
inline void mouse_click(uint8_t btn = MOUSE_LEFT)   { hidMouse.click(btn); }
inline void mouse_press(uint8_t btn = MOUSE_LEFT)   { hidMouse.press(btn); }
inline void mouse_release(uint8_t btn = MOUSE_LEFT) { hidMouse.release(btn); }

// ── Klávesnica ───────────────────────────────────────────────────────────────
inline void kb_print(const char* text)   { hidKeyboard.print(text); }
inline void kb_println(const char* text) { hidKeyboard.println(text); }

// Stlač a pusti jeden kláves (KEY_RETURN, KEY_LEFT_CTRL, ...)
inline void kb_tap(uint8_t key) {
    hidKeyboard.press(key);
    delay(30);
    hidKeyboard.release(key);
}

// Kombinácia: drž modifier (napr. KEY_LEFT_CTRL) a stlač key
inline void kb_combo(uint8_t modifier, uint8_t key) {
    hidKeyboard.press(modifier);
    delay(10);
    hidKeyboard.press(key);
    delay(30);
    hidKeyboard.releaseAll();
}

// ═══════════════════════════════════════════════════════════════════════════════
// GitHub OTA – sťahovanie aktualizácií
// ═══════════════════════════════════════════════════════════════════════════════

// Porovná sémantické verzie "X.Y.Z", vráti true ak newVer > currentVer
static bool ota_isNewer(const char* current, const char* newVer) {
    int cMaj = 0, cMin = 0, cPat = 0;
    int nMaj = 0, nMin = 0, nPat = 0;
    sscanf(current, "%d.%d.%d", &cMaj, &cMin, &cPat);
    const char* nv = (newVer[0] == 'v') ? newVer + 1 : newVer;  // preskoč 'v'
    sscanf(nv, "%d.%d.%d", &nMaj, &nMin, &nPat);
    if (nMaj != cMaj) return nMaj > cMaj;
    if (nMin != cMin) return nMin > cMin;
    return nPat > cPat;
}

// Stiahne firmware z URL a aplikuje cez Update API
static bool ota_download(const String& url) {
    Serial.printf("[OTA] Sťahujem z: %s\n", url.c_str());

    WiFiClientSecure client;
    client.setInsecure();  // Pre produkciu pridaj root CA certifikát

    HTTPClient http;
    http.begin(client, url);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(30000);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[OTA] HTTP chyba: %d\n", code);
        http.end();
        return false;
    }

    int size = http.getSize();
    Serial.printf("[OTA] Veľkosť firmware: %d bajtov\n", size);

    if (!Update.begin(size < 0 ? UPDATE_SIZE_UNKNOWN : (size_t)size)) {
        Serial.printf("[OTA] Nedostatok miesta: %s\n", Update.errorString());
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    size_t written = Update.writeStream(*stream);
    Serial.printf("[OTA] Zapísaných: %u bajtov\n", written);

    if (!Update.end() || !Update.isFinished()) {
        Serial.printf("[OTA] Chyba pri zápise: %s\n", Update.errorString());
        http.end();
        return false;
    }

    http.end();
    Serial.println("[OTA] Aktualizácia úspešná!");
    return true;
}

// Skontroluje GitHub releases, ak je novšia verzia – stiahne ju a reštartuje ESP
bool ota_checkAndUpdate() {
    if (WiFi.status() != WL_CONNECTED) return false;

    Serial.println("[OTA] Kontrolujem GitHub releases...");

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    String apiUrl = "https://api.github.com/repos/" GITHUB_REPO "/releases/latest";
    http.begin(client, apiUrl);
    http.addHeader("User-Agent", "ESP32S3-OTA/" FIRMWARE_VERSION);
    http.addHeader("Accept", "application/vnd.github.v3+json");
    http.setTimeout(15000);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[OTA] GitHub API chyba: %d\n", code);
        http.end();
        return false;
    }

    // Filter – parsujeme len potrebné polia (šetríme RAM)
    JsonDocument filter;
    filter["tag_name"] = true;
    filter["assets"][0]["name"] = true;
    filter["assets"][0]["browser_download_url"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(),
                                               DeserializationOption::Filter(filter));
    http.end();

    if (err) {
        Serial.printf("[OTA] JSON chyba: %s\n", err.c_str());
        return false;
    }

    const char* tag = doc["tag_name"] | "";
    if (strlen(tag) == 0) {
        Serial.println("[OTA] Žiadne releases na GitHub");
        return false;
    }

    Serial.printf("[OTA] Aktuálna: %s  |  GitHub: %s\n", FIRMWARE_VERSION, tag);

    if (!ota_isNewer(FIRMWARE_VERSION, tag)) {
        Serial.println("[OTA] Firmware je aktuálny");
        return false;
    }

    // Nájdi asset firmware.bin
    String downloadUrl;
    for (JsonObject asset : doc["assets"].as<JsonArray>()) {
        const char* name = asset["name"] | "";
        if (strcmp(name, FIRMWARE_BIN_NAME) == 0) {
            downloadUrl = asset["browser_download_url"].as<String>();
            break;
        }
    }

    if (downloadUrl.isEmpty()) {
        Serial.printf("[OTA] Asset '%s' sa nenašiel v release '%s'\n",
                      FIRMWARE_BIN_NAME, tag);
        return false;
    }

    if (ota_download(downloadUrl)) {
        delay(1500);
        ESP.restart();
        return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// WiFi
// ═══════════════════════════════════════════════════════════════════════════════

static unsigned long lastOtaCheck   = 0;
static unsigned long bootBtnPressed = 0;
static bool          btnWasDown     = false;

// ── Mouse jiggler – každých 5 minút pohne myšou o 2 px tam a späť ─────────
#define JIGGLE_INTERVAL_MS (5UL * 60UL * 1000UL)
static bool          jigglerEnabled = false;
static unsigned long lastJiggle     = 0;

static void startWiFiManager(bool forceConfig) {
    WiFiManager wm;
    wm.setConfigPortalTimeout(WIFI_CONFIG_TIMEOUT);
    wm.setConnectTimeout(20);
    wm.setDebugOutput(false);

    Serial.printf("[WiFi] AP: %s\n", WIFI_AP_NAME);

    bool ok = forceConfig
              ? wm.startConfigPortal(WIFI_AP_NAME)
              : wm.autoConnect(WIFI_AP_NAME);

    if (!ok) {
        Serial.println("[WiFi] Pripojenie zlyhalo, reštartujem...");
        delay(2000);
        ESP.restart();
    }

    Serial.printf("[WiFi] Pripojený! IP: %s\n", WiFi.localIP().toString().c_str());
}

// Dlhé stlačenie BOOT tlačidla = vymaže WiFi nastavenia
static void handleResetButton() {
    bool pressed = (digitalRead(WIFI_RESET_PIN) == LOW);

    if (pressed && !btnWasDown) {
        bootBtnPressed = millis();
        btnWasDown = true;
    } else if (!pressed && btnWasDown) {
        btnWasDown = false;
        bootBtnPressed = 0;
    }

    if (btnWasDown && millis() - bootBtnPressed >= WIFI_RESET_HOLD_MS) {
        Serial.println("[WiFi] Reset tlačidlo – mažem WiFi nastavenia...");
        WiFiManager wm;
        wm.resetSettings();
        delay(500);
        ESP.restart();
    }
}

static void ensureWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;

    Serial.println("[WiFi] Spojenie stratené, pokúšam sa znova...");
    WiFi.reconnect();

    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
        delay(500);
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Znova pripojený: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("[WiFi] Reconnect zlyhal, spúšťam config portál...");
        startWiFiManager(false);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Makrá – záznam a prehrávanie sekvencie HID akcií
// ═══════════════════════════════════════════════════════════════════════════════

static volatile bool macroRunning       = false;
static volatile bool macroStopRequested = false;
static String        currentMacroJson;            // držaný kým beží task
static TaskHandle_t  macroTaskHandle    = nullptr;

// Pomocné funkcie keyFromName / modifierFromName sú definované nižšie pri WebUI

// Forward decl
static uint8_t keyFromName(const String& n);
static uint8_t modifierFromName(const String& n);

// Vykoná jeden krok makra
static void executeStep(JsonObject step) {
    const char* a = step["a"] | "";

    if (!strcmp(a, "type")) {
        const char* t = step["t"] | "";
        kb_print(t);
    }
    else if (!strcmp(a, "key")) {
        uint8_t code = keyFromName(String(step["k"] | ""));
        if (code) kb_tap(code);
    }
    else if (!strcmp(a, "combo")) {
        uint8_t mod  = modifierFromName(String(step["m"] | ""));
        uint8_t code = keyFromName(String(step["k"] | ""));
        if (mod && code) kb_combo(mod, code);
    }
    else if (!strcmp(a, "move")) {
        int x = constrain((int)(step["x"] | 0), -127, 127);
        int y = constrain((int)(step["y"] | 0), -127, 127);
        mouse_move((int8_t)x, (int8_t)y);
    }
    else if (!strcmp(a, "click")) {
        const char* b = step["b"] | "left";
        uint8_t btn = MOUSE_LEFT;
        if (!strcmp(b, "right"))  btn = MOUSE_RIGHT;
        if (!strcmp(b, "middle")) btn = MOUSE_MIDDLE;
        mouse_click(btn);
    }
    else if (!strcmp(a, "scroll")) {
        int d = constrain((int)(step["d"] | 0), -127, 127);
        mouse_scroll((int8_t)d);
    }
    else if (!strcmp(a, "wait")) {
        unsigned long ms = step["ms"] | 0UL;
        unsigned long start = millis();
        while (millis() - start < ms) {
            if (macroStopRequested) return;
            delay(20);  // kontroluj stop každých 20 ms
        }
    }
}

// FreeRTOS task – prehrá makro asynchrónne, aby webserver nezamrzol
static void macroTaskFunc(void*) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, currentMacroJson);

    if (!err && doc.is<JsonArray>()) {
        JsonArray arr = doc.as<JsonArray>();
        Serial.printf("[Macro] Spúšťam %u krokov\n", (unsigned)arr.size());

        int i = 0;
        for (JsonObject step : arr) {
            if (macroStopRequested) {
                Serial.printf("[Macro] Zastavené pri kroku %d\n", i);
                break;
            }
            Serial.printf("[Macro] Krok %d: %s\n", i, step["a"] | "?");
            executeStep(step);
            i++;
            delay(15);  // krátka pauza medzi krokmi (HID nepretečie buffer)
        }
        Serial.println("[Macro] Hotovo");
    } else {
        Serial.printf("[Macro] Chyba parsovania JSON: %s\n", err.c_str());
    }

    currentMacroJson    = "";
    macroRunning        = false;
    macroStopRequested  = false;
    macroTaskHandle     = nullptr;
    vTaskDelete(nullptr);
}

static bool macro_start(const String& json) {
    if (macroRunning) return false;
    currentMacroJson    = json;
    macroRunning        = true;
    macroStopRequested  = false;

    BaseType_t r = xTaskCreate(macroTaskFunc, "macro", 8192, nullptr, 1, &macroTaskHandle);
    if (r != pdPASS) {
        currentMacroJson = "";
        macroRunning     = false;
        return false;
    }
    return true;
}

// ── Storage v LittleFS: /macros/<name>.json (obsah = JSON array krokov) ────

static bool macro_name_valid(const String& n) {
    if (n.isEmpty() || n.length() > 40) return false;
    for (size_t i = 0; i < n.length(); i++) {
        char c = n[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

static bool macro_save(const String& name, const String& stepsJson) {
    if (!macro_name_valid(name)) return false;
    if (!LittleFS.exists("/macros")) LittleFS.mkdir("/macros");
    File f = LittleFS.open("/macros/" + name + ".json", "w");
    if (!f) return false;
    f.print(stepsJson);
    f.close();
    return true;
}

static String macro_load(const String& name) {
    if (!macro_name_valid(name)) return "";
    File f = LittleFS.open("/macros/" + name + ".json", "r");
    if (!f) return "";
    String s = f.readString();
    f.close();
    return s;
}

static bool macro_delete(const String& name) {
    if (!macro_name_valid(name)) return false;
    return LittleFS.remove("/macros/" + name + ".json");
}

static String macro_list_json() {
    String result = "[";
    File dir = LittleFS.open("/macros");
    if (dir && dir.isDirectory()) {
        bool first = true;
        File f = dir.openNextFile();
        while (f) {
            String n = f.name();
            // f.name() môže vrátiť absolútnu cestu alebo len meno – orež
            int slash = n.lastIndexOf('/');
            if (slash >= 0) n = n.substring(slash + 1);
            if (n.endsWith(".json")) {
                n = n.substring(0, n.length() - 5);
                if (!first) result += ',';
                result += '"';
                result += n;
                result += '"';
                first = false;
            }
            f = dir.openNextFile();
        }
    }
    result += "]";
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Web UI – ovládanie HID cez prehliadač
// ═══════════════════════════════════════════════════════════════════════════════

WebServer webServer(80);

static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="sk">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 Remote HID</title>
<style>
  *{box-sizing:border-box}
  body{font-family:-apple-system,system-ui,sans-serif;max-width:560px;margin:0 auto;padding:14px;background:#f0f2f5;color:#222}
  h1{font-size:20px;margin:0 0 12px}
  h2{font-size:13px;margin:0 0 8px;color:#555;text-transform:uppercase;letter-spacing:.5px}
  .card{background:#fff;border-radius:12px;padding:14px;margin-bottom:10px;box-shadow:0 1px 3px rgba(0,0,0,.05)}
  input[type=text]{width:100%;padding:10px;font-size:15px;border:1px solid #ddd;border-radius:8px;margin-bottom:6px}
  button{padding:9px 12px;font-size:14px;border:none;border-radius:8px;background:#007aff;color:#fff;cursor:pointer;margin:2px}
  button:active{background:#0051d5}
  button.sec{background:#e8e8ed;color:#222}
  button.sec:active{background:#d1d1d6}
  button.danger{background:#cf222e}
  button.warn{background:#bf8700}
  .row{display:flex;flex-wrap:wrap;gap:3px;align-items:center}
  .pad{display:grid;grid-template-columns:repeat(3,1fr);gap:5px;max-width:230px;margin:0 auto}
  .pad button{padding:16px 0;font-size:17px}
  .pad .empty{visibility:hidden}
  #st{font-size:12px;color:#666;text-align:center;margin-top:6px;min-height:16px}
  .ok{color:#1a7f37 !important}
  .err{color:#cf222e !important}
  .rec{background:#cf222e;color:#fff;padding:10px;border-radius:8px;margin-bottom:10px;font-weight:600;text-align:center;cursor:pointer;user-select:none}
  .rec.off{background:#34c759}
  .jig{background:#e8e8ed;color:#222;padding:10px;border-radius:8px;margin-bottom:10px;font-weight:600;text-align:center;cursor:pointer;user-select:none}
  .jig.on{background:#ff9f0a;color:#fff}
  ol.stp{padding-left:22px;margin:6px 0;font-size:13px;max-height:280px;overflow-y:auto}
  ol.stp li{margin-bottom:3px;line-height:1.4}
  ol.stp li button{padding:1px 7px;font-size:11px;margin:0 1px}
  .sv{display:flex;align-items:center;padding:6px 0;border-bottom:1px solid #eee;gap:4px}
  .sv:last-child{border:none}
  .sv span{flex:1;font-size:14px}
</style>
</head>
<body>

<h1>ESP32 Remote HID</h1>

<div id="rec" class="rec off" onclick="toggleRec()">⏺ Záznam VYPNUTÝ — klikni pre zapnutie</div>
<div id="jig" class="jig" onclick="toggleJiggle()">🖱️ Mouse Jiggler VYPNUTÝ — klikni pre zapnutie</div>

<div class="card">
  <h2>Klávesnica — text</h2>
  <input id="txt" type="text" placeholder="Napíš text…" autocomplete="off">
  <div class="row">
    <button onclick="actText(false)">Odoslať</button>
    <button class="sec" onclick="actText(true)">Odoslať + Enter</button>
    <button id="skBtn" class="sec" onclick="toggleSK()" title="Oprava číslic pre SK klávesnicu">SK</button>
  </div>
</div>

<div class="card">
  <h2>Špeciálne klávesy</h2>
  <div class="row">
    <button class="sec" onclick="actKey('enter')">Enter</button>
    <button class="sec" onclick="actKey('tab')">Tab</button>
    <button class="sec" onclick="actKey('esc')">Esc</button>
    <button class="sec" onclick="actKey('back')">⌫</button>
    <button class="sec" onclick="actKey('space')">Medzera</button>
    <button class="sec" onclick="actKey('del')">Del</button>
    <button class="sec" onclick="actKey('up')">↑</button>
    <button class="sec" onclick="actKey('down')">↓</button>
    <button class="sec" onclick="actKey('left')">←</button>
    <button class="sec" onclick="actKey('right')">→</button>
  </div>
  <h2 style="margin-top:10px">Kombinácie</h2>
  <div class="row">
    <button class="sec" onclick="actCombo('ctrl','c')">Ctrl+C</button>
    <button class="sec" onclick="actCombo('ctrl','v')">Ctrl+V</button>
    <button class="sec" onclick="actCombo('ctrl','x')">Ctrl+X</button>
    <button class="sec" onclick="actCombo('ctrl','z')">Ctrl+Z</button>
    <button class="sec" onclick="actCombo('ctrl','a')">Ctrl+A</button>
    <button class="sec" onclick="actCombo('alt','tab')">Alt+Tab</button>
    <button class="sec" onclick="actCombo('alt','f4')">Alt+F4</button>
    <button class="sec" onclick="actCombo('gui','d')">Win+D</button>
    <button class="sec" onclick="actCombo('gui','r')">Win+R</button>
    <button class="sec" onclick="actCombo('gui','l')">Win+L</button>
    <button class="sec" onclick="customCombo()">+ vlastná…</button>
  </div>
</div>

<div class="card">
  <h2>Myš</h2>
  <div class="pad">
    <button class="sec empty"></button>
    <button class="sec" onclick="actMove(0,-30)">▲</button>
    <button class="sec empty"></button>
    <button class="sec" onclick="actMove(-30,0)">◀</button>
    <button onclick="actClick('left')">●</button>
    <button class="sec" onclick="actMove(30,0)">▶</button>
    <button class="sec empty"></button>
    <button class="sec" onclick="actMove(0,30)">▼</button>
    <button class="sec empty"></button>
  </div>
  <div class="row" style="justify-content:center;margin-top:6px">
    <button class="sec" onclick="actClick('left')">Ľavý</button>
    <button class="sec" onclick="actClick('right')">Pravý</button>
    <button class="sec" onclick="actClick('middle')">Stred</button>
    <button class="sec" onclick="actScroll(-3)">Scroll ▲</button>
    <button class="sec" onclick="actScroll(3)">Scroll ▼</button>
    <button class="sec" onclick="customMove()">+ pohyb…</button>
  </div>
</div>

<div class="card">
  <h2>Aktuálne makro (<span id="cnt">0</span> krokov)</h2>
  <ol id="stp" class="stp"></ol>
  <div class="row">
    <button class="warn" onclick="addPause()">+ Pauza…</button>
    <button onclick="runSteps()">▶ Spustiť</button>
    <button class="danger" onclick="stopMacro()">⏹ Stop</button>
    <button class="sec" onclick="clearSteps()">Vyčistiť</button>
  </div>
  <div class="row" style="margin-top:6px">
    <input id="mn" type="text" placeholder="Názov makra (a-z, 0-9, -, _)" style="flex:1;margin-bottom:0">
    <button onclick="saveMacro()">Uložiť</button>
  </div>
</div>

<div class="card">
  <h2>Uložené makrá</h2>
  <div id="sv"></div>
</div>

<div class="card">
  <div class="row" style="justify-content:space-between;align-items:center">
    <span style="font-size:13px;color:#555">ESP32 Remote HID v1.0.5</span>
    <button class="danger" onclick="reboot()">↺ Reboot / OTA update</button>
  </div>
</div>

<div id="st">Pripravený</div>

<script>
const st = document.getElementById('st');
let recording = false;
let steps = [];

async function api(p, b) {
  try {
    const r = await fetch(p, {method:'POST', body:b||''});
    st.className = r.ok ? 'ok' : 'err';
    st.textContent = r.ok ? '✓ '+p : '✗ '+p+' '+r.status;
    return r;
  } catch(e) { st.className='err'; st.textContent='✗ '+e.message; }
}

function toggleRec() {
  recording = !recording;
  const b = document.getElementById('rec');
  if (recording) {
    b.classList.remove('off');
    b.textContent = '⏺ Záznam ZAPNUTÝ — kliky pridávajú kroky';
  } else {
    b.classList.add('off');
    b.textContent = '⏺ Záznam VYPNUTÝ — klikni pre zapnutie';
  }
}

function rec(s) { steps.push(s); render(); }

function desc(s) {
  switch(s.a) {
    case 'type':   return '📝 "' + s.t + '"';
    case 'key':    return '⌨️ ' + s.k;
    case 'combo':  return '🔗 ' + s.m + '+' + s.k;
    case 'move':   return '🖱️ pohyb ('+s.x+', '+s.y+')';
    case 'click':  return '👆 klik ' + s.b;
    case 'scroll': return '🎡 scroll ' + s.d;
    case 'wait':   return '⏱️ pauza ' + s.ms + ' ms';
  }
  return JSON.stringify(s);
}

function render() {
  const ol = document.getElementById('stp');
  document.getElementById('cnt').textContent = steps.length;
  ol.innerHTML = '';
  steps.forEach((s,i) => {
    const li = document.createElement('li');
    li.innerHTML = desc(s) +
      ' <button class="sec" onclick="mv('+i+',-1)">↑</button>' +
      '<button class="sec" onclick="mv('+i+',1)">↓</button>' +
      '<button class="sec" onclick="del('+i+')">✕</button>';
    ol.appendChild(li);
  });
}

function del(i) { steps.splice(i,1); render(); }
function mv(i,d) {
  const j=i+d; if(j<0||j>=steps.length) return;
  [steps[i],steps[j]]=[steps[j],steps[i]]; render();
}
function clearSteps() {
  if (steps.length && !confirm('Vyčistiť '+steps.length+' krokov?')) return;
  steps=[]; render();
}
function addPause() {
  const ms = parseInt(prompt('Pauza v milisekundách:', '1000'));
  if (ms > 0) rec({a:'wait', ms});
}

// ── SK layout – čísla treba posielať ako Shift+číslica ───────────────────
let skLayout = false;
function toggleSK() {
  skLayout = !skLayout;
  const b = document.getElementById('skBtn');
  b.style.background = skLayout ? '#007aff' : '';
  b.style.color      = skLayout ? '#fff'    : '';
  b.title = skLayout ? 'SK: čísla sa posielajú ako Shift+číslica' : 'Oprava číslic pre SK klávesnicu';
}

// Pošle text s SK opravou: digit → Shift+digit, zvyšok normálne
async function sendTextSK(text) {
  let seg = '';
  for (const c of text) {
    if (c >= '0' && c <= '9') {
      if (seg) { await api('/api/type', seg); seg = ''; }
      await api('/api/combo', 'shift,' + c);
    } else { seg += c; }
  }
  if (seg) await api('/api/type', seg);
}

// ── Action dispatchers: recording → push step | normal → call API ─────────
function actText(withEnter) {
  const t = document.getElementById('txt').value;
  if (!t) return;
  if (recording) {
    // V zázname: split na segmenty ak je SK mode
    if (skLayout) {
      let seg = '';
      for (const c of t) {
        if (c >= '0' && c <= '9') {
          if (seg) { rec({a:'type', t:seg}); seg=''; }
          rec({a:'combo', m:'shift', k:c});
        } else { seg += c; }
      }
      if (seg) rec({a:'type', t:seg});
    } else {
      rec({a:'type', t});
    }
    if (withEnter) rec({a:'key', k:'enter'});
  } else {
    (skLayout ? sendTextSK(t) : api('/api/type', t))
      .then(() => { if (withEnter) api('/api/key', 'enter'); });
  }
  document.getElementById('txt').value='';
}
document.getElementById('txt').addEventListener('keydown', e=>{
  if(e.key==='Enter') actText(true);
});

function actKey(k)        { recording ? rec({a:'key', k})       : api('/api/key', k); }
function actCombo(m, k)   { recording ? rec({a:'combo', m, k})  : api('/api/combo', m+','+k); }
function actMove(x, y)    { recording ? rec({a:'move', x, y})   : api('/api/move', x+','+y); }
function actClick(b)      { recording ? rec({a:'click', b})     : api('/api/click', b); }
function actScroll(d)     { recording ? rec({a:'scroll', d})    : api('/api/scroll', String(d)); }

function customCombo() {
  const m = prompt('Modifier (ctrl/alt/shift/gui):', 'ctrl'); if (!m) return;
  const k = prompt('Kláves (písmeno alebo special key):', 'c'); if (!k) return;
  actCombo(m.trim(), k.trim());
}
function customMove() {
  const x = parseInt(prompt('Δx (-127 až 127):', '50')) || 0;
  const y = parseInt(prompt('Δy (-127 až 127):', '0'))  || 0;
  actMove(x, y);
}

// ── Macro storage ─────────────────────────────────────────────────────────
async function runSteps() {
  if (!steps.length) { alert('Žiadne kroky'); return; }
  await api('/api/macro/run', JSON.stringify(steps));
}
async function stopMacro() { await api('/api/macro/stop'); }

async function saveMacro() {
  const name = document.getElementById('mn').value.trim();
  if (!name) { alert('Zadaj názov makra'); return; }
  if (!/^[a-zA-Z0-9_-]+$/.test(name)) { alert('Iba a-z, 0-9, -, _'); return; }
  await api('/api/macro/save', name + '\n' + JSON.stringify(steps));
  refreshSaved();
}

async function refreshSaved() {
  try {
    const r = await fetch('/api/macro/list');
    const list = await r.json();
    const div = document.getElementById('sv');
    div.innerHTML = '';
    if (!list.length) { div.textContent = '(žiadne uložené makrá)'; return; }
    list.sort();
    list.forEach(name => {
      const it = document.createElement('div');
      it.className = 'sv';
      it.innerHTML = '<span>'+name+'</span>'+
        '<button onclick="runSaved(\''+name+'\')">▶</button>'+
        '<button class="sec" onclick="loadSaved(\''+name+'\')">✎</button>'+
        '<button class="danger" onclick="delSaved(\''+name+'\')">✕</button>';
      div.appendChild(it);
    });
  } catch(e) { console.error(e); }
}

async function runSaved(name) {
  const r = await fetch('/api/macro/get?name='+encodeURIComponent(name));
  if (!r.ok) return;
  const body = await r.text();
  await api('/api/macro/run', body);
}
async function loadSaved(name) {
  const r = await fetch('/api/macro/get?name='+encodeURIComponent(name));
  if (!r.ok) return;
  steps = await r.json();
  document.getElementById('mn').value = name;
  render();
}
async function delSaved(name) {
  if (!confirm('Vymazať makro "'+name+'"?')) return;
  await api('/api/macro/del', name);
  refreshSaved();
}

// ── Mouse Jiggler ─────────────────────────────────────────────────────────
async function toggleJiggle() {
  const r = await api('/api/jiggle', '');
  if (!r) return;
  const data = await r.json();
  const el = document.getElementById('jig');
  if (data.enabled) {
    el.classList.add('on');
    el.textContent = '🖱️ Mouse Jiggler ZAPNUTÝ — pohybuje každých 5 min';
  } else {
    el.classList.remove('on');
    el.textContent = '🖱️ Mouse Jiggler VYPNUTÝ — klikni pre zapnutie';
  }
}

async function initJiggler() {
  try {
    const r = await fetch('/api/jiggle/status');
    const data = await r.json();
    const el = document.getElementById('jig');
    if (data.enabled) {
      el.classList.add('on');
      el.textContent = '🖱️ Mouse Jiggler ZAPNUTÝ — pohybuje každých 5 min';
    }
  } catch(e) {}
}

// ── Reboot ────────────────────────────────────────────────────────────────
async function reboot() {
  if (!confirm('Rebootnúť ESP? (OTA aktualizácia sa spustí automaticky po štarte)')) return;
  st.className = 'ok';
  st.textContent = 'Rebootujem…';
  try { await fetch('/api/reboot', {method:'POST'}); } catch(e) {}
  // Po ~8 sekundách skús znova načítať stránku
  setTimeout(() => {
    st.textContent = 'Pokúšam sa znovu pripojiť…';
    setTimeout(() => location.reload(), 3000);
  }, 8000);
}

window.addEventListener('load', () => { refreshSaved(); initJiggler(); });
</script>
</body>
</html>)HTML";

// Pomocná funkcia – preloží názov klávesu zo stringu na HID kód
static uint8_t keyFromName(const String& n) {
    if (n == "enter")  return KEY_RETURN;
    if (n == "tab")    return KEY_TAB;
    if (n == "esc")    return KEY_ESC;
    if (n == "back")   return KEY_BACKSPACE;
    if (n == "space")  return ' ';
    if (n == "del")    return KEY_DELETE;
    if (n == "up")     return KEY_UP_ARROW;
    if (n == "down")   return KEY_DOWN_ARROW;
    if (n == "left")   return KEY_LEFT_ARROW;
    if (n == "right")  return KEY_RIGHT_ARROW;
    if (n.length() == 1) return (uint8_t)n[0];   // jedno písmeno
    return 0;
}

static uint8_t modifierFromName(const String& n) {
    if (n == "ctrl")  return KEY_LEFT_CTRL;
    if (n == "alt")   return KEY_LEFT_ALT;
    if (n == "shift") return KEY_LEFT_SHIFT;
    if (n == "gui")   return KEY_LEFT_GUI;   // Win / ⌘
    return 0;
}

void setupWebServer() {
    webServer.on("/", HTTP_GET, []() {
        webServer.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
    });

    // POST /api/type   body: "text na napísanie"
    webServer.on("/api/type", HTTP_POST, []() {
        kb_print(webServer.arg("plain").c_str());
        webServer.send(200, "text/plain", "ok");
    });

    // POST /api/key    body: "enter" | "tab" | "esc" | ...
    webServer.on("/api/key", HTTP_POST, []() {
        uint8_t k = keyFromName(webServer.arg("plain"));
        if (k) kb_tap(k);
        webServer.send(200, "text/plain", "ok");
    });

    // POST /api/combo  body: "ctrl,c"
    webServer.on("/api/combo", HTTP_POST, []() {
        String body = webServer.arg("plain");
        int comma = body.indexOf(',');
        if (comma > 0) {
            uint8_t mod = modifierFromName(body.substring(0, comma));
            uint8_t k   = keyFromName(body.substring(comma + 1));
            if (mod && k) kb_combo(mod, k);
        }
        webServer.send(200, "text/plain", "ok");
    });

    // POST /api/move   body: "dx,dy"
    webServer.on("/api/move", HTTP_POST, []() {
        String body = webServer.arg("plain");
        int comma = body.indexOf(',');
        if (comma > 0) {
            int dx = body.substring(0, comma).toInt();
            int dy = body.substring(comma + 1).toInt();
            // Clampuj na rozsah int8_t aby HID API nepretieklo
            dx = constrain(dx, -127, 127);
            dy = constrain(dy, -127, 127);
            mouse_move((int8_t)dx, (int8_t)dy);
        }
        webServer.send(200, "text/plain", "ok");
    });

    // POST /api/click  body: "left" | "right" | "middle"
    webServer.on("/api/click", HTTP_POST, []() {
        String btn = webServer.arg("plain");
        uint8_t b = MOUSE_LEFT;
        if (btn == "right")  b = MOUSE_RIGHT;
        if (btn == "middle") b = MOUSE_MIDDLE;
        mouse_click(b);
        webServer.send(200, "text/plain", "ok");
    });

    // POST /api/scroll body: "+/-N"
    webServer.on("/api/scroll", HTTP_POST, []() {
        int d = constrain(webServer.arg("plain").toInt(), -127, 127);
        mouse_scroll((int8_t)d);
        webServer.send(200, "text/plain", "ok");
    });

    // ── Makrá ─────────────────────────────────────────────────────────────
    // POST /api/macro/run    body: JSON array krokov
    webServer.on("/api/macro/run", HTTP_POST, []() {
        if (macroRunning) {
            webServer.send(409, "text/plain", "macro already running");
            return;
        }
        if (macro_start(webServer.arg("plain"))) {
            webServer.send(200, "text/plain", "started");
        } else {
            webServer.send(500, "text/plain", "start failed");
        }
    });

    // POST /api/macro/stop
    webServer.on("/api/macro/stop", HTTP_POST, []() {
        macroStopRequested = true;
        webServer.send(200, "text/plain", "stop requested");
    });

    // GET /api/macro/status → {"running":bool}
    webServer.on("/api/macro/status", HTTP_GET, []() {
        webServer.send(200, "application/json",
                       macroRunning ? "{\"running\":true}" : "{\"running\":false}");
    });

    // POST /api/macro/save   body: "name\n[json array]"
    webServer.on("/api/macro/save", HTTP_POST, []() {
        String body = webServer.arg("plain");
        int nl = body.indexOf('\n');
        if (nl < 1) { webServer.send(400, "text/plain", "bad format"); return; }
        String name = body.substring(0, nl);  name.trim();
        String steps = body.substring(nl + 1);
        if (!macro_name_valid(name)) {
            webServer.send(400, "text/plain", "invalid name (a-z, 0-9, -, _ only)");
            return;
        }
        if (macro_save(name, steps)) webServer.send(200, "text/plain", "saved");
        else                          webServer.send(500, "text/plain", "save failed");
    });

    // GET /api/macro/list → JSON array of names
    webServer.on("/api/macro/list", HTTP_GET, []() {
        webServer.send(200, "application/json", macro_list_json());
    });

    // GET /api/macro/get?name=X → JSON array of steps
    webServer.on("/api/macro/get", HTTP_GET, []() {
        String name = webServer.arg("name");
        if (!macro_name_valid(name)) { webServer.send(400, "text/plain", "invalid name"); return; }
        String content = macro_load(name);
        if (content.isEmpty()) { webServer.send(404, "text/plain", "not found"); return; }
        webServer.send(200, "application/json", content);
    });

    // POST /api/jiggle        body: "1" zapnúť / "0" vypnúť / prázdne = toggle
    webServer.on("/api/jiggle", HTTP_POST, []() {
        String b = webServer.arg("plain");
        if (b == "1")      jigglerEnabled = true;
        else if (b == "0") jigglerEnabled = false;
        else               jigglerEnabled = !jigglerEnabled;
        if (jigglerEnabled) lastJiggle = millis() - JIGGLE_INTERVAL_MS; // prvý jiggle ihneď
        Serial.printf("[Jiggle] %s\n", jigglerEnabled ? "zapnutý" : "vypnutý");
        webServer.send(200, "application/json",
                       jigglerEnabled ? "{\"enabled\":true}" : "{\"enabled\":false}");
    });

    // GET /api/jiggle/status
    webServer.on("/api/jiggle/status", HTTP_GET, []() {
        webServer.send(200, "application/json",
                       jigglerEnabled ? "{\"enabled\":true}" : "{\"enabled\":false}");
    });

    // POST /api/reboot – reštartuje ESP (spustí OTA kontrolu po boote)
    webServer.on("/api/reboot", HTTP_POST, []() {
        webServer.send(200, "text/plain", "rebooting...");
        delay(300);
        ESP.restart();
    });

    // POST /api/macro/del    body: name
    webServer.on("/api/macro/del", HTTP_POST, []() {
        String name = webServer.arg("plain"); name.trim();
        if (!macro_name_valid(name)) { webServer.send(400, "text/plain", "invalid name"); return; }
        if (macro_delete(name)) webServer.send(200, "text/plain", "deleted");
        else                     webServer.send(500, "text/plain", "delete failed");
    });

    webServer.onNotFound([]() {
        webServer.send(404, "text/plain", "not found");
    });

    webServer.begin();
    Serial.printf("[Web] Server beží na http://%s/\n", WiFi.localIP().toString().c_str());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Setup & Loop
// ═══════════════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(400);

    Serial.println("\n=== ESP32-S3 USB Mouse & Keyboard ===");
    Serial.printf("Verzia firmware: %s\n", FIRMWARE_VERSION);
    Serial.printf("GitHub repo:     %s\n", GITHUB_REPO);

    pinMode(WIFI_RESET_PIN, INPUT_PULLUP);

    // USB HID musí štartovať pred WiFi
    hid_begin();
    delay(1000);  // Počkaj na USB enumeráciu hostom (PC)
    Serial.println("[HID] USB Mouse + Keyboard aktívne");

    // LittleFS – úložisko pre uložené makrá. true = naformátuje pri prvom štarte.
    if (!LittleFS.begin(true)) {
        Serial.println("[FS] LittleFS inicializácia ZLYHALA – makrá nebudú perzistentné");
    } else {
        Serial.println("[FS] LittleFS OK");
        if (!LittleFS.exists("/macros")) LittleFS.mkdir("/macros");
    }

    startWiFiManager(false);

    // Web UI – ovládanie HID cez prehliadač
    setupWebServer();

    // mDNS – ESP bude dostupné na http://usbmouse.local
    if (MDNS.begin("usbmouse")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("[mDNS] Dostupné na http://usbmouse.local");
    } else {
        Serial.println("[mDNS] Inicializácia zlyhala");
    }

    // Prvá OTA kontrola ihneď po štarte (spustí sa pri prvom prechode loop())
    lastOtaCheck = millis() - OTA_CHECK_INTERVAL_MS;
}

void loop() {
    handleResetButton();
    ensureWiFi();

    // Obsluha web requestov (musí byť volaná často)
    webServer.handleClient();

    // Mouse jiggler – každých 5 minút pohne myšou tam a späť (20 px)
    if (jigglerEnabled && millis() - lastJiggle >= JIGGLE_INTERVAL_MS) {
        lastJiggle = millis();
        mouse_move(20, 10);
        delay(300);
        mouse_move(-20, -10);
        Serial.println("[Jiggle] pohyb");
    }

    // Periodická OTA kontrola
    if (millis() - lastOtaCheck >= OTA_CHECK_INTERVAL_MS) {
        lastOtaCheck = millis();
        ota_checkAndUpdate();  // Ak nájde novší firmware, sám reštartuje
    }

    // ── Sem pridaj svoju logiku ovládania myši/klávesnice ────────────────────
    //
    // Príklady:
    //   mouse_move(10, 0);            // pohni myšou doprava o 10 px
    //   mouse_click(MOUSE_LEFT);      // klikni ľavým tlačidlom
    //   kb_print("Hello World");      // napíš text
    //   kb_tap(KEY_RETURN);           // stlač Enter
    //   kb_combo(KEY_LEFT_GUI, 'd');  // Win+D (skryj okná)
    //
    // Môžeš reagovať na fyzické tlačidlá (digitalRead), joystick (analogRead),
    // alebo prijímať príkazy cez sériovú linku.
    // ─────────────────────────────────────────────────────────────────────────

    delay(10);
}
