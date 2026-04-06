const DEFAULT_SESSION_KEY = 'securelock_admin_api_session_v2';

function safeParse(value, fallback) {
    if (!value) {
        return fallback;
    }

    try {
        return JSON.parse(value);
    } catch {
        return fallback;
    }
}

function nowMs() {
    return Date.now();
}

function toTwoDigits(value) {
    return String(Math.max(0, value)).padStart(2, '0');
}

function formatLockout(msRemaining) {
    const totalSec = Math.ceil(Math.max(0, msRemaining) / 1000);
    const min = Math.floor(totalSec / 60);
    const sec = totalSec % 60;
    return `${toTwoDigits(min)}:${toTwoDigits(sec)}`;
}

export function createAuthFeature({
    CONFIG,
    DOM,
    feedback,
    apiFetch,
    setApiAuthToken,
    clearApiAuthToken,
    onAuthenticated,
    onLogout
}) {
    let authenticated = false;
    let sessionTimer = null;
    let lockoutTimer = null;
    let preAuthHealthTimer = null;
    let lockoutUntilMs = 0;

    const authConfig = CONFIG.ADMIN_AUTH || {};
    const SESSION_TTL_MS = Number(authConfig.SESSION_TTL_MS || (15 * 60 * 1000));
    const SESSION_KEY = String(CONFIG.AUTH_SESSION_KEY || DEFAULT_SESSION_KEY);

    function readSession() {
        return safeParse(sessionStorage.getItem(SESSION_KEY), null);
    }

    function writeSession(session) {
        sessionStorage.setItem(SESSION_KEY, JSON.stringify(session));
    }

    function clearSession() {
        sessionStorage.removeItem(SESSION_KEY);
    }

    function isSessionValid(session) {
        if (!session || typeof session !== 'object') {
            return false;
        }

        const token = String(session.token || '').trim();
        const expiresAt = Number(session.expiresAt || 0);
        return token.length > 0 && Number.isFinite(expiresAt) && expiresAt > nowMs();
    }

    function setAuthenticatedUI(isAuthenticated, options = {}) {
        const reachable = (typeof options.reachable === 'boolean') ? options.reachable : true;

        document.body.dataset.authenticated = isAuthenticated ? 'true' : 'false';
        DOM.authOverlay.dataset.visible = isAuthenticated ? 'false' : 'true';
        DOM.authOverlay.hidden = Boolean(isAuthenticated);
        DOM.btnLogout.hidden = !isAuthenticated;

        DOM.statusBadge.classList.remove('badge-success', 'badge-danger', 'badge-warning');

        if (isAuthenticated) {
            DOM.statusBadge.dataset.status = 'online';
            DOM.statusBadge.classList.add('badge-success');
            return;
        }

        DOM.statusBadge.dataset.status = reachable ? 'online' : 'offline';
        DOM.statusBadge.classList.add(reachable ? 'badge-warning' : 'badge-danger');
        DOM.statusText.textContent = reachable ? 'Login Required' : 'Offline';
    }

    async function probeBackendReachability() {
        try {
            await apiFetch(CONFIG.API.AUTH_STATUS, {
                method: 'GET',
                skipAuthHandling: true,
                timeoutMs: 5000
            });
            if (!authenticated) {
                setAuthenticatedUI(false, { reachable: true });
            }
            return true;
        } catch {
            if (!authenticated) {
                setAuthenticatedUI(false, { reachable: false });
            }
            return false;
        }
    }

    function startPreAuthHealthPolling() {
        if (preAuthHealthTimer) {
            clearInterval(preAuthHealthTimer);
            preAuthHealthTimer = null;
        }

        preAuthHealthTimer = setInterval(() => {
            if (authenticated) {
                return;
            }
            probeBackendReachability();
        }, 10000);
    }

    function stopPreAuthHealthPolling() {
        if (preAuthHealthTimer) {
            clearInterval(preAuthHealthTimer);
            preAuthHealthTimer = null;
        }
    }

    function getLockoutRemainingMs() {
        return Math.max(0, Number(lockoutUntilMs || 0) - nowMs());
    }

    function updateLockoutUI() {
        const remainingMs = getLockoutRemainingMs();
        if (remainingMs <= 0) {
            DOM.adminLockoutMessage.textContent = '';
            DOM.btnAdminLogin.disabled = false;
            return;
        }

        DOM.adminLockoutMessage.textContent =
            `Too many failed attempts. Try again in ${formatLockout(remainingMs)}.`;
        DOM.btnAdminLogin.disabled = true;
    }

    function startLockoutTimer() {
        if (lockoutTimer) {
            clearInterval(lockoutTimer);
        }

        lockoutTimer = setInterval(updateLockoutUI, 1000);
        updateLockoutUI();
    }

    function stopLockoutTimer() {
        if (lockoutTimer) {
            clearInterval(lockoutTimer);
            lockoutTimer = null;
        }
    }

    function startSessionTimer() {
        if (sessionTimer) {
            clearInterval(sessionTimer);
        }

        sessionTimer = setInterval(() => {
            if (!authenticated) {
                return;
            }

            const session = readSession();
            if (!isSessionValid(session)) {
                forceLogout('Session expired. Please login again.');
            }
        }, 1000);
    }

    function stopSessionTimer() {
        if (sessionTimer) {
            clearInterval(sessionTimer);
            sessionTimer = null;
        }
    }

    function beginAuthenticatedSession({ token, expiresInMs }) {
        const now = nowMs();
        const ttl = Math.max(1000, Number(expiresInMs || SESSION_TTL_MS));
        const session = {
            token: String(token || '').trim(),
            issuedAt: now,
            expiresAt: now + ttl
        };

        if (!session.token) {
            return false;
        }

        authenticated = true;
        lockoutUntilMs = 0;
        stopPreAuthHealthPolling();
        setApiAuthToken(session.token);
        writeSession(session);

        DOM.adminLoginError.textContent = '';
        DOM.adminLockoutMessage.textContent = '';
        DOM.adminLoginPassword.value = '';

        setAuthenticatedUI(true);
        startSessionTimer();

        if (typeof onAuthenticated === 'function') {
            onAuthenticated();
        }

        return true;
    }

    async function notifyBackendLogout(token) {
        const safeToken = String(token || '').trim();
        if (!safeToken) {
            return;
        }

        try {
            await apiFetch(CONFIG.API.AUTH_LOGOUT, {
                method: 'POST',
                headers: {
                    Authorization: `Bearer ${safeToken}`
                },
                skipAuthHandling: true,
                timeoutMs: 5000
            });
        } catch {
            // Best-effort logout. Local session is still cleared below.
        }
    }

    async function forceLogout(reason, options = {}) {
        const { skipBackendLogout = false } = options;
        const previousSession = readSession();
        const previousToken = String(previousSession?.token || '').trim();
        const wasAuthenticated = authenticated;

        authenticated = false;
        stopSessionTimer();

        if (!skipBackendLogout) {
            await notifyBackendLogout(previousToken);
        }

        clearApiAuthToken();
        clearSession();
        setAuthenticatedUI(false, { reachable: true });
        startPreAuthHealthPolling();
        DOM.adminLoginPassword.value = '';
        DOM.adminLoginError.textContent = reason || '';
        updateLockoutUI();

        if (wasAuthenticated && typeof onLogout === 'function') {
            onLogout();
        }

        DOM.adminLoginUsername.focus();
    }

    async function handleLoginSubmit(event) {
        event.preventDefault();
        updateLockoutUI();

        const lockoutRemainingMs = getLockoutRemainingMs();
        if (lockoutRemainingMs > 0) {
            DOM.adminLoginError.textContent = 'Login is temporarily locked.';
            return;
        }

        const username = DOM.adminLoginUsername.value.trim();
        const password = DOM.adminLoginPassword.value;

        DOM.adminLoginError.textContent = '';

        if (!username || !password) {
            DOM.adminLoginError.textContent = 'Enter admin username and password.';
            return;
        }

        DOM.btnAdminLogin.disabled = true;
        DOM.btnAdminLogin.textContent = 'Verifying...';
        DOM.btnAdminLogin.dataset.state = 'busy';

        try {
            const data = await apiFetch(CONFIG.API.AUTH_LOGIN, {
                method: 'POST',
                body: JSON.stringify({ username, password }),
                skipAuthHandling: true
            });

            if (data?.success && data?.authenticated && data?.token) {
                const started = beginAuthenticatedSession({
                    token: data.token,
                    expiresInMs: data.expiresInMs
                });

                if (started) {
                    feedback.showToast('Admin login successful', 'success');
                    return;
                }
            }

            DOM.adminLoginError.textContent = data?.message || 'Invalid credentials.';
        } catch (error) {
            const status = Number(error?.status || 0);
            const payload = error?.payload || {};

            if (status === 429) {
                const retryAfterMs = Number(payload.retryAfterMs || (payload.retryAfterSec * 1000) || 0);
                if (retryAfterMs > 0) {
                    lockoutUntilMs = nowMs() + retryAfterMs;
                }

                DOM.adminLoginError.textContent = payload.message || 'Too many failed attempts. Login temporarily locked.';
                updateLockoutUI();
                return;
            }

            if (status === 401) {
                const attemptsRemaining = Number(payload.attemptsRemaining);
                if (Number.isFinite(attemptsRemaining) && attemptsRemaining >= 0) {
                    DOM.adminLoginError.textContent = `Invalid credentials. ${attemptsRemaining} attempt(s) left.`;
                    return;
                }
            }

            DOM.adminLoginError.textContent = payload.message || error?.message || 'Unable to verify login right now. Try again.';
            probeBackendReachability();
        } finally {
            DOM.btnAdminLogin.textContent = 'Login';
            DOM.btnAdminLogin.dataset.state = 'ready';
            updateLockoutUI();
        }
    }

    async function restoreSession() {
        const session = readSession();
        if (!isSessionValid(session)) {
            clearApiAuthToken();
            clearSession();
            setAuthenticatedUI(false, { reachable: true });
            return false;
        }

        const token = String(session.token || '').trim();
        setApiAuthToken(token);

        try {
            const status = await apiFetch(CONFIG.API.AUTH_STATUS, {
                headers: {
                    Authorization: `Bearer ${token}`
                },
                skipAuthHandling: true,
                timeoutMs: 5000
            });

            if (!status?.authenticated) {
                clearApiAuthToken();
                clearSession();
                setAuthenticatedUI(false, { reachable: true });
                return false;
            }

            return beginAuthenticatedSession({
                token,
                expiresInMs: status?.expiresInMs || SESSION_TTL_MS
            });
        } catch {
            clearApiAuthToken();
            clearSession();
            setAuthenticatedUI(false, { reachable: false });
            return false;
        }
    }

    function bindEvents() {
        DOM.adminLoginForm.addEventListener('submit', handleLoginSubmit);
        DOM.btnLogout.addEventListener('click', async () => {
            await forceLogout('Logged out. Please login again.');
            feedback.showToast('Logged out', 'info');
        });
    }

    function initialize() {
        startLockoutTimer();
        setAuthenticatedUI(false, { reachable: true });
        startPreAuthHealthPolling();

        restoreSession().then((restored) => {
            if (!restored) {
                probeBackendReachability();
                DOM.adminLoginUsername.focus();
            }
        });
    }

    async function handleUnauthorized() {
        if (!authenticated && !readSession()) {
            return;
        }

        await forceLogout('Session expired. Please login again.', { skipBackendLogout: true });
    }

    return {
        bindEvents,
        initialize,
        isAuthenticated: () => authenticated,
        logout: forceLogout,
        handleUnauthorized,
        dispose: () => {
            stopSessionTimer();
            stopLockoutTimer();
            stopPreAuthHealthPolling();
        }
    };
}
