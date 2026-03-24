export const CONFIG = {
    POLL_INTERVAL: 2500,
    POLL_INTERVAL_HIDDEN: 7000,
    POLL_INTERVAL_MAX: 15000,
    POLL_BACKOFF_MULTIPLIER: 1.6,
    LOGS_REFRESH_INTERVAL: 10000,
    LOGS_REFRESH_INTERVAL_HIDDEN: 20000,
    USERS_REFRESH_INTERVAL: 15000,
    USERS_REFRESH_INTERVAL_HIDDEN: 30000,
    DIAGNOSTICS_INTERVAL: 3000,
    LOGS_PAGE_SIZE_MOBILE: 5,
    LOGS_PAGE_SIZE_TABLET: 8,
    LOGS_PAGE_SIZE_DESKTOP: 10,
    TOAST_DURATION: 3500,
    EMERGENCY_COOLDOWN: 5,
    RFID_POLL_INTERVAL: 400,
    RFID_POLL_INTERVAL_HIDDEN: 1000,
    RFID_POLL_MAX_INTERVAL: 2000,
    AUTH_SESSION_KEY: 'securelock_admin_api_session_v2',
    ADMIN_AUTH: {
        SESSION_TTL_MS: 15 * 60 * 1000,
        MAX_ATTEMPTS: 5,
        LOCKOUT_MS: 5 * 60 * 1000
    },
    API: {
        AUTH_LOGIN: '/api/auth/login',
        AUTH_LOGOUT: '/api/auth/logout',
        AUTH_STATUS: '/api/auth/status',
        STATUS: '/api/status',
        UNLOCK: '/api/unlock',
        USERS: '/api/users',
        USERS_RESET: '/api/users/reset',
        LOGS: '/api/logs',
        LOGS_CLEAR: '/api/logs',
        RFID_SCAN: '/api/rfid/scan',
        DIAGNOSTICS: '/api/diagnostics'
    }
};

export const GUEST_BUTTON_ICON =
    '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M17.65 6.35C16.2 4.9 14.21 4 12 4c-4.42 0-7.99 3.58-7.99 8s3.57 8 7.99 8c3.73 0 6.84-2.55 7.73-6h-2.08c-.82 2.33-3.04 4-5.65 4-3.31 0-6-2.69-6-6s2.69-6 6-6c1.66 0 3.14.69 4.22 1.78L13 11h7V4l-2.35 2.35z"/></svg>';
