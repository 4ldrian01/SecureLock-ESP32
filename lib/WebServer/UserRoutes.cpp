#include "WebServer.h"
#include "WebServerRouteUtils.h"
#include "WebServerRouteLimits.h"
#include "secrets.h"

// Runtime bridge symbols
extern String getLastKeypadKeyLabel();
extern unsigned long getLastKeypadKeyMs();
extern int getTelegramNotificationQueueDepth();
extern int getTelegramNotificationQueueCapacity();
extern unsigned long getTelegramNotificationQueueOldestAgeMs();
extern unsigned long getTelegramNotificationQueuedTotal();
extern unsigned long getTelegramNotificationDeliveredTotal();
extern unsigned long getTelegramNotificationDeliveryFailures();
extern unsigned long getTelegramNotificationDroppedFullTotal();
extern unsigned long getTelegramNotificationDroppedRetryTotal();
extern int getTelegramAdminMetricsSlots();
extern String getTelegramAdminChatIdAt(int index);
extern unsigned long getTelegramAdminSendAttemptsAt(int index);
extern unsigned long getTelegramAdminSendSuccessAt(int index);
extern unsigned long getTelegramAdminSendFailuresAt(int index);
extern unsigned long getTelegramAdminLastSuccessMsAt(int index);
extern unsigned long getTelegramAdminLastFailureMsAt(int index);
extern bool enqueueTelegramUserNotification(const String& chatId, const String& message);

namespace {

String normalizedNameValue(const String& raw) {
    String value = raw;
    value.trim();

    while (value.indexOf("  ") >= 0) {
        value.replace("  ", " ");
    }

    return value;
}

bool isSeededAdminChatId(const String& chatId) {
    String normalizedChatId = chatId;
    normalizedChatId.trim();

    if (normalizedChatId.length() == 0) {
        return false;
    }

    for (int i = 0; i < NUM_ADMINS; i++) {
        String seededAdminChatId = ADMIN_CHAT_IDS[i];
        seededAdminChatId.trim();
        if (seededAdminChatId.length() == 0) {
            continue;
        }

        if (seededAdminChatId == normalizedChatId) {
            return true;
        }
    }

    return false;
}

void splitNameParts(const String& fullName, String* firstName, String* middleName, String* lastName) {
    if (firstName) {
        *firstName = "";
    }
    if (middleName) {
        *middleName = "";
    }
    if (lastName) {
        *lastName = "";
    }

    String normalized = normalizedNameValue(fullName);
    if (normalized.length() == 0) {
        return;
    }

    const int firstSpace = normalized.indexOf(' ');
    if (firstSpace < 0) {
        if (firstName) {
            *firstName = normalized;
        }
        return;
    }

    const int lastSpace = normalized.lastIndexOf(' ');
    if (firstName) {
        *firstName = normalized.substring(0, firstSpace);
        firstName->trim();
    }

    if (lastName) {
        *lastName = normalized.substring(lastSpace + 1);
        lastName->trim();
    }

    if (middleName && lastSpace > firstSpace) {
        *middleName = normalized.substring(firstSpace + 1, lastSpace);
        middleName->trim();
    }
}

bool parseUserNamePayload(
    JsonDocument& body,
    String* fullName,
    String* firstName,
    String* middleName,
    String* lastName,
    String* errorField,
    String* errorMessage
) {
    String parsedFirstName = body["firstName"] | "";
    if (parsedFirstName.length() == 0) {
        parsedFirstName = body["first_name"] | "";
    }

    String parsedMiddleName = body["middleName"] | "";
    if (parsedMiddleName.length() == 0) {
        parsedMiddleName = body["middle_name"] | "";
    }

    String parsedLastName = body["lastName"] | "";
    if (parsedLastName.length() == 0) {
        parsedLastName = body["last_name"] | "";
    }

    parsedFirstName = normalizedNameValue(parsedFirstName);
    parsedMiddleName = normalizedNameValue(parsedMiddleName);
    parsedLastName = normalizedNameValue(parsedLastName);

    const bool hasStructuredName = parsedFirstName.length() > 0
        || parsedMiddleName.length() > 0
        || parsedLastName.length() > 0;

    String composedName = "";

    if (hasStructuredName) {
        if (parsedFirstName.length() == 0) {
            if (errorField) {
                *errorField = "firstName";
            }
            if (errorMessage) {
                *errorMessage = "First name is required";
            }
            return false;
        }

        if (parsedLastName.length() == 0) {
            if (errorField) {
                *errorField = "lastName";
            }
            if (errorMessage) {
                *errorMessage = "Last name is required";
            }
            return false;
        }

        if (!webserver_route_utils::isValidPersonName(parsedFirstName)) {
            if (errorField) {
                *errorField = "firstName";
            }
            if (errorMessage) {
                *errorMessage = "First name can contain letters, spaces, apostrophes, dots, and hyphens only";
            }
            return false;
        }

        if (parsedMiddleName.length() > 0 && !webserver_route_utils::isValidPersonName(parsedMiddleName)) {
            if (errorField) {
                *errorField = "middleName";
            }
            if (errorMessage) {
                *errorMessage = "Middle name can contain letters, spaces, apostrophes, dots, and hyphens only";
            }
            return false;
        }

        if (!webserver_route_utils::isValidPersonName(parsedLastName)) {
            if (errorField) {
                *errorField = "lastName";
            }
            if (errorMessage) {
                *errorMessage = "Last name can contain letters, spaces, apostrophes, dots, and hyphens only";
            }
            return false;
        }

        composedName = parsedFirstName;
        if (parsedMiddleName.length() > 0) {
            composedName += " " + parsedMiddleName;
        }
        composedName += " " + parsedLastName;
    } else {
        composedName = normalizedNameValue(body["name"] | "");
        splitNameParts(composedName, &parsedFirstName, &parsedMiddleName, &parsedLastName);
    }

    composedName = normalizedNameValue(composedName);

    if (fullName) {
        *fullName = composedName;
    }
    if (firstName) {
        *firstName = parsedFirstName;
    }
    if (middleName) {
        *middleName = parsedMiddleName;
    }
    if (lastName) {
        *lastName = parsedLastName;
    }

    return true;
}

String seededAdminUidForIndex(int index) {
    if (index <= 0) {
        return "DEFAULT_ADMIN";
    }

    return "DEFAULT_ADMIN_" + String(index + 1);
}

bool tryParseSeededAdminIndexFromUid(const String& rawUid, int* outIndex) {
    String uid = rawUid;
    uid.trim();
    uid.toUpperCase();

    if (uid == "DEFAULT_ADMIN") {
        if (outIndex) {
            *outIndex = 0;
        }
        return true;
    }

    if (!uid.startsWith("DEFAULT_ADMIN_")) {
        return false;
    }

    String suffix = uid.substring(String("DEFAULT_ADMIN_").length());
    suffix.trim();
    if (suffix.length() == 0) {
        return false;
    }

    for (size_t i = 0; i < suffix.length(); i++) {
        if (!isDigit(suffix.charAt(i))) {
            return false;
        }
    }

    const int parsed = suffix.toInt() - 1;
    if (parsed < 0 || parsed >= NUM_ADMINS) {
        return false;
    }

    if (outIndex) {
        *outIndex = parsed;
    }

    return true;
}

String defaultSeededAdminNameForIndex(int index) {
    if (index == 0) {
        return "Alsamhel Admin";
    }

    return "Default Admin " + String(index + 1);
}

bool loadSeededAdminNameOverride(const String& chatId, String* outName) {
    if (outName) {
        *outName = "";
    }

    if (!LittleFS.exists("/users.json")) {
        return false;
    }

    JsonDocument usersDoc;
    File file = LittleFS.open("/users.json", "r");
    if (!file) {
        return false;
    }

    const DeserializationError err = deserializeJson(usersDoc, file);
    file.close();
    if (err || !usersDoc.is<JsonObject>()) {
        return false;
    }

    JsonObject settings = usersDoc["settings"].as<JsonObject>();
    if (settings.isNull() || !settings["seededAdminProfiles"].is<JsonArray>()) {
        return false;
    }

    JsonArray profiles = settings["seededAdminProfiles"].as<JsonArray>();
    for (JsonObject profile : profiles) {
        String listedChatId = profile["chatId"] | "";
        listedChatId.trim();
        if (listedChatId != chatId) {
            continue;
        }

        String overrideName = profile["name"] | "";
        overrideName = normalizedNameValue(overrideName);
        if (overrideName.length() == 0) {
            return false;
        }

        if (outName) {
            *outName = overrideName;
        }
        return true;
    }

    return false;
}

bool saveSeededAdminNameOverride(int adminIndex, const String& displayName) {
    if (adminIndex < 0 || adminIndex >= NUM_ADMINS) {
        return false;
    }

    String chatId = ADMIN_CHAT_IDS[adminIndex];
    chatId.trim();
    if (chatId.length() == 0) {
        return false;
    }

    JsonDocument usersDoc;
    if (LittleFS.exists("/users.json")) {
        File readFile = LittleFS.open("/users.json", "r");
        if (readFile) {
            const DeserializationError readErr = deserializeJson(usersDoc, readFile);
            readFile.close();
            if (readErr || !usersDoc.is<JsonObject>()) {
                usersDoc.clear();
            }
        }
    }

    if (!usersDoc.is<JsonObject>()) {
        usersDoc.to<JsonObject>();
    }

    if (!usersDoc["users"].is<JsonArray>()) {
        usersDoc["users"] = JsonArray();
    }

    JsonObject settings = usersDoc["settings"].is<JsonObject>()
        ? usersDoc["settings"].as<JsonObject>()
        : usersDoc["settings"].to<JsonObject>();

    JsonArray profiles = settings["seededAdminProfiles"].is<JsonArray>()
        ? settings["seededAdminProfiles"].as<JsonArray>()
        : settings["seededAdminProfiles"].to<JsonArray>();

    const String seededUid = seededAdminUidForIndex(adminIndex);
    bool updated = false;

    for (JsonObject profile : profiles) {
        String listedChatId = profile["chatId"] | "";
        listedChatId.trim();
        String listedUid = profile["uid"] | "";
        listedUid.trim();
        listedUid.toUpperCase();

        if (listedChatId == chatId || listedUid == seededUid) {
            profile["chatId"] = chatId;
            profile["uid"] = seededUid;
            profile["name"] = displayName;
            updated = true;
            break;
        }
    }

    if (!updated) {
        JsonObject profile = profiles.add<JsonObject>();
        profile["chatId"] = chatId;
        profile["uid"] = seededUid;
        profile["name"] = displayName;
    }

    File writeFile = LittleFS.open("/users.json", "w");
    if (!writeFile) {
        return false;
    }

    const size_t written = serializeJson(usersDoc, writeFile);
    writeFile.close();
    return written > 0;
}

void appendSeededAdminProfilesToResponse(JsonDocument* responseDoc) {
    if (!responseDoc || !(*responseDoc)["users"].is<JsonArray>()) {
        return;
    }

    JsonArray users = (*responseDoc)["users"].as<JsonArray>();

    for (JsonObject user : users) {
        String chatId = user["telegramChatID"] | "";
        if (chatId.length() == 0) {
            chatId = user["chat_id"] | "";
        }
        chatId.trim();

        String uid = webserver_route_utils::normalizeUID(user["cardUID"] | "");
        if (uid.length() == 0) {
            uid = webserver_route_utils::normalizeUID(user["uid"] | "");
        }

        const bool seededByUid = uid.startsWith("DEFAULT_ADMIN");
        const bool seededByChat = isSeededAdminChatId(chatId);
        const bool seeded = seededByUid || seededByChat;

        String role = user["type"] | "";
        if (role.length() == 0) {
            role = user["role"] | "user";
        }
        role.trim();
        role.toLowerCase();

        if (seeded) {
            role = "admin";
            user["isAdminChat"] = true;
            user["isSeededAdmin"] = true;
            user["editable"] = true;
            user["deletable"] = false;
            user["source"] = "secrets.h";
        } else {
            user["isSeededAdmin"] = false;
            if (user["editable"].isNull()) {
                user["editable"] = true;
            }
            if (user["deletable"].isNull()) {
                user["deletable"] = true;
            }
        }

        user["type"] = role;
        user["role"] = role;
    }

    for (int i = 0; i < NUM_ADMINS; i++) {
        String chatId = ADMIN_CHAT_IDS[i];
        chatId.trim();
        if (chatId.length() == 0) {
            continue;
        }

        JsonObject matchedUser;
        for (JsonObject user : users) {
            String listedChatId = user["telegramChatID"] | "";
            if (listedChatId.length() == 0) {
                listedChatId = user["chat_id"] | "";
            }
            listedChatId.trim();
            if (listedChatId == chatId) {
                matchedUser = user;
                break;
            }
        }

        String displayName = defaultSeededAdminNameForIndex(i);
        String overriddenName = "";
        if (loadSeededAdminNameOverride(chatId, &overriddenName) && overriddenName.length() > 0) {
            displayName = overriddenName;
        }

        String firstName;
        String middleName;
        String lastName;
        splitNameParts(displayName, &firstName, &middleName, &lastName);

        if (!matchedUser.isNull()) {
            matchedUser["name"] = displayName;
            matchedUser["firstName"] = firstName;
            matchedUser["middleName"] = middleName;
            matchedUser["lastName"] = lastName;
            matchedUser["telegramChatID"] = chatId;
            matchedUser["chat_id"] = chatId;
            matchedUser["type"] = "admin";
            matchedUser["role"] = "admin";
            matchedUser["isAdminChat"] = true;
            matchedUser["isSeededAdmin"] = true;
            matchedUser["editable"] = true;
            matchedUser["deletable"] = false;
            matchedUser["source"] = "secrets.h";
            continue;
        }

        JsonObject seeded = users.add<JsonObject>();
        const String seededUid = seededAdminUidForIndex(i);
        seeded["uid"] = seededUid;
        seeded["cardUID"] = seededUid;
        seeded["name"] = displayName;
        seeded["firstName"] = firstName;
        seeded["middleName"] = middleName;
        seeded["lastName"] = lastName;
        seeded["telegramChatID"] = chatId;
        seeded["chat_id"] = chatId;
        seeded["backupPIN"] = "";
        seeded["type"] = "admin";
        seeded["role"] = "admin";
        seeded["isAdminChat"] = true;
        seeded["isSeededAdmin"] = true;
        seeded["editable"] = true;
        seeded["deletable"] = false;
        seeded["source"] = "secrets.h";
    }
}

}

void WebServer::_handleAPIUsers(AsyncWebServerRequest* request) {
    if (!_requireApiAuth(request)) {
        return;
    }

    JsonDocument responseDoc;
    _syncUsersFileFromAuth(&responseDoc);
    appendSeededAdminProfilesToResponse(&responseDoc);
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

    bool protectedAdmin = tryParseSeededAdminIndexFromUid(uid, nullptr);

    const String currentChatId = _auth->getUserTelegramChatId(uid);
    if (!protectedAdmin && isSeededAdminChatId(currentChatId)) {
        protectedAdmin = true;
    }

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
        doc["message"] = "Protected admin user cannot be deleted";
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

    int seededAdminsRetained = 0;
    for (int i = 0; i < NUM_ADMINS; i++) {
        String chatId = ADMIN_CHAT_IDS[i];
        chatId.trim();
        if (chatId.length() > 0) {
            seededAdminsRetained++;
        }
    }

    _addLogEntry("Admin (Web)", "Reset All Users", "success");

    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "All users reset";
    doc["removedUsers"] = before;
    doc["remainingAuthUsers"] = _auth->getUserCount();
    doc["seededAdminsRetained"] = seededAdminsRetained;
    doc["remainingUsers"] = _auth->getUserCount() + seededAdminsRetained;
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

    String name = "";
    String nameFieldError = "";
    String nameErrorMessage = "";

    if (!parseUserNamePayload(
        body,
        &name,
        nullptr,
        nullptr,
        nullptr,
        &nameFieldError,
        &nameErrorMessage
    )) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = nameFieldError;
        doc["message"] = nameErrorMessage;
        _sendJSON(request, 400, doc);
        return;
    }

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
    name = normalizedNameValue(name);
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
        doc["message"] = "Telegram Chat ID must be exactly 10 digits";
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
        doc["message"] = "Telegram Chat ID must be exactly 10 digits for OTP delivery";
        _sendJSON(request, 400, doc);
        return;
    }

    if (isSeededAdminChatId(telegramChatID)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["errorCode"] = "RESERVED_ADMIN_CHAT_ID";
        doc["field"] = "telegramChatID";
        doc["message"] = "This Telegram Chat ID is reserved for seeded admin access and cannot be enrolled as a regular user";
        _sendJSON(request, 403, doc);
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

    bool userNotified = false;
    if (added) {
        _security->beep(1);
        _syncUsersFileFromAuth();

        const String onboardingMessage =
            "✅ SecureLock enrollment complete\n"
            "• Name: " + name + "\n"
            "• RFID: Registered\n"
            "• Backup PIN: Active\n"
            "You can now use RFID + OTP (or Backup PIN fallback) for door access.";

        userNotified = enqueueTelegramUserNotification(telegramChatID, onboardingMessage);
        if (!userNotified) {
            _addLogEntry("Admin (Web)", "Add User Notify Failed (" + name + ")", "fail");
        }
    }

    _addLogEntry("Admin (Web)", "Add User (" + name + ")", added ? "success" : "fail");

    JsonDocument doc;
    doc["success"] = added;
    doc["message"] = added ? "User added" : (failureReason.length() ? failureReason : "Failed to add user");
    doc["userNotificationSent"] = userNotified;
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
    String name = "";
    String nameFieldError = "";
    String nameErrorMessage = "";

    if (!parseUserNamePayload(
        body,
        &name,
        nullptr,
        nullptr,
        nullptr,
        &nameFieldError,
        &nameErrorMessage
    )) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = nameFieldError;
        doc["message"] = nameErrorMessage;
        _sendJSON(request, 400, doc);
        return;
    }

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

    name = normalizedNameValue(name);
    pin.trim();
    uid = webserver_route_utils::normalizeUID(uid);
    rfid = webserver_route_utils::normalizeUID(rfid);
    telegramChatID.trim();
    backupPIN.trim();

    int seededAdminIndex = -1;
    const bool seededProfileRequested = body["seededAdminProfile"] | false;
    const bool seededUidMatch = tryParseSeededAdminIndexFromUid(uid, &seededAdminIndex);

    if (!seededUidMatch && seededProfileRequested && isSeededAdminChatId(telegramChatID)) {
        for (int i = 0; i < NUM_ADMINS; i++) {
            String seededChat = ADMIN_CHAT_IDS[i];
            seededChat.trim();
            if (seededChat.length() == 0) {
                continue;
            }

            if (seededChat == telegramChatID) {
                seededAdminIndex = i;
                break;
            }
        }
    }

    if (seededAdminIndex >= 0) {
        if (name.length() == 0) {
            JsonDocument doc;
            doc["success"] = false;
            doc["field"] = "name";
            doc["message"] = "Name is required";
            _sendJSON(request, 400, doc);
            return;
        }

        if (name.length() > webserver_limits::MAX_PERSON_NAME_LENGTH) {
            JsonDocument doc;
            doc["success"] = false;
            doc["field"] = "name";
            doc["message"] = "Name is too long (max 48 characters)";
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

        String configuredChatId = ADMIN_CHAT_IDS[seededAdminIndex];
        configuredChatId.trim();
        if (configuredChatId.length() == 0) {
            JsonDocument doc;
            doc["success"] = false;
            doc["message"] = "Seeded admin chat ID is not configured";
            _sendJSON(request, 500, doc);
            return;
        }

        if (telegramChatID.length() > 0 && telegramChatID != configuredChatId) {
            JsonDocument doc;
            doc["success"] = false;
            doc["field"] = "telegramChatID";
            doc["message"] = "Seeded admin chat ID is managed by secrets.h and cannot be changed from dashboard";
            _sendJSON(request, 403, doc);
            return;
        }

        const bool saved = saveSeededAdminNameOverride(seededAdminIndex, name);
        _addLogEntry("Admin (Web)", "Edit Seeded Admin Profile (" + name + ")", saved ? "success" : "fail");

        JsonDocument doc;
        doc["success"] = saved;
        doc["uid"] = seededAdminUidForIndex(seededAdminIndex);
        doc["telegramChatID"] = configuredChatId;
        doc["seededAdminProfile"] = true;
        doc["message"] = saved
            ? "Seeded admin profile updated"
            : "Failed to save seeded admin profile";

        _sendJSON(request, saved ? 200 : 500, doc);
        return;
    }

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
        doc["message"] = "Telegram Chat ID must be exactly 10 digits";
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
        doc["message"] = "Telegram Chat ID must be exactly 10 digits";
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

    if (isSeededAdminChatId(oldTelegramChatId)) {
        _addLogEntry("Admin (Web)", "Edit User Blocked (Seeded Admin)", "fail");

        JsonDocument doc;
        doc["success"] = false;
        doc["errorCode"] = "PROTECTED_ADMIN_USER";
        doc["message"] = "Seeded admin users are protected and cannot be edited from user management";
        _sendJSON(request, 403, doc);
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
        doc["message"] = "Telegram Chat ID must be exactly 10 digits";
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

    if (isSeededAdminChatId(effectiveTelegramChatId)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["errorCode"] = "RESERVED_ADMIN_CHAT_ID";
        doc["field"] = "telegramChatID";
        doc["message"] = "This Telegram Chat ID is reserved for seeded admin access and cannot be assigned to a regular user";
        _sendJSON(request, 403, doc);
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

    bool userNotified = false;
    if (effectiveTelegramChatId.length() > 0) {
        const String updateMessage =
            "ℹ️ Your SecureLock profile was updated by admin\n"
            "• Name: " + name + "\n"
            "• RFID: " + (uidChanged ? String("Updated") : String("Unchanged")) + "\n"
            "• Backup PIN: Active";

        userNotified = enqueueTelegramUserNotification(effectiveTelegramChatId, updateMessage);
        if (!userNotified) {
            _addLogEntry("Admin (Web)", "Edit User Notify Failed (" + name + ")", "fail");
        }
    }

    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "User updated";
    doc["uid"] = targetUid;
    doc["userNotificationSent"] = userNotified;

    _syncUsersFileFromAuth();
    _addLogEntry("Admin (Web)", uidChanged ? ("Edit User (RFID Replaced: " + name + ")") : ("Edit User (" + name + ")"), "success");
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

    const unsigned long nowMs = millis();
    const int queueDepth = getTelegramNotificationQueueDepth();
    const int queueCapacity = getTelegramNotificationQueueCapacity();
    doc["telegramNotifyQueueDepth"] = queueDepth;
    doc["telegramNotifyQueueCapacity"] = queueCapacity;
    doc["telegramNotifyQueueSaturationPct"] = (queueCapacity > 0)
        ? ((queueDepth * 100) / queueCapacity)
        : 0;
    doc["telegramNotifyQueueOldestAgeMs"] = getTelegramNotificationQueueOldestAgeMs();
    doc["telegramNotifyQueuedTotal"] = getTelegramNotificationQueuedTotal();
    doc["telegramNotifyDeliveredTotal"] = getTelegramNotificationDeliveredTotal();
    doc["telegramNotifyDeliveryFailures"] = getTelegramNotificationDeliveryFailures();
    doc["telegramNotifyDroppedFullTotal"] = getTelegramNotificationDroppedFullTotal();
    doc["telegramNotifyDroppedRetryTotal"] = getTelegramNotificationDroppedRetryTotal();

    JsonArray adminDelivery = doc["telegramAdminDelivery"].to<JsonArray>();
    const int adminSlots = getTelegramAdminMetricsSlots();
    for (int i = 0; i < adminSlots; i++) {
        JsonObject admin = adminDelivery.add<JsonObject>();
        const unsigned long lastSuccessMs = getTelegramAdminLastSuccessMsAt(i);
        const unsigned long lastFailureMs = getTelegramAdminLastFailureMsAt(i);
        admin["index"] = i;
        admin["chatId"] = getTelegramAdminChatIdAt(i);
        admin["attempts"] = getTelegramAdminSendAttemptsAt(i);
        admin["success"] = getTelegramAdminSendSuccessAt(i);
        admin["failures"] = getTelegramAdminSendFailuresAt(i);
        admin["lastSuccessMs"] = lastSuccessMs;
        admin["lastFailureMs"] = lastFailureMs;
        admin["lastSuccessAgeMs"] = (lastSuccessMs > 0) ? static_cast<long>(nowMs - lastSuccessMs) : -1;
        admin["lastFailureAgeMs"] = (lastFailureMs > 0) ? static_cast<long>(nowMs - lastFailureMs) : -1;
    }

    doc["timestamp"] = millis();
    _sendJSON(request, 200, doc);
}
