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
#include <USB.h>
#include <USBHIDMouse.h>
#include <USBHIDKeyboard.h>

// ═══════════════════════════════════════════════════════════════════════════════
// KONFIGURÁCIA
// ═══════════════════════════════════════════════════════════════════════════════

// Zvýš pri každom release pushnutom na GitHub (semver "major.minor.patch")
#define FIRMWARE_VERSION       "1.0.1"

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
  body{font-family:-apple-system,system-ui,sans-serif;max-width:520px;margin:0 auto;padding:16px;background:#f0f2f5;color:#222}
  h1{font-size:20px;margin:0 0 16px}
  h2{font-size:14px;margin:0 0 8px;color:#555;text-transform:uppercase;letter-spacing:.5px}
  .card{background:#fff;border-radius:12px;padding:16px;margin-bottom:12px;box-shadow:0 1px 3px rgba(0,0,0,.05)}
  input[type=text]{width:100%;padding:12px;font-size:16px;border:1px solid #ddd;border-radius:8px;margin-bottom:8px}
  button{padding:10px 14px;font-size:15px;border:none;border-radius:8px;background:#007aff;color:#fff;cursor:pointer;margin:3px}
  button:active{background:#0051d5}
  button.sec{background:#e8e8ed;color:#222}
  button.sec:active{background:#d1d1d6}
  .row{display:flex;flex-wrap:wrap;gap:4px}
  .pad{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;max-width:240px;margin:0 auto}
  .pad button{padding:18px 0;font-size:18px}
  .pad .empty{visibility:hidden}
  #st{font-size:13px;color:#666;text-align:center;margin-top:8px;min-height:18px}
  .ok{color:#1a7f37 !important}
  .err{color:#cf222e !important}
</style>
</head>
<body>
<h1>ESP32 Remote HID</h1>

<div class="card">
  <h2>Klávesnica – text</h2>
  <input id="txt" type="text" placeholder="Napíš text…" autocomplete="off">
  <div class="row">
    <button onclick="sendText()">Odoslať</button>
    <button class="sec" onclick="sendText(true)">Odoslať + Enter</button>
  </div>
</div>

<div class="card">
  <h2>Špeciálne klávesy</h2>
  <div class="row">
    <button class="sec" onclick="key('enter')">Enter</button>
    <button class="sec" onclick="key('tab')">Tab</button>
    <button class="sec" onclick="key('esc')">Esc</button>
    <button class="sec" onclick="key('back')">⌫ Backspace</button>
    <button class="sec" onclick="key('space')">Medzera</button>
    <button class="sec" onclick="key('del')">Delete</button>
  </div>
  <h2 style="margin-top:12px">Kombinácie</h2>
  <div class="row">
    <button class="sec" onclick="combo('ctrl','c')">Ctrl+C</button>
    <button class="sec" onclick="combo('ctrl','v')">Ctrl+V</button>
    <button class="sec" onclick="combo('ctrl','z')">Ctrl+Z</button>
    <button class="sec" onclick="combo('alt','tab')">Alt+Tab</button>
    <button class="sec" onclick="combo('gui','d')">Win+D</button>
    <button class="sec" onclick="combo('gui','r')">Win+R</button>
  </div>
</div>

<div class="card">
  <h2>Myš</h2>
  <div class="pad">
    <button class="sec empty"></button>
    <button class="sec" onclick="move(0,-30)">▲</button>
    <button class="sec empty"></button>
    <button class="sec" onclick="move(-30,0)">◀</button>
    <button onclick="click('left')">●</button>
    <button class="sec" onclick="move(30,0)">▶</button>
    <button class="sec empty"></button>
    <button class="sec" onclick="move(0,30)">▼</button>
    <button class="sec empty"></button>
  </div>
  <div class="row" style="justify-content:center;margin-top:8px">
    <button class="sec" onclick="click('left')">Ľavý klik</button>
    <button class="sec" onclick="click('right')">Pravý klik</button>
    <button class="sec" onclick="scrollW(-3)">Scroll ▲</button>
    <button class="sec" onclick="scrollW(3)">Scroll ▼</button>
  </div>
</div>

<div id="st">Pripojený k ESP32 v1.0.1</div>

<script>
const st = document.getElementById('st');
async function api(path, body) {
  try {
    const r = await fetch(path, {method:'POST', body});
    st.className = r.ok ? 'ok' : 'err';
    st.textContent = r.ok ? '✓ '+path : '✗ '+r.status;
  } catch(e) { st.className='err'; st.textContent='✗ '+e.message; }
}
function sendText(withEnter) {
  const t = document.getElementById('txt').value;
  if(!t) return;
  api('/api/type', t).then(()=>{ if(withEnter) api('/api/key','enter'); });
  document.getElementById('txt').value='';
}
document.getElementById('txt').addEventListener('keydown', e=>{ if(e.key==='Enter') sendText(true); });
function key(k)         { api('/api/key', k); }
function combo(m,k)     { api('/api/combo', m+','+k); }
function move(dx,dy)    { api('/api/move', dx+','+dy); }
function click(b)       { api('/api/click', b); }
function scrollW(d)     { api('/api/scroll', String(d)); }
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

    startWiFiManager(false);

    // Web UI – ovládanie HID cez prehliadač na IP adrese ESP
    setupWebServer();

    // Prvá OTA kontrola ihneď po štarte (spustí sa pri prvom prechode loop())
    lastOtaCheck = millis() - OTA_CHECK_INTERVAL_MS;
}

void loop() {
    handleResetButton();
    ensureWiFi();

    // Obsluha web requestov (musí byť volaná často)
    webServer.handleClient();

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
