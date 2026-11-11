#ifndef PS5_SIMPLE_H
#define PS5_SIMPLE_H

#include <Bluepad32.h>
#include <Arduino.h>
#include "pc_control.h"

extern bool ps5Enabled;
extern bool getStablePcState();
extern void startPowerOn();
extern PowerState powerState;
extern bool addPS5ControllerMac(String mac);

class PS5Simple {
private:
    static const int MAX_ALLOWED_CONTROLLERS = 5;
    GamepadPtr myController = nullptr;
    String allowedMacs[MAX_ALLOWED_CONTROLLERS];
    String lastSeenMac = "";
    unsigned long lastSeenTime = 0;
    bool addNextControllerMode = false;
    bool wakeConsumed = false;  // One Bluetooth wake per ESP32 boot/off-cycle.

    String cleanMac(String mac) {
        mac.trim();
        mac.replace(":", "");
        mac.replace("-", "");
        mac.toUpperCase();
        return mac;
    }

public:
    void clearAllowedMacs() {
        for (int i = 0; i < MAX_ALLOWED_CONTROLLERS; i++) allowedMacs[i] = "";
        addNextControllerMode = false;
    }

    bool addAllowedMac(String mac) {
        mac = cleanMac(mac);
        if (mac.length() != 12 || mac == "000000000000") return false;
        for (int i = 0; i < MAX_ALLOWED_CONTROLLERS; i++) {
            if (allowedMacs[i] == mac) return true;
        }
        for (int i = 0; i < MAX_ALLOWED_CONTROLLERS; i++) {
            if (allowedMacs[i].length() == 0) {
                allowedMacs[i] = mac;
                Serial.printf("CONTROLLER: MAC added to slot %d: %s\n", i + 1, mac.c_str());
                return true;
            }
        }
        Serial.println("CONTROLLER: MAC list full");
        return false;
    }

    bool removeAllowedMac(String mac) {
        mac = cleanMac(mac);
        for (int i = 0; i < MAX_ALLOWED_CONTROLLERS; i++) {
            if (allowedMacs[i] == mac) {
                allowedMacs[i] = "";
                return true;
            }
        }
        return false;
    }

    bool isAllowedMac(String mac) {
        mac = cleanMac(mac);
        for (int i = 0; i < MAX_ALLOWED_CONTROLLERS; i++) {
            if (allowedMacs[i].length() > 0 && allowedMacs[i] == mac) return true;
        }
        return false;
    }

    int getAllowedCount() {
        int count = 0;
        for (int i = 0; i < MAX_ALLOWED_CONTROLLERS; i++) if (allowedMacs[i].length() > 0) count++;
        return count;
    }

    int getMaxAllowedCount() { return MAX_ALLOWED_CONTROLLERS; }

    String getAllowedMac(int index) {
        if (index < 0 || index >= MAX_ALLOWED_CONTROLLERS) return "";
        String mac = allowedMacs[index];
        if (mac.length() != 12) return "";
        String formatted = "";
        for (int i = 0; i < 12; i += 2) {
            if (i > 0) formatted += ":";
            formatted += mac.substring(i, i + 2);
        }
        return formatted;
    }

    void beginAddNextController() {
        addNextControllerMode = true;
        lastSeenMac = "";
        lastSeenTime = 0;
        Serial.println("CONTROLLER: ADD NEXT mode enabled - connect the new controller");
    }

    void cancelAddNextController() { addNextControllerMode = false; }
    bool isAddNextControllerMode() { return addNextControllerMode; }

    String getConnectedMac() { return lastSeenMac; }

    bool isConnected() {
        return lastSeenMac.length() > 0 && millis() - lastSeenTime < 5000;
    }

    void disconnect() {}

    void resetControllerData() {
        if (lastSeenMac.length() > 0) {
            Serial.print("CONTROLLER: reset last seen MAC: ");
            Serial.println(lastSeenMac);
        }
        lastSeenMac = "";
        lastSeenTime = 0;
    }

    void onControllerConnected(GamepadPtr gp) {
        if (gp == nullptr) return;

        if (getStablePcState() || powerState != POWER_IDLE) {
            Serial.println("CONTROLLER: PC ON / power operation active - rejected");
            gp->disconnect();
            return;
        }

        GamepadProperties prop = gp->getProperties();
        char macStr[18];
        sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
                prop.btaddr[0], prop.btaddr[1], prop.btaddr[2],
                prop.btaddr[3], prop.btaddr[4], prop.btaddr[5]);
        String mac = String(macStr);

        Serial.print("CONTROLLER: detected MAC: ");
        Serial.println(mac);

        bool authorized = isAllowedMac(mac);

        // First controller after a clean config is saved automatically.
        if (!authorized && getAllowedCount() == 0) {
            Serial.println("CONTROLLER: list empty - saving first controller automatically");
            authorized = addPS5ControllerMac(mac);
        }
        // Additional controllers are accepted only after ADD NEXT is requested in web UI.
        else if (!authorized && addNextControllerMode) {
            Serial.println("CONTROLLER: ADD NEXT mode - saving new controller");
            authorized = addPS5ControllerMac(mac);
            addNextControllerMode = false;
        }

        if (authorized) {
            lastSeenMac = mac;
            lastSeenTime = millis();
            Serial.println("CONTROLLER: authorized");
            gp->disconnect();
            Serial.println("CONTROLLER: disconnected - MAC presence used for wake");
        } else {
            Serial.println("CONTROLLER: rejected - MAC is not in authorized list");
            gp->disconnect();
        }
    }

    // Build #7: one-shot Bluetooth wake guard. After an authorized controller has
    // requested power-on, ignore all later Bluetooth events until the ESP32 reboots.
    // The normal PC-off path already restarts the ESP32, re-arming wake for the next cycle.
    bool tryBluetoothWake(String mac, const char* source) {
        if (!ps5Enabled || getStablePcState() || powerState != POWER_IDLE) return false;

        mac = cleanMac(mac);
        if (!isAllowedMac(mac)) {
            Serial.print("BT WAKE: unauthorized controller via ");
            Serial.print(source);
            Serial.print(": ");
            Serial.println(mac);
            return false;
        }

        if (wakeConsumed) {
            Serial.print("BT WAKE: ignored duplicate event via ");
            Serial.print(source);
            Serial.println(" - wake already consumed for this boot");
            return false;
        }

        wakeConsumed = true;
        Serial.print("BT WAKE: authorized controller via ");
        Serial.print(source);
        Serial.print(": ");
        Serial.println(mac);
        Serial.println("BT WAKE: one-shot wake latched - later Bluetooth retries will be ignored");
        startPowerOn();
        return true;
    }

    void onClassicReconnectAttempt(String mac) {
        tryBluetoothWake(mac, "CLASSIC_HCI");
    }

    void onSecurityManagerReconnectAttempt(String mac, const char* eventName) {
        tryBluetoothWake(mac, eventName);
    }

    void onControllerDisconnected(GamepadPtr gp) {
        (void)gp;
        Serial.println("CONTROLLER: disconnected");
    }

    bool isAuthorizedControllerNearby() { return isConnected(); }
    bool psButtonPressed() { return isAuthorizedControllerNearby(); }

    void handle() {
        bool pcOn = getStablePcState();

        if (pcOn || powerState != POWER_IDLE) {
            if (lastSeenMac.length() > 0) {
                lastSeenMac = "";
                lastSeenTime = 0;
            }
            BP32.update();
            return;
        }

        BP32.update();
        if (!ps5Enabled) return;

        // A low-level HCI/SM wake is intentionally one-shot for this boot.
        // Do not let the higher-level "controller nearby" path fire the relay again.
        if (wakeConsumed) return;

        unsigned long now = millis();
        if (isAuthorizedControllerNearby()) {
            static unsigned long lastTriggerTime = 0;
            if (now - lastSeenTime < 2000 && now - lastTriggerTime > 5000) {
                Serial.println("CONTROLLER: authorized controller nearby - starting PC");
                lastTriggerTime = now;
                startPowerOn();
            }
        }

        static unsigned long lastPrint = 0;
        if (now - lastPrint > 10000) {
            if (addNextControllerMode) Serial.println("CONTROLLER: waiting for NEW controller (ADD NEXT mode)...");
            else if (isAuthorizedControllerNearby()) Serial.println("CONTROLLER: authorized controller nearby...");
            else Serial.println("CONTROLLER: waiting for controller...");
            lastPrint = now;
        }
    }
};

extern PS5Simple ps5Simple;

#endif
