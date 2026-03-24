export async function apiFetch(url, options = {}) {
    const method = options.method || 'GET';
    const headers = {
        'Content-Type': 'application/json',
        ...(options.headers || {})
    };

    // Avoid forcing JSON content-type for FormData uploads.
    if (typeof FormData !== 'undefined' && options.body instanceof FormData) {
        delete headers['Content-Type'];
    }

    const controller = new AbortController();
    const timeoutMs = Number(options.timeoutMs || 10000);
    const timeoutHandle = setTimeout(() => controller.abort(), timeoutMs);

    try {
        const response = await fetch(url, {
            ...options,
            method,
            headers,
            cache: 'no-store',
            credentials: 'same-origin',
            signal: controller.signal
        });

        const contentType = response.headers.get('content-type') || '';
        const payload = contentType.includes('application/json')
            ? await response.json()
            : { message: await response.text() };

        if (!response.ok) {
            const error = new Error(payload.message || `HTTP ${response.status}`);
            error.status = response.status;
            error.payload = payload;
            throw error;
        }

        return payload;
    } catch (error) {
        if (error?.name === 'AbortError') {
            const timeoutError = new Error('Request timed out');
            timeoutError.status = 408;
            timeoutError.payload = { message: 'Request timed out' };
            console.error(`[API] ${method} ${url} failed:`, timeoutError.message);
            throw timeoutError;
        }

        console.error(`[API] ${method} ${url} failed:`, error.message);
        throw error;
    } finally {
        clearTimeout(timeoutHandle);
    }
}
