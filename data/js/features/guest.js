export function createGuestFeature({ CONFIG, state, DOM, apiFetch, feedback, onLogsUpdated }) {
    function displayGuestCode(code, expiresInSec) {
        const digits = code.toString().padStart(4, '0').split('');
        const pinDigits = DOM.pinCode.querySelectorAll('.pin-digit');
        pinDigits.forEach((el, i) => {
            el.textContent = digits[i] || '-';
        });

        state.guestCode = code;
        state.guestExpiry = Math.max(0, Number(expiresInSec) || 0);

        if (state.guestTimer) clearInterval(state.guestTimer);

        state.guestTimer = setInterval(() => {
            state.guestExpiry--;
            if (state.guestExpiry <= 0) {
                clearInterval(state.guestTimer);
                state.guestTimer = null;
                state.guestCode = null;
                clearGuestCodeDisplay();
                return;
            }
            const min = Math.floor(state.guestExpiry / 60);
            const sec = state.guestExpiry % 60;
            DOM.pinTimer.textContent = `Expires in ${min}:${sec.toString().padStart(2, '0')}`;
            DOM.pinTimer.dataset.expired = 'false';
        }, 1000);

        const min = Math.floor(state.guestExpiry / 60);
        const sec = state.guestExpiry % 60;
        DOM.pinTimer.textContent = `Expires in ${min}:${sec.toString().padStart(2, '0')}`;
        DOM.pinTimer.dataset.expired = 'false';
    }

    function clearGuestCodeDisplay() {
        const pinDigits = DOM.pinCode.querySelectorAll('.pin-digit');
        pinDigits.forEach(el => { el.textContent = '-'; });
        DOM.pinTimer.textContent = 'No active code';
        DOM.pinTimer.dataset.expired = 'true';
    }

    function syncFromStatus(statusData) {
        const guestCodeActive = Boolean(statusData?.guestCodeActive);
        const guestCode = String(statusData?.guestCode || '');
        const remainingMs = Number(statusData?.guestCodeRemainingMs || 0);
        const remainingSec = Math.max(0, Math.ceil(remainingMs / 1000));

        if (guestCodeActive && guestCode.length === 4) {
            displayGuestCode(guestCode, remainingSec);
            return;
        }

        if (!guestCodeActive) {
            state.guestCode = null;
            state.guestExpiry = 0;
            if (state.guestTimer) {
                clearInterval(state.guestTimer);
                state.guestTimer = null;
            }
            clearGuestCodeDisplay();
        }
    }

    function bindEvents() {
        // Display-only mode: guest code is generated from Telegram /guest_code.
    }

    return {
        bindEvents,
        syncFromStatus
    };
}
