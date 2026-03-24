import { CONFIG } from './core/config.js';
import { createInitialState } from './core/state.js';
import { getDOM } from './core/dom.js';
import { apiFetch } from './core/api.js';
import { createFeedback } from './ui/feedback.js';
import { createAuthFeature } from './features/auth.js';
import { createStatusFeature } from './features/status.js';
import { createGuestFeature } from './features/guest.js';
import { createLogsFeature } from './features/logs.js';
import { createUsersFeature } from './features/users.js';

export function initApp() {
    const DOM = getDOM();
    const state = createInitialState();
    const feedback = createFeedback({ DOM, CONFIG });

    const logsFeature = createLogsFeature({ CONFIG, state, DOM, apiFetch, feedback });
    const usersFeature = createUsersFeature({
        CONFIG,
        state,
        DOM,
        apiFetch,
        feedback,
        onLogsUpdated: logsFeature.loadLogs
    });
    const guestFeature = createGuestFeature({
        CONFIG,
        state,
        DOM,
        apiFetch,
        feedback,
        onLogsUpdated: logsFeature.loadLogs
    });

    const statusFeature = createStatusFeature({
        CONFIG,
        state,
        DOM,
        apiFetch,
        feedback,
        onLogsUpdated: logsFeature.loadLogs
    });

    let runtimeStarted = false;
    let featureEventsBound = false;

    function stopPollingLoops() {
        if (state.pollTimer) {
            clearInterval(state.pollTimer);
            state.pollTimer = null;
        }

        if (state.logsTimer) {
            clearInterval(state.logsTimer);
            state.logsTimer = null;
        }

        if (state.usersTimer) {
            clearInterval(state.usersTimer);
            state.usersTimer = null;
        }

        if (state.guestTimer) {
            clearInterval(state.guestTimer);
            state.guestTimer = null;
        }

        if (state.emergencyCooldownTimer) {
            clearInterval(state.emergencyCooldownTimer);
            state.emergencyCooldownTimer = null;
        }

        if (state.lockCountdownTimer) {
            clearInterval(state.lockCountdownTimer);
            state.lockCountdownTimer = null;
        }

        if (state.rfidPollTimer) {
            clearInterval(state.rfidPollTimer);
            state.rfidPollTimer = null;
        }
    }

    function bindFeatureEventsOnce(authFeature) {
        if (featureEventsBound) {
            return;
        }

        statusFeature.bindEvents();
        guestFeature.bindEvents();
        logsFeature.bindEvents();
        usersFeature.bindEvents();

        document.addEventListener('visibilitychange', () => {
            if (!runtimeStarted || !authFeature.isAuthenticated()) {
                return;
            }

            if (!document.hidden) {
                statusFeature.pollStatus().then((data) => {
                    if (data) {
                        guestFeature.syncFromStatus(data);
                    }
                });
                logsFeature.loadLogs();
                usersFeature.loadUsers();
            }
        });

        featureEventsBound = true;
    }

    function bindEvents() {
        feedback.bindCoreModalEvents();
    }

    function startPollingLoops() {
        if (!state.pollTimer) {
            state.pollTimer = setInterval(() => {
                if (!document.hidden) {
                    statusFeature.pollStatus().then((data) => {
                        if (data) {
                            guestFeature.syncFromStatus(data);
                        }
                    });
                }
            }, CONFIG.POLL_INTERVAL);
        }

        if (!state.logsTimer) {
            state.logsTimer = setInterval(() => {
                if (!document.hidden) logsFeature.loadLogs();
            }, CONFIG.LOGS_REFRESH_INTERVAL);
        }

        if (!state.usersTimer) {
            state.usersTimer = setInterval(() => {
                if (!document.hidden) usersFeature.loadUsers();
            }, CONFIG.USERS_REFRESH_INTERVAL);
        }
    }

    function startProtectedRuntime(authFeature) {
        if (runtimeStarted) {
            return;
        }

        bindFeatureEventsOnce(authFeature);

        runtimeStarted = true;
        statusFeature.updateEmergencyButton();
        usersFeature.updateAddUserSubmitButton();

        statusFeature.pollStatus().then((data) => {
            if (data) {
                guestFeature.syncFromStatus(data);
            }
        });
        logsFeature.loadLogs();
        usersFeature.loadUsers();

        startPollingLoops();
    }

    function stopProtectedRuntime() {
        runtimeStarted = false;
        stopPollingLoops();

        state.connected = false;
        DOM.statusBadge.dataset.status = 'offline';
        DOM.statusText.textContent = 'Locked';
    }

    const authFeature = createAuthFeature({
        CONFIG,
        DOM,
        feedback,
        onAuthenticated: () => {
            startProtectedRuntime(authFeature);
        },
        onLogout: () => {
            stopProtectedRuntime();
        }
    });

    function init() {
        console.log('[SecureLock] Dashboard modular app initializing...');

        logsFeature.initializePageSize();
        bindEvents();
        authFeature.bindEvents();
        authFeature.initialize();

        console.log('[SecureLock] Dashboard modular app ready');
    }

    init();
}
