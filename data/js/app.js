import { CONFIG } from './core/config.js';
import { createInitialState } from './core/state.js';
import { getDOM } from './core/dom.js';
import { apiFetch } from './core/api.js';
import { createFeedback } from './ui/feedback.js';
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

    function bindEvents() {
        feedback.bindCoreModalEvents();
        statusFeature.bindEvents();
        guestFeature.bindEvents();
        logsFeature.bindEvents();
        usersFeature.bindEvents();

        document.addEventListener('visibilitychange', () => {
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

    function init() {
        console.log('[SecureLock] Dashboard modular app initializing...');

        logsFeature.initializePageSize();
        bindEvents();

        statusFeature.updateEmergencyButton();
        guestFeature.updateGuestGenerateButton();
        usersFeature.updateAddUserSubmitButton();

        statusFeature.pollStatus().then((data) => {
            if (data) {
                guestFeature.syncFromStatus(data);
            }
        });
        logsFeature.loadLogs();
        usersFeature.loadUsers();

        startPollingLoops();

        console.log('[SecureLock] Dashboard modular app ready');
    }

    init();
}
