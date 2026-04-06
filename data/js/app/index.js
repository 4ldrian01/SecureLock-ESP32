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
    let sectionObserver = null;
    let sectionFallbackBound = false;
    let sectionFallbackHandler = null;
    let sectionFallbackRaf = null;

    const pollControl = {
        statusErrorCount: 0
    };

    const lazySections = {
        logs: { seen: false, loading: false },
        users: { seen: false, loading: false }
    };

    function isMobileViewport() {
        return window.matchMedia('(max-width: 768px)').matches;
    }

    function isSectionNearViewport(sectionEl) {
        if (!sectionEl) {
            return true;
        }

        const preloadPx = Math.max(80, Number(CONFIG.LAZY_SECTION_PRELOAD_PX || 220));
        const rect = sectionEl.getBoundingClientRect();
        const viewportHeight = window.innerHeight || document.documentElement.clientHeight || 800;
        return rect.bottom >= -preloadPx && rect.top <= (viewportHeight + preloadPx);
    }

    function resetLazySectionState() {
        lazySections.logs.seen = false;
        lazySections.logs.loading = false;
        lazySections.users.seen = false;
        lazySections.users.loading = false;
    }

    async function loadSectionIfNeeded(sectionKey, authFeature, options = {}) {
        const { force = false } = options;

        if (!runtimeStarted || !authFeature.isAuthenticated()) {
            return;
        }

        const sectionState = lazySections[sectionKey];
        if (!sectionState || sectionState.loading) {
            return;
        }

        if (sectionState.seen && !force) {
            return;
        }

        const sectionEl = sectionKey === 'logs' ? DOM.logsSection : DOM.usersSection;
        if (!force && !isSectionNearViewport(sectionEl)) {
            return;
        }

        sectionState.loading = true;
        try {
            if (sectionKey === 'logs') {
                await logsFeature.loadLogs();
            } else if (sectionKey === 'users') {
                await usersFeature.loadUsers();
            }

            sectionState.seen = true;
        } finally {
            sectionState.loading = false;
        }
    }

    async function refreshVisibleSections() {
        if (!runtimeStarted) {
            return;
        }

        if (lazySections.logs.seen && isSectionNearViewport(DOM.logsSection)) {
            await logsFeature.loadLogs();
        }

        if (lazySections.users.seen && isSectionNearViewport(DOM.usersSection)) {
            await usersFeature.loadUsers();
        }
    }

    function shouldPollSection(sectionKey) {
        const sectionState = lazySections[sectionKey];
        if (!sectionState || !sectionState.seen) {
            return false;
        }

        if (document.hidden) {
            return false;
        }

        const sectionEl = sectionKey === 'logs' ? DOM.logsSection : DOM.usersSection;
        return isSectionNearViewport(sectionEl);
    }

    function bindSectionLazyLoading(authFeature) {
        if (sectionObserver || sectionFallbackBound) {
            return;
        }

        const preloadPx = Math.max(80, Number(CONFIG.LAZY_SECTION_PRELOAD_PX || 220));

        if ('IntersectionObserver' in window) {
            sectionObserver = new IntersectionObserver((entries) => {
                if (!runtimeStarted || !authFeature.isAuthenticated()) {
                    return;
                }

                entries.forEach((entry) => {
                    if (!entry.isIntersecting && entry.intersectionRatio <= 0) {
                        return;
                    }

                    if (entry.target === DOM.logsSection && !lazySections.logs.seen) {
                        loadSectionIfNeeded('logs', authFeature, { force: true });
                        return;
                    }

                    if (entry.target === DOM.usersSection && !lazySections.users.seen) {
                        loadSectionIfNeeded('users', authFeature, { force: true });
                    }
                });
            }, {
                root: null,
                rootMargin: `${preloadPx}px 0px ${preloadPx}px 0px`,
                threshold: 0.01
            });

            if (DOM.logsSection) {
                sectionObserver.observe(DOM.logsSection);
            }

            if (DOM.usersSection) {
                sectionObserver.observe(DOM.usersSection);
            }
        }

        sectionFallbackHandler = () => {
            if (sectionFallbackRaf) {
                return;
            }

            sectionFallbackRaf = window.requestAnimationFrame(() => {
                sectionFallbackRaf = null;

                if (!runtimeStarted || !authFeature.isAuthenticated()) {
                    return;
                }

                if (!lazySections.logs.seen) {
                    loadSectionIfNeeded('logs', authFeature);
                }

                if (!lazySections.users.seen) {
                    loadSectionIfNeeded('users', authFeature);
                }
            });
        };

        window.addEventListener('scroll', sectionFallbackHandler, { passive: true });
        window.addEventListener('resize', sectionFallbackHandler);
        sectionFallbackBound = true;
    }

    function getStatusBaseIntervalMs() {
        const visibleDefault = Number(CONFIG.POLL_INTERVAL || 2500);
        const hiddenDefault = Number(CONFIG.POLL_INTERVAL_HIDDEN || 7000);
        const visibleMobile = Number(CONFIG.POLL_INTERVAL_MOBILE || visibleDefault);
        const hiddenMobile = Number(CONFIG.POLL_INTERVAL_HIDDEN_MOBILE || hiddenDefault);

        if (document.hidden) {
            return isMobileViewport() ? hiddenMobile : hiddenDefault;
        }

        return isMobileViewport() ? visibleMobile : visibleDefault;
    }

    function stopPollingLoops() {
        if (state.pollTimer) {
            clearTimeout(state.pollTimer);
            state.pollTimer = null;
        }

        if (state.logsTimer) {
            clearTimeout(state.logsTimer);
            state.logsTimer = null;
        }

        if (state.logsClockTimer) {
            clearInterval(state.logsClockTimer);
            state.logsClockTimer = null;
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

                loadSectionIfNeeded('logs', authFeature);
                loadSectionIfNeeded('users', authFeature);
                refreshVisibleSections();
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

            const baseInterval = getStatusBaseIntervalMs();

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

    function schedulePeriodicPoll(timerKey, task, baseInterval, hiddenInterval, delayMs = 0, shouldRun = null) {
        if (state[timerKey]) {
            clearTimeout(state[timerKey]);
            state[timerKey] = null;
        }

        state[timerKey] = setTimeout(async () => {
            if (!runtimeStarted) {
                return;
            }

            const canRun = typeof shouldRun === 'function' ? shouldRun() : true;
            if (canRun) {
                await task();
            }

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
                Number(CONFIG.LOGS_REFRESH_INTERVAL || 10000),
                () => shouldPollSection('logs')
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
                Number(CONFIG.USERS_REFRESH_INTERVAL || 15000),
                () => shouldPollSection('users')
            );
        }
    }

    function startProtectedRuntime(authFeature) {
        if (runtimeStarted) {
            return;
        }

        bindFeatureEventsOnce(authFeature);
        bindSectionLazyLoading(authFeature);

        runtimeStarted = true;
        resetLazySectionState();
        statusFeature.updateEmergencyButton();
        usersFeature.updateAddUserSubmitButton();

        statusFeature.pollStatus().then((data) => {
            if (data) {
                guestFeature.syncFromStatus(data);
            }
        });

        setTimeout(() => {
            if (!runtimeStarted || !authFeature.isAuthenticated()) {
                return;
            }
            loadSectionIfNeeded('logs', authFeature);
        }, 120);

        setTimeout(() => {
            if (!runtimeStarted || !authFeature.isAuthenticated()) {
                return;
            }
            loadSectionIfNeeded('users', authFeature);
        }, 200);

        startPollingLoops(authFeature);
    }

    function stopProtectedRuntime() {
        runtimeStarted = false;
        stopPollingLoops();
        resetLazySectionState();
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
