#include "WebServer.h"

namespace {
constexpr bool kVerboseHttpLogs = false;
}

void WebServer::_handleRoot(AsyncWebServerRequest* request) {
    const char* candidates[] = {
        "/html/pages/dashboard.html",
        "/html/index.html",
        "/index.html"
    };

    for (const char* path : candidates) {
        if (_serveStaticAsset(request, path, false)) {
            if (kVerboseHttpLogs) {
                Serial.print("[WEB] GET / -> ");
                Serial.println(path);
            }
            return;
        }
    }

    Serial.println("[WEB] ✗ dashboard HTML not found in LittleFS!");
    request->send(404, "text/html",
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>SecureLock Dashboard Missing</title></head><body>"
        "<h2>SecureLock Dashboard files are missing</h2>"
        "<p>Upload LittleFS data first, then reload this page.</p>"
        "<pre>platformio run --target uploadfs</pre>"
        "<p>Expected file: <code>data/html/pages/dashboard.html</code></p>"
        "</body></html>");
}

void WebServer::_handleCSS(AsyncWebServerRequest* request) {
    if (_serveStaticAsset(request, "/css/style.css", true)) {
        if (kVerboseHttpLogs) {
            Serial.println("[WEB] GET /css/style.css -> OK");
        }
        return;
    }

    if (_serveStaticAsset(request, "/style.css", true)) {
        if (kVerboseHttpLogs) {
            Serial.println("[WEB] GET /css/style.css -> fallback /style.css");
        }
        return;
    }

    Serial.println("[WEB] ✗ style.css not found!");
    request->send(404, "text/plain", "CSS not found");
}

void WebServer::_handleNotFound(AsyncWebServerRequest* request) {
    const String url = request->url();
    String staticPath = url;

    const int queryPos = staticPath.indexOf('?');
    if (queryPos >= 0) {
        staticPath = staticPath.substring(0, queryPos);
    }

    const int fragmentPos = staticPath.indexOf('#');
    if (fragmentPos >= 0) {
        staticPath = staticPath.substring(0, fragmentPos);
    }

    if (!staticPath.startsWith("/api/")) {
        const bool spaAliasRoute = staticPath == "/login"
            || staticPath == "/login/"
            || staticPath == "/dashboard"
            || staticPath == "/dashboard/"
            || staticPath.startsWith("/dashboard/");

        if (spaAliasRoute) {
            _handleRoot(request);
            if (kVerboseHttpLogs) {
                Serial.print("[WEB] SPA alias route -> dashboard HTML: ");
                Serial.println(staticPath);
            }
            return;
        }
    }

    if (!staticPath.startsWith("/api/")) {
        const bool noCacheUiAsset = staticPath.endsWith(".html");
        if (_serveStaticAsset(request, staticPath, !noCacheUiAsset)) {
            if (kVerboseHttpLogs) {
                Serial.print("[WEB] Static fallback served: ");
                Serial.println(staticPath);
            }
            return;
        }
    }

    Serial.print("[WEB] 404: ");
    Serial.println(url);

    request->send(404, "text/plain", "404 - Not Found");
}

bool WebServer::_clientAcceptsGzip(AsyncWebServerRequest* request) const {
    if (!request || !request->hasHeader("Accept-Encoding")) {
        return false;
    }

    String acceptEncoding = request->header("Accept-Encoding");
    acceptEncoding.toLowerCase();
    return acceptEncoding.indexOf("gzip") >= 0;
}

bool WebServer::_serveStaticAsset(AsyncWebServerRequest* request, const String& path, bool cacheable) {
    if (!request || path.length() == 0) {
        return false;
    }

    const String mimeType = _getMimeType(path);
    const String gzipPath = path + ".gz";
    const bool acceptsGzip = _clientAcceptsGzip(request);
    const bool plainExists = LittleFS.exists(path);
    const bool gzipExists = LittleFS.exists(gzipPath);

    if (!plainExists && !gzipExists) {
        return false;
    }

    auto sendFile = [&](const String& targetPath, bool compressed) -> bool {
        AsyncWebServerResponse* response = request->beginResponse(LittleFS, targetPath, mimeType);
        if (!response) {
            return false;
        }

        if (compressed) {
            response->addHeader("Content-Encoding", "gzip");
        }

        // Ensure downstream caches vary on encoding when gzip/plain variants exist.
        // Cacheable paths already get this header via _addStaticCacheHeaders.
        if (!cacheable && gzipExists) {
            response->addHeader("Vary", "Accept-Encoding");
        }

        if (cacheable) {
            _addStaticCacheHeaders(response);
        } else {
            _addNoCacheHeaders(response);
        }

        request->send(response);
        return true;
    };

    if (acceptsGzip && gzipExists && sendFile(gzipPath, true)) {
        return true;
    }

    if (plainExists && sendFile(path, false)) {
        return true;
    }

    return gzipExists && sendFile(gzipPath, true);
}

String WebServer::_getMimeType(const String& filename) {
    if (filename.endsWith(".html")) return "text/html";
    if (filename.endsWith(".css"))  return "text/css";
    if (filename.endsWith(".js"))   return "application/javascript";
    if (filename.endsWith(".json")) return "application/json";
    if (filename.endsWith(".png"))  return "image/png";
    if (filename.endsWith(".jpg"))  return "image/jpeg";
    if (filename.endsWith(".ico"))  return "image/x-icon";
    if (filename.endsWith(".svg"))  return "image/svg+xml";
    return "text/plain";
}
