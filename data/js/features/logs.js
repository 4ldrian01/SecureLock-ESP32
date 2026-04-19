import { escapeHtml, formatLogTime } from '../core/helpers.js?v=20260418r7';

const LOG_STATUS_OPTIONS = ['all', 'success', 'fail', 'alarm', 'info'];
const LOG_STATUS_LABELS = {
    all: 'All',
    success: 'Success',
    fail: 'Failed',
    alarm: 'Alarm',
    info: 'Info'
};

export function createLogsFeature({ CONFIG, state, DOM, apiFetch, feedback }) {
    function getLogSortScore(log, index) {
        const epochMs = Number(log?.epochMs || 0);
        if (Number.isFinite(epochMs) && epochMs > 0) {
            return epochMs;
        }

        const uptimeMs = Number(log?.uptimeMs || 0);
        if (Number.isFinite(uptimeMs) && uptimeMs > 0) {
            // Offset to keep uptime-based logs sorted after true-epoch logs while
            // still preserving newest-first among uptime-only entries.
            return uptimeMs;
        }

        return index;
    }

    function sortLogsLatestFirst(logs) {
        return (Array.isArray(logs) ? logs : [])
            .map((log, index) => ({ log, index }))
            .sort((a, b) => getLogSortScore(b.log, b.index) - getLogSortScore(a.log, a.index))
            .map((entry) => entry.log);
    }

    function normalizeLogStatus(rawStatus) {
        const value = String(rawStatus || '').trim().toLowerCase();
        if (value === 'ok' || value === 'pass') return 'success';
        if (value === 'error' || value === 'failed' || value === 'deny' || value === 'denied') return 'fail';
        if (value === 'warn' || value === 'warning' || value === 'alert') return 'alarm';
        if (value === 'success' || value === 'fail' || value === 'alarm' || value === 'info') {
            return value;
        }
        return 'info';
    }

    function normalizeActorRole(rawRole) {
        const value = String(rawRole || '').trim().toLowerCase();

        if (value === 'admin' || value === 'administrator') return 'admin';
        if (value === 'user' || value === 'regular' || value === 'regular-user' || value === 'member') return 'user';
        if (value === 'guest' || value === 'visitor') return 'guest';
        if (value === 'system' || value === 'service' || value === 'device') return 'system';

        return '';
    }

    function deriveActorRole(log) {
        const explicitRole = normalizeActorRole(log?.actorRole);
        if (explicitRole) {
            return explicitRole;
        }

        const userText = String(log?.user || '').trim().toLowerCase();
        const methodText = String(log?.method || '').trim().toLowerCase();

        if (userText.includes('guest') || methodText.includes('guest')) {
            return 'guest';
        }

        if (
            userText.includes('unregistered')
            || userText.includes('unknown rfid')
            || methodText.includes('unauthorized')
        ) {
            return 'guest';
        }

        if (userText.includes('(telegram admin)')) {
            return 'admin';
        }

        if (userText.includes('(telegram user)')) {
            return 'user';
        }

        if (
            userText.includes('admin')
            || userText.includes('(web)')
            || methodText.includes('(web)')
            || methodText.includes('admin')
        ) {
            return 'admin';
        }

        if (
            (userText.includes('telegram') || methodText.includes('telegram'))
            && methodText.includes('command')
        ) {
            return 'user';
        }

        if (
            userText.includes('user')
            || methodText.includes('rfid')
            || methodText.includes('otp')
            || methodText.includes('backup')
            || methodText.includes('pin')
            || methodText.includes('keypad')
        ) {
            return 'user';
        }

        return 'system';
    }

    function getActorRoleMeta(role) {
        const normalized = normalizeActorRole(role) || 'system';

        if (normalized === 'admin') {
            return {
                key: 'admin',
                label: 'Admin',
                className: 'actor-role-admin'
            };
        }

        if (normalized === 'user') {
            return {
                key: 'user',
                label: 'User',
                className: 'actor-role-user'
            };
        }

        if (normalized === 'guest') {
            return {
                key: 'guest',
                label: 'Guest',
                className: 'actor-role-guest'
            };
        }

        return {
            key: 'system',
            label: 'System',
            className: 'actor-role-system'
        };
    }

    function getLogEpochMs(log) {
        const epochMs = Number(log?.epochMs || 0);
        if (Number.isFinite(epochMs) && epochMs > 0) {
            return epochMs;
        }

        const timeText = String(log?.time || '').trim();
        if (!timeText || timeText.includes('(uptime)')) {
            return 0;
        }

        const normalized = timeText.endsWith(' UTC')
            ? `${timeText.replace(' UTC', '').replace(' ', 'T')}Z`
            : timeText;

        const parsed = new Date(normalized).getTime();
        if (!Number.isFinite(parsed) || Number.isNaN(parsed)) {
            return 0;
        }

        return parsed;
    }

    function getDateRangeBounds() {
        const fromRaw = String(state.logsDateFrom || '').trim();
        const toRaw = String(state.logsDateTo || '').trim();

        const fromMs = /^\d{4}-\d{2}-\d{2}$/.test(fromRaw)
            ? new Date(`${fromRaw}T00:00:00.000`).getTime()
            : 0;
        const toMs = /^\d{4}-\d{2}-\d{2}$/.test(toRaw)
            ? new Date(`${toRaw}T23:59:59.999`).getTime()
            : 0;

        return {
            fromRaw,
            toRaw,
            fromMs: Number.isFinite(fromMs) ? fromMs : 0,
            toMs: Number.isFinite(toMs) ? toMs : 0
        };
    }

    function normalizeDisplayUser(rawUser, role) {
        const raw = String(rawUser || '').trim();
        if (!raw) {
            return role === 'admin' ? 'Admin'
                : role === 'guest' ? 'Guest'
                    : role === 'user' ? 'User'
                        : 'System';
        }

        const cleaned = raw
            .replace(/\s*\((web|telegram|telegram admin|telegram user|telegram guest|device|system)\)\s*$/i, '')
            .replace(/\s{2,}/g, ' ')
            .trim();

        if (cleaned) {
            return cleaned;
        }

        return raw;
    }

    function getBaseFilteredLogs() {
        const allLogs = Array.isArray(state.allLogs) ? state.allLogs : [];
        const query = String(state.logsSearchQuery || '').trim().toLowerCase();
        const queryTokens = query.length > 0
            ? query.split(/\s+/).filter(Boolean)
            : [];
        const { fromMs, toMs } = getDateRangeBounds();
        const shouldFilterByDate = fromMs > 0 || toMs > 0;

        if (queryTokens.length === 0 && !shouldFilterByDate) {
            return allLogs;
        }

        return allLogs.filter((log) => {
            if (shouldFilterByDate) {
                const epochMs = getLogEpochMs(log);
                if (epochMs <= 0) {
                    return false;
                }

                if (fromMs > 0 && epochMs < fromMs) {
                    return false;
                }

                if (toMs > 0 && epochMs > toMs) {
                    return false;
                }
            }

            if (queryTokens.length === 0) {
                return true;
            }

            const status = normalizeLogStatus(log?.status);
            const actorRole = deriveActorRole(log);
            const actorRoleMeta = getActorRoleMeta(actorRole);
            const methodLabel = deriveMethodLabel(log);
            const source = deriveSourceLabel(log);
            const displayUser = normalizeDisplayUser(log?.user, actorRole);

            const haystack = [
                log?.time,
                log?.user,
                displayUser,
                actorRole,
                actorRoleMeta.label,
                log?.method,
                methodLabel,
                source?.label,
                status,
                formatLogTime(log, false)
            ]
                .map((value) => String(value || '').toLowerCase())
                .join(' ');

            return queryTokens.every((token) => haystack.includes(token));
        });
    }

    function getFilteredLogs() {
        const baseLogs = getBaseFilteredLogs();
        const rawSelectedStatus = String(state.logsStatusFilter || 'all').trim().toLowerCase();
        const selectedStatus = rawSelectedStatus === 'all' ? 'all' : normalizeLogStatus(rawSelectedStatus);
        const shouldFilterByStatus = selectedStatus !== 'all';

        if (!shouldFilterByStatus) {
            return baseLogs;
        }

        return baseLogs.filter((log) => {
            const status = normalizeLogStatus(log?.status);
            if (status !== selectedStatus) {
                return false;
            }

            return true;
        });
    }

    function startLogsClockTicker() {
        if (state.logsClockTimer) {
            clearInterval(state.logsClockTimer);
            state.logsClockTimer = null;
        }

        const desktopTickMs = Math.max(5000, Number(CONFIG.LOGS_CLOCK_TICK_MS || 20000));
        const mobileTickMs = Math.max(desktopTickMs, Number(CONFIG.LOGS_CLOCK_TICK_MS_MOBILE || 30000));
        const tickMs = window.matchMedia('(max-width: 768px)').matches ? mobileTickMs : desktopTickMs;

        state.logsClockTimer = setInterval(() => {
            if (document.hidden) {
                return;
            }

            if (!Array.isArray(state.allLogs) || state.allLogs.length === 0) {
                return;
            }

            if (DOM.logsSection) {
                const rect = DOM.logsSection.getBoundingClientRect();
                const viewportHeight = window.innerHeight || document.documentElement.clientHeight || 0;
                const sectionVisible = !DOM.logsSection.hidden
                    && DOM.logsSection.getAttribute('aria-hidden') !== 'true';
                const sectionNearViewport = rect.bottom > -120 && rect.top < viewportHeight + 120;

                if (!sectionVisible || !sectionNearViewport) {
                    return;
                }
            }

            // Force row HTML refresh so relative time labels stay fresh.
            state.renderedLogsPageFingerprint = '';
            renderCurrentLogsPage();
        }, tickMs);
    }

    function createLogSignature(log) {
        return [
            String(log?.epochMs || ''),
            String(log?.time || ''),
            String(log?.user || ''),
            String(log?.method || ''),
            String(log?.methodCode || ''),
            String(log?.status || ''),
            String(log?.actorRole || '')
        ].join('|');
    }

    function createLogsCollectionSignature(logs) {
        if (!Array.isArray(logs) || logs.length === 0) {
            return '';
        }

        return logs.map(createLogSignature).join('||');
    }

    function deriveMethodLabel(log) {
        const explicitCode = String(log?.methodCode || '').trim().toUpperCase();
        if (explicitCode) {
            return explicitCode;
        }

        const rawMethod = String(log?.method || '').trim();
        const rawLower = rawMethod.toLowerCase();

        if (!rawLower) return 'SYSTEM';
        if (rawLower.includes('rfid')) return 'RFID';
        if (rawLower.includes('otp')) return 'OTP';
        if (rawLower.includes('pin')) return 'PIN';
        if (rawLower.includes('guest')) return 'GUEST';
        if (rawLower.includes('auth')) return 'AUTH';
        if (rawLower.includes('emergency')) return 'EMERGENCY';
        if (rawLower.includes('lockdown')) return 'LOCKDOWN';
        if (rawLower.includes('user')) return 'USER';
        return 'SYSTEM';
    }

    function deriveSourceLabel(log) {
        const userText = String(log?.user || '').trim().toLowerCase();
        const methodText = String(log?.method || '').trim().toLowerCase();

        if (userText.includes('telegram') || methodText.includes('telegram')) {
            return { label: 'TELEGRAM', className: 'source-telegram' };
        }

        if (userText.includes('(web)') || methodText.includes('(web)') || methodText.includes('api ')) {
            return { label: 'WEB', className: 'source-web' };
        }

        if (methodText.includes('rfid') || methodText.includes('keypad') || methodText.includes('backup') || methodText.includes('otp')) {
            return { label: 'DEVICE', className: 'source-device' };
        }

        return { label: 'SYSTEM', className: 'source-system' };
    }

    function getStatusCounts(logs) {
        const counts = {
            all: 0,
            success: 0,
            fail: 0,
            alarm: 0,
            info: 0
        };

        (Array.isArray(logs) ? logs : []).forEach((log) => {
            counts.all += 1;
            const status = normalizeLogStatus(log?.status);
            if (status in counts) {
                counts[status] += 1;
            } else {
                counts.info += 1;
            }
        });

        return counts;
    }

    function updateStatusFilterOptions(baseLogs) {
        if (!DOM.logsStatusFilter) {
            return;
        }

        const counts = getStatusCounts(baseLogs);

        Array.from(DOM.logsStatusFilter.options || []).forEach((option) => {
            const value = String(option.value || '').trim().toLowerCase();
            if (!(value in LOG_STATUS_LABELS)) {
                return;
            }

            const label = LOG_STATUS_LABELS[value];
            const count = Number(counts[value] || 0);
            option.textContent = `${label} (${count})`;
        });
    }

    function buildLogRowHTML(log, isCompactMobile) {
        const normalizedStatus = normalizeLogStatus(log?.status);
        const actorRole = deriveActorRole(log);
        const actorRoleMeta = getActorRoleMeta(actorRole);
        const statusClass = normalizedStatus === 'success'
            ? 'status-success'
            : normalizedStatus === 'alarm'
                ? 'status-alarm'
                : normalizedStatus === 'fail'
                    ? 'status-error'
                    : 'status-info';

        const statusLabel = isCompactMobile
            ? (normalizedStatus === 'success'
                ? 'OK'
                : normalizedStatus === 'alarm'
                    ? 'ALRM'
                    : normalizedStatus === 'fail'
                        ? 'DENY'
                        : 'INFO')
            : (normalizedStatus === 'success'
                ? '\u2705 Granted'
                : normalizedStatus === 'alarm'
                    ? '\uD83D\uDEA8 Alarm'
                    : normalizedStatus === 'fail'
                        ? '\u274C Denied'
                        : '\u2139\uFE0F Info');

        const displayTime = formatLogTime(log, isCompactMobile);
        const userLabel = normalizeDisplayUser(log?.user, actorRole);
        const methodLabel = deriveMethodLabel(log);
        const methodDetail = String(log?.method || '').trim() || methodLabel;
        const source = deriveSourceLabel(log);

        return `
            <td class="col-time">${escapeHtml(displayTime)}</td>
            <td class="col-user" title="${escapeHtml(userLabel)}">
                <div class="log-user-cell">
                    <span class="log-user-name">${escapeHtml(userLabel)}</span>
                    <span class="badge actor-role-badge ${escapeHtml(actorRoleMeta.className)}">${escapeHtml(actorRoleMeta.label)}</span>
                </div>
            </td>
            <td class="col-method" title="${escapeHtml(methodDetail)}">
                <div class="method-cell">
                    <span class="badge source-badge ${escapeHtml(source.className)}">${escapeHtml(source.label)}</span>
                    <span class="badge method-badge">${escapeHtml(methodLabel)}</span>
                </div>
            </td>
            <td class="col-status"><span class="badge status-badge ${statusClass}">${statusLabel}</span></td>
        `;
    }

    function getLogsPageSize() {
        const width = window.innerWidth || document.documentElement.clientWidth || 1024;
        if (width <= 640) return CONFIG.LOGS_PAGE_SIZE_MOBILE;
        if (width < 1024) return CONFIG.LOGS_PAGE_SIZE_TABLET;
        return CONFIG.LOGS_PAGE_SIZE_DESKTOP;
    }

    function updateLogsFilterSummary(filteredCount, totalCount, baseCount = filteredCount) {
        if (!DOM.logsFilterSummary) {
            return;
        }

        const query = String(state.logsSearchQuery || '').trim();
        const selectedStatus = String(state.logsStatusFilter || 'all').trim().toLowerCase();
        const fromDate = String(state.logsDateFrom || '').trim();
        const toDate = String(state.logsDateTo || '').trim();
        const hasQuery = query.length > 0;
        const hasStatus = selectedStatus !== 'all';
        const hasDate = fromDate.length > 0 || toDate.length > 0;

        if (!hasQuery && !hasStatus && !hasDate) {
            DOM.logsFilterSummary.textContent = `Showing all ${totalCount} log${totalCount === 1 ? '' : 's'}`;
            return;
        }

        const filters = [];
        if (hasQuery) {
            filters.push(`search "${query}"`);
        }
        if (hasStatus) {
            filters.push(`status ${selectedStatus.toUpperCase()}`);
        }

        if (hasDate) {
            const fromLabel = fromDate || 'start';
            const toLabel = toDate || 'today';
            filters.push(`date ${fromLabel} → ${toLabel}`);
        }

        if (hasStatus) {
            DOM.logsFilterSummary.textContent = `Showing ${filteredCount} of ${baseCount} status-matched logs (${filters.join(' • ')}) • ${totalCount} total`;
            return;
        }

        DOM.logsFilterSummary.textContent = `Showing ${filteredCount} of ${totalCount} logs (${filters.join(' • ')})`;
    }

    function updateLogsPaginationUI(totalLogs, totalUnfilteredLogs = totalLogs, totalBaseLogs = totalLogs) {
        const totalPages = Math.max(1, Math.ceil(totalLogs / state.logsPageSize));
        state.logsPage = Math.min(Math.max(1, state.logsPage), totalPages);

        if (DOM.logsPageInfo) {
            if (totalLogs === totalUnfilteredLogs) {
                DOM.logsPageInfo.textContent = `Page ${state.logsPage} of ${totalPages} (${totalLogs} logs)`;
            } else if (totalLogs === totalBaseLogs) {
                DOM.logsPageInfo.textContent = `Page ${state.logsPage} of ${totalPages} (${totalLogs} filtered of ${totalUnfilteredLogs} logs)`;
            } else {
                DOM.logsPageInfo.textContent = `Page ${state.logsPage} of ${totalPages} (${totalLogs} status-matched of ${totalBaseLogs} filtered, ${totalUnfilteredLogs} total)`;
            }
        }

        if (DOM.logsPrevPage) {
            const showPrev = totalLogs > 0 && state.logsPage > 1;
            DOM.logsPrevPage.hidden = !showPrev;
            DOM.logsPrevPage.disabled = !showPrev;
        }

        if (DOM.logsNextPage) {
            const showNext = totalLogs > 0 && state.logsPage < totalPages;
            DOM.logsNextPage.hidden = !showNext;
            DOM.logsNextPage.disabled = !showNext;
        }

        if (DOM.logsPagination) {
            DOM.logsPagination.hidden = totalLogs === 0;
            if (totalLogs === 0 || totalPages <= 1) {
                DOM.logsPagination.dataset.page = 'single';
            } else if (state.logsPage <= 1) {
                DOM.logsPagination.dataset.page = 'first';
            } else if (state.logsPage >= totalPages) {
                DOM.logsPagination.dataset.page = 'last';
            } else {
                DOM.logsPagination.dataset.page = 'paged';
            }
        }

        updateLogsFilterSummary(totalLogs, totalUnfilteredLogs, totalBaseLogs);
    }

    function renderCurrentLogsPage() {
        const allLogs = Array.isArray(state.allLogs) ? state.allLogs : [];
        const baseLogs = getBaseFilteredLogs();
        const logs = getFilteredLogs();
        const hasActiveFilters = String(state.logsSearchQuery || '').trim().length > 0
            || String(state.logsStatusFilter || 'all').trim().toLowerCase() !== 'all'
            || String(state.logsDateFrom || '').trim().length > 0
            || String(state.logsDateTo || '').trim().length > 0;

        updateStatusFilterOptions(baseLogs);

        if (allLogs.length === 0) {
            if (state.renderedLogsPageFingerprint !== 'empty') {
                DOM.logsTableBody.innerHTML =
                    '<tr><td colspan="4">' +
                    '<div class="logs-empty">' +
                    '<div class="logs-empty-icon">\uD83D\uDCCB</div>' +
                    '<div class="logs-empty-text">No activity recorded yet</div>' +
                    '</div></td></tr>';
                state.renderedLogsPageFingerprint = 'empty';
            }

            updateLogsPaginationUI(0, 0);
            return;
        }

        if (logs.length === 0) {
            const emptyFingerprint = hasActiveFilters ? 'empty-filtered' : 'empty';
            if (state.renderedLogsPageFingerprint !== emptyFingerprint) {
                DOM.logsTableBody.innerHTML =
                    '<tr><td colspan="4">' +
                    '<div class="logs-empty">' +
                    '<div class="logs-empty-icon">\uD83D\uDCCB</div>' +
                    `<div class="logs-empty-text">${hasActiveFilters ? 'No matching logs for the current filters' : 'No activity recorded yet'}</div>` +
                    '</div></td></tr>';
                state.renderedLogsPageFingerprint = emptyFingerprint;
            }

            updateLogsPaginationUI(0, allLogs.length, baseLogs.length);
            return;
        }

        const totalPages = Math.max(1, Math.ceil(logs.length / state.logsPageSize));
        state.logsPage = Math.min(Math.max(1, state.logsPage), totalPages);

        const startIndex = (state.logsPage - 1) * state.logsPageSize;
        const endIndex = startIndex + state.logsPageSize;
        const pageLogs = logs.slice(startIndex, endIndex);

        const isCompactMobile = window.matchMedia('(max-width: 640px)').matches;
        const pageSignatures = pageLogs.map(createLogSignature);
        const nextPageFingerprint = [
            isCompactMobile ? 'compact' : 'desktop',
            state.logsPage,
            state.logsPageSize,
            String(state.logsSearchQuery || '').trim().toLowerCase(),
            String(state.logsStatusFilter || 'all').trim().toLowerCase(),
            String(state.logsDateFrom || '').trim(),
            String(state.logsDateTo || '').trim(),
            pageSignatures.join('~')
        ].join('|');

        if (state.renderedLogsPageFingerprint !== nextPageFingerprint) {
            const existingRows = Array.from(DOM.logsTableBody.querySelectorAll('tr'));

            pageLogs.forEach((log, index) => {
                const signature = pageSignatures[index];
                let row = existingRows[index];

                if (!row) {
                    row = document.createElement('tr');
                    DOM.logsTableBody.appendChild(row);
                }

                const rowMode = isCompactMobile ? 'compact' : 'desktop';
                if (row.dataset.sig !== signature || row.dataset.mode !== rowMode) {
                    row.dataset.sig = signature;
                    row.dataset.mode = rowMode;
                    row.innerHTML = buildLogRowHTML(log, isCompactMobile);
                }
            });

            for (let i = pageLogs.length; i < existingRows.length; i += 1) {
                existingRows[i].remove();
            }

            state.renderedLogsPageFingerprint = nextPageFingerprint;
        }

        updateLogsPaginationUI(logs.length, allLogs.length, baseLogs.length);
    }

    async function loadLogs() {
        if (state.logsRequestInFlight) {
            return;
        }

        state.logsRequestInFlight = true;
        try {
            const logsTimeoutMs = Math.max(1200, Number(CONFIG.LOGS_TIMEOUT_MS || 4200));
            const defaultRetryCount = Math.max(0, Number(CONFIG.API_RETRY_COUNT || 0));
            const retryBaseDelayMs = Math.max(100, Number(CONFIG.API_RETRY_BASE_DELAY_MS || 170));
            const retryMaxDelayMs = Math.max(retryBaseDelayMs, Number(CONFIG.API_RETRY_MAX_DELAY_MS || 700));

            const data = await apiFetch(CONFIG.API.LOGS, {
                timeoutMs: logsTimeoutMs,
                retries: defaultRetryCount,
                retryBaseDelayMs,
                retryMaxDelayMs
            });
            const logsRaw = Array.isArray(data.logs)
                ? data.logs
                : (Array.isArray(data) ? data : []);
            const logs = sortLogsLatestFirst(logsRaw);

            const logsHash = createLogsCollectionSignature(logs);
            if (logsHash !== state.lastLogsHash) {
                state.lastLogsHash = logsHash;
                state.allLogs = logs;
                state.renderedLogsPageFingerprint = '';
            }

            renderCurrentLogsPage();
        } catch {
            DOM.logsTableBody.innerHTML =
                '<tr><td colspan="4"><div class="logs-empty"><div class="logs-empty-text">Unable to load logs</div></div></td></tr>';
            state.renderedLogsPageFingerprint = 'error';
            if (DOM.logsPagination) {
                DOM.logsPagination.hidden = true;
            }
            if (DOM.logsFilterSummary) {
                DOM.logsFilterSummary.textContent = 'Unable to load logs';
            }
        } finally {
            state.logsRequestInFlight = false;
        }
    }

    async function handleClearLogs() {
        if (state.logsClearInFlight) {
            return;
        }

        feedback.showModal(
            'Clear Activity Logs',
            'This will erase all activity logs permanently. Continue?',
            'danger',
            async () => {
                if (state.logsClearInFlight) {
                    return;
                }

                try {
                    state.logsClearInFlight = true;
                    if (DOM.btnClearLogs) {
                        DOM.btnClearLogs.disabled = true;
                        DOM.btnClearLogs.textContent = 'Clearing...';
                    }

                    const data = await apiFetch(CONFIG.API.LOGS_CLEAR, { method: 'DELETE' });
                    if (data.success) {
                        state.allLogs = [];
                        state.lastLogsHash = '';
                        state.logsPage = 1;
                        renderCurrentLogsPage();
                        feedback.showToast('All logs cleared', 'success');
                    } else {
                        feedback.showToast(data.message || 'Failed to clear logs', 'error');
                    }
                } catch {
                    feedback.showToast('Connection error — could not clear logs', 'error');
                } finally {
                    state.logsClearInFlight = false;
                    if (DOM.btnClearLogs) {
                        DOM.btnClearLogs.disabled = false;
                        DOM.btnClearLogs.textContent = 'Clear Logs';
                    }
                }
            }
        );
    }

    function bindEvents() {
        let searchDebounceTimer = null;

        startLogsClockTicker();

        if (DOM.btnClearLogs) {
            DOM.btnClearLogs.addEventListener('click', handleClearLogs);
        }

        if (DOM.logsSearchInput) {
            DOM.logsSearchInput.value = String(state.logsSearchQuery || '');
            DOM.logsSearchInput.addEventListener('input', (event) => {
                const nextValue = String(event?.target?.value || '');

                if (searchDebounceTimer) {
                    clearTimeout(searchDebounceTimer);
                    searchDebounceTimer = null;
                }

                searchDebounceTimer = setTimeout(() => {
                    if (nextValue === state.logsSearchQuery) {
                        return;
                    }

                    state.logsSearchQuery = nextValue;
                    state.logsPage = 1;
                    state.renderedLogsPageFingerprint = '';
                    renderCurrentLogsPage();
                }, 100);
            });
        }

        if (DOM.logsStatusFilter) {
            DOM.logsStatusFilter.value = String(state.logsStatusFilter || 'all').toLowerCase();
            DOM.logsStatusFilter.addEventListener('change', (event) => {
                const rawStatus = String(event?.target?.value || 'all').trim().toLowerCase();
                const nextStatus = LOG_STATUS_OPTIONS.includes(rawStatus)
                    ? rawStatus
                    : 'all';
                if (nextStatus === state.logsStatusFilter) {
                    return;
                }

                state.logsStatusFilter = nextStatus;
                state.logsPage = 1;
                state.renderedLogsPageFingerprint = '';
                renderCurrentLogsPage();
            });
        }

        if (DOM.logsDateFrom) {
            DOM.logsDateFrom.value = String(state.logsDateFrom || '');
            DOM.logsDateFrom.addEventListener('change', (event) => {
                const nextValue = String(event?.target?.value || '').trim();
                if (nextValue === state.logsDateFrom) {
                    return;
                }

                state.logsDateFrom = nextValue;

                if (state.logsDateFrom && state.logsDateTo && state.logsDateFrom > state.logsDateTo) {
                    state.logsDateTo = state.logsDateFrom;
                    if (DOM.logsDateTo) {
                        DOM.logsDateTo.value = state.logsDateTo;
                    }
                }

                state.logsPage = 1;
                state.renderedLogsPageFingerprint = '';
                renderCurrentLogsPage();
            });
        }

        if (DOM.logsDateTo) {
            DOM.logsDateTo.value = String(state.logsDateTo || '');
            DOM.logsDateTo.addEventListener('change', (event) => {
                const nextValue = String(event?.target?.value || '').trim();
                if (nextValue === state.logsDateTo) {
                    return;
                }

                state.logsDateTo = nextValue;

                if (state.logsDateFrom && state.logsDateTo && state.logsDateTo < state.logsDateFrom) {
                    state.logsDateFrom = state.logsDateTo;
                    if (DOM.logsDateFrom) {
                        DOM.logsDateFrom.value = state.logsDateFrom;
                    }
                }

                state.logsPage = 1;
                state.renderedLogsPageFingerprint = '';
                renderCurrentLogsPage();
            });
        }

        if (DOM.btnLogsClearFilters) {
            DOM.btnLogsClearFilters.addEventListener('click', () => {
                const hadFilters = String(state.logsSearchQuery || '').trim().length > 0
                    || String(state.logsStatusFilter || 'all').trim().toLowerCase() !== 'all'
                    || String(state.logsDateFrom || '').trim().length > 0
                    || String(state.logsDateTo || '').trim().length > 0;

                state.logsSearchQuery = '';
                state.logsStatusFilter = 'all';
                state.logsDateFrom = '';
                state.logsDateTo = '';
                state.logsPage = 1;
                state.renderedLogsPageFingerprint = '';

                if (DOM.logsSearchInput) {
                    DOM.logsSearchInput.value = '';
                }

                if (DOM.logsStatusFilter) {
                    DOM.logsStatusFilter.value = 'all';
                }

                if (DOM.logsDateFrom) {
                    DOM.logsDateFrom.value = '';
                }

                if (DOM.logsDateTo) {
                    DOM.logsDateTo.value = '';
                }

                renderCurrentLogsPage();

                if (hadFilters) {
                    feedback.showToast('Log filters cleared', 'info');
                }
            });
        }

        if (DOM.logsPrevPage) {
            DOM.logsPrevPage.addEventListener('click', () => {
                if (state.logsPage > 1) {
                    state.logsPage -= 1;
                    renderCurrentLogsPage();
                }
            });
        }

        if (DOM.logsNextPage) {
            DOM.logsNextPage.addEventListener('click', () => {
                const totalPages = Math.max(1, Math.ceil(getFilteredLogs().length / state.logsPageSize));
                if (state.logsPage < totalPages) {
                    state.logsPage += 1;
                    renderCurrentLogsPage();
                }
            });
        }

        window.addEventListener('resize', () => {
            const nextPageSize = getLogsPageSize();
            if (nextPageSize !== state.logsPageSize) {
                state.logsPageSize = nextPageSize;
                state.logsPage = 1;
                state.renderedLogsPageFingerprint = '';
                renderCurrentLogsPage();
            }
        });
    }

    function initializePageSize() {
        state.logsPageSize = getLogsPageSize();
        state.logsSearchQuery = String(state.logsSearchQuery || '');
        const nextStatus = String(state.logsStatusFilter || 'all').toLowerCase();
        state.logsStatusFilter = LOG_STATUS_OPTIONS.includes(nextStatus)
            ? nextStatus
            : 'all';
        state.logsDateFrom = String(state.logsDateFrom || '').trim();
        state.logsDateTo = String(state.logsDateTo || '').trim();
    }

    return {
        bindEvents,
        loadLogs,
        initializePageSize
    };
}
