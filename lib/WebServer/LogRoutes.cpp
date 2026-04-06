#include "WebServer.h"
#include <time.h>

extern void forwardWebAdminActivityToTelegram(const String& user, const String& method, const String& status);

namespace {
String normalizeMethodCode(const String& methodRaw) {
    String method = methodRaw;
    method.trim();
    method.toLowerCase();

    if (method.length() == 0) return "SYSTEM";
    if (method.indexOf("rfid") >= 0) return "RFID";
    if (method.indexOf("otp") >= 0) return "OTP";
    if (method.indexOf("pin") >= 0 || method.indexOf("backup") >= 0) return "PIN";
    if (method.indexOf("guest") >= 0) return "GUEST";
    if (method.indexOf("auth") >= 0) return "AUTH";
    if (method.indexOf("emergency") >= 0 || method.indexOf("override") >= 0) return "EMERGENCY";
    if (method.indexOf("lockdown") >= 0) return "LOCKDOWN";
    if (method.indexOf("buzzer") >= 0) return "BUZZER";
    if (method.indexOf("status") >= 0) return "STATUS";
    if (method.indexOf("user") >= 0) return "USER";
    return "SYSTEM";
}
}

void WebServer::_handleAPILogs(AsyncWebServerRequest* request) {
    if (!_requireApiAuth(request)) {
        return;
    }

    JsonDocument storageDoc;
    if (LittleFS.exists("/logs.json")) {
        File file = LittleFS.open("/logs.json", "r");
        DeserializationError error = deserializeJson(storageDoc, file);
        file.close();

        if (error || !storageDoc["logs"].is<JsonArray>()) {
            storageDoc.clear();
            storageDoc["logs"] = JsonArray();
        }
    } else {
        storageDoc["logs"] = JsonArray();
    }

    JsonDocument responseDoc;
    JsonArray responseLogs = responseDoc["logs"].to<JsonArray>();
    JsonArray sourceLogs = storageDoc["logs"].as<JsonArray>();

    for (int i = static_cast<int>(sourceLogs.size()) - 1; i >= 0; --i) {
        if (!sourceLogs[i].is<JsonObject>()) {
            continue;
        }

        JsonObject src = sourceLogs[i].as<JsonObject>();
        JsonObject dst = responseLogs.add<JsonObject>();
        for (JsonPair kv : src) {
            dst[kv.key().c_str()] = kv.value();
        }

        String user = dst["user"] | "";
        user.trim();
        if (user.length() == 0) {
            dst["user"] = "System";
        }

        String method = dst["method"] | "";
        const String methodCode = dst["methodCode"] | "";
        if (methodCode.length() == 0) {
            dst["methodCode"] = normalizeMethodCode(method);
        }
    }

    _sendJSON(request, 200, responseDoc);
}

void WebServer::_handleAPIClearLogs(AsyncWebServerRequest* request) {
    if (!_requireApiAuth(request)) {
        return;
    }

    Serial.println("[API] DELETE /api/logs - Clear all logs");

    JsonDocument logsDoc;
    logsDoc["logs"] = JsonArray();

    bool cleared = false;
    File file = LittleFS.open("/logs.json", "w");
    if (file) {
        serializeJson(logsDoc, file);
        file.close();
        cleared = true;
    }

    JsonDocument response;
    response["success"] = cleared;
    response["message"] = cleared ? "All logs cleared" : "Failed to clear logs";

    forwardWebAdminActivityToTelegram("Admin (Web)", "Clear Logs", cleared ? "success" : "fail");

    _sendJSON(request, cleared ? 200 : 500, response);
}

void WebServer::_addLogEntry(const String& user, const String& method, const String& status) {
    JsonDocument logsDoc;

    if (LittleFS.exists("/logs.json")) {
        File file = LittleFS.open("/logs.json", "r");
        if (file) {
            DeserializationError err = deserializeJson(logsDoc, file);
            file.close();
            if (err || !logsDoc.is<JsonObject>()) {
                logsDoc.clear();
            }
        }
    }

    if (!logsDoc["logs"].is<JsonArray>()) {
        logsDoc["logs"] = JsonArray();
    }

    JsonArray logs = logsDoc["logs"].as<JsonArray>();

    while (logs.size() >= 50) {
        logs.remove(0);
    }

    char timeStr[40];
    unsigned long long epochMs = 0;
    time_t now = time(nullptr);
    if (now > 1700000000) {
        struct tm utcTime;
        gmtime_r(&now, &utcTime);
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%dT%H:%M:%SZ", &utcTime);
        epochMs = static_cast<unsigned long long>(now) * 1000ULL;
    } else {
        unsigned long sec = millis() / 1000;
        unsigned long m = (sec / 60) % 60;
        unsigned long h = (sec / 3600) % 24;
        snprintf(timeStr, sizeof(timeStr), "%02lu:%02lu (uptime)", h, m);
    }

    JsonObject entry = logs.add<JsonObject>();
    entry["time"] = String(timeStr);
    entry["epochMs"] = epochMs;
    entry["uptimeMs"] = millis();
    entry["user"] = user;
    entry["method"] = method;
    entry["methodCode"] = normalizeMethodCode(method);
    entry["status"] = status;

    File wFile = LittleFS.open("/logs.json", "w");
    if (!wFile) {
        Serial.println("[LOG][WARN] Failed to open /logs.json for write");
        return;
    }

    const size_t written = serializeJson(logsDoc, wFile);
    wFile.close();

    if (written == 0) {
        Serial.println("[LOG][WARN] Failed to write /logs.json");
        return;
    }

    Serial.print("[LOG] ");
    Serial.print(user);
    Serial.print(" | ");
    Serial.print(method);
    Serial.print(" | ");
    Serial.println(status);

    if (user == "Admin (Web)") {
        forwardWebAdminActivityToTelegram(user, method, status);
    }
}
