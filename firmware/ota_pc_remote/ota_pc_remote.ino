#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include "security_config.h"
#include <Update.h>
#include "LittleFS.h"
#include <Bluepad32.h>
#include <ArduinoJson.h>
#include <esp_mac.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <Preferences.h>
#include <btstack.h>
#include <esp32-hal-bt.h>

#include "version.h"
#include "pins.h"
#include "ps5_simple.h"

// IMPORTANT: Include pc_control.h first so PowerState is defined
#include "pc_control.h"

// ================ BUILD #7 EARLY BLUETOOTH MAC ================
// Bluepad32 starts BTstack before Arduino setup(). A DualSense paired to SteamOS
// reconnects to the host BD_ADDR when PS is pressed, so the spoofed address must
// already be installed before BTstack initializes. The web/LittleFS configuration
// is mirrored to NVS; this constructor reads NVS before app_main()/BTstack.
static volatile bool gEarlyBtSpoofApplied = false;
static volatile esp_err_t gEarlyBtSpoofResult = ESP_FAIL;
static uint8_t gEarlyBtTarget[6] = {0};

static void subtractMacEarly(uint8_t mac[6], uint8_t value) {
    int i = 5;
    uint16_t borrow = value;
    while (i >= 0 && borrow > 0) {
        uint16_t part = borrow & 0xFF;
        if (mac[i] >= part) {
            mac[i] = (uint8_t)(mac[i] - part);
            borrow >>= 8;
        } else {
            mac[i] = (uint8_t)(0x100 + mac[i] - part);
            borrow = (borrow >> 8) + 1;
        }
        i--;
    }
}

__attribute__((constructor(101))) static void applyBtSpoofBeforeBluepad32() {
    // No Serial here: constructors run before Arduino setup/Serial.
    esp_err_t initResult = nvs_flash_init();
    if (initResult != ESP_OK && initResult != ESP_ERR_NVS_NO_FREE_PAGES &&
        initResult != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        gEarlyBtSpoofResult = initResult;
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open("bc250bt", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        gEarlyBtSpoofResult = err;
        return;
    }

    size_t len = sizeof(gEarlyBtTarget);
    err = nvs_get_blob(handle, "target", gEarlyBtTarget, &len);
    nvs_close(handle);
    if (err != ESP_OK || len != 6) {
        gEarlyBtSpoofResult = err;
        return;
    }

    uint8_t base[6];
    memcpy(base, gEarlyBtTarget, sizeof(base));
    // On this ESP32 / Arduino core, ESP_MAC_BT = base MAC + 2.
    subtractMacEarly(base, 2);
    gEarlyBtSpoofResult = esp_base_mac_addr_set(base);
    gEarlyBtSpoofApplied = (gEarlyBtSpoofResult == ESP_OK);
}

// Global variables
WebServer server(80);

bool pcIsOn = false;
bool shutdownRequested = false;
bool forceShutdown = false;
unsigned long forceShutdownStartTime = 0;
const unsigned long forceShutdownDuration = 5000;

// WiFi variables
String wifiSSID = "";
String wifiPassword = "";
bool wifiConfigured = false;
bool apMode = false;
bool wifiRadioEnabled = false;
// Exclusive radio mode: normal boot = Bluetooth; configuration = WiFi AP.
// The mode is stored in NVS, and each transition uses ESP.restart().
bool configApMode = false;
unsigned long configLastActivityMs = 0;
const unsigned long CONFIG_IDLE_TIMEOUT_MS = 15UL * 60UL * 1000UL;
void noteConfigWebActivity() {
    if (configApMode) configLastActivityMs = millis();
}
void persistWifiEnabled(bool enabled);
void requestRadioMode(bool ap) {
    if (pcIsOn || powerState != POWER_IDLE || digitalRead(PC_MONITOR_PIN) != LOW) {
        Serial.println("RADIO MODE: refused while PC is on or power transition active");
        return;
    }
    Serial.printf("RADIO MODE: switching to %s via reboot\n", ap ? "WIFI_AP" : "BLUETOOTH");
    digitalWrite(OPTO_PIN, LOW); // X1: relay1 off; GPIO17 untouched.
    persistWifiEnabled(ap);
    delay(250);
    ESP.restart();
}

bool wifiDisablePending = false;
unsigned long wifiDisableRequestedAt = 0;
Preferences wifiModePreferences;

void persistWifiEnabled(bool enabled) {
    wifiModePreferences.begin("bc250wifi", false);
    wifiModePreferences.putBool("enabled", enabled);
    wifiModePreferences.end();
}

void enableWifiAccessPoint() {
    requestRadioMode(true);
}

void disableWifiRadio() {
    // WiFi mode is never switched in-place: BTstack is reinitialized on reboot.
    requestRadioMode(false);
}

// Bluetooth Classic spoof configuration. This must contain the BC-250 Bluetooth adapter MAC, not a controller MAC.
String classicBtSpoofMac = "";
bool classicBtSpoofEnabled = false;

// Controller variables
String ps5MacAddress = "";
bool ps5Enabled = false;
bool ps5AutoConnect = false;
unsigned long lastPS5ConnectionAttempt = 0;

// Optimized intervals
unsigned long lastPinRead = 0;
const unsigned long pinReadInterval = 50;

unsigned long lastServerHandle = 0;
const unsigned long serverHandleInterval = 20;

unsigned long lastPcStateHandle = 0;
const unsigned long pcStateHandleInterval = 50;

unsigned long lastButtonDebounce = 0;
const unsigned long debounceDelay = 50;

// Cached pin states
bool cachedButtonState = HIGH;
bool lastStableButtonState = HIGH;
bool buttonPressed = false;

// Filtered PC state
bool filteredPcState = false;
unsigned long lastPcChangeTime = 0;
const unsigned long pcStableDelay = 100;

// Power state machine variables
PowerState powerState = POWER_IDLE;
unsigned long powerStateStartTime = 0;

// Controller handler
PS5Simple ps5Simple;

// ================ PROTOTYPES ================
bool getStablePcState();
void startPowerOn();
void startForceShutdown();
void startNormalShutdown();
void savePS5Config(bool enabled, String mac, bool autoConnect);
bool addPS5ControllerMac(String mac);
bool removePS5ControllerMac(String mac);
void clearPS5ControllerMacs();
void persistPS5Config();
bool startMDNS();
void loadBluetoothConfig();
bool saveBluetoothSpoofMac(String mac);
void applyClassicBluetoothSpoof();
bool syncEarlyBluetoothSpoofNvs(String mac, bool* changed = nullptr);

// ================ XBOX PASSIVE BLE ADVERTISING EXPERIMENT ================
// 1 = trigger from a recognized BLE advertising report, without initiating
// a pairing operation in THIS handler. 0 = legacy SM pairing wake.
// NOTE: Bluepad32 may independently initiate BLE connections; this experiment
// does not make the entire BTstack/Bluepad32 system passive.
#define XBOX_ADV_WAKE_EXPERIMENT 1
static btstack_packet_callback_registration_t xboxAdvEventRegistration;
// On the first accepted wake, stop Bluepad32 from accepting NEW controllers.
// This does not disable its existing connections; PC-ON power-down follows.
static bool xboxWakeRadioQuiesceRequested = false;
static bool bluetoothStoppedForPc = false;
static unsigned long pcOnBtSince = 0;
static void handlePcOnBluetoothShutdown() {
    if (!pcIsOn) { pcOnBtSince = 0; return; }
    if (bluetoothStoppedForPc) return;
    if (pcOnBtSince == 0) {
        pcOnBtSince = millis();
        BP32.enableNewBluetoothConnections(false);
        Serial.println("BT HANDOFF: PC ON; blocking new Bluepad32 connections");
    }
    // Allow a short grace period for controller events already in flight.
    if ((unsigned long)(millis() - pcOnBtSince) < 1500) return;
    Serial.println("BT HANDOFF: stopping ESP32 Bluetooth controller for SteamOS");
    bool ok = btStop();
    bluetoothStoppedForPc = true;
    Serial.printf("BT HANDOFF: btStop result=%s; no further BP32.update until restart\n", ok ? "OK" : "FAILED");
}
static unsigned long lastAdvLogMs = 0;
static void xboxAdvEventHandler(uint8_t packetType, uint16_t channel, uint8_t* packet, uint16_t size) {
    (void)channel;
    if (packetType != HCI_EVENT_PACKET || packet == nullptr || size < 5) return;
    // HCI LE Meta Event 0x3E; LE Advertising Report subevent 0x02.
    if (packet[0] != 0x3E || packet[2] != 0x02) return;
    uint16_t offset = 4;
    const uint8_t count = packet[3];
    for (uint8_t i = 0; i < count; ++i) {
        if (offset + 10 > size) return;
        const uint8_t eventType = packet[offset];
        const uint8_t addressType = packet[offset + 1];
        const uint8_t* a = packet + offset + 2;
        const uint8_t dataLen = packet[offset + 8];
        if (offset + 10 + dataLen > size) return;
        const int8_t rssi = (int8_t)packet[offset + 9 + dataLen];
        char mac[18];
        // HCI advertising reports carry the 48-bit address least-significant byte first.
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", a[5],a[4],a[3],a[2],a[1],a[0]);
        const bool allowed = ps5Simple.isAllowedMac(String(mac));
        if (allowed && !pcIsOn && powerState == POWER_IDLE) {
            Serial.printf("XBOX ADV: authorized MAC=%s addrType=%u event=%u rssi=%d dataLen=%u\n",
                          mac, addressType, eventType, rssi, dataLen);
#if XBOX_ADV_WAKE_EXPERIMENT
            if (!xboxWakeRadioQuiesceRequested) {
                xboxWakeRadioQuiesceRequested = true;
                BP32.enableNewBluetoothConnections(false);
                Serial.println("XBOX ADV: new Bluepad32 connections disabled after wake");
            }
            ps5Simple.onSecurityManagerReconnectAttempt(String(mac), "BLE_ADVERTISEMENT");
#endif
        } else if ((unsigned long)(millis() - lastAdvLogMs) >= 10000 && !pcIsOn) {
            lastAdvLogMs = millis();
            Serial.printf("BLE ADV SAMPLE: MAC=%s addrType=%u event=%u rssi=%d dataLen=%u allowed=%u\n",
                          mac, addressType, eventType, rssi, dataLen, allowed);
        }
        offset += 10 + dataLen;
    }
}
static void registerXboxAdvEventHandler() {
    xboxAdvEventRegistration.callback = &xboxAdvEventHandler;
    hci_add_event_handler(&xboxAdvEventRegistration);
    Serial.println("XBOX ADV: HCI advertising report observer registered (experimental)");
}

// ================ BUILD #5 SECURITY MANAGER WAKE ================
// Xbox controllers paired with SteamOS can reach ESP32 over BLE but fail authentication
// because the ESP32 does not own SteamOS bonding keys. Bluepad32 logs that failure as
// SM_EVENT_PAIRING_COMPLETE. Build #5 listens directly to BTstack Security Manager events,
// extracts the peer address, and uses the address only as a wake trigger.
static btstack_packet_callback_registration_t build6ClassicEventRegistration;

static void build6ClassicEventHandler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || packet == nullptr) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_CONNECTION_REQUEST) return;

    bd_addr_t addr;
    hci_event_connection_request_get_bd_addr(packet, addr);
    const uint8_t linkType = hci_event_connection_request_get_link_type(packet);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    Serial.printf("BT WAKE: Classic HCI connection request from %s, link_type=%u\n", macStr, linkType);

    // HID controllers use an ACL link (link_type=1). Ignore SCO/eSCO audio requests.
    if (linkType != 1) return;
    ps5Simple.onClassicReconnectAttempt(String(macStr));
}

static void registerBuild6ClassicWakeHandler() {
    build6ClassicEventRegistration.callback = &build6ClassicEventHandler;
    hci_add_event_handler(&build6ClassicEventRegistration);
    Serial.println("BT WAKE: Classic HCI wake handler registered (DualSense PS reconnect)");
}

static btstack_packet_callback_registration_t build5SmEventRegistration;

static void build5SmEventHandler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || packet == nullptr) return;

    const uint8_t eventType = hci_event_packet_get_type(packet);
    bd_addr_t addr;

    if (eventType == SM_EVENT_PAIRING_STARTED) {
        sm_event_pairing_started_get_address(packet, addr);
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
        Serial.print("BT WAKE: SM pairing started from ");
        Serial.println(macStr);
        #if !XBOX_ADV_WAKE_EXPERIMENT
        ps5Simple.onSecurityManagerReconnectAttempt(String(macStr), "PAIRING_STARTED");
#else
        Serial.println("XBOX ADV: SM pairing wake suppressed (diagnostic mode)");
#endif
        return;
    }

    if (eventType == SM_EVENT_PAIRING_COMPLETE) {
        sm_event_pairing_complete_get_address(packet, addr);
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
        const uint8_t status = sm_event_pairing_complete_get_status(packet);
        Serial.printf("BT WAKE: SM pairing complete from %s, status=%u, t=%lu ms (diagnostic only)\n", macStr, status, millis());
    }
}

static void registerBuild5SecurityManagerWakeHandler() {
    build5SmEventRegistration.callback = &build5SmEventHandler;
    sm_add_event_handler(&build5SmEventRegistration);
    Serial.println("BT WAKE: Security Manager BLE wake handler registered");
}

// ================ MANUAL BLUETOOTH BOND RECOVERY ================
// Type ERASE_BONDS in Serial Monitor (newline, 115200) ONLY while BC-250 is OFF.
// This erases Bluepad32's saved Bluetooth bonding keys, not LittleFS MAC whitelist,
// Wi-Fi settings or the spoofed adapter MAC. Re-pair Xbox afterward if necessary.
static String recoverySerialLine;
static void pollBluetoothRecoverySerial() {
    while (Serial.available()) {
        const char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            recoverySerialLine.trim();
            if (recoverySerialLine == "ERASE_BONDS") {
                if (pcIsOn || powerState != POWER_IDLE || filteredPcState) {
                    Serial.println("BLE RECOVERY: REFUSED - PC must be OFF and power state IDLE");
                } else {
                    Serial.println("BLE RECOVERY: erasing Bluepad32 bonding keys; saved MAC whitelist preserved");
                    BP32.forgetBluetoothKeys();
                    Serial.println("BLE RECOVERY: erase requested; rebooting in 2s");
                    Serial.flush();
                    delay(2000);
                    ESP.restart();
                }
            } else if (recoverySerialLine.length()) {
                Serial.println("BLE RECOVERY: available command: ERASE_BONDS (PC OFF only)");
            }
            recoverySerialLine = "";
        } else if (recoverySerialLine.length() < 40 && c >= 32 && c <= 126) {
            recoverySerialLine += c;
        }
    }
}

// ================ CALLBACKS ================
void onConnectedGamepad(GamepadPtr gp) {
    Serial.println("=== NEW CONTROLLER DETECTED ===");
    if (gp != nullptr) {
        ps5Simple.onControllerConnected(gp);
    }
}

void onDisconnectedGamepad(GamepadPtr gp) {
    Serial.println("=== CONTROLLER DISCONNECTED ===");
    ps5Simple.onControllerDisconnected(gp);
}

#include "web_server.h"

// ================ BLUETOOTH SPOOF CONFIGURATION ================
String normalizeBtMac(String mac) {
    mac.trim();
    mac.toUpperCase();
    mac.replace("-", ":");
    return mac;
}

bool isHexCharBt(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

bool isValidBtMac(const String& input) {
    String mac = normalizeBtMac(input);
    if (mac.length() != 17) return false;
    for (int i = 0; i < 17; i++) {
        if ((i + 1) % 3 == 0) {
            if (mac.charAt(i) != ':') return false;
        } else if (!isHexCharBt(mac.charAt(i))) {
            return false;
        }
    }
    unsigned int b[6];
    if (sscanf(mac.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
    bool anyNonZero = false;
    for (int i = 0; i < 6; i++) if (b[i] != 0) anyNonZero = true;
    return anyNonZero && ((b[0] & 0x01) == 0);
}

void loadBluetoothConfig() {
    classicBtSpoofMac = "";
    if (!LittleFS.exists("/bluetooth_config.json")) {
        Serial.println("BLUETOOTH: no spoof configuration - using factory Bluetooth MAC");
        return;
    }
    File file = LittleFS.open("/bluetooth_config.json", "r");
    if (!file) {
        Serial.println("BLUETOOTH: ERROR - cannot open /bluetooth_config.json");
        return;
    }
    StaticJsonDocument<192> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    if (error) {
        Serial.print("BLUETOOTH: ERROR - invalid config JSON: ");
        Serial.println(error.c_str());
        return;
    }
    String mac = normalizeBtMac(String((const char*)(doc["spoofMac"] | "")));
    if (mac.length() > 0 && !isValidBtMac(mac)) {
        Serial.println("BLUETOOTH: WARNING - stored spoof MAC is invalid; ignoring it");
        return;
    }
    classicBtSpoofMac = mac;
    if (classicBtSpoofMac.length() > 0) {
        Serial.print("BLUETOOTH: BC-250 adapter spoof MAC loaded: ");
        Serial.println(classicBtSpoofMac);
    } else {
        Serial.println("BLUETOOTH: spoofing disabled");
    }
}

bool syncEarlyBluetoothSpoofNvs(String mac, bool* changed) {
    if (changed) *changed = false;
    mac = normalizeBtMac(mac);

    esp_err_t initResult = nvs_flash_init();
    if (initResult != ESP_OK && initResult != ESP_ERR_NVS_NO_FREE_PAGES &&
        initResult != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        Serial.printf("BLUETOOTH EARLY: NVS init failed, esp_err=%d\n", (int)initResult);
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open("bc250bt", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        Serial.printf("BLUETOOTH EARLY: NVS open failed, esp_err=%d\n", (int)err);
        return false;
    }

    uint8_t existing[6] = {0};
    size_t existingLen = sizeof(existing);
    bool hadExisting = (nvs_get_blob(handle, "target", existing, &existingLen) == ESP_OK && existingLen == 6);

    if (mac.length() == 0) {
        if (hadExisting) {
            nvs_erase_key(handle, "target");
            nvs_commit(handle);
            if (changed) *changed = true;
        }
        nvs_close(handle);
        return true;
    }

    unsigned int b[6];
    if (sscanf(mac.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        nvs_close(handle);
        return false;
    }
    uint8_t target[6];
    for (int i = 0; i < 6; i++) target[i] = (uint8_t)b[i];

    bool different = !hadExisting || memcmp(existing, target, sizeof(target)) != 0;
    if (different) {
        err = nvs_set_blob(handle, "target", target, sizeof(target));
        if (err == ESP_OK) err = nvs_commit(handle);
        if (err != ESP_OK) {
            Serial.printf("BLUETOOTH EARLY: NVS write failed, esp_err=%d\n", (int)err);
            nvs_close(handle);
            return false;
        }
        if (changed) *changed = true;
    }
    nvs_close(handle);
    return true;
}

bool saveBluetoothSpoofMac(String mac) {
    mac = normalizeBtMac(mac);
    if (mac.length() > 0 && !isValidBtMac(mac)) return false;
    StaticJsonDocument<192> doc;
    doc["spoofMac"] = mac;
    File file = LittleFS.open("/bluetooth_config.json", "w");
    if (!file) return false;
    size_t written = serializeJson(doc, file);
    file.close();
    if (written == 0) return false;
    classicBtSpoofMac = mac;
    bool earlyChanged = false;
    if (!syncEarlyBluetoothSpoofNvs(mac, &earlyChanged)) {
        Serial.println("BLUETOOTH: WARNING - LittleFS saved but early NVS mirror failed");
    }
    Serial.print("BLUETOOTH: spoof MAC configuration saved: ");
    Serial.println(mac.length() ? mac : "DISABLED");
    if (earlyChanged) Serial.println("BLUETOOTH EARLY: address updated - reboot required to apply before BTstack");
    return true;
}

// Bluepad32 4.1.0 uses Arduino-ESP32 / ESP-IDF where esp_iface_mac_addr_set()
// is not available. On the original ESP32, the Bluetooth MAC is derived from
// the base MAC with an offset of +2. Therefore we set base = requested BT MAC - 2
// before Bluepad32 initializes the Bluetooth controller.
static void subtractFromMac(uint8_t mac[6], uint8_t value) {
    int i = 5;
    uint16_t borrow = value;
    while (i >= 0 && borrow > 0) {
        uint16_t part = borrow & 0xFF;
        if (mac[i] >= part) {
            mac[i] = (uint8_t)(mac[i] - part);
            borrow >>= 8;
        } else {
            mac[i] = (uint8_t)(0x100 + mac[i] - part);
            borrow = (borrow >> 8) + 1;
        }
        i--;
    }
}

static void printMacAddress(const uint8_t mac[6]) {
    Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void applyClassicBluetoothSpoof() {
    classicBtSpoofEnabled = false;

    if (classicBtSpoofMac.length() == 0) {
        Serial.println("BLUETOOTH: using factory Bluetooth MAC");
        return;
    }

    unsigned int b[6];
    if (sscanf(classicBtSpoofMac.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        Serial.println("BLUETOOTH: ERROR - cannot parse spoof MAC");
        return;
    }

    uint8_t targetBtMac[6];
    for (int i = 0; i < 6; i++) targetBtMac[i] = (uint8_t)b[i];

    // ESP32 derives ESP_MAC_BT as base MAC + 2.
    uint8_t customBaseMac[6];
    memcpy(customBaseMac, targetBtMac, sizeof(customBaseMac));
    subtractFromMac(customBaseMac, 2);

    Serial.print("BLUETOOTH: target BC-250 BT MAC: ");
    printMacAddress(targetBtMac);
    Serial.println();
    Serial.print("BLUETOOTH: calculated ESP32 base MAC: ");
    printMacAddress(customBaseMac);
    Serial.println();

    esp_err_t result = esp_base_mac_addr_set(customBaseMac);
    if (result != ESP_OK) {
        Serial.printf("BLUETOOTH: ERROR - base MAC change failed, esp_err=%d\n", (int)result);
        return;
    }

    uint8_t actualBtMac[6] = {0};
    esp_err_t readResult = esp_read_mac(actualBtMac, ESP_MAC_BT);
    if (readResult != ESP_OK) {
        Serial.printf("BLUETOOTH: ERROR - cannot read resulting BT MAC, esp_err=%d\n", (int)readResult);
        return;
    }

    Serial.print("BLUETOOTH: actual ESP32 BT MAC: ");
    printMacAddress(actualBtMac);
    Serial.println();

    bool matches = memcmp(actualBtMac, targetBtMac, sizeof(targetBtMac)) == 0;
    if (matches) {
        classicBtSpoofEnabled = true;
        Serial.println("BLUETOOTH: spoof verification: OK");
    } else {
        Serial.println("BLUETOOTH: ERROR - spoof verification FAILED");
    }
}

// ================ WIFI CONFIGURATION ================

void saveWiFiConfig(String ssid, String pass) {
    Serial.println("WIFI: Saving configuration...");
    Serial.print("WIFI: SSID = ");
    Serial.println(ssid);

    if (!LittleFS.begin(true)) {
        Serial.println("WIFI: ERROR - LittleFS mount failed while saving");
        return;
    }

    File file = LittleFS.open("/wifi_config.json", "w");
    if (!file) {
        Serial.println("WIFI: ERROR - cannot open /wifi_config.json for writing");
        return;
    }

    StaticJsonDocument<256> doc;
    doc["ssid"] = ssid;
    doc["password"] = pass;

    size_t written = serializeJson(doc, file);
    file.flush();
    file.close();

    if (written == 0) {
        Serial.println("WIFI: ERROR - nothing was written to config file");
        return;
    }

    wifiSSID = ssid;
    wifiPassword = pass;
    wifiConfigured = (wifiSSID.length() > 0);
    apMode = !wifiConfigured;

    Serial.printf("WIFI: configuration saved successfully (%u bytes)\n", (unsigned)written);
}

void loadWiFiConfig() {
    Serial.println("WIFI: Loading /wifi_config.json...");

    if (!LittleFS.begin(true)) {
        Serial.println("WIFI: ERROR - LittleFS mount failed");
        wifiConfigured = false;
        apMode = true;
        return;
    }

    if (!LittleFS.exists("/wifi_config.json")) {
        Serial.println("WIFI: config file not found - using AP mode");
        wifiConfigured = false;
        apMode = true;
        return;
    }

    Serial.println("WIFI: config file found");
    File file = LittleFS.open("/wifi_config.json", "r");
    if (!file) {
        Serial.println("WIFI: ERROR - cannot open config file");
        wifiConfigured = false;
        apMode = true;
        return;
    }

    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        Serial.print("WIFI: ERROR - invalid JSON: ");
        Serial.println(error.c_str());
        wifiConfigured = false;
        apMode = true;
        return;
    }

    wifiSSID = doc["ssid"] | "";
    wifiPassword = doc["password"] | "";
    wifiConfigured = (wifiSSID.length() > 0);
    apMode = !wifiConfigured;

    Serial.print("WIFI: SSID = ");
    Serial.println(wifiSSID);
    Serial.print("WIFI: password = ");
    if (wifiPassword.length() == 0) {
        Serial.println("<empty>");
    } else {
        for (unsigned int i = 0; i < wifiPassword.length(); i++) Serial.print('*');
        Serial.println();
    }
    Serial.print("WIFI: configured = ");
    Serial.println(wifiConfigured ? "YES" : "NO");
}

bool connectToWiFi() {
    loadWiFiConfig();

    if (!wifiConfigured || wifiSSID.length() == 0) {
        Serial.println("WIFI: no valid STA configuration - starting AP");
        apMode = true;
        WiFi.mode(WIFI_AP);
        bool ok = WiFi.softAP(AP_SSID, AP_PASSWORD);
        Serial.println(ok ? "WIFI: AP started" : "WIFI: ERROR - AP start failed");
        return ok;
    }

    Serial.print("WIFI: connecting to ");
    Serial.println(wifiSSID);

    WiFi.mode(WIFI_STA);
    const char* hostname = "bc250";
    WiFi.setHostname(hostname);
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

    // Give association + DHCP enough time. The old build waited only ~2 seconds.
    const unsigned long timeoutMs = 15000;
    unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < timeoutMs) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        apMode = false;
        Serial.println("WIFI: CONNECTED!");
        Serial.print("WIFI: IP = ");
        Serial.println(WiFi.localIP());
        Serial.print("WIFI: RSSI = ");
        Serial.print(WiFi.RSSI());
        Serial.println(" dBm");
        return true;
    }

    Serial.print("WIFI: connection failed, status = ");
    Serial.println((int)WiFi.status());
    Serial.println("WIFI: falling back to AP mode");

    WiFi.disconnect();
    delay(100);
    apMode = true;
    WiFi.mode(WIFI_AP);
    bool ok = WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.println(ok ? "WIFI: AP started" : "WIFI: ERROR - AP start failed");
    return false;
}

bool startMDNS() {
    if (apMode || WiFi.status() != WL_CONNECTED) {
        Serial.println("mDNS: skipped - ESP32 is not connected to the home WiFi network");
        return false;
    }

    Serial.println("mDNS: starting hostname bc250.local...");
    if (!MDNS.begin("bc250")) {
        Serial.println("mDNS: ERROR - failed to start");
        return false;
    }

    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS: started successfully");
    Serial.println("Web interface: http://bc250.local");
    return true;
}

// ================ PC STATE FILTERING ================



// ================ CONTROLLER CONFIGURATION ================

void persistPS5Config() {
    File file = LittleFS.open("/ps5_config.json", "w");
    if (!file) {
        Serial.println("CONTROLLER: failed to open /ps5_config.json for writing");
        return;
    }

    StaticJsonDocument<1024> doc;
    doc["enabled"] = ps5Enabled;
    doc["autoConnect"] = ps5AutoConnect;
    JsonArray macs = doc.createNestedArray("macAddresses");

    ps5MacAddress = ""; // legacy: first MAC for old status clients
    for (int i = 0; i < ps5Simple.getMaxAllowedCount(); i++) {
        String mac = ps5Simple.getAllowedMac(i);
        if (mac.length() > 0) {
            macs.add(mac);
            if (ps5MacAddress.length() == 0) ps5MacAddress = mac;
        }
    }
    doc["macAddress"] = ps5MacAddress; // backward compatibility

    serializeJson(doc, file);
    file.close();
}

bool addPS5ControllerMac(String mac) {
    if (!ps5Simple.addAllowedMac(mac)) return false;
    persistPS5Config();
    return true;
}

bool removePS5ControllerMac(String mac) {
    if (!ps5Simple.removeAllowedMac(mac)) return false;
    persistPS5Config();
    return true;
}

void clearPS5ControllerMacs() {
    ps5Simple.clearAllowedMacs();
    persistPS5Config();
}

// Legacy compatibility: setting a non-empty MAC replaces the list with that MAC.
// The new web UI uses add/remove endpoints instead.
void savePS5Config(bool enabled, String mac, bool autoConnect) {
    ps5Enabled = enabled;
    ps5AutoConnect = autoConnect;

    if (mac.length() > 0 && mac != "00:00:00:00:00:00") {
        ps5Simple.clearAllowedMacs();
        ps5Simple.addAllowedMac(mac);
    }
    persistPS5Config();
}

void loadPS5Config() {
    ps5Simple.clearAllowedMacs();

    if (!LittleFS.exists("/ps5_config.json")) {
        ps5Enabled = true;
        ps5MacAddress = "";
        ps5AutoConnect = false;
        Serial.println("CONTROLLER: no config - first controller will be saved automatically");
        return;
    }

    File file = LittleFS.open("/ps5_config.json", "r");
    if (!file) return;

    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        ps5Enabled = true;
        ps5MacAddress = "";
        ps5AutoConnect = false;
        Serial.println("CONTROLLER: invalid config - first controller will be saved automatically");
        return;
    }

    ps5Enabled = doc["enabled"] | true;
    ps5AutoConnect = doc["autoConnect"] | false;

    // New format: macAddresses[]
    if (doc["macAddresses"].is<JsonArray>()) {
        for (JsonVariant v : doc["macAddresses"].as<JsonArray>()) {
            const char* mac = v.as<const char*>();
            if (mac) ps5Simple.addAllowedMac(String(mac));
        }
    } else {
        // Automatic migration from the original one-MAC format.
        String legacyMac = doc["macAddress"] | "";
        if (legacyMac.length() > 0) ps5Simple.addAllowedMac(legacyMac);
    }

    ps5MacAddress = ps5Simple.getAllowedMac(0);

    Serial.print("CONTROLLER: config loaded - authorized controllers: ");
    Serial.println(ps5Simple.getAllowedCount());
    for (int i = 0; i < ps5Simple.getMaxAllowedCount(); i++) {
        String mac = ps5Simple.getAllowedMac(i);
        if (mac.length() > 0) {
            Serial.printf("  Slot %d: %s\n", i + 1, mac.c_str());
        }
    }

    // If old format was loaded, rewrite it once in the new format.
    persistPS5Config();
}

// ================ SETUP ================

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n=== BC-250 STARTING ===");
    
    initPins();
    Serial.println("Pins initialized");
    
    Serial.print("PC_MONITOR_PIN (4): ");
    Serial.println(digitalRead(PC_MONITOR_PIN) ? "HIGH" : "LOW");
    Serial.print("OPTO_PIN (16): ");
    Serial.println(digitalRead(OPTO_PIN) ? "HIGH" : "LOW");
    #if BOARD_HAS_RELAY2
    Serial.print("EXTRA_PIN (17): ");
    Serial.println(digitalRead(EXTRA_PIN) ? "HIGH" : "LOW");
    #else
    Serial.println("Board X1: GPIO17 untouched (no relay2)");
    #endif
    
    filteredPcState = digitalRead(PC_MONITOR_PIN);
    pcIsOn = filteredPcState;
    
    wifiModePreferences.begin("bc250wifi", true);
    configApMode = wifiModePreferences.getBool("enabled", false);
    wifiModePreferences.end();
    // Safety: PC on always suppresses configuration mode.
    if (pcIsOn && configApMode) {
        Serial.println("RADIO MODE: PC ON at boot; restoring Bluetooth default");
        persistWifiEnabled(false);
        configApMode = false;
    }
    wifiRadioEnabled = configApMode;
    if (configApMode) {
        // Bluepad32/BTstack may already have started before setup(). Stop its
        // controller and never call BP32.update() or BP32.setup() in AP mode.
        Serial.println("RADIO MODE: WIFI_AP; stopping Bluetooth before WiFi start");
        bool stopped = btStop();
        bluetoothStoppedForPc = true;
        Serial.printf("RADIO MODE: btStop result=%s\n", stopped ? "OK" : "FAILED");
        if (!stopped) {
            Serial.println("RADIO MODE: Bluetooth stop failed; returning to safe default");
            persistWifiEnabled(false);
            delay(500);
            ESP.restart();
        }
        WiFi.mode(WIFI_AP);
        apMode = WiFi.softAP(AP_SSID, AP_PASSWORD);
        if (!apMode) {
            Serial.println("RADIO MODE: AP start failed; returning to Bluetooth");
            persistWifiEnabled(false);
            delay(500);
            ESP.restart();
        }
        configLastActivityMs = millis();
        Serial.print("WIFI: configuration AP at http://");
        Serial.println(WiFi.softAPIP());
    } else {
        Serial.println("RADIO MODE: BLUETOOTH (default); WiFi OFF");
        WiFi.mode(WIFI_OFF);
        apMode = false;
    }

    Serial.println("Mounting LittleFS...");
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed!");
    } else {
        Serial.println("LittleFS mounted");
    }

    Serial.println("Loading Bluetooth configuration...");
    loadBluetoothConfig();

    // Mirror legacy LittleFS configuration into NVS used by the pre-BTstack constructor.
    // On the first Build #7 boot this may require one automatic restart.
    bool earlyConfigChanged = false;
    if (!syncEarlyBluetoothSpoofNvs(classicBtSpoofMac, &earlyConfigChanged)) {
        Serial.println("BLUETOOTH EARLY: failed to synchronize spoof configuration to NVS");
    }

    Serial.print("BLUETOOTH EARLY: pre-BTstack spoof on this boot: ");
    Serial.println(gEarlyBtSpoofApplied ? "YES" : "NO");
    if (gEarlyBtSpoofApplied) {
        Serial.print("BLUETOOTH EARLY: target used before BTstack: ");
        printMacAddress(gEarlyBtTarget);
        Serial.println();
    }

    if (earlyConfigChanged) {
        Serial.println("BLUETOOTH EARLY: configuration migrated/changed - restarting once so BTstack starts with the spoofed BD_ADDR...");
        delay(1200);
        ESP.restart();
    }

    // Late verification only. The address that matters to DualSense is the one shown by
    // the earlier 'BTstack up and running at ...' banner.
    applyClassicBluetoothSpoof();

    // Load the authorized MAC list before registering wake handlers.
    Serial.println("Loading controller config...");
    loadPS5Config();

    if (!configApMode) {
        Serial.println("Setting up Bluepad32...");
        BP32.setup(&onConnectedGamepad, &onDisconnectedGamepad);
        BP32.enableVirtualDevice(false);
        registerBuild6ClassicWakeHandler();
        registerBuild5SecurityManagerWakeHandler();
        registerXboxAdvEventHandler();
        Serial.println("Bluepad32 ready - Xbox ADV wake with PC-ON Bluetooth handoff test");
        Serial.println("BLE RECOVERY: Serial command ERASE_BONDS available while PC OFF");
    } else {
        Serial.println("RADIO MODE: Bluetooth handlers skipped in WiFi AP mode");
    }

    Serial.println("Setting up web server...");
    setupWebServer();
    
    Serial.println("=== BC-250 READY ===\n");
}

void loop() {
    unsigned long now = millis();
    
    // ================ AUTOMATIC ESP32 RESTART ================
    static unsigned long pcOffStartTime = 0;
    
    // Track how long the PC has been off
    if (!pcIsOn && powerState == POWER_IDLE) {
        if (pcOffStartTime == 0) {
            pcOffStartTime = now;
            Serial.println("PC is off - ESP32 will restart after 2 hours");
        }
        
        // Restart ESP32 if the PC has remained off for more than 2 hours
        if (!configApMode && now - pcOffStartTime >= 7200000) { // 2 hours = 7200000 ms
            Serial.println("=== PC HAS BEEN OFF FOR 2 HOURS - RESTARTING ESP32 ===");
            delay(1000);
            ESP.restart();
        }
    } else {
        // PC is on or starting - reset the timer
        pcOffStartTime = 0;
    }
    
    // ================ POWER STATE DEBUG ================
    static unsigned long lastStatePrint = 0;
    static PowerState lastPowerState = POWER_IDLE;
    
    if (powerState != lastPowerState) {
        // State changed - print the new state
        Serial.print("STATE: ");
        switch(powerState) {
            case POWER_IDLE: Serial.print("IDLE"); break;
            case POWER_ON_START: Serial.print("ON_START"); break;
            case POWER_ON_WAITING_RELAY2: Serial.print("ON_WAITING_RELAY2"); break;
            case POWER_ON_COMPLETE: Serial.print("ON_COMPLETE"); break;
            case POWER_OFF_START: Serial.print("OFF_START"); break;
            case POWER_OFF_WAITING: Serial.print("OFF_WAITING"); break;
            case POWER_OFF_WAITING_POWEROFF: Serial.print("OFF_WAITING_POWEROFF"); break;
            case POWER_FORCE_START: Serial.print("FORCE_START"); break;
            case POWER_FORCE_WAITING: Serial.print("FORCE_WAITING"); break;
            default: Serial.print("UNKNOWN"); break;
        }
        Serial.print(" (pcIsOn=");
        Serial.print(pcIsOn ? "ON" : "OFF");
        Serial.print(", monitor=");
        Serial.print(digitalRead(PC_MONITOR_PIN) ? "HIGH" : "LOW");
        Serial.println(")");
        lastPowerState = powerState;
        lastStatePrint = now;
    }
    
    // Print status every 60 seconds
    if (now - lastStatePrint >= 60000) {
        Serial.print("HEARTBEAT: ");
        Serial.print(millis() / 1000);
        Serial.print("s - State: ");
        switch(powerState) {
            case POWER_IDLE: 
                Serial.print("IDLE");
                if (!pcIsOn) {
                    Serial.print(" (restart in ");
                    Serial.print((7200000 - (now - pcOffStartTime)) / 1000);
                    Serial.print("s)");
                }
                break;
            case POWER_ON_START: Serial.print("ON_START"); break;
            case POWER_ON_WAITING_RELAY2: Serial.print("ON_WAITING_RELAY2"); break;
            case POWER_ON_COMPLETE: Serial.print("ON_COMPLETE"); break;
            case POWER_OFF_START: Serial.print("OFF_START"); break;
            case POWER_OFF_WAITING: Serial.print("OFF_WAITING"); break;
            case POWER_OFF_WAITING_POWEROFF: Serial.print("OFF_WAITING_POWEROFF"); break;
            case POWER_FORCE_START: Serial.print("FORCE_START"); break;
            case POWER_FORCE_WAITING: Serial.print("FORCE_WAITING"); break;
            default: Serial.print("UNKNOWN"); break;
        }
        Serial.print(", PC: ");
        Serial.print(pcIsOn ? "ON" : "OFF");
        Serial.println();
        lastStatePrint = now;
    }

    if (!configApMode) pollBluetoothRecoverySerial();

    // ================ PIN READING ================
    if (now - lastPinRead >= pinReadInterval) {
        cachedButtonState = digitalRead(BUTTON_PIN);
        lastPinRead = now;
    }

    // ================ PC STATE HANDLING ================
    if (now - lastPcStateHandle >= pcStateHandleInterval) {
        handlePcStates();
        lastPcStateHandle = now;
    }

    // ================ POWER STATE HANDLING ================
    handlePowerStates();

    // ================ WEB SERVER ================
    if (wifiRadioEnabled && now - lastServerHandle >= serverHandleInterval) {
        server.handleClient();
        lastServerHandle = now;
    }
    
    // Finish a web-requested WiFi shutdown after the HTTP response has been sent.
    if (wifiDisablePending && (unsigned long)(now - wifiDisableRequestedAt) >= 1200) {
        disableWifiRadio();
    }

    // IMPORTANT: HTTP handlers above can update configLastActivityMs during
    // server.handleClient(). The loop's `now` snapshot is from BEFORE that
    // handler; subtracting a newer timestamp from it underflows unsigned long
    // and causes an immediate false 15-minute timeout on page/auth requests.
    // Read millis() AFTER handling HTTP and guard against a newer activity time.
    if (configApMode && !pcIsOn && powerState == POWER_IDLE) {
        const unsigned long idleCheckNow = millis();
        if (idleCheckNow >= configLastActivityMs) {
            const unsigned long idleMs = idleCheckNow - configLastActivityMs;
            if (idleMs >= CONFIG_IDLE_TIMEOUT_MS) {
                Serial.printf("RADIO MODE: AP idle timeout: now=%lu lastActivity=%lu idle=%lu ms limit=%lu ms\n",
                              idleCheckNow, configLastActivityMs, idleMs, CONFIG_IDLE_TIMEOUT_MS);
                Serial.println("RADIO MODE: WiFi AP idle for 15 minutes; returning to Bluetooth");
                requestRadioMode(false);
            }
        } else {
            // Only possible if millis() wrapped or an activity timestamp was
            // recorded after a stale snapshot. Reset to a safe baseline.
            Serial.printf("RADIO MODE: AP activity timestamp ahead of clock: now=%lu last=%lu; resetting idle timer\n",
                          idleCheckNow, configLastActivityMs);
            configLastActivityMs = idleCheckNow;
        }
    }
    if (!configApMode) handlePcOnBluetoothShutdown();

    // ================ CONTROLLER HANDLING ================
    // Process controller wake only while the PC is off and the state machine is idle
    if (!configApMode && !pcIsOn && powerState == POWER_IDLE) {
        ps5Simple.handle();
    } else {
        // PC is on - do not trigger controller wake, but keep Bluepad32 updated
        static unsigned long lastBP32Update = 0;
        if (!configApMode && !bluetoothStoppedForPc && now - lastBP32Update >= 100) {
            BP32.update();
            lastBP32Update = now;
        }
    }
    
    // ================ BUTTON HANDLING ================
    static unsigned long buttonPressStartTime = 0;
    static bool buttonPressDetected = false;
    static bool lastStableButtonState = HIGH;
    static bool wifiHoldHandled = false;
    
    // Debounce the button state
    if (cachedButtonState != lastStableButtonState) {
        lastButtonDebounce = now;
        lastStableButtonState = cachedButtonState;
    }
    
    // Continue when the button state is stable
    if ((now - lastButtonDebounce) > debounceDelay) {
        
        // Button pressed (LOW)
        if (cachedButtonState == LOW && !buttonPressDetected) {
            buttonPressDetected = true;
            buttonPressStartTime = now;
            wifiHoldHandled = false;
            Serial.println("BUTTON: pressed");
        }
        
        // All gestures are classified ON RELEASE to prevent a 10s hold from
        // first triggering the 5s action. No live radio changes while held.

        // Button released (HIGH)
        if (cachedButtonState == HIGH && buttonPressDetected) {
            unsigned long pressDuration = now - buttonPressStartTime;
            buttonPressDetected = false;
            
            Serial.print("BUTTON: released - duration: ");
            Serial.print(pressDuration);
            Serial.println(" ms");
            
            bool pcOn = getStablePcState();
            if (!pcOn && powerState == POWER_IDLE && pressDuration >= 10000) {
                Serial.println("BUTTON: 10s hold - toggling exclusive WiFi/Bluetooth mode");
                requestRadioMode(!configApMode);
            } else if (!pcOn && powerState == POWER_IDLE && pressDuration >= 5000) {
                Serial.println("BUTTON: 5-9s hold - rebooting ESP32");
                digitalWrite(OPTO_PIN, LOW);
                delay(250);
                ESP.restart();
            } else if (pcOn && pressDuration >= 5000 &&
                       (powerState == POWER_IDLE || powerState == POWER_OFF_WAITING_POWEROFF)) {
                Serial.println("BUTTON: long press - FORCE SHUTDOWN");
                startForceShutdown();
            } else if (powerState == POWER_IDLE) {
                if (pcOn) {
                    Serial.println("BUTTON: short press - NORMAL SHUTDOWN");
                    startNormalShutdown();
                } else if (configApMode) {
                    Serial.println("BUTTON: short press ignored in WiFi configuration mode");
                } else {
                    Serial.println("BUTTON: PC is off - POWER ON");
                    startPowerOn();
                }
            } else {
                Serial.println("BUTTON: power transition active; command ignored");
            }
        }
    }
    
    // ================ SHORT LOOP DELAY ================
    delay(1);
}