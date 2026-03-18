/**
 * Legacy compatibility entrypoint.
 *
 * This file intentionally stays at /js/script.js so older cached dashboards
 * can still bootstrap the app while the codebase uses /js/main.js modules.
 */
(function bootstrapModularApp() {
    if (window.__secureLockModularBootstrapped) {
        return;
    }
    window.__secureLockModularBootstrapped = true;

    const moduleScript = document.createElement('script');
    moduleScript.type = 'module';
    moduleScript.src = '/js/main.js';
    document.head.appendChild(moduleScript);
})();
