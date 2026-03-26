#include "WebServer.h"
#include "WebServerRouteUtils.h"
#include "WebServerRouteLimits.h"

// Runtime bridge symbols
extern String getLastKeypadKeyLabel();
extern unsigned long getLastKeypadKeyMs();

void WebServer::_handleAPIUsers(AsyncWebServerRequest* request) {
    if (!_requireApiAuth(request)) {
        return;
    }

    JsonDocument responseDoc;
    _syncUsersFileFromAuth(&responseDoc);
    _sendJSON(request, 200, responseDoc);
}

void WebServer::_handleAPIDeleteUser(AsyncWebServerRequest* request) {
    if (!_requireApiAuth(request)) {
        return;
    }

    if (!request->hasParam("uid")) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Missing uid parameter";
        _sendJSON(request, 400, doc);
        return;
    }

    String uid = webserver_route_utils::normalizeUID(request->getParam("uid")->value());
    Serial.print("[API] DELETE /api/users?uid=");
    Serial.println(uid);

    bool protectedAdmin = (uid == "DEFAULT_ADMIN");
    if (!protectedAdmin && LittleFS.exists("/users.json")) {
        JsonDocument usersDoc;
        File file = LittleFS.open("/users.json", "r");
        if (file) {
            const DeserializationError err = deserializeJson(usersDoc, file);
            file.close();

            if (!err && usersDoc["users"].is<JsonArray>()) {
                JsonArray users = usersDoc["users"].as<JsonArray>();
                for (JsonObject user : users) {
                    String listedUid = webserver_route_utils::normalizeUID(user["cardUID"] | "");
                    if (listedUid.length() == 0) {
                        listedUid = webserver_route_utils::normalizeUID(user["uid"] | "");
                    }

                    if (listedUid != uid) {
                        continue;
                    }

                    String type = user["type"] | "";
                    type.trim();
                    type.toLowerCase();
                    if (type == "admin") {
                        protectedAdmin = true;
                    }
                    break;
                }
            }
        }
    }

    if (protectedAdmin) {
        _addLogEntry("Admin (Web)", "Delete User (blocked admin " + uid + ")", "fail");

        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Admin user cannot be deleted";
        doc["errorCode"] = "PROTECTED_ADMIN_USER";
        _sendJSON(request, 403, doc);
        return;
    }

    bool removedFromAuth = _auth->removeUser(uid);
    const bool deleted = removedFromAuth;

    if (deleted) {
        _security->beep(1);
        _syncUsersFileFromAuth();
    }

    _addLogEntry("Admin (Web)", "Delete User (" + uid + ")", deleted ? "success" : "fail");

    JsonDocument doc;
    doc["success"] = deleted;
    doc["removedFromAuth"] = removedFromAuth;
    doc["removedFromList"] = removedFromAuth;

    if (deleted) {
        doc["message"] = "User deleted";
        _sendJSON(request, 200, doc);
        return;
    }

    doc["message"] = "User not found";
    _sendJSON(request, 404, doc);
}

void WebServer::_handleAPIResetUsers(AsyncWebServerRequest* request, uint8_t* data, size_t len) {
    if (!_requireApiAuth(request)) {
        return;
    }

    if (len > webserver_limits::MAX_RESET_PAYLOAD_BYTES) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Request body too large";
        _sendJSON(request, 413, doc);
        return;
    }

    JsonDocument body;
    const DeserializationError err = deserializeJson(body, data, len);
    if (err) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Invalid JSON body";
        _sendJSON(request, 400, doc);
        return;
    }

    String confirm = body["confirm"] | "";
    confirm.trim();
    confirm.toUpperCase();

    if (confirm != "RESET_ALL_USERS") {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Confirmation mismatch. Send confirm=RESET_ALL_USERS";
        _sendJSON(request, 400, doc);
        return;
    }

    const int before = _auth->getUserCount();
    _auth->performFactoryReset();
    _syncUsersFileFromAuth();
    _security->beep(2);

    _addLogEntry("Admin (Web)", "Reset All Users", "success");

    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "All users reset";
    doc["removedUsers"] = before;
    doc["remainingUsers"] = _auth->getUserCount();
    _sendJSON(request, 200, doc);
}

void WebServer::_handleAPIAddUser(AsyncWebServerRequest* request, uint8_t* data, size_t len) {
    if (!_requireApiAuth(request)) {
        return;
    }

    if (len > webserver_limits::MAX_USER_PAYLOAD_BYTES) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Request body too large";
        _sendJSON(request, 413, doc);
        return;
    }

    Serial.println("[API] POST /api/users - Add user");

    JsonDocument body;
    DeserializationError err = deserializeJson(body, data, len);

    if (err) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Invalid JSON body";
        _sendJSON(request, 400, doc);
        return;
    }

    String name = body["name"] | "";
    String pin = body["pin"] | "";
    String uid = body["cardUID"] | "";
    if (uid.isEmpty()) {
        uid = body["uid"] | "";
    }
    String type = body["type"] | "user";
    String telegramChatID = body["telegramChatID"] | "";
    if (telegramChatID.isEmpty()) {
        telegramChatID = body["chat_id"] | "";
    }
    String backupPIN = body["backupPIN"] | "";
    if (backupPIN.isEmpty()) {
        backupPIN = body["backup_pin"] | "";
    }

    (void)type;
    name.trim();
    uid = webserver_route_utils::normalizeUID(uid);
    telegramChatID.trim();
    backupPIN.trim();

    if (name.length() > webserver_limits::MAX_PERSON_NAME_LENGTH) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "name";
        doc["message"] = "Name is too long (max 48 characters)";
        _sendJSON(request, 400, doc);
        return;
    }

    if (telegramChatID.length() > webserver_limits::MAX_TELEGRAM_CHAT_ID_LENGTH) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "telegramChatID";
        doc["message"] = "Telegram Chat ID is too long";
        _sendJSON(request, 400, doc);
        return;
    }

    if (pin.isEmpty() && !backupPIN.isEmpty()) {
        pin = backupPIN;
    }

    if (name.isEmpty() || pin.isEmpty() || uid.isEmpty()) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Missing required fields: name, cardUID/uid, and pin or backupPIN";
        _sendJSON(request, 400, doc);
        return;
    }

    if (telegramChatID.isEmpty()) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "telegramChatID";
        doc["message"] = "Telegram Chat ID is required for OTP delivery";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!webserver_route_utils::isValidTelegramChatId(telegramChatID)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "telegramChatID";
        doc["message"] = "A valid Telegram Chat ID is required for OTP delivery";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!webserver_route_utils::isFourDigitCode(pin)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "pin";
        doc["message"] = "PIN must be exactly 4 digits";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!webserver_route_utils::isValidPersonName(name)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "name";
        doc["message"] = "Name can contain letters, spaces, apostrophes, dots, and hyphens only";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!webserver_route_utils::isValidUID(uid)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "uid";
        doc["message"] = "RFID UID must be valid uppercase hex (8 to 20 chars)";
        _sendJSON(request, 400, doc);
        return;
    }

    if (webserver_route_utils::isTelegramChatIdInUse(_auth, telegramChatID)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["errorCode"] = "CHAT_ID_ALREADY_REGISTERED";
        doc["field"] = "telegramChatID";
        doc["message"] = "This Telegram Chat ID is already linked to another user";
        _sendJSON(request, 409, doc);
        return;
    }

    if (!backupPIN.isEmpty() && !webserver_route_utils::isFourDigitCode(backupPIN)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "backupPIN";
        doc["message"] = "Backup PIN must be exactly 4 digits";
        _sendJSON(request, 400, doc);
        return;
    }

    bool duplicateRFID = _auth->userExists(uid);
    if (!duplicateRFID && LittleFS.exists("/users.json")) {
        JsonDocument usersDoc;
        File file = LittleFS.open("/users.json", "r");
        if (file) {
            DeserializationError fileErr = deserializeJson(usersDoc, file);
            file.close();

            if (!fileErr && usersDoc["users"].is<JsonArray>()) {
                JsonArray users = usersDoc["users"].as<JsonArray>();
                for (size_t i = 0; i < users.size(); i++) {
                    String existingUID = users[i]["cardUID"].as<String>();
                    if (existingUID.length() == 0) {
                        existingUID = users[i]["uid"].as<String>();
                    }
                    existingUID = webserver_route_utils::normalizeUID(existingUID);
                    if (existingUID == uid) {
                        duplicateRFID = true;
                        break;
                    }
                }
            }
        }
    }

    if (duplicateRFID) {
        Serial.print("[API][WARN] Duplicate RFID enrollment blocked (Add User): ");
        Serial.println(uid);
        _security->beep(3);
        _addLogEntry("Admin (Web)", "Duplicate RFID " + uid, "fail");

        JsonDocument doc;
        doc["success"] = false;
        doc["errorCode"] = "RFID_ALREADY_REGISTERED";
        doc["field"] = "uid";
        doc["message"] = "This RFID card is already registered. Please scan a different card.";
        _sendJSON(request, 409, doc);
        return;
    }

    bool added = _auth->addUser(uid, pin, name);
    String failureReason = "";

    if (!added) {
        if (_auth->userExists(uid)) {
            failureReason = "This RFID card is already registered.";
        } else if (_auth->getUserCount() >= 20) {
            failureReason = "User limit reached (max 20 users). Delete an existing user first.";
        } else {
            failureReason = "Unable to write user record. Storage may be full or unavailable.";
        }
    }

    if (added) {
        const bool chatSaved = _auth->setUserTelegramChatId(uid, telegramChatID);
        if (!chatSaved) {
            _auth->removeUser(uid);
            added = false;
            failureReason = "Failed to persist Telegram Chat ID for this user.";
        }

        if (added && !backupPIN.isEmpty()) {
            const bool backupSaved = _auth->setUserBackupPIN(uid, backupPIN);
            if (!backupSaved) {
                _auth->removeUser(uid);
                added = false;
                failureReason = "Failed to persist Backup PIN for this user.";
            }
        }
    }

    if (added) {
        _security->beep(1);
        _syncUsersFileFromAuth();
    }

    _addLogEntry(name, "Add User", added ? "success" : "fail");

    JsonDocument doc;
    doc["success"] = added;
    doc["message"] = added ? "User added" : (failureReason.length() ? failureReason : "Failed to add user");
    _sendJSON(request, added ? 201 : 500, doc);
}

void WebServer::_handleAPIEditUser(AsyncWebServerRequest* request, uint8_t* data, size_t len) {
    if (!_requireApiAuth(request)) {
        return;
    }

    if (len > webserver_limits::MAX_USER_PAYLOAD_BYTES) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Request body too large";
        _sendJSON(request, 413, doc);
        return;
    }

    Serial.println("[API] PUT /api/users - Edit user");

    JsonDocument body;
    DeserializationError err = deserializeJson(body, data, len);

    if (err) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Invalid JSON body";
        _sendJSON(request, 400, doc);
        return;
    }

    String uid = body["uid"] | "";
    String name = body["name"] | "";
    String pin = body["pin"] | "";
    String rfid = body["rfid"] | "";
    if (rfid.isEmpty()) {
        rfid = body["cardUID"] | "";
    }
    String telegramChatID = body["telegramChatID"] | "";
    if (telegramChatID.isEmpty()) {
        telegramChatID = body["chat_id"] | "";
    }
    String backupPIN = body["backupPIN"] | "";
    if (backupPIN.isEmpty()) {
        backupPIN = body["backup_pin"] | "";
    }

    name.trim();
    pin.trim();
    uid = webserver_route_utils::normalizeUID(uid);
    rfid = webserver_route_utils::normalizeUID(rfid);
    telegramChatID.trim();
    backupPIN.trim();

    if (name.length() > webserver_limits::MAX_PERSON_NAME_LENGTH) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "name";
        doc["message"] = "Name is too long (max 48 characters)";
        _sendJSON(request, 400, doc);
        return;
    }

    if (telegramChatID.length() > webserver_limits::MAX_TELEGRAM_CHAT_ID_LENGTH) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "telegramChatID";
        doc["message"] = "Telegram Chat ID is too long";
        _sendJSON(request, 400, doc);
        return;
    }

    if (pin.isEmpty() && !backupPIN.isEmpty()) {
        pin = backupPIN;
    }

    if (uid.isEmpty() || name.isEmpty()) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Missing required fields: uid, name";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!telegramChatID.isEmpty() && !webserver_route_utils::isValidTelegramChatId(telegramChatID)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "telegramChatID";
        doc["message"] = "Telegram Chat ID must be numeric when provided";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!pin.isEmpty() && !webserver_route_utils::isFourDigitCode(pin)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "pin";
        doc["message"] = "PIN must be exactly 4 digits";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!webserver_route_utils::isValidPersonName(name)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "name";
        doc["message"] = "Name can contain letters, spaces, apostrophes, dots, and hyphens only";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!webserver_route_utils::isValidUID(uid)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "uid";
        doc["message"] = "RFID UID must be valid uppercase hex (8 to 20 chars)";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!rfid.isEmpty() && !webserver_route_utils::isValidUID(rfid)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "rfid";
        doc["message"] = "Replacement RFID UID must be valid uppercase hex (8 to 20 chars)";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!backupPIN.isEmpty() && !webserver_route_utils::isFourDigitCode(backupPIN)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "backupPIN";
        doc["message"] = "Backup PIN must be exactly 4 digits";
        _sendJSON(request, 400, doc);
        return;
    }

    bool duplicateRFID = false;
    if (!rfid.isEmpty() && rfid != uid) {
        duplicateRFID = _auth->userExists(rfid);

        if (!duplicateRFID && LittleFS.exists("/users.json")) {
            JsonDocument usersDoc;
            File usersFile = LittleFS.open("/users.json", "r");
            if (usersFile) {
                DeserializationError usersErr = deserializeJson(usersDoc, usersFile);
                usersFile.close();

                if (!usersErr && usersDoc["users"].is<JsonArray>()) {
                    JsonArray users = usersDoc["users"].as<JsonArray>();
                    for (size_t i = 0; i < users.size(); i++) {
                        String existingUID = users[i]["cardUID"].as<String>();
                        if (existingUID.length() == 0) {
                            existingUID = users[i]["uid"].as<String>();
                        }
                        existingUID = webserver_route_utils::normalizeUID(existingUID);

                        if (existingUID == rfid && existingUID != uid) {
                            duplicateRFID = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    if (duplicateRFID) {
        Serial.print("[API][WARN] Duplicate RFID replacement blocked (Edit User): old=");
        Serial.print(uid);
        Serial.print(" new=");
        Serial.println(rfid);

        _security->beep(3);
        _addLogEntry("Admin (Web)", "Duplicate RFID " + rfid, "fail");

        JsonDocument doc;
        doc["success"] = false;
        doc["errorCode"] = "RFID_ALREADY_REGISTERED";
        doc["field"] = "uid";
        doc["message"] = "This RFID card is already registered. Please scan a different card.";
        _sendJSON(request, 409, doc);
        return;
    }

    const String oldName = _auth->getUserName(uid);
    const String oldPin = _auth->getUserPIN(uid);
    const String oldTelegramChatId = _auth->getUserTelegramChatId(uid);
    const String oldBackupPIN = _auth->getUserBackupPIN(uid);

    if (oldPin.isEmpty()) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "User not found";
        _sendJSON(request, 404, doc);
        return;
    }

    auto restorePreviousUserState = [this, &uid, &oldPin, &oldName, &oldTelegramChatId, &oldBackupPIN]() {
        _auth->removeUser(uid);
        if (_auth->addUser(uid, oldPin, oldName)) {
            _auth->setUserTelegramChatId(uid, oldTelegramChatId);
            if (oldBackupPIN.length() == 4) {
                _auth->setUserBackupPIN(uid, oldBackupPIN);
            }
        }
    };

    String effectivePin = pin;
    if (effectivePin.isEmpty()) {
        effectivePin = _auth->getUserPIN(uid);
    }
    if (effectivePin.isEmpty()) {
        effectivePin = oldBackupPIN;
    }

    if (effectivePin.isEmpty()) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "PIN required to update this user. Provide a valid 4-digit backup PIN.";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!webserver_route_utils::isFourDigitCode(effectivePin)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "PIN must be exactly 4 digits";
        _sendJSON(request, 400, doc);
        return;
    }

    const String targetUid = rfid.isEmpty() ? uid : rfid;
    const bool uidChanged = (targetUid != uid);

    String effectiveTelegramChatId = telegramChatID;
    if (effectiveTelegramChatId.isEmpty()) {
        effectiveTelegramChatId = _auth->getUserTelegramChatId(uid);
    }

    effectiveTelegramChatId.trim();

    if (!effectiveTelegramChatId.isEmpty() && !webserver_route_utils::isValidTelegramChatId(effectiveTelegramChatId)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "telegramChatID";
        doc["message"] = "Telegram Chat ID must be numeric when provided";
        _sendJSON(request, 400, doc);
        return;
    }

    if (effectiveTelegramChatId.isEmpty()) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "telegramChatID";
        doc["message"] = "Telegram Chat ID is required for OTP delivery";
        _sendJSON(request, 400, doc);
        return;
    }

    if (webserver_route_utils::isTelegramChatIdInUse(_auth, effectiveTelegramChatId, uid)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["errorCode"] = "CHAT_ID_ALREADY_REGISTERED";
        doc["field"] = "telegramChatID";
        doc["message"] = "This Telegram Chat ID is already linked to another user";
        _sendJSON(request, 409, doc);
        return;
    }

    String effectiveBackupPIN = backupPIN;
    if (effectiveBackupPIN.isEmpty()) {
        effectiveBackupPIN = _auth->getUserBackupPIN(uid);
    }

    if (effectiveBackupPIN.isEmpty()) {
        effectiveBackupPIN = effectivePin;
    }

    bool authUpdated = false;
    if (uidChanged) {
        const bool removedOld = _auth->removeUser(uid);
        if (!removedOld) {
            JsonDocument doc;
            doc["success"] = false;
            doc["message"] = "Original user record could not be updated";
            _sendJSON(request, 500, doc);
            return;
        }

        authUpdated = _auth->addUser(targetUid, effectivePin, name);

        if (!authUpdated) {
            restorePreviousUserState();
        }
    } else {
        authUpdated = _auth->addUser(uid, effectivePin, name);
    }

    if (!authUpdated) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Failed to update user credentials";
        _sendJSON(request, 500, doc);
        return;
    }

    if (!_auth->setUserTelegramChatId(targetUid, effectiveTelegramChatId)) {
        if (uidChanged) {
            _auth->removeUser(targetUid);
            restorePreviousUserState();
        } else {
            restorePreviousUserState();
        }

        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Failed to persist Telegram Chat ID";
        _sendJSON(request, 500, doc);
        return;
    }

    if (effectiveBackupPIN.length() == 4 && !_auth->setUserBackupPIN(targetUid, effectiveBackupPIN)) {
        if (uidChanged) {
            _auth->removeUser(targetUid);
            restorePreviousUserState();
        } else {
            restorePreviousUserState();
        }

        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Failed to persist Backup PIN";
        _sendJSON(request, 500, doc);
        return;
    }

    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "User updated";
    doc["uid"] = targetUid;

    _syncUsersFileFromAuth();
    _addLogEntry(name, uidChanged ? "Edit User (RFID Replaced)" : "Edit User", "success");
    _sendJSON(request, 200, doc);
}

void WebServer::_handleAPIRfidScan(AsyncWebServerRequest* request) {
    if (!_requireApiAuth(request)) {
        return;
    }

    const unsigned long scanMs = _auth->getLastRFIDScanMs();
    const String lastUID = _auth->getLastRFIDUID();
    const bool scanned = (scanMs > 0 && lastUID.length() > 0);
    const bool known = scanned ? _auth->userExists(lastUID) : false;
    const String knownUserName = known ? _auth->getUserName(lastUID) : "";
    String scanStatus = "idle";
    if (scanned) {
        scanStatus = known ? "registered" : "unregistered";
    }

    JsonDocument doc;
    doc["scanned"] = scanned;
    doc["known"] = known;
    doc["status"] = scanStatus;
    if (scanned) {
        doc["uid"] = lastUID;
        doc["scanTimestamp"] = scanMs;
        doc["userName"] = knownUserName;
    }
    doc["lastUid"] = lastUID;
    doc["lastScanTimestamp"] = scanMs;
    doc["timestamp"] = millis();

    _sendJSON(request, 200, doc);
}

void WebServer::_handleAPIDiagnostics(AsyncWebServerRequest* request) {
    if (!_requireApiAuth(request)) {
        return;
    }

    JsonDocument doc;
    int rawUsers = 0;
    int uniqueUsers = 0;
    int invalidUsers = 0;
    int duplicateUsers = 0;
    _collectUsersStorageStats(&rawUsers, &uniqueUsers, &invalidUsers, &duplicateUsers);

    doc["rfidReady"] = _auth->isRFIDReady();
    doc["rfidRstActivePin"] = _auth->getActiveRFIDRstPin();
    doc["rfidCooldownActive"] = _auth->isRFIDCooldownActive();
    doc["lastRfidUid"] = _auth->getLastRFIDUID();
    doc["lastRfidScanMs"] = _auth->getLastRFIDScanMs();
    doc["keypadReady"] = _auth->isKeypadReady();
    doc["keypadMuted"] = _auth->isKeypadMuted();
    doc["keypadMuteRemainingMs"] = _auth->getKeypadMuteRemainingMs();
    doc["keypadLastKey"] = getLastKeypadKeyLabel();
    doc["keypadLastKeyMs"] = getLastKeypadKeyMs();
    doc["buzzerActive"] = _security->isBuzzerActive();
    doc["sirenActive"] = _security->isSirenActive();
    doc["vibrationLatched"] = _security->isVibrationLatched();
    doc["activeUsers"] = _auth->getUserCount();
    doc["rawUsers"] = rawUsers;
    doc["uniqueUsers"] = uniqueUsers;
    doc["invalidUsers"] = invalidUsers;
    doc["duplicateUsers"] = duplicateUsers;
    doc["usersStorageMismatch"] = (_auth->getUserCount() != uniqueUsers) || (invalidUsers > 0) || (duplicateUsers > 0);
    doc["timestamp"] = millis();
    _sendJSON(request, 200, doc);
}
