import { CONFIG } from '../core/config.js?v=20260418r5';
import { createInitialState } from '../core/state.js?v=20260418r5';
import { getDOM } from '../core/dom.js?v=20260418r5';
import {
    apiFetch,
    setApiAuthToken,
    clearApiAuthToken,
    setApiUnauthorizedHandler
} from '../core/api.js?v=20260418r5';
import { createFeedback } from '../ui/feedback.js?v=20260418r5';
import { createAuthFeature } from '../features/auth.js?v=20260418r5';
import { createStatusFeature } from '../features/status.js?v=20260418r5';
import { createGuestFeature } from '../features/guest.js?v=20260418r5';
import { createLogsFeature } from '../features/logs.js?v=20260418r5';
import { createUsersFeature } from '../features/users.js?v=20260418r5';

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
    let routeSyncRaf = null;

    const pollControl = {
        statusErrorCount: 0
    };

    const lazySections = {
        logs: { seen: false, loading: false },
        users: { seen: false, loading: false }
    };

    const ROUTE_PATHS = {
        LOGIN: '/login',
        DASHBOARD: '/dashboard',
        GUEST: '/dashboard/guest',
        LOGS: '/dashboard/logs',
        USERS: '/dashboard/users'
    };

    function isMobileViewport() {
        return window.matchMedia('(max-width: 768px)').matches;
    }

    function getSectionPreloadPx() {
        const desktopPreload = Math.max(80, Number(CONFIG.LAZY_SECTION_PRELOAD_PX || 220));
        const mobilePreload = Math.max(80, Number(CONFIG.LAZY_SECTION_PRELOAD_PX_MOBILE || desktopPreload));
        return isMobileViewport() ? mobilePreload : desktopPreload;
    }

    function isSectionNearViewport(sectionEl) {
        if (!sectionEl) {
            return true;
        }

        const preloadPx = getSectionPreloadPx();
        const rect = sectionEl.getBoundingClientRect();
        const viewportHeight = window.innerHeight || document.documentElement.clientHeight || 800;
        return rect.bottom >= -preloadPx && rect.top <= (viewportHeight + preloadPx);
    }

    function getNetworkPressureMultiplier() {
        let multiplier = 1;

        const rttSlowMs = Math.max(1, Number(CONFIG.POLL_INTERVAL_RTT_SLOW_MS || 500));
        const rttCriticalMs = Math.max(rttSlowMs, Number(CONFIG.POLL_INTERVAL_RTT_CRITICAL_MS || 900));
        const slowMultiplier = Math.max(1, Number(CONFIG.POLL_INTERVAL_SLOW_MULTIPLIER || 1.35));
        const criticalMultiplier = Math.max(slowMultiplier, Number(CONFIG.POLL_INTERVAL_CRITICAL_MULTIPLIER || 1.85));
        const saveDataMultiplier = Math.max(1, Number(CONFIG.POLL_INTERVAL_SAVE_DATA_MULTIPLIER || 1.5));

        const rtt = Math.max(0, Number(state.statusApiRttSmoothedMs || state.statusApiRttMs || 0));
        if (rtt >= rttCriticalMs) {
            multiplier *= criticalMultiplier;
        } else if (rtt >= rttSlowMs) {
            multiplier *= slowMultiplier;
        }

        if (typeof navigator !== 'undefined' && navigator.connection) {
            const connection = navigator.connection;
            const effectiveType = String(connection.effectiveType || '').toLowerCase();

            if (connection.saveData) {
                multiplier *= saveDataMultiplier;
            }

            if (effectiveType === 'slow-2g' || effectiveType === '2g') {
                multiplier *= 1.6;
            } else if (effectiveType === '3g') {
                multiplier *= 1.25;
            }
        }

        return Math.max(1, multiplier);
    }

    function applyPollJitter(intervalMs) {
        const baseMs = Math.max(0, Number(intervalMs) || 0);
        const jitterPct = Math.max(0, Math.min(0.25, Number(CONFIG.POLL_JITTER_PCT || 0.08)));
        if (baseMs <= 0 || jitterPct <= 0) {
            return baseMs;
        }

        const jitterRangeMs = baseMs * jitterPct;
        const jitter = (Math.random() * (2 * jitterRangeMs)) - jitterRangeMs;
        return Math.max(0, Math.round(baseMs + jitter));
    }

    function resetLazySectionState() {
        lazySections.logs.seen = false;
        lazySections.logs.loading = false;
        lazySections.users.seen = false;
        lazySections.users.loading = false;
    }

    function normalizePath(pathname = window.location.pathname) {
        const raw = String(pathname || '').trim();
        if (!raw || raw === '/') {
            return '/';
        }

        const normalized = raw.replace(/\/+$/, '');
        return normalized || '/';
    }

    function isLoginPath(pathname = window.location.pathname) {
        const path = normalizePath(pathname);
        return path === ROUTE_PATHS.LOGIN;
    }

    function isDashboardPath(pathname = window.location.pathname) {
        const path = normalizePath(pathname);
        return path === ROUTE_PATHS.DASHBOARD
            || path === ROUTE_PATHS.GUEST
            || path === ROUTE_PATHS.LOGS
            || path === ROUTE_PATHS.USERS
            || path === '/dashboard/'
            || path.startsWith('/dashboard/');
    }

    function replaceRoute(pathname) {
        const next = normalizePath(pathname);
        const current = normalizePath(window.location.pathname);
        if (next === current) {
            return;
        }

        window.history.replaceState({ securelockRoute: next }, '', next);
    }

    function getVisibleSectionRoute() {
        const scrollTop = window.scrollY || document.documentElement.scrollTop || 0;
        if (scrollTop <= 80) {
            return ROUTE_PATHS.DASHBOARD;
        }

        const viewportHeight = window.innerHeight || document.documentElement.clientHeight || 800;
        const usersRect = DOM.usersSection?.getBoundingClientRect();
        if (usersRect && usersRect.top <= viewportHeight * 0.55 && usersRect.bottom > 40) {
            return ROUTE_PATHS.USERS;
        }

        const logsRect = DOM.logsSection?.getBoundingClientRect();
        if (logsRect && logsRect.top <= viewportHeight * 0.6 && logsRect.bottom > 40) {
            return ROUTE_PATHS.LOGS;
        }

        const guestRect = DOM.guestSection?.getBoundingClientRect();
        if (guestRect && guestRect.bottom > 20) {
            return ROUTE_PATHS.GUEST;
        }

        return ROUTE_PATHS.DASHBOARD;
    }

    function scrollToRouteSection(pathname, behavior = 'auto') {
        const path = normalizePath(pathname);

        if (path === ROUTE_PATHS.USERS) {
            DOM.usersSection?.scrollIntoView({ behavior, block: 'start' });
            return;
        }

        if (path === ROUTE_PATHS.LOGS) {
            DOM.logsSection?.scrollIntoView({ behavior, block: 'start' });
            return;
        }

        if (path === ROUTE_PATHS.GUEST) {
            DOM.guestSection?.scrollIntoView({ behavior, block: 'start' });
            return;
        }

        if (path === ROUTE_PATHS.DASHBOARD || path === '/dashboard/') {
            window.scrollTo({ top: 0, behavior });
        }
    }

    function syncRouteWithState(authFeature, options = {}) {
        const { fromPopState = false, preserveKnownDashboardRoute = false } = options;
        const currentPath = normalizePath(window.location.pathname);

        if (!authFeature.isAuthenticated()) {
            if (!isLoginPath(currentPath)) {
                replaceRoute(ROUTE_PATHS.LOGIN);
            }
            return;
        }

        if (!isDashboardPath(currentPath)) {
            replaceRoute(ROUTE_PATHS.DASHBOARD);
            return;
        }

        if (fromPopState || preserveKnownDashboardRoute) {
            scrollToRouteSection(currentPath, 'auto');
            return;
        }

        replaceRoute(getVisibleSectionRoute());
    }

    function scheduleRouteSync(authFeature, options = {}) {
        if (routeSyncRaf) {
            return;
        }

        routeSyncRaf = window.requestAnimationFrame(() => {
            routeSyncRaf = null;
            syncRouteWithState(authFeature, options);
        });
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

        const preloadPx = getSectionPreloadPx();

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
        const maxInterval = Math.max(1500, Number(CONFIG.POLL_INTERVAL_MAX || 15000));
        const pressureMultiplier = getNetworkPressureMultiplier();

        const preferredBase = document.hidden
            ? (isMobileViewport() ? hiddenMobile : hiddenDefault)
            : (isMobileViewport() ? visibleMobile : visibleDefault);

        const pressureAdjusted = Math.max(900, Math.round(preferredBase * pressureMultiplier));
        return Math.min(maxInterval, pressureAdjusted);
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

        if (state.guestCooldownTimer) {
            clearInterval(state.guestCooldownTimer);
            state.guestCooldownTimer = null;
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
                scheduleRouteSync(authFeature);
            }
        });

        window.addEventListener('scroll', () => {
            if (!runtimeStarted || !authFeature.isAuthenticated()) {
                return;
            }
            scheduleRouteSync(authFeature);
        }, { passive: true });

        window.addEventListener('resize', () => {
            if (!runtimeStarted || !authFeature.isAuthenticated()) {
                return;
            }
            scheduleRouteSync(authFeature);
        });

        window.addEventListener('popstate', () => {
            syncRouteWithState(authFeature, { fromPopState: true });
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
            scheduleStatusPoll(authFeature, applyPollJitter(nextDelay));
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

            schedulePeriodicPoll(
                timerKey,
                task,
                baseInterval,
                hiddenInterval,
                applyPollJitter(nextInterval)
            );
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
        syncRouteWithState(authFeature, {
            preserveKnownDashboardRoute: true,
            fromPopState: true
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
        scheduleRouteSync(authFeature);
    }

    function stopProtectedRuntime() {
        runtimeStarted = false;
        stopPollingLoops();
        resetLazySectionState();
        if (routeSyncRaf) {
            window.cancelAnimationFrame(routeSyncRaf);
            routeSyncRaf = null;
        }
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
            syncRouteWithState(authFeature);
        }
    });

    setApiUnauthorizedHandler(() => authFeature.handleUnauthorized());

    function init() {
        console.log('[SecureLock] Dashboard modular app initializing...');

        const initialPath = normalizePath(window.location.pathname);
        if (!isLoginPath(initialPath) && !isDashboardPath(initialPath)) {
            replaceRoute(ROUTE_PATHS.LOGIN);
        }

        logsFeature.initializePageSize();
        bindEvents();
        authFeature.bindEvents();
        authFeature.initialize();

        console.log('[SecureLock] Dashboard modular app ready');
    }

    init();
}
