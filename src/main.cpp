#include "app/Application.h"

static securelock::app::Application app;

void setup() {
    app.setup();
}

void loop() {
    app.loop();
}
