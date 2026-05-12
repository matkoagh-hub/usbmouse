#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include "config.h"

// Porovná sémantické verzie "X.Y.Z", vráti true ak newVer > currentVer
static bool _ota_isNewer(const char* current, const char* newVer) {
    int cMaj = 0, cMin = 0, cPat = 0;
    int nMaj = 0, nMin = 0, nPat = 0;
    sscanf(current, "%d.%d.%d", &cMaj, &cMin, &cPat);
    // Preskočí prípadné 'v' na začiatku (napr. "v1.0.1")
    const char* nv = (newVer[0] == 'v') ? newVer + 1 : newVer;
    sscanf(nv, "%d.%d.%d", &nMaj, &nMin, &nPat);
    if (nMaj != cMaj) return nMaj > cMaj;
    if (nMin != cMin) return nMin > cMin;
    return nPat > cPat;
}

// Stiahne a nainštaluje firmware zo zadanej URL, vráti true ak úspešné
static bool _ota_download(const String& url) {
    Serial.printf("[OTA] Sťahujem z: %s\n", url.c_str());

    WiFiClientSecure client;
    client.setInsecure(); // Pre produkciu pridaj root CA certifikát

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

// Hlavná funkcia: skontroluje GitHub releases a ak je nová verzia, aktualizuje
// Vráti true ak sa aktualizácia aplikovala (zariadenie sa samo reštartuje)
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

    // Filtrujeme len potrebné polia aby sme šetrili RAM
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

    if (!_ota_isNewer(FIRMWARE_VERSION, tag)) {
        Serial.println("[OTA] Firmware je aktuálny, aktualizácia nie je potrebná");
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

    if (_ota_download(downloadUrl)) {
        delay(1500);
        ESP.restart();
        return true;
    }
    return false;
}
