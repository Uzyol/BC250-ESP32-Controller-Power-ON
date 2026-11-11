#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <WebServer.h>
#include "security_config.h"
#include <Update.h>
#include "LittleFS.h"
#include <ArduinoJson.h>
#include "pc_control.h"
#include "version.h"
#include "pins.h"

extern WebServer server;
void noteConfigWebActivity();
extern bool pcIsOn;
extern bool shutdownRequested;
extern bool forceShutdown;
extern PowerState powerState;
extern unsigned long powerStateStartTime;
extern String ps5MacAddress;
extern bool ps5Enabled;
extern bool ps5AutoConnect;
extern String wifiSSID;
extern String wifiPassword;
extern bool wifiConfigured;
extern bool apMode;
extern bool wifiRadioEnabled;
extern bool wifiDisablePending;
extern unsigned long wifiDisableRequestedAt;
extern String classicBtSpoofMac;
extern bool classicBtSpoofEnabled;
extern PS5Simple ps5Simple;

// Funktioprototyypit
void saveWiFiConfig(String ssid, String pass);
bool getStablePcState();
void startPowerOn();
void startNormalShutdown();
void startForceShutdown();
void savePS5Config(bool enabled, String mac, bool autoConnect);
bool addPS5ControllerMac(String mac);
bool removePS5ControllerMac(String mac);
void clearPS5ControllerMacs();
void persistPS5Config();
bool saveBluetoothSpoofMac(String mac);
bool isValidBtMac(const String& mac);

String indexHtml = "";
String updateHtml = "";
String setupHtml = "";
String styleCss = "";
bool filesLoaded = false;

void loadFiles() {
    if (filesLoaded) return;
    
    Serial.println("Loading web files from LittleFS...");
    
    if (LittleFS.exists("/index.html")) {
        File file = LittleFS.open("/index.html", "r");
        indexHtml = file.readString();
        file.close();
        Serial.println("  - index.html loaded");
    } else {
        Serial.println("  - index.html NOT FOUND!");
    }
    
    if (LittleFS.exists("/update.html")) {
        File file = LittleFS.open("/update.html", "r");
        updateHtml = file.readString();
        file.close();
        Serial.println("  - update.html loaded");
    } else {
        Serial.println("  - update.html NOT FOUND!");
    }
    
    if (LittleFS.exists("/setup.html")) {
        File file = LittleFS.open("/setup.html", "r");
        setupHtml = file.readString();
        file.close();
        Serial.println("  - setup.html loaded");
    } else {
        Serial.println("  - setup.html NOT FOUND!");
    }
    
    if (LittleFS.exists("/style.css")) {
        File file = LittleFS.open("/style.css", "r");
        styleCss = file.readString();
        file.close();
        Serial.println("  - style.css loaded");
    } else {
        Serial.println("  - style.css NOT FOUND!");
    }
    
    filesLoaded = true;
}

// Every HTTP route (including read-only endpoints) requires authentication.
// Upload handlers also check credentials BEFORE accepting any firmware/FS bytes.
bool requireWebAuth() {
    if (server.authenticate(WEB_USERNAME, WEB_PASSWORD)) return true;
    server.requestAuthentication(BASIC_AUTH, "BC250 Control");
    return false;
}

void setupWebServer() {
    loadFiles();
    
    Serial.println("Setting up web server routes...");
    
    // ========== TÄRKEÄÄ: POST-reitit ENNEN GET-reittejä! ==========
    
    // Manual ESP32 reboot from the setup page. Send the HTTP response first so the browser
    // can show confirmation before the MCU restarts.
    server.on("/api/reboot", HTTP_POST, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        Serial.println("API: ESP32 reboot requested from web interface");
        server.send(200, "application/json", "{\"status\":\"rebooting\"}");
        delay(500);
        ESP.restart();
    });

    // FIRMWARE UPDATE - POST (tämä käsittelee tiedoston lähetyksen)
    server.on("/update", HTTP_POST, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        // Tämä suoritetaan kun upload on valmis
        if (Update.hasError()) {
            server.send(500, "text/plain", "Update failed!");
            Serial.println("Update failed!");
        } else {
            server.send(200, "text/plain", "OK");
            Serial.println("Update successful! Rebooting...");
            delay(1000);
            ESP.restart();
        }
    }, []() {
        noteConfigWebActivity();
        // Tämä suoritetaan uploadin aikana
        if (!server.authenticate(WEB_USERNAME, WEB_PASSWORD)) {
            return;
        }
        HTTPUpload& upload = server.upload();
        
        if (upload.status == UPLOAD_FILE_START) {
            Serial.printf("Update Start: %s (size: %u bytes)\n", 
                         upload.filename.c_str(), upload.totalSize);
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
            // Tulosta edistyminen joka 100kB välein
            static unsigned int lastProgress = 0;
            unsigned int progress = (Update.progress() * 100) / Update.size();
            if (progress / 10 != lastProgress / 10) {
                Serial.printf("Progress: %u%%\n", progress);
                lastProgress = progress;
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) {
                Serial.printf("Update Success: %u bytes\n", upload.totalSize);
            } else {
                Update.printError(Serial);
            }
        }
    });
    
    // FIRMWARE UPDATE - GET (näyttää web-sivun)
    server.on("/update", HTTP_GET, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        server.send(200, "text/html", updateHtml);
    });
    
    // Etusivu
    server.on("/", []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        server.send(200, "text/html", indexHtml);
    });
    
    // Setup-sivu
    server.on("/setup", []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        server.send(200, "text/html", setupHtml);
    });
    
    // CSS-tyylit
    server.on("/style.css", []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        server.send(200, "text/css", styleCss);
    });

    // SVG-logo
    server.on("/steam-machines.svg", []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        if (LittleFS.exists("/steam-machines.svg")) {
            File file = LittleFS.open("/steam-machines.svg", "r");
            server.streamFile(file, "image/svg+xml");
            file.close();
        } else {
            server.send(200, "image/svg+xml", 
                "<svg width='180' height='50' xmlns='http://www.w3.org/2000/svg'>"
                "<text x='10' y='35' font-family='Share Tech Mono' font-size='24' fill='#00d9ff'>BC-250</text>"
                "</svg>");
        }
    });

    // API: Bluetooth MAC-osoite
    server.on("/api/bluetooth/mac", HTTP_GET, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        String btMac = "";
        const uint8_t* addr = BP32.localBdAddress();
        if (addr != nullptr) {
            char macStr[18];
            sprintf(macStr, "%02x:%02x:%02x:%02x:%02x:%02x",
                    addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
            btMac = String(macStr);
        }
        
        StaticJsonDocument<100> doc;
        doc["macAddress"] = btMac;
        
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

    // API: BC-250 Bluetooth adapter spoof configuration
    server.on("/api/bluetooth/config", HTTP_GET, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        StaticJsonDocument<256> doc;
        doc["spoofMac"] = classicBtSpoofMac;
        doc["enabled"] = classicBtSpoofMac.length() > 0;
        doc["active"] = classicBtSpoofEnabled;
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

    server.on("/api/bluetooth/config", HTTP_POST, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        if (!server.hasArg("plain")) {
            server.send(400, "text/plain", "Body missing");
            return;
        }
        StaticJsonDocument<256> doc;
        if (deserializeJson(doc, server.arg("plain"))) {
            server.send(400, "text/plain", "Invalid JSON");
            return;
        }
        String mac = String((const char*)(doc["spoofMac"] | ""));
        mac.trim();
        if (mac.length() > 0 && !isValidBtMac(mac)) {
            server.send(400, "text/plain", "Invalid MAC. Use AA:BB:CC:DD:EE:FF");
            return;
        }
        if (!saveBluetoothSpoofMac(mac)) {
            server.send(500, "text/plain", "Failed to save Bluetooth configuration");
            return;
        }
        server.send(200, "application/json", "{\"status\":\"ok\",\"restartRequired\":true}");
    });

    // Authenticated WiFi OFF: reply first; radio shutdown is deferred in loop().
    server.on("/api/wifi/off", HTTP_POST, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        server.send(200, "application/json", "{\"status\":\"switching_to_bluetooth\",\"restore\":\"hold GPIO22 button 10 seconds\"}");
        wifiDisableRequestedAt = millis();
        wifiDisablePending = true;
        Serial.println("API: WiFi OFF requested; will persist OFF across restarts");
    });

    // API: WiFi-asetukset - GET
    server.on("/api/wifi/config", HTTP_GET, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        StaticJsonDocument<200> doc;
        doc["ssid"] = wifiSSID;
        doc["configured"] = wifiConfigured;
        doc["apMode"] = apMode;
        doc["radioEnabled"] = wifiRadioEnabled;
        
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });
    
    // API: WiFi-asetukset - POST
    server.on("/api/wifi/config", HTTP_POST, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        if (!server.hasArg("plain")) {
            server.send(400, "text/plain", "Body missing");
            return;
        }
        
        String body = server.arg("plain");
        
        StaticJsonDocument<200> doc;
        DeserializationError error = deserializeJson(doc, body);
        
        if (error) {
            server.send(400, "text/plain", "Invalid JSON");
            return;
        }
        
        const char* ssid = doc["ssid"];
        const char* password = doc["password"];
        
        if (!ssid || strlen(ssid) == 0) {
            server.send(400, "text/plain", "SSID required");
            return;
        }
        
        String passStr = password ? String(password) : "";
        
        saveWiFiConfig(String(ssid), passStr);
        server.send(200, "text/plain", "OK");
        delay(1000);
        ESP.restart();
    });
    
    // API: WiFi skannaus
    server.on("/api/wifi/scan", HTTP_GET, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        int n = WiFi.scanComplete();
        if (n == -2) {
            WiFi.scanNetworks(true);
            StaticJsonDocument<100> doc;
            doc["scanning"] = true;
            
            String response;
            serializeJson(doc, response);
            server.send(200, "application/json", response);
        } else if (n >= 0) {
            StaticJsonDocument<2000> doc;
            JsonArray networks = doc.to<JsonArray>();
            
            for (int i = 0; i < n; ++i) {
                JsonObject net = networks.createNestedObject();
                net["ssid"] = WiFi.SSID(i);
                net["rssi"] = WiFi.RSSI(i);
                net["encryption"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? 1 : 0;
            }
            
            String response;
            serializeJson(doc, response);
            WiFi.scanDelete();
            server.send(200, "application/json", response);
        }
    });

    // API: Status
    server.on("/api/status", HTTP_GET, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        bool currentMonitor = getStablePcState();
        bool currentOpto = digitalRead(OPTO_PIN);
        bool currentExtra = false;
        #if BOARD_HAS_RELAY2
        currentExtra = digitalRead(EXTRA_PIN);
        #endif
        
        StaticJsonDocument<300> doc;
        doc["pcOn"] = currentMonitor;
        doc["shutdownRequested"] = shutdownRequested;
        doc["forceShutdown"] = forceShutdown;
        doc["optoState"] = currentOpto;
        doc["extraPinState"] = currentExtra;
        doc["monitorState"] = currentMonitor;
        doc["version"] = VERSION;
        
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

    // API: Power ON
    server.on("/power/on", HTTP_POST, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        Serial.println("API: Power ON requested");
        if (getStablePcState() == LOW) {
            startPowerOn();
            server.send(200, "text/plain", "OK");
        } else {
            server.send(200, "text/plain", "Already on");
        }
    });

    // API: Power OFF (pakkosammutus)
    server.on("/power/off", HTTP_POST, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        Serial.println("API: Power OFF requested");
        if (getStablePcState() == HIGH) {
            startForceShutdown();
            server.send(200, "text/plain", "OK");
        } else {
            server.send(200, "text/plain", "Already off");
        }
    });

    // API: Force shutdown
    server.on("/power/force", HTTP_POST, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        Serial.println("API: Force shutdown requested");
        if (getStablePcState() == HIGH) {
            startForceShutdown();
            server.send(200, "text/plain", "OK");
        } else {
            server.send(200, "text/plain", "Already off");
        }
    });

    // API: Controller configuration - GET
    server.on("/api/ps5/config", HTTP_GET, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        StaticJsonDocument<1024> doc;
        doc["enabled"] = ps5Enabled;
        doc["autoConnect"] = ps5AutoConnect;
        doc["addMode"] = ps5Simple.isAddNextControllerMode();
        doc["maxControllers"] = ps5Simple.getMaxAllowedCount();
        JsonArray macs = doc.createNestedArray("macAddresses");
        for (int i = 0; i < ps5Simple.getMaxAllowedCount(); i++) {
            String mac = ps5Simple.getAllowedMac(i);
            if (mac.length() > 0) macs.add(mac);
        }
        doc["macAddress"] = ps5Simple.getAllowedMac(0); // legacy
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

        // LittleFS OTA update
        server.on("/update-fs", HTTP_POST, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
            server.sendHeader("Connection", "close");
            server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
            delay(1000);
            ESP.restart();
        }, []() {
        noteConfigWebActivity();
            if (!server.authenticate(WEB_USERNAME, WEB_PASSWORD)) {
            return;
        }
        HTTPUpload& upload = server.upload();
            
            if (upload.status == UPLOAD_FILE_START) {
                Serial.printf("LittleFS Update: %s\n", upload.filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) {  // HUOM: U_SPIFFS!
                    Update.printError(Serial);
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                    Update.printError(Serial);
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (Update.end(true)) {
                    Serial.printf("LittleFS Update Success: %u bytes\n", upload.totalSize);
                } else {
                    Update.printError(Serial);
                }
            }
        });

    // API: Controller settings - POST (enable/disable only; list is preserved)
    server.on("/api/ps5/config", HTTP_POST, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        if (!server.hasArg("plain")) {
            server.send(400, "text/plain", "Body missing");
            return;
        }
        StaticJsonDocument<300> doc;
        if (deserializeJson(doc, server.arg("plain"))) {
            server.send(400, "text/plain", "Invalid JSON");
            return;
        }
        ps5Enabled = doc["enabled"] | ps5Enabled;
        ps5AutoConnect = doc["autoConnect"] | ps5AutoConnect;
        persistPS5Config();
        server.send(200, "text/plain", "OK");
    });

    // API: Add a controller MAC manually without pairing it to ESP32.
    // This is the preferred Build #5 enrollment method: keep the controller bonded to SteamOS.
    server.on("/api/controllers/add-manual", HTTP_POST, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        if (!server.hasArg("plain")) {
            server.send(400, "text/plain", "Body missing");
            return;
        }
        StaticJsonDocument<200> doc;
        if (deserializeJson(doc, server.arg("plain"))) {
            server.send(400, "text/plain", "Invalid JSON");
            return;
        }
        String mac = doc["macAddress"] | "";
        mac.trim();
        mac.replace(":", "");
        mac.replace("-", "");
        mac.toUpperCase();
        if (mac.length() != 12) {
            server.send(400, "application/json", "{\"status\":\"invalid\",\"message\":\"Use MAC format AA:BB:CC:DD:EE:FF\"}");
            return;
        }
        for (int i = 0; i < 12; i++) {
            char c = mac.charAt(i);
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
                server.send(400, "application/json", "{\"status\":\"invalid\",\"message\":\"MAC contains non-hex characters\"}");
                return;
            }
        }
        if (ps5Simple.getAllowedCount() >= ps5Simple.getMaxAllowedCount() && !ps5Simple.isAllowedMac(mac)) {
            server.send(409, "application/json", "{\"status\":\"full\",\"message\":\"Controller list is full\"}");
            return;
        }
        bool added = addPS5ControllerMac(mac);
        server.send(added ? 200 : 400, "application/json",
                    added ? "{\"status\":\"ok\"}" : "{\"status\":\"error\"}");
    });

    // API: Start one-shot enrollment of the next controller
    server.on("/api/controllers/add-next", HTTP_POST, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        if (ps5Simple.getAllowedCount() >= ps5Simple.getMaxAllowedCount()) {
            server.send(409, "application/json", "{\"status\":\"full\",\"message\":\"Controller list is full\"}");
            return;
        }
        ps5Simple.beginAddNextController();
        server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Connect the new controller now\"}");
    });

    // API: Remove one controller by MAC
    server.on("/api/controllers/remove", HTTP_POST, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        if (!server.hasArg("plain")) {
            server.send(400, "text/plain", "Body missing");
            return;
        }
        StaticJsonDocument<200> doc;
        if (deserializeJson(doc, server.arg("plain"))) {
            server.send(400, "text/plain", "Invalid JSON");
            return;
        }
        const char* mac = doc["macAddress"];
        if (!mac || strlen(mac) == 0) {
            server.send(400, "text/plain", "MAC required");
            return;
        }
        bool removed = removePS5ControllerMac(String(mac));
        server.send(removed ? 200 : 404, "application/json",
                    removed ? "{\"status\":\"ok\"}" : "{\"status\":\"not_found\"}");
    });

    // API: Clear all authorized controllers
    server.on("/api/controllers/clear", HTTP_POST, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        clearPS5ControllerMacs();
        server.send(200, "application/json", "{\"status\":\"ok\"}");
    });

    // Legacy unlock endpoint now clears the complete list
    server.on("/api/ps5/unlock", HTTP_POST, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        clearPS5ControllerMacs();
        server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"All controller MACs removed\"}");
    });

    // API: Controller status
    server.on("/api/ps5/status", HTTP_GET, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        StaticJsonDocument<300> doc;
        doc["state"] = !ps5Enabled ? "disabled" : (ps5Simple.isConnected() ? "connected" : "disconnected");
        doc["btAllowed"] = !getStablePcState();
        doc["connectedMac"] = ps5Simple.getConnectedMac();
        doc["addMode"] = ps5Simple.isAddNextControllerMode();
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

    server.on("/api/ps5/connected-mac", HTTP_GET, []() {
        noteConfigWebActivity();
        if (!requireWebAuth()) return;
        StaticJsonDocument<200> doc;
        String controllerMac = ps5Simple.getConnectedMac();
        doc["connected"] = ps5Simple.isConnected();
        doc["macAddress"] = controllerMac;
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
    });

    // 404 - Not found
    server.onNotFound([]() {
        noteConfigWebActivity();
        server.send(404, "text/plain", "404: Not Found");
    });

    if (wifiRadioEnabled) server.begin();
    Serial.println(wifiRadioEnabled ? "Web server started!" : "Web server routes registered; WiFi is OFF");
    Serial.print("  - IP: ");
    Serial.println(WiFi.localIP());
}

#endif // WEB_SERVER_H