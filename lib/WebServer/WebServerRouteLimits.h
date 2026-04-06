#ifndef WEB_SERVER_ROUTE_LIMITS_H
#define WEB_SERVER_ROUTE_LIMITS_H

#include <stddef.h>

namespace webserver_limits {
constexpr size_t MAX_AUTH_PAYLOAD_BYTES = 512;
constexpr size_t MAX_USER_PAYLOAD_BYTES = 1536;
constexpr size_t MAX_RESET_PAYLOAD_BYTES = 256;
constexpr size_t MAX_ADMIN_CREDENTIAL_LENGTH = 64;
constexpr size_t MAX_PERSON_NAME_LENGTH = 48;
constexpr size_t MAX_TELEGRAM_CHAT_ID_LENGTH = 10;
}

#endif // WEB_SERVER_ROUTE_LIMITS_H
