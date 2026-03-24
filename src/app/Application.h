#ifndef APP_APPLICATION_H
#define APP_APPLICATION_H

#include <Arduino.h>
#include <LockManager.h>
#include <SecurityManager.h>
#include <AuthHandler.h>
#include <WebServer.h>
#include "controllers/WiFiController.h"

namespace securelock {
namespace app {

class Application {
public:
    Application();

    void setup();
    void loop();

    // WebServer status bridge accessors
    String getActiveGuestCode() const;
    bool isTemporaryGuestCodeActive() const;
    unsigned long getTemporaryGuestCodeRemainingMs() const;
    unsigned long getGuestCodeCommandCooldownRemainingMs() const;
    String getAuthPrompt() const;
    bool isPendingAccessActive() const;
    String getLastKeypadKeyLabel() const;
    unsigned long getLastKeypadKeyMs() const;

    unsigned long getTelegramPollIntervalMs() const;
    unsigned long getTelegramLastPollDurationMs() const;
    unsigned long getTelegramLastSuccessMs() const;
    unsigned long getTelegramLastErrorMs() const;
    unsigned long getTelegramLastCommandMs() const;
    unsigned long getTelegramLastCommandLatencyMs() const;
    unsigned long getTelegramCommandsHandled() const;
    unsigned long getTelegramPollErrors() const;
    int getTelegramPendingApprox() const;
    String getTelegramLastCommandText() const;
    String getTelegramLastCommandRole() const;
    String getTelegramLastCommandResult() const;

private:
    LockManager _lockManager;
    SecurityManager _securityManager;
    AuthHandler _authHandler;
    WebServer _webServer;
    WiFiController _wifiController;

    String _keypadBuffer;
    String _authPrompt;
    char _lastKeypadKey;
    unsigned long _lastKeypadKeyMs;

    void _processRFID();
    void _processKeypad();
};

} // namespace app
} // namespace securelock

#endif // APP_APPLICATION_H
