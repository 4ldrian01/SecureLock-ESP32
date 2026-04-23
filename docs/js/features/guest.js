export function createGuestFeature({ CONFIG, state, DOM, apiFetch, feedback, onLogsUpdated }) {
    function setGuestVisualState(mode) {
        const stateValue = String(mode || 'idle');
        DOM.pinCode.dataset.state = stateValue;
        DOM.pinTimer.dataset.state = stateValue;

        const display = DOM.pinCode.closest('.pin-display');
        if (display) {
            display.dataset.state = stateValue;
        }
    }

    function stopGuestCooldownTimer() {
        if (state.guestCooldownTimer) {
            clearInterval(state.guestCooldownTimer);
            state.guestCooldownTimer = null;
        }
    }

    function ensureGuestCooldownTimer() {
        if (state.guestCooldownTimer || (state.guestCode && state.guestExpiry > 0)) {
            return;
        }

        if (Number(state.guestCooldown || 0) <= 0) {
            stopGuestCooldownTimer();
            return;
        }

        state.guestCooldownTimer = setInterval(() => {
            if (state.guestRequestInFlight || (state.guestCode && state.guestExpiry > 0)) {
                return;
            }

            state.guestCooldown = Math.max(0, Number(state.guestCooldown || 0) - 1);
            updateGuestButtonState();

            if (state.guestCooldown <= 0) {
                stopGuestCooldownTimer();
            }
        }, 1000);
    }

    function updateGuestButtonState() {
        if (!DOM.btnGuestCode) {
            return;
        }

        if (state.guestRequestInFlight) {
            DOM.btnGuestCode.disabled = true;
            DOM.btnGuestCode.textContent = 'Generating...';
            DOM.btnGuestCode.dataset.state = 'busy';
            return;
        }

        if (state.guestCode && state.guestExpiry > 0) {
            stopGuestCooldownTimer();
            DOM.btnGuestCode.disabled = true;
            DOM.btnGuestCode.textContent = `Active (${state.guestExpiry}s)`;
            DOM.btnGuestCode.dataset.state = 'active';
            return;
        }

        const cooldownSec = Math.max(0, Number(state.guestCooldown || 0));
        if (cooldownSec > 0) {
            ensureGuestCooldownTimer();
            DOM.btnGuestCode.disabled = true;
            DOM.btnGuestCode.textContent = `Available in ${cooldownSec}s`;
            DOM.btnGuestCode.dataset.state = 'cooldown';
            return;
        }

        stopGuestCooldownTimer();

        DOM.btnGuestCode.disabled = false;
        DOM.btnGuestCode.textContent = 'Generate Guest Code';
        DOM.btnGuestCode.dataset.state = 'ready';
    }

    function displayGuestCode(code, expiresInSec) {
        const digits = code.toString().padStart(4, '0').split('');
        const pinDigits = DOM.pinCode.querySelectorAll('.pin-digit');
        pinDigits.forEach((el, i) => {
            el.textContent = digits[i] || '-';
        });

        state.guestCode = code;
        state.guestExpiry = Math.max(0, Number(expiresInSec) || 0);
        state.guestCooldown = Math.max(state.guestCooldown || 0, state.guestExpiry);
        stopGuestCooldownTimer();

        if (state.guestTimer) clearInterval(state.guestTimer);

        state.guestTimer = setInterval(() => {
            state.guestExpiry--;
            state.guestCooldown = Math.max(0, Number(state.guestCooldown || 0) - 1);

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
            setGuestVisualState('active');
            updateGuestButtonState();
        }, 1000);

        const min = Math.floor(state.guestExpiry / 60);
        const sec = state.guestExpiry % 60;
        DOM.pinTimer.textContent = `Expires in ${min}:${sec.toString().padStart(2, '0')}`;
        DOM.pinTimer.dataset.expired = 'false';
        setGuestVisualState('active');
        updateGuestButtonState();
    }

    function clearGuestCodeDisplay() {
        const pinDigits = DOM.pinCode.querySelectorAll('.pin-digit');
        pinDigits.forEach(el => { el.textContent = '-'; });
        DOM.pinTimer.textContent = 'No active code';
        DOM.pinTimer.dataset.expired = 'true';
        setGuestVisualState('idle');
        updateGuestButtonState();
    }

    function syncFromStatus(statusData) {
        const guestCodeActive = Boolean(statusData?.guestCodeActive);
        const guestCode = String(statusData?.guestCode || '');
        const remainingMs = Number(statusData?.guestCodeRemainingMs || 0);
        const remainingSec = Math.max(0, Math.ceil(remainingMs / 1000));
        const cooldownMs = Number(statusData?.guestCodeCooldownRemainingMs || 0);
        const cooldownSec = Math.max(0, Math.ceil(cooldownMs / 1000));

        state.guestCooldown = Math.max(remainingSec, cooldownSec);

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
            updateGuestButtonState();
        }

        ensureGuestCooldownTimer();
    }

    async function handleGenerateGuestCode() {
        if (state.guestRequestInFlight) {
            return;
        }

        try {
            state.guestRequestInFlight = true;
            updateGuestButtonState();

            const statusTimeoutMs = Math.max(1200, Number(CONFIG.STATUS_TIMEOUT_MS || 3200));
            const data = await apiFetch(CONFIG.API.GUEST_CODE, {
                method: 'POST',
                timeoutMs: statusTimeoutMs,
                retries: 0
            });
            if (!data?.success) {
                feedback.showToast(data?.message || 'Guest code generation failed', 'error');
                return;
            }

            const code = String(data.guestCode || '').trim();
            const remainingMs = Number(data.guestCodeRemainingMs || data.expiresInMs || 0);
            const remainingSec = Math.max(0, Math.ceil(remainingMs / 1000));

            if (code.length === 4) {
                displayGuestCode(code, remainingSec);
            } else {
                updateGuestButtonState();
            }

            feedback.showToast(
                data.reused
                    ? `Guest code already active (${remainingSec}s remaining)`
                    : 'Guest code generated successfully',
                'success'
            );

            if (typeof onLogsUpdated === 'function') {
                onLogsUpdated();
            }
        } catch (error) {
            const retryAfterSec = Number(error?.payload?.retryAfterSec || 0);
            if (retryAfterSec > 0) {
                state.guestCooldown = Math.max(state.guestCooldown || 0, retryAfterSec);
            }

            feedback.showToast(
                error?.payload?.message || 'Connection error — could not generate guest code',
                'error'
            );
        } finally {
            state.guestRequestInFlight = false;
            updateGuestButtonState();
        }
    }

    function bindEvents() {
        if (DOM.btnGuestCode) {
            DOM.btnGuestCode.addEventListener('click', handleGenerateGuestCode);
        }

        updateGuestButtonState();
    }

    return {
        bindEvents,
        syncFromStatus
    };
}
