#ifndef WEB_SERVER_ROUTE_UTILS_H
#define WEB_SERVER_ROUTE_UTILS_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ctype.h>
#include "AuthHandler.h"

namespace webserver_route_utils {

inline String normalizeUID(const String& input) {
    String uid = input;
    uid.trim();
    uid.toUpperCase();
    uid.replace(" ", "");
    uid.replace(":", "");
    uid.replace("-", "");
    return uid;
}

inline bool isValidPersonName(const String& input) {
    String name = input;
    name.trim();

    if (name.length() == 0) {
        return false;
    }

    bool hasLetter = false;
    for (size_t i = 0; i < name.length(); i++) {
        const char c = name.charAt(i);
        if (c == ' ' || c == '-' || c == '\'' || c == '.') {
            continue;
        }

        if (!isalpha(static_cast<unsigned char>(c))) {
            return false;
        }

        hasLetter = true;
    }

    return hasLetter;
}

inline bool isValidUID(const String& uid) {
    if (uid.length() < 8 || uid.length() > 20) {
        return false;
    }

    if ((uid.length() % 2) != 0) {
        return false;
    }

    for (size_t i = 0; i < uid.length(); i++) {
        const char c = uid.charAt(i);
        const bool hex = (c >= '0' && c <= '9')
            || (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }

    return true;
}

inline bool isFourDigitCode(const String& code) {
    if (code.length() != 4) {
        return false;
    }

    for (size_t i = 0; i < code.length(); i++) {
        if (!isDigit(code.charAt(i))) {
            return false;
        }
    }

    return true;
}

inline bool isValidTelegramChatId(const String& chatId) {
    String value = chatId;
    value.trim();
    if (value.length() != 10) {
        return false;
    }

    for (size_t i = 0; i < value.length(); i++) {
        if (!isDigit(value.charAt(i))) {
            return false;
        }
    }

    return true;
}

inline bool isTelegramChatIdInUse(AuthHandler* auth, const String& chatId, const String& excludeUid = "") {
    if (!auth) {
        return false;
    }

    String normalizedChatId = chatId;
    normalizedChatId.trim();
    if (normalizedChatId.length() == 0) {
        return false;
    }

    String normalizedExcludeUid = normalizeUID(excludeUid);

    const int userCount = auth->getUserCount();
    for (int i = 0; i < userCount; i++) {
        String uid = normalizeUID(auth->getUserUIDAt(i));
        if (uid.length() == 0 || uid == normalizedExcludeUid) {
            continue;
        }

        String listedChatId = auth->getUserTelegramChatId(uid);
        listedChatId.trim();
        if (listedChatId == normalizedChatId) {
            return true;
        }
    }

    return false;
}

inline String* getOrCreateRequestBody(AsyncWebServerRequest* request, size_t totalLen) {
    String* body = reinterpret_cast<String*>(request->_tempObject);
    if (!body) {
        body = new String();
        body->reserve(totalLen);
        request->_tempObject = body;
    }
    return body;
}

inline void releaseRequestBody(AsyncWebServerRequest* request) {
    String* body = reinterpret_cast<String*>(request->_tempObject);
    if (body) {
        delete body;
        request->_tempObject = nullptr;
    }
}

} // namespace webserver_route_utils

#endif // WEB_SERVER_ROUTE_UTILS_H
