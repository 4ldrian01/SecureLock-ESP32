const SESSION_KEY = 'securelock_admin_session_v1';
const LOCKOUT_KEY = 'securelock_admin_lockout_v1';

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

async function sha256Hex(input) {
    const data = new TextEncoder().encode(String(input || ''));
    const digest = await crypto.subtle.digest('SHA-256', data);
    return Array.from(new Uint8Array(digest))
        .map((b) => b.toString(16).padStart(2, '0'))
        .join('');
}

export function createAuthFeature({ CONFIG, DOM, feedback, onAuthenticated, onLogout }) {
    let authenticated = false;
    let sessionTimer = null;
    let lockoutTimer = null;

    const authConfig = CONFIG.ADMIN_AUTH || {};

    const SESSION_TTL_MS = Number(authConfig.SESSION_TTL_MS || (15 * 60 * 1000));
    const MAX_ATTEMPTS = Number(authConfig.MAX_ATTEMPTS || 5);
    const LOCKOUT_MS = Number(authConfig.LOCKOUT_MS || (5 * 60 * 1000));
    const ADMIN_USERNAME = String(authConfig.USERNAME || 'admin').trim().toLowerCase();
    const ADMIN_PASSWORD_SHA256 = String(authConfig.PASSWORD_SHA256 || '').trim().toLowerCase();

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

        const expiresAt = Number(session.expiresAt || 0);
        return Number.isFinite(expiresAt) && expiresAt > nowMs();
    }

    function createSession() {
        const current = nowMs();
        return {
            issuedAt: current,
            lastActivityAt: current,
            expiresAt: current + SESSION_TTL_MS
        };
    }

    function touchSession() {
        const session = readSession();
        if (!isSessionValid(session)) {
            return false;
        }

        const current = nowMs();
        session.lastActivityAt = current;
        session.expiresAt = current + SESSION_TTL_MS;
        writeSession(session);
        return true;
    }

    function readLockoutState() {
        const parsed = safeParse(localStorage.getItem(LOCKOUT_KEY), null);
        return {
            failedAttempts: Number(parsed?.failedAttempts || 0),
            lockoutUntil: Number(parsed?.lockoutUntil || 0)
        };
    }

    function writeLockoutState(nextState) {
        localStorage.setItem(LOCKOUT_KEY, JSON.stringify({
            failedAttempts: Number(nextState?.failedAttempts || 0),
            lockoutUntil: Number(nextState?.lockoutUntil || 0)
        }));
    }

    function resetLockoutState() {
        writeLockoutState({ failedAttempts: 0, lockoutUntil: 0 });
    }

    function getLockoutRemainingMs() {
        const { lockoutUntil } = readLockoutState();
        return Math.max(0, lockoutUntil - nowMs());
    }

    function setAuthenticatedUI(isAuthenticated) {
        document.body.dataset.authenticated = isAuthenticated ? 'true' : 'false';
        DOM.authOverlay.dataset.visible = isAuthenticated ? 'false' : 'true';
        DOM.btnLogout.hidden = !isAuthenticated;

        if (!isAuthenticated) {
            DOM.statusBadge.dataset.status = 'offline';
            DOM.statusText.textContent = 'Locked';
        }
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

    function onUserActivity() {
        if (!authenticated) {
            return;
        }

        touchSession();
    }

    function registerActivityListeners() {
        ['pointerdown', 'keydown', 'touchstart', 'mousemove'].forEach((eventName) => {
            window.addEventListener(eventName, onUserActivity, { passive: true });
        });
    }

    function beginAuthenticatedSession() {
        authenticated = true;
        writeSession(createSession());
        resetLockoutState();
        DOM.adminLoginError.textContent = '';
        DOM.adminLockoutMessage.textContent = '';
        DOM.adminLoginPassword.value = '';
        setAuthenticatedUI(true);
        startSessionTimer();

        if (typeof onAuthenticated === 'function') {
            onAuthenticated();
        }
    }

    function forceLogout(reason) {
        authenticated = false;
        clearSession();
        stopSessionTimer();
        setAuthenticatedUI(false);
        DOM.adminLoginPassword.value = '';
        DOM.adminLoginError.textContent = reason || '';
        updateLockoutUI();

        if (typeof onLogout === 'function') {
            onLogout();
        }

        DOM.adminLoginUsername.focus();
    }

    async function verifyCredentials(username, password) {
        if (!ADMIN_PASSWORD_SHA256 || ADMIN_PASSWORD_SHA256.length !== 64) {
            DOM.adminLoginError.textContent =
                'Admin password hash is not configured. Update CONFIG.ADMIN_AUTH.PASSWORD_SHA256.';
            return false;
        }

        const normalizedUsername = String(username || '').trim().toLowerCase();
        if (normalizedUsername !== ADMIN_USERNAME) {
            return false;
        }

        const hashedPassword = await sha256Hex(password);
        return hashedPassword === ADMIN_PASSWORD_SHA256;
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

        try {
            const success = await verifyCredentials(username, password);
            if (success) {
                beginAuthenticatedSession();
                feedback.showToast('Admin login successful', 'success');
                return;
            }

            const lockoutState = readLockoutState();
            lockoutState.failedAttempts = Number(lockoutState.failedAttempts || 0) + 1;

            if (lockoutState.failedAttempts >= MAX_ATTEMPTS) {
                lockoutState.failedAttempts = 0;
                lockoutState.lockoutUntil = nowMs() + LOCKOUT_MS;
                writeLockoutState(lockoutState);
                updateLockoutUI();
                DOM.adminLoginError.textContent = 'Too many failed attempts. Login temporarily locked.';
                return;
            }

            writeLockoutState(lockoutState);
            const remainingAttempts = Math.max(0, MAX_ATTEMPTS - lockoutState.failedAttempts);
            DOM.adminLoginError.textContent = `Invalid credentials. ${remainingAttempts} attempt(s) left.`;
        } catch {
            DOM.adminLoginError.textContent = 'Unable to verify login right now. Try again.';
        } finally {
            DOM.btnAdminLogin.textContent = 'Login';
            updateLockoutUI();
        }
    }

    function restoreSession() {
        const session = readSession();
        if (!isSessionValid(session)) {
            setAuthenticatedUI(false);
            return false;
        }

        authenticated = true;
        touchSession();
        setAuthenticatedUI(true);
        startSessionTimer();

        if (typeof onAuthenticated === 'function') {
            onAuthenticated();
        }

        return true;
    }

    function bindEvents() {
        DOM.adminLoginForm.addEventListener('submit', handleLoginSubmit);
        DOM.btnLogout.addEventListener('click', () => {
            forceLogout('Logged out. Please login again.');
            feedback.showToast('Logged out', 'info');
        });

        registerActivityListeners();
    }

    function initialize() {
        startLockoutTimer();

        const restored = restoreSession();
        if (!restored) {
            DOM.adminLoginUsername.focus();
        }

        return restored;
    }

    return {
        bindEvents,
        initialize,
        isAuthenticated: () => authenticated,
        logout: forceLogout,
        dispose: () => {
            stopSessionTimer();
            stopLockoutTimer();
        }
    };
}
