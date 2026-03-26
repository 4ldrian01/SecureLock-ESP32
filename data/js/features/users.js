import { clearFormErrors, setFormError, escapeHtml } from '../core/helpers.js';

export function createUsersFeature({ CONFIG, state, DOM, apiFetch, feedback, onLogsUpdated }) {
    const ADD_USER_SUBMIT_IDLE_LABEL = DOM.btnSubmitAdd
        ? DOM.btnSubmitAdd.innerHTML
        : 'Save User';

    function normalizeUsersForDisplay(users) {
        // Enforce single-admin view: keep first admin only, convert duplicates to regular users.
        let adminSeen = false;

        return (Array.isArray(users) ? users : []).map((user) => {
            const normalized = { ...user };
            const role = (normalized.type || normalized.role || 'user').toString().toLowerCase();
            const uid = (normalized.uid || normalized.cardUID || '').toString().trim().toUpperCase();
            const isAdmin = role === 'admin' || uid === 'DEFAULT_ADMIN';

            if (isAdmin) {
                if (!adminSeen) {
                    normalized.type = 'admin';
                    adminSeen = true;
                } else {
                    normalized.type = 'user';
                }
            }

            return normalized;
        });
    }

    function createUserFingerprint(user) {
        return [
            String(user?.cardUID || user?.uid || '').trim().toUpperCase(),
            String(user?.name || '').trim(),
            String(user?.type || user?.role || 'user').trim().toLowerCase(),
            String(user?.telegramChatID || user?.chat_id || '').trim(),
            String(user?.backupPIN || user?.backup_pin || '').trim()
        ].join('|');
    }

    function buildUserCardInnerHTML(user) {
        const role = user.type || user.role || 'user';
        const badgeClass = role === 'admin' ? 'badge-admin'
            : role === 'guest' ? 'badge-guest'
                : 'badge-user';
        const isAdmin = role === 'admin';
        const uid = String(user.cardUID || user.uid || '').trim().toUpperCase();

        return `<div class="user-info">
                <div class="user-name-row">
                    <span class="user-name">${escapeHtml(user.name || 'Unknown')}</span>
                    <span class="badge user-badge ${badgeClass}">${escapeHtml(role.toUpperCase())}</span>
                </div>
                <div class="user-meta">
                    <span class="user-uid">${escapeHtml(uid || '--')}</span>
                </div>
            </div>
            <div class="user-actions">
                <button class="btn btn-ghost btn-edit" title="Edit user" data-action="edit" data-uid="${escapeHtml(uid)}">
                    <svg viewBox="0 0 24 24"><path d="M3 17.25V21h3.75L17.81 9.94l-3.75-3.75L3 17.25zM20.71 7.04c.39-.39.39-1.02 0-1.41l-2.34-2.34c-.39-.39-1.02-.39-1.41 0l-1.83 1.83 3.75 3.75 1.83-1.83z"/></svg>
                </button>
                ${isAdmin ? '' : `<button class="btn btn-ghost btn-delete" title="Delete user" data-action="delete" data-uid="${escapeHtml(uid)}">
                    <svg viewBox="0 0 24 24"><path d="M6 19c0 1.1.9 2 2 2h8c1.1 0 2-.9 2-2V7H6v12zM19 4h-3.5l-1-1h-5l-1 1H5v2h14V4z"/></svg>
                </button>`}
            </div>`;
    }

    function isAlphabeticName(name) {
        const trimmed = String(name || '').trim();
        if (!trimmed) {
            return false;
        }

        if (!/^[A-Za-z .'-]+$/.test(trimmed)) {
            return false;
        }

        return /[A-Za-z]/.test(trimmed);
    }

    function sanitizeAlphabeticName(value) {
        const cleaned = String(value || '')
            .replace(/[^A-Za-z .'-]+/g, '')
            .replace(/\s{2,}/g, ' ')
            .replace(/^\s+/, '');
        return cleaned;
    }

    function sanitizeUID(value) {
        return String(value || '')
            .toUpperCase()
            .replace(/[^0-9A-F]/g, '');
    }

    function isValidUid(uid) {
        return /^[0-9A-F]{8,20}$/.test(uid) && uid.length % 2 === 0;
    }

    function isWaitingScanText(value) {
        return /waiting|tap|scan|card/i.test(String(value || ''));
    }

    function isValidChatId(value) {
        const v = String(value || '').trim();
        return /^-?\d+$/.test(v);
    }
    function isDuplicateChatId(chatId, excludeUid = '') {
        const normalizedChatId = String(chatId || '').trim();
        if (!normalizedChatId) {
            return false;
        }

        const normalizedExcludeUid = String(excludeUid || '').trim().toUpperCase();
        return Object.entries(state.usersByUid || {}).some(([uid, user]) => {
            const normalizedUid = String(uid || '').trim().toUpperCase();
            if (normalizedUid && normalizedUid === normalizedExcludeUid) {
                return false;
            }

            const userChatId = String(user?.telegramChatID || user?.chat_id || '').trim();
            return userChatId && userChatId === normalizedChatId;
        });
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

        if (DOM.userRfid) {
            DOM.userRfid.readOnly = true;

            DOM.userRfid.addEventListener('keydown', (event) => {
                event.preventDefault();
            });

            DOM.userRfid.addEventListener('paste', (event) => {
                event.preventDefault();
            });

            DOM.userRfid.addEventListener('input', () => {
                const sanitized = sanitizeUID(DOM.userRfid.value);
                if (DOM.userRfid.value !== sanitized) {
                    DOM.userRfid.value = sanitized;
                }

                if (sanitized.length > 0) {
                    DOM.userRfid.classList.remove('input-error');
                    setFormError('userRfidError', '');
                }
            });
        }

        if (DOM.editUserRfid) {
            DOM.editUserRfid.addEventListener('input', () => {
                const sanitized = sanitizeUID(DOM.editUserRfid.value);
                if (DOM.editUserRfid.value !== sanitized) {
                    DOM.editUserRfid.value = sanitized;
                }

                if (sanitized.length > 0) {
                    DOM.editUserRfid.classList.remove('input-error');
                    setFormError('editUserRfidError', '');
                }
            });
        }
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

    function getUsersEndpointNoCache() {
        const separator = CONFIG.API.USERS.includes('?') ? '&' : '?';
        return `${CONFIG.API.USERS}${separator}_ts=${Date.now()}`;
    }

    async function loadUsers() {
        try {
            const data = await apiFetch(getUsersEndpointNoCache());
            const users = data.users || data || [];
            const normalizedUsers = normalizeUsersForDisplay(users);

            state.usersByUid = {};
            normalizedUsers.forEach((user) => {
                const key = String(user.cardUID || user.uid || '').trim().toUpperCase();
                if (key) {
                    state.usersByUid[key] = user;
                }
            });

            const usersHash = normalizedUsers.map(createUserFingerprint).join('||');
            if (usersHash === state.lastUsersHash) {
                return;
            }

            state.lastUsersHash = usersHash;
            renderUsers(normalizedUsers);
        } catch {
            DOM.usersGrid.innerHTML = '<p class="users-empty">Unable to load users</p>';
        }
    }

    function renderUsers(users) {
        if (!Array.isArray(users) || users.length === 0) {
            if (!DOM.usersGrid.querySelector('.users-empty')) {
                DOM.usersGrid.innerHTML =
                    '<p class="users-empty">' +
                    'No users registered. Tap "Add User" to get started.</p>';
            }

            state.renderedUserFingerprints = {};
            state.renderedUserOrder = [];
            return;
        }

        const emptyStateNode = DOM.usersGrid.querySelector('.users-empty');
        if (emptyStateNode) {
            emptyStateNode.remove();
        }

        const existingCards = new Map();
        DOM.usersGrid.querySelectorAll('.user-card[data-uid]').forEach((card) => {
            existingCards.set(String(card.dataset.uid || '').trim().toUpperCase(), card);
        });

        const nextFingerprints = {};
        const nextOrder = [];
        const cardsInOrder = [];

        users.forEach((user) => {
            const uid = String(user.cardUID || user.uid || '').trim().toUpperCase();
            if (!uid) {
                return;
            }

            const fingerprint = createUserFingerprint(user);
            nextFingerprints[uid] = fingerprint;
            nextOrder.push(uid);

            let card = existingCards.get(uid);
            if (!card) {
                card = document.createElement('div');
                card.className = 'user-card card';
                card.setAttribute('role', 'listitem');
                card.dataset.uid = uid;
                DOM.usersGrid.appendChild(card);
                existingCards.set(uid, card);
            }

            if (state.renderedUserFingerprints[uid] !== fingerprint || card.dataset.rendered !== '1') {
                card.innerHTML = buildUserCardInnerHTML(user);
                card.dataset.rendered = '1';
            }

            cardsInOrder.push(card);
        });

        existingCards.forEach((card, uid) => {
            if (!nextFingerprints[uid]) {
                card.remove();
            }
        });

        cardsInOrder.forEach((card) => {
            DOM.usersGrid.appendChild(card);
        });

        state.renderedUserFingerprints = nextFingerprints;
        state.renderedUserOrder = nextOrder;
    }

    function handleDeleteUser(uid) {
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
    }

    function handleEditUserRequest(uid) {
        state.editingUserId = uid;
        const key = String(uid || '').trim().toUpperCase();
        const user = state.usersByUid?.[key];

        DOM.editUserName.value = user?.name || '';
        DOM.editUserChatId.value = user?.telegramChatID || '';
        DOM.editUserBackupPin.value = user?.backupPIN || '';
        DOM.editUserRfid.value = user?.cardUID || user?.uid || uid || '';

        DOM.editUserRfid.classList.remove('input-error', 'scanned', 'scanning');
        setFormError('editUserRfidError', '');

        DOM.editUserModal.showModal();
    }

    function bindUsersGridActions() {
        DOM.usersGrid.addEventListener('click', (event) => {
            const actionButton = event.target.closest('button[data-action][data-uid]');
            if (!actionButton) {
                return;
            }

            const action = String(actionButton.dataset.action || '').trim();
            const uid = String(actionButton.dataset.uid || '').trim();

            if (!uid) {
                return;
            }

            if (action === 'delete') {
                handleDeleteUser(uid);
                return;
            }

            if (action === 'edit') {
                handleEditUserRequest(uid);
            }
        });
    }

    function stopRfidPoll() {
        state.rfidPollSession = Number(state.rfidPollSession || 0) + 1;

        if (state.rfidPollTimer) {
            clearTimeout(state.rfidPollTimer);
            state.rfidPollTimer = null;
        }
    }

    function startRfidPoll(targetInput) {
        stopRfidPoll();
        const sessionId = Number(state.rfidPollSession || 0);
        targetInput.value = 'Waiting for card tap...';
        targetInput.classList.add('scanning');
        targetInput.classList.remove('scanned', 'input-error');
        let lastScanTimestamp = 0;

        const isEditFlow = targetInput === DOM.editUserRfid;
        const editingUid = String(state.editingUserId || '').trim().toUpperCase();

        if (targetInput === DOM.userRfid) {
            setFormError('userRfidError', '');
        } else if (targetInput === DOM.editUserRfid) {
            setFormError('editUserRfidError', '');
        }

        const basePollInterval = Math.max(150, Number(CONFIG.RFID_POLL_INTERVAL || 400));
        const hiddenPollInterval = Math.max(basePollInterval, Number(CONFIG.RFID_POLL_INTERVAL_HIDDEN || 1000));
        const maxPollInterval = Math.max(hiddenPollInterval, Number(CONFIG.RFID_POLL_MAX_INTERVAL || 2000));
        let currentPollInterval = document.hidden ? hiddenPollInterval : basePollInterval;

        const pollRfidOnce = async () => {
            if (sessionId !== Number(state.rfidPollSession || 0)) {
                return { captured: false, error: false };
            }

            try {
                const data = await apiFetch(CONFIG.API.RFID_SCAN);
                const uid = String(data?.uid || data?.lastUid || '').trim();
                const scanTs = Number(data?.scanTimestamp || data?.lastScanTimestamp || data?.timestamp || 0);
                const hasFreshScan = Boolean(data?.scanned)
                    && uid.length > 0
                    && scanTs > lastScanTimestamp;

                if (hasFreshScan) {
                    lastScanTimestamp = scanTs;
                    const normalizedUid = uid.toUpperCase();
                    const knownFromApi = Boolean(data?.known);
                    const statusFromApi = String(data?.status || '').toLowerCase();
                    const known = knownFromApi || statusFromApi === 'registered';
                    const ownerName = String(data?.userName || '').trim();

                    if (!isEditFlow && known) {
                        targetInput.value = 'Waiting for card tap...';
                        targetInput.classList.add('scanning');
                        targetInput.classList.add('input-error');
                        targetInput.classList.remove('scanned');
                        setFormError(
                            'userRfidError',
                            ownerName
                                ? `RFID already registered to ${ownerName}. Scan a different card.`
                                : 'RFID already registered. Scan a different card.'
                        );
                        feedback.showToast('Registered RFID detected. Please scan an unregistered card.', 'error');
                        return { captured: false, error: false };
                    }

                    if (isEditFlow && known && normalizedUid !== editingUid) {
                        targetInput.value = 'Waiting for card tap...';
                        targetInput.classList.add('scanning');
                        targetInput.classList.add('input-error');
                        targetInput.classList.remove('scanned');
                        setFormError(
                            'editUserRfidError',
                            ownerName
                                ? `RFID already belongs to ${ownerName}. Scan another card.`
                                : 'RFID already belongs to another user. Scan another card.'
                        );
                        feedback.showToast('Card is already assigned to another user.', 'error');
                        return { captured: false, error: false };
                    }

                    targetInput.value = normalizedUid;
                    targetInput.classList.remove('scanning');
                    targetInput.classList.remove('input-error');
                    targetInput.classList.add('scanned');
                    stopRfidPoll();

                    if (known) {
                        feedback.showToast(
                            ownerName
                                ? `Registered card recognized (${ownerName})`
                                : 'Registered card recognized',
                            'info'
                        );
                    } else {
                        feedback.showToast('Unregistered RFID detected: ' + normalizedUid, 'success');
                    }

                    return { captured: true, error: false };
                }

                return { captured: false, error: false };
            } catch {
                return { captured: false, error: true };
            }
        };

        const scheduleNextPoll = (delayMs) => {
            if (sessionId !== Number(state.rfidPollSession || 0)) {
                return;
            }

            state.rfidPollTimer = setTimeout(async () => {
                if (sessionId !== Number(state.rfidPollSession || 0)) {
                    return;
                }

                const result = await pollRfidOnce();

                if (result.captured || sessionId !== Number(state.rfidPollSession || 0)) {
                    return;
                }

                const preferredInterval = document.hidden ? hiddenPollInterval : basePollInterval;
                if (result.error) {
                    currentPollInterval = Math.min(
                        maxPollInterval,
                        Math.round(Math.max(currentPollInterval, preferredInterval) * 1.35)
                    );
                } else {
                    currentPollInterval = preferredInterval;
                }

                scheduleNextPoll(currentPollInterval);
            }, Math.max(120, Number(delayMs) || basePollInterval));
        };

        // Baseline from current scan state so we only accept scans that happen
        // after this polling session starts.
        (async () => {
            try {
                const baseline = await apiFetch(CONFIG.API.RFID_SCAN);
                const baselineTs = Number(baseline?.scanTimestamp || baseline?.lastScanTimestamp || 0);
                if (Number.isFinite(baselineTs) && baselineTs > 0) {
                    lastScanTimestamp = baselineTs;
                }
            } catch {
                // If baseline read fails, immediate polling below will still recover.
            }

            if (sessionId !== Number(state.rfidPollSession || 0)) {
                return;
            }

            // Run once immediately after baseline so stale scans don't auto-fill.
            const firstResult = await pollRfidOnce();
            if (!firstResult.captured) {
                scheduleNextPoll(currentPollInterval);
            }
        })();
    }

    function openAddUserDialog() {
        DOM.addUserForm.reset();
        DOM.userRfid.value = '';
        DOM.userRfid.readOnly = true;
        DOM.userRfid.placeholder = 'Tap card on reader, then press Scan Card';
        DOM.userRfid.classList.remove('scanned', 'scanning', 'input-error');
        setFormError('userRfidError', '');
        setAddUserBusy(false);
        DOM.addUserModal.showModal();
        startRfidPoll(DOM.userRfid);
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

    function handleResetUsers() {
        feedback.showModal(
            'Reset All Users',
            'This will remove all users from device storage. Continue?',
            'danger',
            async () => {
                try {
                    const result = await apiFetch(CONFIG.API.USERS_RESET, {
                        method: 'POST',
                        body: JSON.stringify({ confirm: 'RESET_ALL_USERS' })
                    });

                    if (result?.success) {
                        feedback.showToast('All users were reset successfully', 'success');
                        await loadUsers();
                        if (typeof onLogsUpdated === 'function') {
                            onLogsUpdated();
                        }
                        return;
                    }

                    feedback.showToast(result?.message || 'Failed to reset users', 'error');
                } catch (error) {
                    feedback.showToast(
                        error?.payload?.message || error?.message || 'Connection error - could not reset users',
                        'error'
                    );
                }
            }
        );
    }

    async function handleAddUser(e) {
        e.preventDefault();

        if (state.addUserRequestInFlight) {
            return;
        }

        clearFormErrors(DOM);

        const name = DOM.userName.value.trim();
        const chatId = DOM.userChatId.value.trim();
        const backupPin = DOM.userBackupPin.value.trim();
        const rawRfidInput = String(DOM.userRfid.value || '').trim();
        let rfid = sanitizeUID(rawRfidInput);

        // If polling missed a valid tap, recover from latest scan once before blocking submit.
        if (!rfid || isWaitingScanText(rawRfidInput)) {
            try {
                const scan = await apiFetch(CONFIG.API.RFID_SCAN);
                const scannedUid = sanitizeUID(scan?.uid || scan?.lastUid || '');
                if (scan?.scanned && scannedUid.length > 0) {
                    rfid = scannedUid;
                    DOM.userRfid.value = scannedUid;
                    DOM.userRfid.classList.remove('scanning', 'input-error');
                    DOM.userRfid.classList.add('scanned');
                }
            } catch {
                // Continue to normal validation and show user-facing form errors below.
            }
        }

        let valid = true;
        if (!name) {
            setFormError('userNameError', 'Name is required');
            valid = false;
        } else if (!isAlphabeticName(name)) {
            setFormError('userNameError', 'Name can include letters, spaces, apostrophes, dots, and hyphens only');
            valid = false;
        }
        if (!chatId) {
            setFormError('userChatIdError', 'Telegram Chat ID is required');
            valid = false;
        } else if (!isValidChatId(chatId)) {
            setFormError('userChatIdError', 'Telegram Chat ID must be numeric');
            valid = false;
        } else if (isDuplicateChatId(chatId)) {
            setFormError('userChatIdError', 'Telegram Chat ID already linked to another user');
            valid = false;
        }
        if (!backupPin || !/^\d{4}$/.test(backupPin)) {
            setFormError('userBackupPinError', 'Backup PIN must be exactly 4 digits');
            valid = false;
        }
        if (!rfid) {
            setFormError('userRfidError', 'RFID tag is required - tap a card');
            DOM.userRfid.classList.add('input-error');
            feedback.showToast('Scan an RFID card first.', 'error');
            valid = false;
        } else if (!isValidUid(rfid)) {
            setFormError('userRfidError', 'RFID UID must be 8-20 hex characters (A-F, 0-9)');
            DOM.userRfid.classList.add('input-error');
            feedback.showToast('Invalid RFID format. Scan again or enter a valid UID.', 'error');
            valid = false;
        }
        if (!valid) return;

        DOM.userRfid.classList.remove('input-error');

        try {
            setAddUserBusy(true);

            const data = await apiFetch(CONFIG.API.USERS, {
                method: 'POST',
                body: JSON.stringify({
                    name,
                    pin: backupPin,
                    cardUID: rfid,
                    uid: rfid,
                    telegramChatID: chatId,
                    chat_id: chatId,
                    backupPIN: backupPin,
                    backup_pin: backupPin,
                    type: 'user'
                })
            });

            if (data.success) {
                feedback.showToast('User added successfully', 'success');
                closeAddUserDialog();
                await loadUsers();
                if (typeof onLogsUpdated === 'function') onLogsUpdated();
            } else {
                feedback.showToast(data.message || 'Failed to add user', 'error');
            }
        } catch (error) {
            if (Number(error?.status) === 400 && error?.payload?.field === 'name') {
                setFormError('userNameError', error?.payload?.message || 'Name can include letters, spaces, apostrophes, dots, and hyphens only');
                feedback.showToast(error?.payload?.message || 'Invalid name format', 'error');
                return;
            }

            if (Number(error?.status) === 409 && error?.payload?.field === 'telegramChatID') {
                setFormError(
                    'userChatIdError',
                    error?.payload?.message || 'Telegram Chat ID already linked to another user'
                );
                feedback.showToast('Telegram Chat ID already registered — use a different account', 'error');
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

            feedback.showToast(
                error?.payload?.message || error?.message || 'Connection error — could not add user',
                'error'
            );
        } finally {
            setAddUserBusy(false);
        }
    }

    async function handleEditUser(e) {
        e.preventDefault();
        clearFormErrors(DOM);

        const name = DOM.editUserName.value.trim();
        const chatId = DOM.editUserChatId.value.trim();
        const backupPin = DOM.editUserBackupPin.value.trim();
        const rfid = sanitizeUID(DOM.editUserRfid.value);

        if (DOM.editUserRfid.value !== rfid) {
            DOM.editUserRfid.value = rfid;
        }

        let valid = true;
        if (!name) {
            setFormError('editUserNameError', 'Name is required');
            valid = false;
        } else if (!isAlphabeticName(name)) {
            setFormError('editUserNameError', 'Name can include letters, spaces, apostrophes, dots, and hyphens only');
            valid = false;
        }
        if (chatId && !isValidChatId(chatId)) {
            setFormError('editUserChatIdError', 'Telegram Chat ID must be numeric');
            valid = false;
        } else if (!chatId) {
            setFormError('editUserChatIdError', 'Telegram Chat ID is required');
            valid = false;
        } else if (chatId && isDuplicateChatId(chatId, state.editingUserId)) {
            setFormError('editUserChatIdError', 'Telegram Chat ID already linked to another user');
            valid = false;
        }
        if (backupPin && !/^\d{4}$/.test(backupPin)) {
            setFormError('editUserBackupPinError', 'Backup PIN must be exactly 4 digits');
            valid = false;
        } else if (!backupPin) {
            setFormError('editUserBackupPinError', 'Backup PIN is required');
            valid = false;
        }
        if (!rfid) {
            setFormError('editUserRfidError', 'RFID tag is required');
            DOM.editUserRfid.classList.add('input-error');
            valid = false;
        } else if (!isValidUid(rfid)) {
            setFormError('editUserRfidError', 'RFID UID must be 8-20 hex characters (A-F, 0-9)');
            DOM.editUserRfid.classList.add('input-error');
            valid = false;
        }
        if (!valid) return;

        DOM.editUserRfid.classList.remove('input-error');

        try {
            const body = {
                uid: state.editingUserId,
                name,
                pin: backupPin,
                rfid,
                cardUID: rfid,
                telegramChatID: chatId,
                chat_id: chatId,
                backupPIN: backupPin
            };

            const data = await apiFetch(CONFIG.API.USERS, {
                method: 'PUT',
                body: JSON.stringify(body)
            });

            if (data.success) {
                feedback.showToast('User updated successfully', 'success');
                closeEditUserDialog();
                await loadUsers();
                if (typeof onLogsUpdated === 'function') onLogsUpdated();
            } else {
                feedback.showToast(data.message || 'Failed to update user', 'error');
            }
        } catch (error) {
            if (Number(error?.status) === 400 && error?.payload?.field === 'name') {
                setFormError('editUserNameError', error?.payload?.message || 'Name can include letters, spaces, apostrophes, dots, and hyphens only');
                feedback.showToast(error?.payload?.message || 'Invalid name format', 'error');
                return;
            }

            if (Number(error?.status) === 409 && error?.payload?.field === 'telegramChatID') {
                setFormError(
                    'editUserChatIdError',
                    error?.payload?.message || 'Telegram Chat ID already linked to another user'
                );
                feedback.showToast('Telegram Chat ID already registered — use a different account', 'error');
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

            feedback.showToast(
                error?.payload?.message || error?.message || 'Connection error — could not update user',
                'error'
            );
        }
    }

    function bindEvents() {
        bindNameInputRestrictions();
        bindUsersGridActions();

        DOM.btnAddUser.addEventListener('click', openAddUserDialog);
        if (DOM.btnResetUsers) {
            DOM.btnResetUsers.addEventListener('click', handleResetUsers);
        }
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

    }

    return {
        bindEvents,
        loadUsers,
        updateAddUserSubmitButton
    };
}
