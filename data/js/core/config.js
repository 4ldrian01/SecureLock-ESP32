export const CONFIG = {
    POLL_INTERVAL: 1000,
    LOGS_REFRESH_INTERVAL: 5000,
    USERS_REFRESH_INTERVAL: 7000,
    DIAGNOSTICS_INTERVAL: 3000,
    LOGS_PAGE_SIZE_MOBILE: 5,
    LOGS_PAGE_SIZE_TABLET: 8,
    LOGS_PAGE_SIZE_DESKTOP: 10,
    TOAST_DURATION: 3500,
    EMERGENCY_COOLDOWN: 5,
    RFID_POLL_INTERVAL: 120,
    ADMIN_AUTH: {
        USERNAME: 'admin',
        // SHA-256 hash of the admin dashboard password.
        // Current placeholder password is: CHANGE_ME_NOW
        // Replace this hash before production use.
        PASSWORD_SHA256: 'd1e47cfc5b8eeaee72789781cf13ccf57d6f133e16dc7ee478c65a346e465fd3',
        SESSION_TTL_MS: 15 * 60 * 1000,
        MAX_ATTEMPTS: 5,
        LOCKOUT_MS: 5 * 60 * 1000
    },
    API: {
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
