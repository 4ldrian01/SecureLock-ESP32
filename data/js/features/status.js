export function createStatusFeature({ CONFIG, state, DOM, apiFetch, feedback, onLogsUpdated }) {
    let lastDiagnosticsFetchMs = 0;
    let lastDiagnosticsText = 'Diagnostics: initializing...';
    let diagnosticsRequestInFlight = false;

    function applyStatusBadgeVariant(online) {
        DOM.statusBadge.classList.toggle('badge-success', Boolean(online));
        DOM.statusBadge.classList.toggle('badge-danger', !online);
    }

    function setConnectionState(online) {
        state.connected = online;
        DOM.statusBadge.dataset.status = online ? 'online' : 'offline';
        applyStatusBadgeVariant(online);
        DOM.statusText.textContent = online ? 'Online' : 'Offline';
    }

    function renderLockStatusSub() {
        if (state.alarm) {
            DOM.lockStatusSub.textContent = '\u26A0 ALARM ACTIVE';
            DOM.lockStatusSub.style.color = 'var(--danger)';
            return;
        }

        if (state.buzzerActive) {
            DOM.lockStatusSub.textContent = state.sirenActive ? 'Siren Pulse Active' : 'Buzzer Feedback Active';
            DOM.lockStatusSub.style.color = 'var(--warning, #f59e0b)';
            return;
        }

        if (state.authPrompt) {
            DOM.lockStatusSub.textContent = state.authPrompt;
            DOM.lockStatusSub.style.color = 'var(--accent, #60A5FA)';
            return;
        }

        DOM.lockStatusSub.style.color = '';

        if (state.locked) {
            DOM.lockStatusSub.textContent = 'System Armed \u2022 Secure';
            return;
        }

        if (state.lockCountdownSeconds > 0) {
            DOM.lockStatusSub.textContent = `Door Open \u2022 Auto-lock in ${state.lockCountdownSeconds}s`;
            return;
        }

        DOM.lockStatusSub.textContent = 'Door Open \u2022 Auto-locking...';
    }

    function updateLockUI(locked) {
        const lockState = locked ? 'locked' : 'unlocked';
        DOM.lockVisual.dataset.lockState = lockState;
        DOM.lockStatusLabel.textContent = locked ? 'LOCKED' : 'UNLOCKED';
        renderLockStatusSub();
    }

    function updateAlarmState(alarming) {
        state.alarm = Boolean(alarming);
        renderLockStatusSub();
    }

    function stopDoorCountdown() {
        if (state.lockCountdownTimer) {
            clearInterval(state.lockCountdownTimer);
            state.lockCountdownTimer = null;
        }

        state.lockCountdownSeconds = 0;
    }

    function setDoorCountdownFromMs(remainingMs) {
        const safeMs = Math.max(0, Number(remainingMs) || 0);
        const nextSeconds = Math.max(0, Math.ceil(safeMs / 1000));

        if (state.lockCountdownTimer) {
            if (nextSeconds > 0 && (state.lockCountdownSeconds <= 0 || nextSeconds < state.lockCountdownSeconds)) {
                state.lockCountdownSeconds = nextSeconds;
                renderLockStatusSub();
            }
            return;
        }

        state.lockCountdownSeconds = nextSeconds;
        renderLockStatusSub();

        if (state.lockCountdownSeconds <= 0) {
            if (state.lockCountdownTimer) {
                clearInterval(state.lockCountdownTimer);
                state.lockCountdownTimer = null;
            }
            return;
        }

        state.lockCountdownTimer = setInterval(() => {
            if (state.locked) {
                stopDoorCountdown();
                return;
            }

            state.lockCountdownSeconds = Math.max(0, state.lockCountdownSeconds - 1);
            renderLockStatusSub();

            if (state.lockCountdownSeconds === 0 && state.lockCountdownTimer) {
                clearInterval(state.lockCountdownTimer);
                state.lockCountdownTimer = null;
            }
        }, 1000);
    }

    function updateEmergencyButton() {
        if (state.emergencyRequestInFlight) {
            DOM.btnEmergency.disabled = true;
            DOM.btnEmergency.textContent = 'Unlocking...';
            DOM.btnEmergency.dataset.state = 'busy';
            DOM.btnEmergency.classList.remove('btn-danger');
            DOM.btnEmergency.classList.add('btn-ghost');
            return;
        }

        if (state.emergencyGuestLock > 0) {
            DOM.btnEmergency.disabled = true;
            DOM.btnEmergency.textContent = `Guest code active (${state.emergencyGuestLock}s)`;
            DOM.btnEmergency.dataset.state = 'guest-lock';
            DOM.btnEmergency.classList.remove('btn-danger');
            DOM.btnEmergency.classList.add('btn-ghost');
            return;
        }

        if (state.emergencyCooldown > 0) {
            DOM.btnEmergency.disabled = true;
            DOM.btnEmergency.textContent = `Emergency Override (${state.emergencyCooldown}s)`;
            DOM.btnEmergency.dataset.state = 'cooldown';
            DOM.btnEmergency.classList.remove('btn-danger');
            DOM.btnEmergency.classList.add('btn-ghost');
            return;
        }

        DOM.btnEmergency.disabled = false;
        DOM.btnEmergency.textContent = 'Emergency Override';
        DOM.btnEmergency.dataset.state = 'ready';
        DOM.btnEmergency.classList.add('btn-danger');
        DOM.btnEmergency.classList.remove('btn-ghost');
    }

    function getDiagnosticsIntervalMs() {
        const base = Math.max(3000, Number(CONFIG.DIAGNOSTICS_INTERVAL || 3000));
        const hidden = Math.max(base, Number(CONFIG.DIAGNOSTICS_INTERVAL_HIDDEN || 12000));
        const mobile = Math.max(base, Number(CONFIG.DIAGNOSTICS_INTERVAL_MOBILE || 8000));

        if (document.hidden) {
            return hidden;
        }

        if (window.matchMedia('(max-width: 768px)').matches) {
            return mobile;
        }

        return base;
    }

    function refreshDiagnosticsAsync() {
        if (!DOM.diagStatus) {
            return;
        }

        DOM.diagStatus.textContent = lastDiagnosticsText;

        const now = Date.now();
        const diagIntervalMs = getDiagnosticsIntervalMs();
        if (diagnosticsRequestInFlight || (now - lastDiagnosticsFetchMs) < diagIntervalMs) {
            return;
        }

        diagnosticsRequestInFlight = true;

        apiFetch(CONFIG.API.DIAGNOSTICS)
            .then((diag) => {
                const compactMobile = window.matchMedia('(max-width: 640px)').matches;
                const rfidText = diag.rfidReady ? 'RFID OK' : 'RFID WAIT';
                const keypadText = diag.keypadReady
                    ? (diag.keypadMuted ? `KEYPAD MUTED ${Math.ceil((Number(diag.keypadMuteRemainingMs) || 0) / 1000)}s` : 'KEYPAD OK')
                    : 'KEYPAD INIT';
                const keyText = diag.keypadLastKey ? `Last key: ${diag.keypadLastKey}` : 'Last key: -';
                const tgAge = state.telegramLastCommandAgeMs >= 0
                    ? `${Math.ceil(state.telegramLastCommandAgeMs / 1000)}s`
                    : '-';
                const tgCmd = state.telegramLastCommandText
                    ? `${state.telegramLastCommandRole || 'user'}:${state.telegramLastCommandText}(${state.telegramLastCommandResult || 'ok'})`
                    : 'none';
                const tgText = `TG ${state.telegramLastPollDurationMs}ms@${state.telegramPollIntervalMs}ms, cmd ${tgAge}, q${state.telegramPendingApprox}, e${state.telegramPollErrors}, last ${tgCmd}`;
                const activeUsers = Number(diag.activeUsers || 0);
                const rawUsers = Number(diag.rawUsers || 0);
                const badUsers = Number(diag.invalidUsers || 0) + Number(diag.duplicateUsers || 0);
                const usersFlag = diag.usersStorageMismatch ? `MISMATCH(${badUsers})` : 'OK';
                const usersText = `USERS ${activeUsers}/${rawUsers} ${usersFlag}`;

                if (compactMobile) {
                    const tgCompact = `TG q${state.telegramPendingApprox} e${state.telegramPollErrors}`;
                    lastDiagnosticsText = `Diagnostics: ${rfidText} • ${keypadText} • ${usersText} • ${tgCompact}`;
                } else {
                    lastDiagnosticsText = `Diagnostics: ${rfidText} • ${keypadText} • ${keyText} • ${usersText} • ${tgText}`;
                }

                lastDiagnosticsFetchMs = Date.now();
            })
            .catch(() => {
                if ((Date.now() - lastDiagnosticsFetchMs) > Math.max(diagIntervalMs * 2, 30000)) {
                    lastDiagnosticsText = 'Diagnostics: unavailable';
                }
            })
            .finally(() => {
                diagnosticsRequestInFlight = false;
                if (DOM.diagStatus) {
                    DOM.diagStatus.textContent = lastDiagnosticsText;
                }
            });
    }

    function setEmergencyBusy(isBusy) {
        state.emergencyRequestInFlight = isBusy;
        updateEmergencyButton();
    }

    function startEmergencyCooldown(seconds) {
        const cooldownSeconds = Math.max(0, Math.ceil(Number(seconds) || 0));
        state.emergencyCooldown = cooldownSeconds;

        if (state.emergencyCooldownTimer) {
            clearInterval(state.emergencyCooldownTimer);
            state.emergencyCooldownTimer = null;
        }

        updateEmergencyButton();

        if (cooldownSeconds === 0) return;

        state.emergencyCooldownTimer = setInterval(() => {
            state.emergencyCooldown = Math.max(0, state.emergencyCooldown - 1);
            updateEmergencyButton();

            if (state.emergencyCooldown === 0 && state.emergencyCooldownTimer) {
                clearInterval(state.emergencyCooldownTimer);
                state.emergencyCooldownTimer = null;
            }
        }, 1000);
    }

    async function pollStatus() {
        try {
            const data = await apiFetch(CONFIG.API.STATUS);
            setConnectionState(true);

            state.locked = Boolean(data.locked);
            state.alarm = Boolean(data.alarm);
            state.buzzerActive = Boolean(data.buzzerActive);
            state.sirenActive = Boolean(data.sirenActive);
            state.authPrompt = String(data.authPrompt || '');
            state.telegramPollIntervalMs = Number(data.telegramPollIntervalMs || 0);
            state.telegramLastPollDurationMs = Number(data.telegramLastPollDurationMs || 0);
            state.telegramLastCommandAgeMs = Number(data.telegramLastCommandAgeMs ?? -1);
            state.telegramLastCommandLatencyMs = Number(data.telegramLastCommandLatencyMs || 0);
            state.telegramLastCommandText = String(data.telegramLastCommandText || '');
            state.telegramLastCommandRole = String(data.telegramLastCommandRole || '');
            state.telegramLastCommandResult = String(data.telegramLastCommandResult || '');
            state.telegramPendingApprox = Number(data.telegramPendingApprox || 0);
            state.telegramPollErrors = Number(data.telegramPollErrors || 0);
            state.emergencyGuestLock = Math.max(
                0,
                Math.ceil(Number(data.emergencyGuestLockRemainingMs || 0) / 1000)
            );

            const emergencyCooldownRemainingMs = Number(data.emergencyCooldownRemainingMs);
            if (Number.isFinite(emergencyCooldownRemainingMs) && emergencyCooldownRemainingMs > 0) {
                const emergencySec = Math.ceil(emergencyCooldownRemainingMs / 1000);
                if (emergencySec > state.emergencyCooldown) {
                    startEmergencyCooldown(emergencySec);
                }
            }

            if (!state.locked) {
                const remainingMs = Number(data.unlockRemainingMs);
                const autoLockDelayMs = Number(data.autoLockDelayMs);
                const hasServerRemaining = Number.isFinite(remainingMs) && remainingMs > 0;
                const hasValidDelay = Number.isFinite(autoLockDelayMs) && autoLockDelayMs > 0;

                if (hasServerRemaining) {
                    setDoorCountdownFromMs(remainingMs);
                } else if (!state.lockCountdownTimer && state.lockCountdownSeconds <= 0 && hasValidDelay) {
                    setDoorCountdownFromMs(autoLockDelayMs);
                }
            } else {
                stopDoorCountdown();
            }

            updateLockUI(state.locked);
            updateAlarmState(state.alarm);
            updateEmergencyButton();

            refreshDiagnosticsAsync();

            return data;
        } catch {
            setConnectionState(false);
            return null;
        }
    }

    function handleEmergencyUnlock() {
        if (state.emergencyCooldown > 0 || state.emergencyRequestInFlight) {
            feedback.showToast(`Please wait ${state.emergencyCooldown || 1}s before emergency override`, 'info');
            return;
        }

        feedback.showModal(
            'Emergency Override',
            'This will immediately unlock the door. Are you sure?',
            'danger',
            async () => {
                if (state.emergencyCooldown > 0 || state.emergencyRequestInFlight) {
                    return;
                }

                try {
                    setEmergencyBusy(true);

                    const data = await apiFetch(CONFIG.API.UNLOCK, { method: 'POST' });

                    if (data.success) {
                        state.locked = false;
                        const unlockRemainingMs = Number(data.unlockRemainingMs || data.autoLockDelayMs || 5000);
                        setDoorCountdownFromMs(unlockRemainingMs);
                        updateLockUI(false);
                        feedback.showToast('Door unlocked via emergency override', 'success');
                        const cooldownSeconds = Math.ceil((data.cooldownMs || (CONFIG.EMERGENCY_COOLDOWN * 1000)) / 1000);
                        startEmergencyCooldown(cooldownSeconds);
                    } else {
                        feedback.showToast('Unlock failed: ' + (data.message || 'Unknown error'), 'error');
                    }
                    onLogsUpdated();
                } catch (error) {
                    if (String(error?.payload?.errorCode || '') === 'GUEST_CODE_ACTIVE') {
                        const retryAfter = Number(error?.payload?.retryAfterSec || 0);
                        if (retryAfter > 0) {
                            state.emergencyGuestLock = retryAfter;
                            updateEmergencyButton();
                        }
                        feedback.showToast(
                            error?.payload?.message || 'Emergency override is unavailable while guest code is active',
                            'info'
                        );
                        onLogsUpdated();
                        return;
                    }

                    const retryAfter = Number(error?.payload?.retryAfterSec || 0);
                    if (retryAfter > 0) {
                        startEmergencyCooldown(retryAfter);
                        feedback.showToast(`Emergency override cooling down (${retryAfter}s remaining)`, 'info');
                        onLogsUpdated();
                    } else {
                        feedback.showToast('Connection error — could not unlock', 'error');
                    }
                } finally {
                    setEmergencyBusy(false);
                }
            }
        );
    }

    function bindEvents() {
        DOM.btnEmergency.addEventListener('click', handleEmergencyUnlock);
    }

    return {
        pollStatus,
        bindEvents,
        updateEmergencyButton
    };
}
