/**
 * ============================================================
 * SecureLock Dashboard - Frontend Controller
 * ============================================================
 * 
 * Connects to ESP32 backend via RESTful API endpoints.
 * All data is fetched from the hardware — no mock/static data.
 * 
 * API Endpoints:
 *   GET  /api/status      → System status (lock, alarm, door)
 *   POST /api/unlock      → Emergency override unlock
 *   POST /api/guest-code  → Generate temporary guest PIN
 *   GET  /api/users       → List registered users
 *   POST /api/users       → Add new user
 *   PUT  /api/users       → Edit existing user
 *   DELETE /api/users?uid=X → Delete user
 *   GET  /api/logs        → Activity log entries
 *   GET  /api/rfid/scan   → Poll for last scanned RFID UID
 * 
 * Version: 2.0.0
 * ============================================================
 */

(function () {
    'use strict';

    // ── Configuration ──────────────────────────────────────
    const CONFIG = {
        POLL_INTERVAL: 3000,        // Status poll every 3 seconds
        LOGS_REFRESH_INTERVAL: 15000,
        USERS_REFRESH_INTERVAL: 20000,
        TOAST_DURATION: 3500,       // Toast notification display time
        GUEST_CODE_EXPIRY: 300,     // 5 minutes in seconds
        RFID_POLL_INTERVAL: 1000,   // RFID scan poll every 1 second
        API: {
            STATUS:     '/api/status',
            UNLOCK:     '/api/unlock',
            GUEST_CODE: '/api/guest-code',
            USERS:      '/api/users',
            LOGS:       '/api/logs',
            RFID_SCAN:  '/api/rfid/scan'
        }
    };

    // ── State ──────────────────────────────────────────────
    let state = {
        connected: false,
        locked: true,
        alarm: false,
        pollTimer: null,
        logsTimer: null,
        usersTimer: null,
        lastUsersHash: '',
        lastLogsHash: '',
        guestCode: null,
        guestExpiry: 0,
        guestTimer: null,
        rfidPollTimer: null,
        editingUserId: null
    };

    // ── DOM References ─────────────────────────────────────
    const DOM = {
        // Status
        statusBadge:      document.getElementById('statusBadge'),
        statusText:       document.getElementById('statusText'),

        // Lock
        lockVisual:       document.getElementById('lockVisual'),
        lockStatusLabel:  document.getElementById('lockStatusLabel'),
        lockStatusSub:    document.getElementById('lockStatusSub'),
        btnEmergency:     document.getElementById('btnEmergency'),

        // Guest Access
        pinCode:          document.getElementById('pinCode'),
        pinTimer:         document.getElementById('pinTimer'),
        btnGenerate:      document.getElementById('btnGenerate'),

        // Activity Logs
        logsTableBody:    document.getElementById('logsTableBody'),

        // Users
        usersGrid:        document.getElementById('usersGrid'),
        btnAddUser:       document.getElementById('btnAddUser'),

        // Confirmation Modal
        modalOverlay:     document.getElementById('modalOverlay'),
        modalTitle:       document.getElementById('modalTitle'),
        modalMessage:     document.getElementById('modalMessage'),
        modalIcon:        document.getElementById('modalIcon'),
        btnModalCancel:   document.getElementById('btnModalCancel'),
        btnModalConfirm:  document.getElementById('btnModalConfirm'),

        // Add User Dialog
        addUserModal:     document.getElementById('addUserModal'),
        addUserForm:      document.getElementById('addUserForm'),
        userName:         document.getElementById('userName'),
        userPin:          document.getElementById('userPin'),
        userRfid:         document.getElementById('userRfid'),
        btnScanCard:      document.getElementById('btnScanCard'),
        btnCloseDialog:   document.getElementById('btnCloseDialog'),
        btnCancelAdd:     document.getElementById('btnCancelAdd'),

        // Edit User Dialog
        editUserModal:    document.getElementById('editUserModal'),
        editUserForm:     document.getElementById('editUserForm'),
        editUserName:     document.getElementById('editUserName'),
        editUserPin:      document.getElementById('editUserPin'),
        editUserRfid:     document.getElementById('editUserRfid'),
        btnReplaceCard:   document.getElementById('btnReplaceCard'),
        btnCloseEditDialog: document.getElementById('btnCloseEditDialog'),
        btnCancelEdit:    document.getElementById('btnCancelEdit'),

        // Toast
        toast:            document.getElementById('toast'),
        toastMessage:     document.getElementById('toastMessage')
    };

    // ════════════════════════════════════════════════════════
    //  API COMMUNICATION
    // ════════════════════════════════════════════════════════

    /**
     * Generic fetch wrapper with error handling
     */
    async function apiFetch(url, options = {}) {
        try {
            const response = await fetch(url, {
                headers: { 'Content-Type': 'application/json' },
                ...options
            });
            if (!response.ok) {
                throw new Error(`HTTP ${response.status}`);
            }
            return await response.json();
        } catch (error) {
            console.error(`[API] ${options.method || 'GET'} ${url} failed:`, error.message);
            throw error;
        }
    }

    // ════════════════════════════════════════════════════════
    //  STATUS POLLING
    // ════════════════════════════════════════════════════════

    /**
     * Poll /api/status and update the entire UI
     */
    async function pollStatus() {
        try {
            const data = await apiFetch(CONFIG.API.STATUS);
            setConnectionState(true);

            if (state.locked !== data.locked) {
                updateLockUI(data.locked);
            }

            if (state.alarm !== data.alarm) {
                updateAlarmState(data.alarm);
            }

            state.locked = data.locked;
            state.alarm = data.alarm;
        } catch {
            setConnectionState(false);
        }
    }

    /**
     * Update connection status badge
     */
    function setConnectionState(online) {
        state.connected = online;
        DOM.statusBadge.dataset.status = online ? 'online' : 'offline';
        DOM.statusText.textContent = online ? 'Online' : 'Offline';
    }

    /**
     * Update lock visual, label, and sub-text
     */
    function updateLockUI(locked) {
        const lockState = locked ? 'locked' : 'unlocked';
        DOM.lockVisual.dataset.lockState = lockState;
        DOM.lockStatusLabel.textContent = locked ? 'LOCKED' : 'UNLOCKED';
        DOM.lockStatusSub.textContent = locked
            ? 'System Armed \u2022 Secure'
            : 'Door Open \u2022 Auto-lock in 5s';
    }

    /**
     * Update alarm indication
     */
    function updateAlarmState(alarming) {
        if (alarming) {
            DOM.lockStatusSub.textContent = '\u26A0 ALARM ACTIVE';
            DOM.lockStatusSub.style.color = 'var(--danger)';
        } else {
            DOM.lockStatusSub.style.color = '';
        }
    }

    // ════════════════════════════════════════════════════════
    //  EMERGENCY OVERRIDE
    // ════════════════════════════════════════════════════════

    function handleEmergencyUnlock() {
        showModal(
            'Emergency Override',
            'This will immediately unlock the door. Are you sure?',
            'danger',
            async () => {
                try {
                    DOM.btnEmergency.disabled = true;
                    DOM.btnEmergency.textContent = 'Unlocking...';
                    const data = await apiFetch(CONFIG.API.UNLOCK, { method: 'POST' });
                    if (data.success) {
                        updateLockUI(false);
                        showToast('Door unlocked via emergency override', 'success');
                    } else {
                        showToast('Unlock failed: ' + (data.message || 'Unknown error'), 'error');
                    }
                } catch {
                    showToast('Connection error — could not unlock', 'error');
                } finally {
                    DOM.btnEmergency.disabled = false;
                    DOM.btnEmergency.textContent = 'Emergency Override';
                }
            }
        );
    }

    // ════════════════════════════════════════════════════════
    //  GUEST CODE
    // ════════════════════════════════════════════════════════

    async function handleGenerateGuestCode() {
        try {
            DOM.btnGenerate.disabled = true;
            DOM.btnGenerate.textContent = 'Generating...';

            const data = await apiFetch(CONFIG.API.GUEST_CODE, { method: 'POST' });

            if (data.success && data.code) {
                displayGuestCode(data.code, data.expiresIn || CONFIG.GUEST_CODE_EXPIRY);
                showToast(`Guest code generated\n${data.code}`, 'success');
            } else {
                showToast('Failed to generate guest code', 'error');
            }
        } catch {
            showToast('Connection error — could not generate code', 'error');
        } finally {
            DOM.btnGenerate.disabled = false;
            DOM.btnGenerate.innerHTML =
                '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M17.65 6.35C16.2 4.9 14.21 4 12 4c-4.42 0-7.99 3.58-7.99 8s3.57 8 7.99 8c3.73 0 6.84-2.55 7.73-6h-2.08c-.82 2.33-3.04 4-5.65 4-3.31 0-6-2.69-6-6s2.69-6 6-6c1.66 0 3.14.69 4.22 1.78L13 11h7V4l-2.35 2.35z"/></svg> Generate New Code';
        }
    }

    /**
     * Display the 4-digit guest code with countdown timer
     */
    function displayGuestCode(code, expiresIn) {
        const digits = code.toString().padStart(4, '0').split('');
        const pinDigits = DOM.pinCode.querySelectorAll('.pin-digit');
        pinDigits.forEach((el, i) => {
            el.textContent = digits[i] || '-';
        });

        state.guestCode = code;
        state.guestExpiry = expiresIn;

        // Clear previous timer
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

        // Show initial timer
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

    // ════════════════════════════════════════════════════════
    //  ACTIVITY LOGS
    // ════════════════════════════════════════════════════════

    async function loadLogs() {
        try {
            const data = await apiFetch(CONFIG.API.LOGS);
            const logs = data.logs || data || [];

            const logsHash = JSON.stringify(logs);
            if (logsHash === state.lastLogsHash) {
                return;
            }

            state.lastLogsHash = logsHash;
            renderLogs(logs);
        } catch {
            DOM.logsTableBody.innerHTML =
                '<tr><td colspan="4" class="logs-empty">Unable to load logs</td></tr>';
        }
    }

    function renderLogs(logs) {
        if (!Array.isArray(logs) || logs.length === 0) {
            DOM.logsTableBody.innerHTML =
                '<tr><td colspan="4" class="logs-empty">' +
                '<div class="logs-empty-icon">\uD83D\uDCCB</div>' +
                'No activity recorded yet</td></tr>';
            return;
        }

        const isCompactMobile = window.matchMedia('(max-width: 640px)').matches;

        DOM.logsTableBody.innerHTML = logs.map(log => {
            const statusClass = log.status === 'success' ? 'status-success'
                              : log.status === 'alarm'   ? 'status-alarm'
                              : 'status-error';
            const statusLabel = isCompactMobile
                ? (log.status === 'success' ? 'OK' : log.status === 'alarm' ? 'ALRM' : 'DENY')
                : (log.status === 'success' ? '\u2705 Granted'
                   : log.status === 'alarm'   ? '\uD83D\uDEA8 Alarm'
                   : '\u274C Denied');

            const displayTime = formatLogTime(log.time, isCompactMobile);

            return `<tr>
                <td class="col-time">${escapeHtml(displayTime)}</td>
                <td class="col-user">${escapeHtml(log.user || 'Unknown')}</td>
                <td class="col-method"><span class="method-badge">${escapeHtml(log.method || '--')}</span></td>
                <td><span class="status-badge ${statusClass}">${statusLabel}</span></td>
            </tr>`;
        }).join('');
    }

    function formatLogTime(timeValue, compact) {
        if (!timeValue) return '--';
        if (!compact) return String(timeValue);

        const matched = String(timeValue).match(/(\d{2}:\d{2})(?::\d{2})?/);
        if (matched && matched[1]) {
            return matched[1];
        }

        return String(timeValue).length > 8
            ? String(timeValue).slice(0, 8)
            : String(timeValue);
    }

    // ════════════════════════════════════════════════════════
    //  USER MANAGEMENT
    // ════════════════════════════════════════════════════════

    async function loadUsers() {
        try {
            const data = await apiFetch(CONFIG.API.USERS);
            const users = data.users || data || [];

            const usersHash = JSON.stringify(users);
            if (usersHash === state.lastUsersHash) {
                return;
            }

            state.lastUsersHash = usersHash;
            renderUsers(users);
        } catch {
            DOM.usersGrid.innerHTML =
                '<p style="color:var(--text-muted);text-align:center;padding:2rem;">Unable to load users</p>';
        }
    }

    function renderUsers(users) {
        if (!Array.isArray(users) || users.length === 0) {
            DOM.usersGrid.innerHTML =
                '<p class="users-empty">' +
                'No users registered. Tap "Add User" to get started.</p>';
            return;
        }

        DOM.usersGrid.innerHTML = users.map(user => {
            const initials = (user.name || '?').charAt(0).toUpperCase();
            const role = user.type || user.role || 'user';
            const badgeClass = role === 'admin' ? 'badge-admin'
                             : role === 'guest' ? 'badge-guest'
                             : 'badge-user';
            const isAdmin = role === 'admin';

            return `<div class="user-card" role="listitem" data-uid="${escapeHtml(user.uid || '')}">
                <div class="user-info">
                    <div class="user-name-row">
                        <span class="user-name">${escapeHtml(user.name || 'Unknown')}</span>
                        <span class="user-badge ${badgeClass}">${escapeHtml(role.toUpperCase())}</span>
                    </div>
                    <div class="user-meta">
                        <span class="user-uid">${escapeHtml(user.uid || '--')}</span>
                    </div>
                </div>
                <div class="user-actions">
                    <button class="btn-edit" title="Edit user" data-uid="${escapeHtml(user.uid || '')}"
                            onclick="window.__editUser('${escapeHtml(user.uid || '')}')">
                        <svg viewBox="0 0 24 24"><path d="M3 17.25V21h3.75L17.81 9.94l-3.75-3.75L3 17.25zM20.71 7.04c.39-.39.39-1.02 0-1.41l-2.34-2.34c-.39-.39-1.02-.39-1.41 0l-1.83 1.83 3.75 3.75 1.83-1.83z"/></svg>
                    </button>
                    <button class="btn-delete" title="Delete user" ${isAdmin ? 'disabled' : ''}
                            data-uid="${escapeHtml(user.uid || '')}"
                            onclick="window.__deleteUser('${escapeHtml(user.uid || '')}')">
                        <svg viewBox="0 0 24 24"><path d="M6 19c0 1.1.9 2 2 2h8c1.1 0 2-.9 2-2V7H6v12zM19 4h-3.5l-1-1h-5l-1 1H5v2h14V4z"/></svg>
                    </button>
                </div>
            </div>`;
        }).join('');
    }

    // ── Delete User ────────────────────────────────────────

    window.__deleteUser = function (uid) {
        showModal(
            'Delete User',
            `Are you sure you want to remove this user? This action cannot be undone.`,
            'danger',
            async () => {
                try {
                    const data = await apiFetch(`${CONFIG.API.USERS}?uid=${encodeURIComponent(uid)}`, {
                        method: 'DELETE'
                    });
                    if (data.success) {
                        showToast('User deleted successfully', 'success');
                        loadUsers();
                    } else {
                        showToast(data.message || 'Failed to delete user', 'error');
                    }
                } catch {
                    showToast('Connection error — could not delete user', 'error');
                }
            }
        );
    };

    // ── Edit User ──────────────────────────────────────────

    window.__editUser = function (uid) {
        state.editingUserId = uid;

        // Find user card and pre-fill form
        const card = document.querySelector(`.user-card[data-uid="${uid}"]`);
        if (card) {
            const name = card.querySelector('.user-name')?.textContent || '';
            const rfid = card.querySelector('.user-uid')?.textContent || '';
            DOM.editUserName.value = name;
            DOM.editUserPin.value = '';
            DOM.editUserRfid.value = rfid;
        }

        DOM.editUserModal.showModal();
    };

    // ── Add User Dialog ────────────────────────────────────

    function openAddUserDialog() {
        DOM.addUserForm.reset();
        DOM.userRfid.value = '';
        DOM.userRfid.classList.remove('scanned', 'scanning');
        DOM.addUserModal.showModal();
    }

    function closeAddUserDialog() {
        stopRfidPoll();
        DOM.addUserModal.close();
    }

    function closeEditUserDialog() {
        stopRfidPoll();
        DOM.editUserModal.close();
        state.editingUserId = null;
    }

    // ── RFID Scan Polling ──────────────────────────────────

    function startRfidPoll(targetInput) {
        stopRfidPoll();
        targetInput.value = 'Waiting for card tap...';
        targetInput.classList.add('scanning');
        targetInput.classList.remove('scanned');

        state.rfidPollTimer = setInterval(async () => {
            try {
                const data = await apiFetch(CONFIG.API.RFID_SCAN);
                if (data.uid && data.uid.length > 0) {
                    targetInput.value = data.uid;
                    targetInput.classList.remove('scanning');
                    targetInput.classList.add('scanned');
                    stopRfidPoll();
                    showToast('RFID card detected: ' + data.uid, 'success');
                }
            } catch {
                // Keep polling silently
            }
        }, CONFIG.RFID_POLL_INTERVAL);
    }

    function stopRfidPoll() {
        if (state.rfidPollTimer) {
            clearInterval(state.rfidPollTimer);
            state.rfidPollTimer = null;
        }
    }

    // ── Form Submission: Add User ──────────────────────────

    async function handleAddUser(e) {
        e.preventDefault();
        clearFormErrors();

        const name = DOM.userName.value.trim();
        const pin  = DOM.userPin.value.trim();
        const rfid = DOM.userRfid.value.trim();

        // Validation
        let valid = true;
        if (!name) {
            setFormError('userNameError', 'Name is required');
            valid = false;
        }
        if (!pin || !/^\d{4}$/.test(pin)) {
            setFormError('userPinError', 'PIN must be exactly 4 digits');
            valid = false;
        }
        if (!rfid || rfid === 'Waiting for card tap...') {
            setFormError('userRfidError', 'RFID tag is required — tap a card');
            valid = false;
        }
        if (!valid) return;

        try {
            const data = await apiFetch(CONFIG.API.USERS, {
                method: 'POST',
                body: JSON.stringify({ name, pin, uid: rfid, type: 'user' })
            });

            if (data.success) {
                showToast('User added successfully', 'success');
                closeAddUserDialog();
                loadUsers();
            } else {
                showToast(data.message || 'Failed to add user', 'error');
            }
        } catch {
            showToast('Connection error — could not add user', 'error');
        }
    }

    // ── Form Submission: Edit User ─────────────────────────

    async function handleEditUser(e) {
        e.preventDefault();
        clearFormErrors();

        const name = DOM.editUserName.value.trim();
        const pin  = DOM.editUserPin.value.trim();
        const rfid = DOM.editUserRfid.value.trim();

        // Validation
        let valid = true;
        if (!name) {
            setFormError('editUserNameError', 'Name is required');
            valid = false;
        }
        if (pin && !/^\d{4}$/.test(pin)) {
            setFormError('editUserPinError', 'PIN must be exactly 4 digits');
            valid = false;
        }
        if (!rfid) {
            setFormError('editUserRfidError', 'RFID tag is required');
            valid = false;
        }
        if (!valid) return;

        try {
            const body = { uid: state.editingUserId, name, rfid };
            if (pin) body.pin = pin;

            const data = await apiFetch(CONFIG.API.USERS, {
                method: 'PUT',
                body: JSON.stringify(body)
            });

            if (data.success) {
                showToast('User updated successfully', 'success');
                closeEditUserDialog();
                loadUsers();
            } else {
                showToast(data.message || 'Failed to update user', 'error');
            }
        } catch {
            showToast('Connection error — could not update user', 'error');
        }
    }

    // ════════════════════════════════════════════════════════
    //  MODAL & TOAST
    // ════════════════════════════════════════════════════════

    let modalConfirmCallback = null;

    function showModal(title, message, type, onConfirm) {
        DOM.modalTitle.textContent = title;
        DOM.modalMessage.textContent = message;
        DOM.modalIcon.dataset.type = type || 'warning';
        DOM.modalOverlay.dataset.visible = 'true';
        modalConfirmCallback = onConfirm;
    }

    function hideModal() {
        DOM.modalOverlay.dataset.visible = 'false';
        modalConfirmCallback = null;
    }

    function showToast(message, type) {
        DOM.toastMessage.textContent = message;
        DOM.toast.dataset.type = type || 'info';
        DOM.toast.dataset.visible = 'true';

        setTimeout(() => {
            DOM.toast.dataset.visible = 'false';
        }, CONFIG.TOAST_DURATION);
    }

    function startPollingLoops() {
        if (!state.pollTimer) {
            state.pollTimer = setInterval(() => {
                if (!document.hidden) pollStatus();
            }, CONFIG.POLL_INTERVAL);
        }

        if (!state.logsTimer) {
            state.logsTimer = setInterval(() => {
                if (!document.hidden) loadLogs();
            }, CONFIG.LOGS_REFRESH_INTERVAL);
        }

        if (!state.usersTimer) {
            state.usersTimer = setInterval(() => {
                if (!document.hidden) loadUsers();
            }, CONFIG.USERS_REFRESH_INTERVAL);
        }
    }

    // ════════════════════════════════════════════════════════
    //  FORM HELPERS
    // ════════════════════════════════════════════════════════

    function setFormError(elementId, message) {
        const el = document.getElementById(elementId);
        if (el) el.textContent = message;
    }

    function clearFormErrors() {
        document.querySelectorAll('.form-error').forEach(el => {
            el.textContent = '';
        });
    }

    function escapeHtml(str) {
        const div = document.createElement('div');
        div.appendChild(document.createTextNode(str));
        return div.innerHTML;
    }

    // ════════════════════════════════════════════════════════
    //  EVENT BINDINGS
    // ════════════════════════════════════════════════════════

    function bindEvents() {
        // Emergency Override
        DOM.btnEmergency.addEventListener('click', handleEmergencyUnlock);

        // Guest Code
        DOM.btnGenerate.addEventListener('click', handleGenerateGuestCode);

        // Add User
        DOM.btnAddUser.addEventListener('click', openAddUserDialog);
        DOM.btnCloseDialog.addEventListener('click', closeAddUserDialog);
        DOM.btnCancelAdd.addEventListener('click', closeAddUserDialog);
        DOM.addUserForm.addEventListener('submit', handleAddUser);
        DOM.btnScanCard.addEventListener('click', () => startRfidPoll(DOM.userRfid));

        // Edit User
        DOM.btnCloseEditDialog.addEventListener('click', closeEditUserDialog);
        DOM.btnCancelEdit.addEventListener('click', closeEditUserDialog);
        DOM.editUserForm.addEventListener('submit', handleEditUser);
        DOM.btnReplaceCard.addEventListener('click', () => startRfidPoll(DOM.editUserRfid));

        // Confirmation Modal
        DOM.btnModalCancel.addEventListener('click', hideModal);
        DOM.btnModalConfirm.addEventListener('click', () => {
            if (typeof modalConfirmCallback === 'function') {
                modalConfirmCallback();
            }
            hideModal();
        });

        // Close modal on overlay click
        DOM.modalOverlay.addEventListener('click', (e) => {
            if (e.target === DOM.modalOverlay) hideModal();
        });

        // Close dialogs on backdrop click
        DOM.addUserModal.addEventListener('click', (e) => {
            if (e.target === DOM.addUserModal) closeAddUserDialog();
        });
        DOM.editUserModal.addEventListener('click', (e) => {
            if (e.target === DOM.editUserModal) closeEditUserDialog();
        });

        // Escape key to close modals
        document.addEventListener('keydown', (e) => {
            if (e.key === 'Escape') {
                if (DOM.modalOverlay.dataset.visible === 'true') hideModal();
            }
        });

        // Pause heavy UI refresh work while tab is not visible
        document.addEventListener('visibilitychange', () => {
            if (!document.hidden) {
                pollStatus();
                loadLogs();
                loadUsers();
            }
        });
    }

    // ════════════════════════════════════════════════════════
    //  INITIALIZATION
    // ════════════════════════════════════════════════════════

    function init() {
        console.log('[SecureLock] Dashboard initializing...');

        bindEvents();

        // Initial data load
        pollStatus();
        loadLogs();
        loadUsers();

        startPollingLoops();

        console.log('[SecureLock] Dashboard ready');
    }

    // Start when DOM is ready
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }

})();
