export function createStatusFeature({ CONFIG, state, DOM, apiFetch, feedback, onLogsUpdated }) {
    let lastDiagnosticsFetchMs = Date.now();
    let lastDiagnosticsText = 'Diagnostics: initializing...';
    let diagnosticsRequestInFlight = false;
    let diagnosticsFailureCount = 0;
    let statusConsecutiveFailures = 0;
    let lastStatusSuccessAtMs = 0;

    function formatMs(value) {
        const ms = Math.max(0, Math.round(Number(value) || 0));
        return `${ms}ms`;
    }

    function setNodeText(node, nextText) {
        if (!node) {
            return;
        }

        const normalizedText = String(nextText ?? '');
        if (node.textContent !== normalizedText) {
            node.textContent = normalizedText;
        }
    }

    function setNodeColor(node, nextColor) {
        if (!node) {
            return;
        }

        const normalizedColor = String(nextColor ?? '');
        if (node.style.color !== normalizedColor) {
            node.style.color = normalizedColor;
        }
    }

    function updateTelemetryPanel() {
        if (!DOM.telemetryPanel) {
            return;
        }

        if (DOM.telemetryApiRtt) {
            const rtt = Number(state.statusApiRttSmoothedMs || state.statusApiRttMs || 0);
            const nextRttText = rtt > 0 ? formatMs(rtt) : '--';
            setNodeText(DOM.telemetryApiRtt, nextRttText);
        }

        if (DOM.telemetryQueue) {
            const depth = Math.max(0, Number(state.telegramNotifyQueueDepth || 0));
            const capacity = Math.max(0, Number(state.telegramNotifyQueueCapacity || 0));
            const oldestAgeMs = Math.max(0, Number(state.telegramNotifyQueueOldestAgeMs || 0));

            if (capacity > 0) {
                setNodeText(DOM.telemetryQueue, `${depth}/${capacity} (${Math.ceil(oldestAgeMs / 1000)}s)`);
            } else {
                setNodeText(DOM.telemetryQueue, `${depth}`);
            }
        }

        if (DOM.telemetryPoll) {
            const pollDuration = Math.max(0, Number(state.telegramLastPollDurationMs || 0));
            const pollInterval = Math.max(0, Number(state.telegramPollIntervalMs || 0));
            const nextPollText = (pollDuration > 0 || pollInterval > 0)
                ? `${pollDuration}ms @ ${pollInterval}ms`
                : '--';
            setNodeText(DOM.telemetryPoll, nextPollText);
        }
    }

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
        let nextText = '';
        let nextColor = '';

        if (state.alarm) {
            nextText = '\u26A0 ALARM ACTIVE';
            nextColor = 'var(--danger)';
        } else if (state.buzzerActive) {
            nextText = state.sirenActive ? 'Siren Pulse Active' : 'Buzzer Feedback Active';
            nextColor = 'var(--warning, #f59e0b)';
        } else if (state.authPrompt) {
            nextText = state.authPrompt;
            nextColor = 'var(--accent, #60A5FA)';
        } else if (state.locked) {
            nextText = 'System Armed \u2022 Secure';
        } else if (state.lockCountdownSeconds > 0) {
            nextText = `Door Open \u2022 Auto-lock in ${state.lockCountdownSeconds}s`;
        } else {
            nextText = 'Door Open \u2022 Auto-locking...';
        }

        setNodeColor(DOM.lockStatusSub, nextColor);
        setNodeText(DOM.lockStatusSub, nextText);
    }

    function updateLockUI(locked) {
        const lockState = locked ? 'locked' : 'unlocked';
        if (DOM.lockVisual?.dataset?.lockState !== lockState) {
            DOM.lockVisual.dataset.lockState = lockState;
        }
        setNodeText(DOM.lockStatusLabel, locked ? 'LOCKED' : 'UNLOCKED');
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

        let networkMultiplier = 1;
        const rtt = Math.max(0, Number(state.statusApiRttSmoothedMs || state.statusApiRttMs || 0));
        if (rtt >= 900) {
            networkMultiplier *= 1.85;
        } else if (rtt >= 500) {
            networkMultiplier *= 1.4;
        }

        if (typeof navigator !== 'undefined' && navigator.connection) {
            const connection = navigator.connection;
            const effectiveType = String(connection.effectiveType || '').toLowerCase();
            const saveData = Boolean(connection.saveData);

            if (saveData) {
                networkMultiplier *= 1.5;
            }

            if (effectiveType === 'slow-2g' || effectiveType === '2g') {
                networkMultiplier *= 1.5;
            } else if (effectiveType === '3g') {
                networkMultiplier *= 1.2;
            }
        }

        if (document.hidden) {
            return Math.round(hidden * networkMultiplier);
        }

        if (window.matchMedia('(max-width: 768px)').matches) {
            return Math.round(mobile * networkMultiplier);
        }

        return Math.round(base * networkMultiplier);
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

        const diagnosticsTimeoutMs = Math.max(1000, Number(CONFIG.DIAGNOSTICS_TIMEOUT_MS || 2200));
        const defaultRetryCount = Math.max(0, Number(CONFIG.API_RETRY_COUNT || 0));
        const retryBaseDelayMs = Math.max(100, Number(CONFIG.API_RETRY_BASE_DELAY_MS || 170));
        const retryMaxDelayMs = Math.max(retryBaseDelayMs, Number(CONFIG.API_RETRY_MAX_DELAY_MS || 700));

        apiFetch(CONFIG.API.DIAGNOSTICS, {
            timeoutMs: diagnosticsTimeoutMs,
            retries: defaultRetryCount,
            retryBaseDelayMs,
            retryMaxDelayMs
        })
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
                const tgNotifyDepth = Math.max(0, Number(state.telegramNotifyQueueDepth || 0));
                const tgNotifyCap = Math.max(0, Number(state.telegramNotifyQueueCapacity || 0));
                const tgNotifyAgeSec = Math.ceil(Math.max(0, Number(state.telegramNotifyQueueOldestAgeMs || 0)) / 1000);
                const tgText = `TG ${state.telegramLastPollDurationMs}ms@${state.telegramPollIntervalMs}ms, cmd ${tgAge}, q${state.telegramPendingApprox}, nq${tgNotifyDepth}/${tgNotifyCap}(${tgNotifyAgeSec}s), e${state.telegramPollErrors}, last ${tgCmd}`;
                const fallbackApText = state.fallbackApActive
                    ? `AP ${state.fallbackApSSID || 'active'} @ ${state.fallbackApIP || '192.168.4.1'}`
                    : '';
                const activeUsers = Number(diag.activeUsers || 0);
                const rawUsers = Number(diag.rawUsers || 0);
                const badUsers = Number(diag.invalidUsers || 0) + Number(diag.duplicateUsers || 0);
                const usersFlag = diag.usersStorageMismatch ? `MISMATCH(${badUsers})` : 'OK';
                const usersText = `USERS ${activeUsers}/${rawUsers} ${usersFlag}`;

                if (compactMobile) {
                    const tgCompact = `TG q${state.telegramPendingApprox} nq${tgNotifyDepth}/${tgNotifyCap} e${state.telegramPollErrors}`;
                    const apCompact = fallbackApText ? ` • ${fallbackApText}` : '';
                    lastDiagnosticsText = `Diagnostics: ${rfidText} • ${keypadText} • ${usersText} • ${tgCompact}${apCompact}`;
                } else {
                    const apDetail = fallbackApText ? ` • ${fallbackApText}` : '';
                    lastDiagnosticsText = `Diagnostics: ${rfidText} • ${keypadText} • ${keyText} • ${usersText} • ${tgText}${apDetail}`;
                }

                lastDiagnosticsFetchMs = Date.now();
                diagnosticsFailureCount = 0;
            })
            .catch(() => {
                diagnosticsFailureCount += 1;
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
        const requestStartedAt = (typeof performance !== 'undefined' && typeof performance.now === 'function')
            ? performance.now()
            : Date.now();

        try {
            const statusTimeoutMs = Math.max(1200, Number(CONFIG.STATUS_TIMEOUT_MS || 3200));
            const defaultRetryCount = Math.max(0, Number(CONFIG.API_RETRY_COUNT || 0));
            const retryBaseDelayMs = Math.max(100, Number(CONFIG.API_RETRY_BASE_DELAY_MS || 170));
            const retryMaxDelayMs = Math.max(retryBaseDelayMs, Number(CONFIG.API_RETRY_MAX_DELAY_MS || 700));

            const data = await apiFetch(CONFIG.API.STATUS, {
                timeoutMs: statusTimeoutMs,
                retries: defaultRetryCount,
                retryBaseDelayMs,
                retryMaxDelayMs
            });
            const requestFinishedAt = (typeof performance !== 'undefined' && typeof performance.now === 'function')
                ? performance.now()
                : Date.now();
            const requestRttMs = Math.max(0, Number(requestFinishedAt - requestStartedAt) || 0);

            state.statusApiRttMs = Math.round(requestRttMs);
            if (!Number.isFinite(state.statusApiRttSmoothedMs) || state.statusApiRttSmoothedMs <= 0) {
                state.statusApiRttSmoothedMs = state.statusApiRttMs;
            } else {
                state.statusApiRttSmoothedMs = Math.round((state.statusApiRttSmoothedMs * 0.72) + (state.statusApiRttMs * 0.28));
            }

            statusConsecutiveFailures = 0;
            lastStatusSuccessAtMs = Date.now();
            setConnectionState(true);

            state.locked = Boolean(data.locked);
            state.autoLockActive = Boolean(data.autoLockActive);
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
            state.telegramNotifyQueueDepth = Number(data.telegramNotifyQueueDepth || 0);
            state.telegramNotifyQueueCapacity = Number(data.telegramNotifyQueueCapacity || 0);
            state.telegramNotifyQueueOldestAgeMs = Number(data.telegramNotifyQueueOldestAgeMs || 0);
            state.fallbackApActive = Boolean(data.fallbackApActive);
            state.fallbackApSSID = String(data.fallbackApSSID || '');
            state.fallbackApIP = String(data.fallbackApIP || '');
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
                } else if (!hasValidDelay && state.lockCountdownSeconds <= 0) {
                    stopDoorCountdown();
                }
            } else {
                stopDoorCountdown();
            }

            updateLockUI(state.locked);
            updateAlarmState(state.alarm);
            updateEmergencyButton();
            updateTelemetryPanel();

            refreshDiagnosticsAsync();

            return data;
        } catch {
            statusConsecutiveFailures += 1;
            const staleSinceSuccessMs = lastStatusSuccessAtMs > 0
                ? (Date.now() - lastStatusSuccessAtMs)
                : Number.POSITIVE_INFINITY;

            if (statusConsecutiveFailures >= 2 || staleSinceSuccessMs >= 15000) {
                setConnectionState(false);
            }
            updateTelemetryPanel();
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
                        state.autoLockActive = true;
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
