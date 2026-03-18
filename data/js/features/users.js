import { clearFormErrors, setFormError, escapeHtml } from '../core/helpers.js';

export function createUsersFeature({ CONFIG, state, DOM, apiFetch, feedback, onLogsUpdated }) {
    const ADD_USER_SUBMIT_IDLE_LABEL = DOM.btnSubmitAdd
        ? DOM.btnSubmitAdd.innerHTML
        : 'Save User';

    function isAlphabeticName(name) {
        const trimmed = String(name || '').trim();
        if (!trimmed) {
            return false;
        }

        if (!/^[A-Za-z ]+$/.test(trimmed)) {
            return false;
        }

        return /[A-Za-z]/.test(trimmed);
    }

    function sanitizeAlphabeticName(value) {
        const cleaned = String(value || '')
            .replace(/[^A-Za-z ]+/g, '')
            .replace(/\s{2,}/g, ' ')
            .replace(/^\s+/, '');
        return cleaned;
    }

    function bindNameInputRestrictions() {
        const fields = [DOM.userName, DOM.editUserName].filter(Boolean);

        fields.forEach((field) => {
            field.addEventListener('input', () => {
                const sanitized = sanitizeAlphabeticName(field.value);
                if (field.value !== sanitized) {
                    field.value = sanitized;
                }
            });
        });
    }

    function updateAddUserSubmitButton() {
        if (!DOM.btnSubmitAdd) return;

        if (state.addUserRequestInFlight) {
            DOM.btnSubmitAdd.disabled = true;
            DOM.btnSubmitAdd.textContent = 'Saving...';
            return;
        }

        DOM.btnSubmitAdd.disabled = false;
        DOM.btnSubmitAdd.innerHTML = ADD_USER_SUBMIT_IDLE_LABEL;
    }

    function setAddUserBusy(isBusy) {
        state.addUserRequestInFlight = Boolean(isBusy);
        updateAddUserSubmitButton();
    }

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

    function stopRfidPoll() {
        if (state.rfidPollTimer) {
            clearInterval(state.rfidPollTimer);
            state.rfidPollTimer = null;
        }
    }

    function startRfidPoll(targetInput) {
        stopRfidPoll();
        targetInput.value = 'Waiting for card tap...';
        targetInput.classList.add('scanning');
        targetInput.classList.remove('scanned', 'input-error');

        if (targetInput === DOM.userRfid) {
            setFormError('userRfidError', '');
        } else if (targetInput === DOM.editUserRfid) {
            setFormError('editUserRfidError', '');
        }

        state.rfidPollTimer = setInterval(async () => {
            try {
                const data = await apiFetch(CONFIG.API.RFID_SCAN);
                if (data.uid && data.uid.length > 0) {
                    targetInput.value = data.uid;
                    targetInput.classList.remove('scanning');
                    targetInput.classList.remove('input-error');
                    targetInput.classList.add('scanned');
                    stopRfidPoll();
                    feedback.showToast('RFID card detected: ' + data.uid, 'success');
                }
            } catch {
                // Keep polling silently
            }
        }, CONFIG.RFID_POLL_INTERVAL);
    }

    function openAddUserDialog() {
        DOM.addUserForm.reset();
        DOM.userRfid.value = '';
        DOM.userRfid.classList.remove('scanned', 'scanning', 'input-error');
        setFormError('userRfidError', '');
        setAddUserBusy(false);
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

    async function handleAddUser(e) {
        e.preventDefault();

        if (state.addUserRequestInFlight) {
            return;
        }

        clearFormErrors(DOM);

        const name = DOM.userName.value.trim();
        const pin = DOM.userPin.value.trim();
        const rfid = DOM.userRfid.value.trim();

        let valid = true;
        if (!name) {
            setFormError('userNameError', 'Name is required');
            valid = false;
        } else if (!isAlphabeticName(name)) {
            setFormError('userNameError', 'Name must contain letters only');
            valid = false;
        }
        if (!pin || !/^\d{4}$/.test(pin)) {
            setFormError('userPinError', 'PIN must be exactly 4 digits');
            valid = false;
        }
        if (!rfid || rfid === 'Waiting for card tap...') {
            setFormError('userRfidError', 'RFID tag is required — tap a card');
            DOM.userRfid.classList.add('input-error');
            valid = false;
        }
        if (!valid) return;

        DOM.userRfid.classList.remove('input-error');

        try {
            setAddUserBusy(true);

            const data = await apiFetch(CONFIG.API.USERS, {
                method: 'POST',
                body: JSON.stringify({ name, pin, uid: rfid, type: 'user' })
            });

            if (data.success) {
                feedback.showToast('User added successfully', 'success');
                closeAddUserDialog();
                loadUsers();
                if (typeof onLogsUpdated === 'function') onLogsUpdated();
            } else {
                feedback.showToast(data.message || 'Failed to add user', 'error');
            }
        } catch (error) {
            if (Number(error?.status) === 400 && error?.payload?.field === 'name') {
                setFormError('userNameError', error?.payload?.message || 'Name must contain letters only');
                feedback.showToast(error?.payload?.message || 'Invalid name format', 'error');
                return;
            }

            const isDuplicateRFID =
                Number(error?.status) === 409 &&
                (error?.payload?.errorCode === 'RFID_ALREADY_REGISTERED' || error?.payload?.field === 'uid');

            if (isDuplicateRFID) {
                DOM.userRfid.classList.add('input-error');
                setFormError(
                    'userRfidError',
                    error?.payload?.message || 'This RFID is already registered. Please scan another card.'
                );
                feedback.showToast('RFID already registered — please use another card', 'error');
                return;
            }

            feedback.showToast('Connection error — could not add user', 'error');
        } finally {
            setAddUserBusy(false);
        }
    }

    async function handleEditUser(e) {
        e.preventDefault();
        clearFormErrors(DOM);

        const name = DOM.editUserName.value.trim();
        const pin = DOM.editUserPin.value.trim();
        const rfid = DOM.editUserRfid.value.trim();

        let valid = true;
        if (!name) {
            setFormError('editUserNameError', 'Name is required');
            valid = false;
        } else if (!isAlphabeticName(name)) {
            setFormError('editUserNameError', 'Name must contain letters only');
            valid = false;
        }
        if (pin && !/^\d{4}$/.test(pin)) {
            setFormError('editUserPinError', 'PIN must be exactly 4 digits');
            valid = false;
        }
        if (!rfid) {
            setFormError('editUserRfidError', 'RFID tag is required');
            DOM.editUserRfid.classList.add('input-error');
            valid = false;
        }
        if (!valid) return;

        DOM.editUserRfid.classList.remove('input-error');

        try {
            const body = { uid: state.editingUserId, name, rfid };
            if (pin) body.pin = pin;

            const data = await apiFetch(CONFIG.API.USERS, {
                method: 'PUT',
                body: JSON.stringify(body)
            });

            if (data.success) {
                feedback.showToast('User updated successfully', 'success');
                closeEditUserDialog();
                loadUsers();
                if (typeof onLogsUpdated === 'function') onLogsUpdated();
            } else {
                feedback.showToast(data.message || 'Failed to update user', 'error');
            }
        } catch (error) {
            if (Number(error?.status) === 400 && error?.payload?.field === 'name') {
                setFormError('editUserNameError', error?.payload?.message || 'Name must contain letters only');
                feedback.showToast(error?.payload?.message || 'Invalid name format', 'error');
                return;
            }

            const isDuplicateRFID =
                Number(error?.status) === 409 &&
                (error?.payload?.errorCode === 'RFID_ALREADY_REGISTERED' || error?.payload?.field === 'uid');

            if (isDuplicateRFID) {
                DOM.editUserRfid.classList.add('input-error');
                setFormError(
                    'editUserRfidError',
                    error?.payload?.message || 'This RFID is already registered. Please scan another card.'
                );
                feedback.showToast('RFID already registered — please use another card', 'error');
                return;
            }

            feedback.showToast('Connection error — could not update user', 'error');
        }
    }

    function bindWindowActions() {
        window.__deleteUser = function (uid) {
            feedback.showModal(
                'Delete User',
                'Are you sure you want to remove this user? This action cannot be undone.',
                'danger',
                async () => {
                    try {
                        const data = await apiFetch(`${CONFIG.API.USERS}?uid=${encodeURIComponent(uid)}`, {
                            method: 'DELETE'
                        });
                        if (data.success) {
                            feedback.showToast('User deleted successfully', 'success');
                            loadUsers();
                            if (typeof onLogsUpdated === 'function') onLogsUpdated();
                        } else {
                            feedback.showToast(data.message || 'Failed to delete user', 'error');
                        }
                    } catch {
                        feedback.showToast('Connection error — could not delete user', 'error');
                    }
                }
            );
        };

        window.__editUser = function (uid) {
            state.editingUserId = uid;
            const card = document.querySelector(`.user-card[data-uid="${uid}"]`);
            if (card) {
                const name = card.querySelector('.user-name')?.textContent || '';
                const rfid = card.querySelector('.user-uid')?.textContent || '';
                DOM.editUserName.value = name;
                DOM.editUserPin.value = '';
                DOM.editUserRfid.value = rfid;
            }

            DOM.editUserRfid.classList.remove('input-error', 'scanned', 'scanning');
            setFormError('editUserRfidError', '');

            DOM.editUserModal.showModal();
        };
    }

    function bindEvents() {
        bindNameInputRestrictions();

        DOM.btnAddUser.addEventListener('click', openAddUserDialog);
        DOM.btnCloseDialog.addEventListener('click', closeAddUserDialog);
        DOM.btnCancelAdd.addEventListener('click', closeAddUserDialog);
        DOM.addUserForm.addEventListener('submit', handleAddUser);
        DOM.btnScanCard.addEventListener('click', () => startRfidPoll(DOM.userRfid));

        DOM.btnCloseEditDialog.addEventListener('click', closeEditUserDialog);
        DOM.btnCancelEdit.addEventListener('click', closeEditUserDialog);
        DOM.editUserForm.addEventListener('submit', handleEditUser);
        DOM.btnReplaceCard.addEventListener('click', () => startRfidPoll(DOM.editUserRfid));

        DOM.addUserModal.addEventListener('click', (e) => {
            if (e.target === DOM.addUserModal) closeAddUserDialog();
        });

        DOM.editUserModal.addEventListener('click', (e) => {
            if (e.target === DOM.editUserModal) closeEditUserDialog();
        });

        bindWindowActions();
    }

    return {
        bindEvents,
        loadUsers,
        updateAddUserSubmitButton
    };
}
