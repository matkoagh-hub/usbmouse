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
#define FIRMWARE_VERSION       "1.0.0"

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

    // Prvá OTA kontrola ihneď po štarte (spustí sa pri prvom prechode loop())
    lastOtaCheck = millis() - OTA_CHECK_INTERVAL_MS;
}

void loop() {
    handleResetButton();
    ensureWiFi();

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
