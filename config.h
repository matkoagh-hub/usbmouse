#pragma once

// ── Firmware version ─────────────────────────────────────────────────────────
// Zvýš toto číslo pri každom release nahratom na GitHub (napr. "1.0.1", "1.1.0")
#define FIRMWARE_VERSION "1.0.0"

// ── GitHub OTA ───────────────────────────────────────────────────────────────
#define GITHUB_REPO          "matkoagh-hub/usbmouse"
#define FIRMWARE_BIN_NAME    "firmware.bin"
// Interval kontroly aktualizácií (každú hodinu)
#define OTA_CHECK_INTERVAL_MS (60UL * 60UL * 1000UL)

// ── WiFi Manager ─────────────────────────────────────────────────────────────
// Názov AP siete ktorá sa vytvorí pri prvom spustení (alebo dlhom stlačení BOOT)
#define WIFI_AP_NAME         "ESP32-Mouse-Setup"
// Koľko sekúnd čakať v config portáli pred reštartom
#define WIFI_CONFIG_TIMEOUT  180

// ── Reset WiFi tlačidlo ──────────────────────────────────────────────────────
// GPIO 0 = BOOT tlačidlo na väčšine ESP32-S3 DevKit dosiek
#define WIFI_RESET_PIN       0
// Dlhé stlačenie (ms) na vymazanie WiFi a vstup do config portálu
#define WIFI_RESET_HOLD_MS   3000
