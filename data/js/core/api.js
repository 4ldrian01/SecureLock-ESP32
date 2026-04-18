let apiAuthToken = '';
let unauthorizedHandler = null;
let unauthorizedInFlight = false;
const inFlightGetRequests = new Map();

export function setApiAuthToken(token) {
    apiAuthToken = String(token || '').trim();
}

export function clearApiAuthToken() {
    apiAuthToken = '';
}

export function setApiUnauthorizedHandler(handler) {
    unauthorizedHandler = typeof handler === 'function' ? handler : null;
}

function wait(ms) {
    return new Promise((resolve) => {
        setTimeout(resolve, Math.max(0, Number(ms) || 0));
    });
}

function normalizeMethod(method) {
    return String(method || 'GET').trim().toUpperCase() || 'GET';
}

function isLikelyNetworkError(error) {
    const message = String(error?.message || '').toLowerCase();
    return message.includes('failed to fetch')
        || message.includes('networkerror')
        || message.includes('load failed')
        || message.includes('request timed out');
}

function shouldRetryRequest(method, error, attempt, maxRetries) {
    if (method !== 'GET' || attempt >= maxRetries) {
        return false;
    }

    const status = Number(error?.status || 0);
    if (status === 401 || status === 403 || status === 404 || status === 422) {
        return false;
    }

    if (status === 0 || status === 408 || status === 425 || status === 429
        || status === 500 || status === 502 || status === 503 || status === 504) {
        return true;
    }

    return isLikelyNetworkError(error);
}

function computeRetryDelayMs(baseDelayMs, maxDelayMs, attempt) {
    const base = Math.max(60, Number(baseDelayMs) || 170);
    const cap = Math.max(base, Number(maxDelayMs) || 700);
    const exponential = Math.min(cap, Math.round(base * Math.pow(1.75, Math.max(0, attempt))));
    const jitter = Math.round(exponential * ((Math.random() * 0.2) - 0.1));
    return Math.max(40, exponential + jitter);
}

function getRequestCacheMode(method, options) {
    if (options && typeof options.cache === 'string' && options.cache.length > 0) {
        return options.cache;
    }

    return method === 'GET' ? 'no-cache' : 'no-store';
}

function buildGetDedupeKey(url, method, headers, options = {}) {
    const auth = String(headers?.Authorization || '').trim();
    const customKey = String(options.dedupeKey || '').trim();
    if (customKey) {
        return `${method}:${customKey}:${auth}`;
    }

    return `${method}:${String(url || '')}:${auth}`;
}

function shouldHandleUnauthorized(url, options = {}) {
    if (options.skipAuthHandling) {
        return false;
    }

    const endpoint = String(url || '');
    if (endpoint.includes('/api/auth/login')) {
        return false;
    }

    return Boolean(apiAuthToken);
}

function triggerUnauthorized(error, context) {
    if (!unauthorizedHandler || unauthorizedInFlight) {
        return;
    }

    unauthorizedInFlight = true;
    Promise.resolve(unauthorizedHandler(error, context))
        .catch(() => {
            // No-op: best-effort session invalidation hook.
        })
        .finally(() => {
            unauthorizedInFlight = false;
        });
}

export async function apiFetch(url, options = {}) {
    const method = normalizeMethod(options.method);
    const headers = {
        'Content-Type': 'application/json',
        ...(options.headers || {})
    };

    if (!headers.Authorization && apiAuthToken) {
        headers.Authorization = `Bearer ${apiAuthToken}`;
    }

    // Avoid forcing JSON content-type for FormData uploads.
    if (typeof FormData !== 'undefined' && options.body instanceof FormData) {
        delete headers['Content-Type'];
    }

    if (method === 'GET' || method === 'HEAD') {
        delete headers['Content-Type'];
    }

    const defaultRetryCount = Number(options.defaultRetryCount || 0);
    const maxRetries = Number.isFinite(Number(options.retries))
        ? Math.max(0, Number(options.retries))
        : Math.max(0, defaultRetryCount);
    const retryBaseDelayMs = Number(options.retryBaseDelayMs || 170);
    const retryMaxDelayMs = Number(options.retryMaxDelayMs || 700);

    const {
        timeoutMs: _timeoutMs,
        retries: _retries,
        defaultRetryCount: _defaultRetryCount,
        retryBaseDelayMs: _retryBaseDelayMs,
        retryMaxDelayMs: _retryMaxDelayMs,
        dedupeKey: _dedupeKey,
        disableRequestDedupe: _disableRequestDedupe,
        skipAuthHandling: _skipAuthHandling,
        ...fetchOptions
    } = options;

    const executeRequest = async () => {
        let lastError = null;

        for (let attempt = 0; attempt <= maxRetries; attempt += 1) {
            const controller = new AbortController();
            const timeoutMs = Math.max(250, Number(options.timeoutMs || 10000));
            const timeoutHandle = setTimeout(() => controller.abort(), timeoutMs);

            try {
                const response = await fetch(url, {
                    ...fetchOptions,
                    method,
                    headers,
                    cache: getRequestCacheMode(method, options),
                    credentials: 'same-origin',
                    signal: controller.signal
                });

                const contentType = response.headers.get('content-type') || '';
                let payload = {};
                if (contentType.includes('application/json')) {
                    try {
                        payload = await response.json();
                    } catch {
                        payload = {};
                    }
                } else {
                    const textPayload = await response.text();
                    payload = { message: textPayload };
                }

                if (!response.ok) {
                    const error = new Error(payload.message || `HTTP ${response.status}`);
                    error.status = response.status;
                    error.payload = payload;

                    if (response.status === 401 && shouldHandleUnauthorized(url, options)) {
                        triggerUnauthorized(error, { url, method });
                    }

                    throw error;
                }

                return payload;
            } catch (error) {
                let normalizedError = error;

                if (error?.name === 'AbortError') {
                    normalizedError = new Error('Request timed out');
                    normalizedError.status = 408;
                    normalizedError.payload = { message: 'Request timed out' };
                }

                lastError = normalizedError;
                const retryable = shouldRetryRequest(method, normalizedError, attempt, maxRetries);
                if (!retryable) {
                    throw normalizedError;
                }

                const retryDelayMs = computeRetryDelayMs(retryBaseDelayMs, retryMaxDelayMs, attempt);
                await wait(retryDelayMs);
            } finally {
                clearTimeout(timeoutHandle);
            }
        }

        throw lastError || new Error('Request failed');
    };

    const dedupeEligible = method === 'GET' && !Boolean(options.disableRequestDedupe);
    if (dedupeEligible) {
        const dedupeKey = buildGetDedupeKey(url, method, headers, options);
        const existing = inFlightGetRequests.get(dedupeKey);
        if (existing) {
            return existing;
        }

        const requestPromise = executeRequest()
            .catch((error) => {
                console.error(`[API] ${method} ${url} failed:`, error?.message || 'Unknown error');
                throw error;
            })
            .finally(() => {
                inFlightGetRequests.delete(dedupeKey);
            });

        inFlightGetRequests.set(dedupeKey, requestPromise);
        return requestPromise;
    }

    try {
        return await executeRequest();
    } catch (error) {
        console.error(`[API] ${method} ${url} failed:`, error?.message || 'Unknown error');
        throw error;
    }
}
