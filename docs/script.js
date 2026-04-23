/**
 * Root-level legacy fallback for very old cached HTML.
 *
 * Uses classic-script-safe dynamic module injection.
 */
(function bootstrapLegacyRootScript() {
	if (window.__secureLockModularBootstrapped) {
		return;
	}
	window.__secureLockModularBootstrapped = true;

	const moduleScript = document.createElement('script');
	moduleScript.type = 'module';
	moduleScript.src = '/js/entry/main.js';
	document.head.appendChild(moduleScript);
})();
