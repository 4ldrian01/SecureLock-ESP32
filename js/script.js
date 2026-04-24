/**
 * Legacy compatibility entrypoint.
 *
 * Keeps older cached dashboards working while canonical bootstrap is /js/entry/main.js.
 */
(function bootstrapModularApp() {
    if (window.__secureLockModularBootstrapped) {
        return;
    }
    window.__secureLockModularBootstrapped = true;

    const moduleScript = document.createElement('script');
    moduleScript.type = 'module';
    moduleScript.src = '/js/entry/main.js';
    document.head.appendChild(moduleScript);
})();
