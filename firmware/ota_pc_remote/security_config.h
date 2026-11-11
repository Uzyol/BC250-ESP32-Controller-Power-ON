#ifndef BC250_SECURITY_CONFIG_H
#define BC250_SECURITY_CONFIG_H

// CHANGE THESE THREE VALUES BEFORE FLASHING. Passwords are stored in firmware.
// AP password: 8..63 ASCII characters. Avoid quotation marks and backslashes.
static const char* const AP_SSID = "BC-250-POWER-CONTROL";
static const char* const AP_PASSWORD = "CHANGE-AP-PASS-2026";

// HTTP Basic authentication for ALL pages, APIs and OTA uploads.
static const char* const WEB_USERNAME = "admin";
static const char* const WEB_PASSWORD = "CHANGE-WEB-PASS-2026";

#endif
