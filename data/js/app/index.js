import { CONFIG } from '../core/config.js';
import { createInitialState } from '../core/state.js';
import { getDOM } from '../core/dom.js';
import {
    apiFetch,
    setApiAuthToken,
    clearApiAuthToken,
    setApiUnauthorizedHandler
} from '../core/api.js';
import { createFeedback } from '../ui/feedback.js';
import { createAuthFeature } from '../features/auth.js';
import { createStatusFeature } from '../features/status.js';
import { createGuestFeature } from '../features/guest.js';
import { createLogsFeature } from '../features/logs.js';
import { createUsersFeature } from '../features/users.js';

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

    const pollControl = {
        statusErrorCount: 0
    };

    function stopPollingLoops() {
        if (state.pollTimer) {
            clearTimeout(state.pollTimer);
            state.pollTimer = null;
        }

        if (state.logsTimer) {
            clearTimeout(state.logsTimer);
            state.logsTimer = null;
        }

        if (state.usersTimer) {
            clearTimeout(state.usersTimer);
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

    function scheduleStatusPoll(authFeature, delayMs = 0) {
        if (state.pollTimer) {
            clearTimeout(state.pollTimer);
            state.pollTimer = null;
        }

        state.pollTimer = setTimeout(async () => {
            if (!runtimeStarted || !authFeature.isAuthenticated()) {
                return;
            }

            const startedAt = Date.now();

            try {
                const data = await statusFeature.pollStatus();
                if (!data) {
                    throw new Error('Status unavailable');
                }

                guestFeature.syncFromStatus(data);
                pollControl.statusErrorCount = 0;
            } catch {
                pollControl.statusErrorCount += 1;
            }

            if (!runtimeStarted || !authFeature.isAuthenticated()) {
                return;
            }

            const baseInterval = document.hidden
                ? Number(CONFIG.POLL_INTERVAL_HIDDEN || 7000)
                : Number(CONFIG.POLL_INTERVAL || 2500);

            const backoffMultiplier = Number(CONFIG.POLL_BACKOFF_MULTIPLIER || 1.6);
            const maxInterval = Number(CONFIG.POLL_INTERVAL_MAX || 15000);
            const backoffInterval = pollControl.statusErrorCount > 0
                ? Math.min(maxInterval, Math.round(baseInterval * Math.pow(backoffMultiplier, pollControl.statusErrorCount)))
                : baseInterval;

            const elapsed = Date.now() - startedAt;
            const nextDelay = Math.max(350, backoffInterval - elapsed);
            scheduleStatusPoll(authFeature, nextDelay);
        }, Math.max(0, Number(delayMs) || 0));
    }

    function schedulePeriodicPoll(timerKey, task, baseInterval, hiddenInterval, delayMs = 0) {
        if (state[timerKey]) {
            clearTimeout(state[timerKey]);
            state[timerKey] = null;
        }

        state[timerKey] = setTimeout(async () => {
            if (!runtimeStarted) {
                return;
            }

            await task();

            if (!runtimeStarted) {
                return;
            }

            const nextInterval = document.hidden
                ? Number(hiddenInterval || baseInterval)
                : Number(baseInterval);

            schedulePeriodicPoll(timerKey, task, baseInterval, hiddenInterval, nextInterval);
        }, Math.max(0, Number(delayMs) || 0));
    }

    function startPollingLoops(authFeature) {
        if (!state.pollTimer) {
            scheduleStatusPoll(authFeature, 0);
        }

        if (!state.logsTimer) {
            schedulePeriodicPoll(
                'logsTimer',
                async () => {
                    await logsFeature.loadLogs();
                },
                CONFIG.LOGS_REFRESH_INTERVAL,
                CONFIG.LOGS_REFRESH_INTERVAL_HIDDEN,
                Number(CONFIG.LOGS_REFRESH_INTERVAL || 10000)
            );
        }

        if (!state.usersTimer) {
            schedulePeriodicPoll(
                'usersTimer',
                async () => {
                    await usersFeature.loadUsers();
                },
                CONFIG.USERS_REFRESH_INTERVAL,
                CONFIG.USERS_REFRESH_INTERVAL_HIDDEN,
                Number(CONFIG.USERS_REFRESH_INTERVAL || 15000)
            );
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

        startPollingLoops(authFeature);
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
        apiFetch,
        setApiAuthToken,
        clearApiAuthToken,
        onAuthenticated: () => {
            startProtectedRuntime(authFeature);
        },
        onLogout: () => {
            stopProtectedRuntime();
        }
    });

    setApiUnauthorizedHandler(() => authFeature.handleUnauthorized());

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
