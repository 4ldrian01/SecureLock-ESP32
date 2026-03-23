import { escapeHtml, formatLogTime } from '../core/helpers.js';

export function createLogsFeature({ CONFIG, state, DOM, apiFetch, feedback }) {
    function getLogsPageSize() {
        const width = window.innerWidth || document.documentElement.clientWidth || 1024;
        if (width <= 640) return CONFIG.LOGS_PAGE_SIZE_MOBILE;
        if (width < 1024) return CONFIG.LOGS_PAGE_SIZE_TABLET;
        return CONFIG.LOGS_PAGE_SIZE_DESKTOP;
    }

    function updateLogsPaginationUI(totalLogs) {
        const totalPages = Math.max(1, Math.ceil(totalLogs / state.logsPageSize));
        state.logsPage = Math.min(Math.max(1, state.logsPage), totalPages);

        if (DOM.logsPageInfo) {
            DOM.logsPageInfo.textContent = `Page ${state.logsPage} of ${totalPages} (${totalLogs} logs)`;
        }

        if (DOM.logsPrevPage) {
            DOM.logsPrevPage.disabled = state.logsPage <= 1 || totalLogs === 0;
        }

        if (DOM.logsNextPage) {
            DOM.logsNextPage.disabled = state.logsPage >= totalPages || totalLogs === 0;
        }

        if (DOM.logsPagination) {
            DOM.logsPagination.hidden = totalLogs === 0;
        }
    }

    function renderCurrentLogsPage() {
        const logs = Array.isArray(state.allLogs) ? state.allLogs : [];

        if (logs.length === 0) {
            DOM.logsTableBody.innerHTML =
                '<tr><td colspan="4">' +
                '<div class="logs-empty">' +
                '<div class="logs-empty-icon">\uD83D\uDCCB</div>' +
                '<div class="logs-empty-text">No activity recorded yet</div>' +
                '</div></td></tr>';
            updateLogsPaginationUI(0);
            return;
        }

        const totalPages = Math.max(1, Math.ceil(logs.length / state.logsPageSize));
        state.logsPage = Math.min(Math.max(1, state.logsPage), totalPages);

        const startIndex = (state.logsPage - 1) * state.logsPageSize;
        const endIndex = startIndex + state.logsPageSize;
        const pageLogs = logs.slice(startIndex, endIndex);

        const isCompactMobile = window.matchMedia('(max-width: 640px)').matches;

        DOM.logsTableBody.innerHTML = pageLogs.map(log => {
            const statusClass = log.status === 'success' ? 'status-success'
                : log.status === 'alarm' ? 'status-alarm'
                : 'status-error';
            const statusLabel = isCompactMobile
                ? (log.status === 'success' ? 'OK' : log.status === 'alarm' ? 'ALRM' : 'DENY')
                : (log.status === 'success' ? '\u2705 Granted'
                    : log.status === 'alarm' ? '\uD83D\uDEA8 Alarm'
                        : '\u274C Denied');

            const displayTime = formatLogTime(log, isCompactMobile);

            return `<tr>
                <td class="col-time">${escapeHtml(displayTime)}</td>
                <td class="col-user">${escapeHtml(log.user || 'Unknown')}</td>
                <td class="col-method"><span class="method-badge">${escapeHtml(log.method || '--')}</span></td>
                <td class="col-status"><span class="status-badge ${statusClass}">${statusLabel}</span></td>
            </tr>`;
        }).join('');

        updateLogsPaginationUI(logs.length);
    }

    async function loadLogs() {
        if (state.logsRequestInFlight) {
            return;
        }

        state.logsRequestInFlight = true;
        try {
            const data = await apiFetch(CONFIG.API.LOGS);
            const logs = Array.isArray(data.logs)
                ? data.logs
                : (Array.isArray(data) ? data : []);

            const logsHash = JSON.stringify(logs);
            if (logsHash !== state.lastLogsHash) {
                state.lastLogsHash = logsHash;
                state.allLogs = logs;
                state.logsPage = 1;
            }

            renderCurrentLogsPage();
        } catch {
            DOM.logsTableBody.innerHTML =
                '<tr><td colspan="4"><div class="logs-empty"><div class="logs-empty-text">Unable to load logs</div></div></td></tr>';
            if (DOM.logsPagination) {
                DOM.logsPagination.hidden = true;
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
                        state.lastLogsHash = JSON.stringify([]);
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
        if (DOM.btnClearLogs) {
            DOM.btnClearLogs.addEventListener('click', handleClearLogs);
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
                const totalPages = Math.max(1, Math.ceil((state.allLogs || []).length / state.logsPageSize));
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
                renderCurrentLogsPage();
            }
        });
    }

    function initializePageSize() {
        state.logsPageSize = getLogsPageSize();
    }

    return {
        bindEvents,
        loadLogs,
        initializePageSize
    };
}
