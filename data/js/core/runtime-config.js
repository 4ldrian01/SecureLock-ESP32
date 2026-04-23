(function initializeSecureLockRuntimeConfig() {
    const STORAGE_KEY = 'securelock_api_base_url';

    function normalizeBaseUrl(value) {
        const raw = String(value || '').trim();
        if (!raw) {
            return '';
        }

        return raw.replace(/\/+$/, '');
    }

    function readMetaDefault() {
        const meta = document.querySelector('meta[name="securelock-api-base-url"]');
        return meta ? String(meta.getAttribute('content') || '').trim() : '';
    }

    function readQueryParam() {
        try {
            const url = new URL(window.location.href);
            return String(url.searchParams.get('apiBase') || '').trim();
        } catch {
            return '';
        }
    }

    const queryBase = normalizeBaseUrl(readQueryParam());
    const storedBase = normalizeBaseUrl(window.localStorage.getItem(STORAGE_KEY));
    const metaBase = normalizeBaseUrl(readMetaDefault());

    const resolvedBase = queryBase || storedBase || metaBase;

    if (queryBase) {
        window.localStorage.setItem(STORAGE_KEY, queryBase);
    }

    window.SECURELOCK_API_BASE_URL = resolvedBase;
})();
