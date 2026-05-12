/**
 * ESP32-S3 USB HID Mouse & Keyboard Controller
 *
 * Čo robí:
 *   - ESP32-S3 sa pripojí k PC cez USB a tvári sa ako myš + klávesnica (USB HID)
 *   - Pri prvom spustení (alebo po stlačení BOOT tlačidla 3 s) vytvorí WiFi AP
 *     "ESP32-Mouse-Setup" kde nastavíš WiFi sieť cez captive portál (mobil/PC)
 *   - Každú hodinu skontroluje GitHub releases a ak je nová verzia firmware.bin,
 *     automaticky sa aktualizuje cez OTA a reštartuje
 *
 * Nastavenie Arduino IDE:
 *   Board       : ESP32S3 Dev Module  (alebo tvoja konkrétna doska)
 *   USB Mode    : USB-OTG (TinyUSB)          ← DÔLEŽITÉ
 *   USB CDC on Boot: Disabled                ← vypni aby HID fungoval správne
 *   Upload Mode : UART0 / Hardware CDC
 *   Partition   : Default 4MB with spiffs    (alebo "OTA" schéma pre OTA)
 *
 * Potrebné knižnice (Arduino Library Manager):
 *   - WiFiManager  od tablatronix  >= 2.0.17
 *   - ArduinoJson  od bblanchon    >= 7.0
 *   (USB, HTTPClient, Update – sú súčasťou ESP32 Arduino core)
 *
 * Ako nahrať aktualizáciu:
 *   1. Zmeň FIRMWARE_VERSION v config.h (napr. "1.0.1")
 *   2. Skompiluj: Sketch → Export Compiled Binary  →  vznikne firmware.bin
 *   3. Vytvor GitHub Release s tagom "v1.0.1" a prihoď firmware.bin ako asset
 *   4. ESP si ho stiahne samo do hodiny, alebo reštartuj pre okamžitú kontrolu
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include "config.h"
#include "hid_control.h"
#include "github_ota.h"

// ── Stav ─────────────────────────────────────────────────────────────────────

static unsigned long lastOtaCheck   = 0;
static unsigned long bootBtnPressed = 0;
static bool          btnWasDown     = false;

// ── WiFi setup ───────────────────────────────────────────────────────────────

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

// ── Tlačidlo BOOT – dlhé stlačenie = reset WiFi ──────────────────────────────

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

// ── Reconnect pri strate WiFi ────────────────────────────────────────────────

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
        // Ak dlhodobý výpadok, spusti config portál odznova
        Serial.println("[WiFi] Reconnect zlyhal, spúšťam config portál...");
        startWiFiManager(false);
    }
}

// ── Setup ────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(400);

    Serial.println("\n=== ESP32-S3 USB Mouse & Keyboard ===");
    Serial.printf("Verzia firmware: %s\n", FIRMWARE_VERSION);
    Serial.printf("GitHub repo:     %s\n", GITHUB_REPO);

    // Reset tlačidlo
    pinMode(WIFI_RESET_PIN, INPUT_PULLUP);

    // USB HID musí štartovať pred WiFi
    hid_begin();
    delay(1000); // Počkaj na USB enumeráciu hostom (PC)
    Serial.println("[HID] USB Mouse + Keyboard aktívne");

    // WiFi
    startWiFiManager(false);

    // Prvá OTA kontrola hneď po štarte
    lastOtaCheck = millis() - OTA_CHECK_INTERVAL_MS; // Spustí kontrolu ihneď v loop()
}

// ── Loop ─────────────────────────────────────────────────────────────────────

void loop() {
    // Kontrola BOOT tlačidla (reset WiFi)
    handleResetButton();

    // Udržuj WiFi pripojenie
    ensureWiFi();

    // Periodická OTA kontrola
    if (millis() - lastOtaCheck >= OTA_CHECK_INTERVAL_MS) {
        lastOtaCheck = millis();
        ota_checkAndUpdate(); // Ak nájde nový firmware, reštartuje sa samo
    }

    // ── Tu pridaj svoju logiku ovládania myši/klávesnice ─────────────────────
    //
    // Príklady:
    //   mouse_move(10, 0);           // Pohni myšou doprava o 10px
    //   mouse_click(MOUSE_LEFT);     // Klikni ľavým tlačidlom
    //   kb_print("Hello World");     // Napíš text
    //   kb_tap(KEY_RETURN);          // Stlač Enter
    //   kb_combo(KEY_LEFT_GUI, 'd'); // Win+D (skryj okná)
    //
    // Piny GPIO môžeš čítať cez digitalRead() a reagovať na fyzické tlačidlá,
    // joystick (analogRead), alebo prijímať príkazy cez sériovú linku.
    // ─────────────────────────────────────────────────────────────────────────

    delay(10);
}
