import { GUEST_BUTTON_ICON } from '../core/config.js';

export function createGuestFeature({ CONFIG, state, DOM, apiFetch, feedback, onLogsUpdated }) {
    function formatCooldownClock(seconds) {
        const safeSeconds = Math.max(0, Number(seconds) || 0);
        const min = Math.floor(safeSeconds / 60);
        const sec = safeSeconds % 60;
        return `${min}:${sec.toString().padStart(2, '0')}`;
    }

    function renderGuestGenerateLabel(text) {
        DOM.btnGenerate.innerHTML = `${GUEST_BUTTON_ICON} ${text}`;
    }

    function updateGuestGenerateButton() {
        if (state.guestRequestInFlight) {
            DOM.btnGenerate.disabled = true;
            renderGuestGenerateLabel('Generating...');
            return;
        }

        if (state.guestCooldown > 0) {
            DOM.btnGenerate.disabled = true;
            renderGuestGenerateLabel(`Generate New Code (${formatCooldownClock(state.guestCooldown)})`);
            return;
        }

        DOM.btnGenerate.disabled = false;
        renderGuestGenerateLabel('Generate New Code');
    }

    function setGuestGenerateBusy(isBusy) {
        state.guestRequestInFlight = isBusy;
        updateGuestGenerateButton();
    }

    function startGuestGenerateCooldown(seconds) {
        const cooldownSeconds = Math.max(0, Math.ceil(Number(seconds) || 0));
        state.guestCooldown = cooldownSeconds;

        if (state.guestCooldownTimer) {
            clearInterval(state.guestCooldownTimer);
            state.guestCooldownTimer = null;
        }

        updateGuestGenerateButton();

        if (cooldownSeconds === 0) return;

        state.guestCooldownTimer = setInterval(() => {
            state.guestCooldown = Math.max(0, state.guestCooldown - 1);
            updateGuestGenerateButton();

            if (state.guestCooldown === 0 && state.guestCooldownTimer) {
                clearInterval(state.guestCooldownTimer);
                state.guestCooldownTimer = null;
            }
        }, 1000);
    }

    function displayGuestCode(code, expiresIn) {
        const digits = code.toString().padStart(4, '0').split('');
        const pinDigits = DOM.pinCode.querySelectorAll('.pin-digit');
        pinDigits.forEach((el, i) => {
            el.textContent = digits[i] || '-';
        });

        state.guestCode = code;
        state.guestExpiry = expiresIn;

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

        const min = Math.floor(expiresIn / 60);
        const sec = expiresIn % 60;
        DOM.pinTimer.textContent = `Expires in ${min}:${sec.toString().padStart(2, '0')}`;
        DOM.pinTimer.dataset.expired = 'false';
    }

    function clearGuestCodeDisplay() {
        const pinDigits = DOM.pinCode.querySelectorAll('.pin-digit');
        pinDigits.forEach(el => { el.textContent = '-'; });
        DOM.pinTimer.textContent = 'Code expired';
        DOM.pinTimer.dataset.expired = 'true';
    }

    async function handleGenerateGuestCode() {
        if (state.guestCooldown > 0 || state.guestRequestInFlight) {
            feedback.showToast(`Guest code cooldown active (${formatCooldownClock(state.guestCooldown || 1)} left)`, 'info');
            return;
        }

        try {
            setGuestGenerateBusy(true);

            const data = await apiFetch(CONFIG.API.GUEST_CODE, { method: 'POST' });

            if (data.success && data.code) {
                displayGuestCode(data.code, data.expiresIn || CONFIG.GUEST_CODE_EXPIRY);
                feedback.showToast(`Guest code generated\n${data.code}`, 'success');
                const cooldownSeconds = Math.ceil((Number(data.cooldownMs) || (CONFIG.GUEST_GENERATE_COOLDOWN * 1000)) / 1000);
                startGuestGenerateCooldown(cooldownSeconds);
            } else {
                feedback.showToast('Failed to generate guest code', 'error');
            }
            onLogsUpdated();
        } catch (error) {
            const retryAfter = Number(error?.payload?.retryAfterSec || 0);
            if (retryAfter > 0) {
                startGuestGenerateCooldown(retryAfter);
                feedback.showToast(`Guest code cooling down (${formatCooldownClock(retryAfter)} remaining)`, 'info');
                onLogsUpdated();
            } else {
                feedback.showToast('Connection error — could not generate code', 'error');
            }
        } finally {
            setGuestGenerateBusy(false);
        }
    }

    function syncFromStatus(statusData) {
        const guestCooldownRemainingMs = Number(statusData?.guestCodeCooldownRemainingMs);
        if (Number.isFinite(guestCooldownRemainingMs) && guestCooldownRemainingMs > 0) {
            const guestSec = Math.ceil(guestCooldownRemainingMs / 1000);
            if (guestSec > state.guestCooldown) {
                startGuestGenerateCooldown(guestSec);
            }
        }
    }

    function bindEvents() {
        DOM.btnGenerate.addEventListener('click', handleGenerateGuestCode);
    }

    return {
        bindEvents,
        updateGuestGenerateButton,
        startGuestGenerateCooldown,
        syncFromStatus
    };
}
