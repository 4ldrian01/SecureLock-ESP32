import { clearFormErrors, setFormError, escapeHtml } from '../core/helpers.js?v=20260419r2';

export function createUsersFeature({ CONFIG, state, DOM, apiFetch, feedback, onLogsUpdated }) {
    const ADD_USER_SUBMIT_IDLE_LABEL = DOM.btnSubmitAdd
        ? DOM.btnSubmitAdd.innerHTML
        : 'Save User';
    const TELEGRAM_CHAT_ID_RULE_MESSAGE = 'Telegram Chat ID must be 6-15 digits (optional leading -)';
    const RESET_USERS_IDLE_LABEL = DOM.btnResetUsers
        ? DOM.btnResetUsers.textContent
        : 'Reset Users';
    const SCAN_CARD_IDLE_LABEL = DOM.btnScanCard
        ? DOM.btnScanCard.innerHTML
        : '📡 Scan Card';
    const REPLACE_CARD_IDLE_LABEL = DOM.btnReplaceCard
        ? DOM.btnReplaceCard.innerHTML
        : 'Replace Card';
    let resetUsersInFlight = false;

    function isTruthyAdminFlag(value) {
        if (value === true || value === 1) {
            return true;
        }

        const normalized = String(value ?? '').trim().toLowerCase();
        return normalized === 'true' || normalized === '1' || normalized === 'yes';
    }

    function resolveOptionalBoolean(value, fallback) {
        if (value === undefined || value === null || value === '') {
            return fallback;
        }

        return isTruthyAdminFlag(value);
    }

    function isSeededAdminUID(uid) {
        const normalizedUid = String(uid || '').trim().toUpperCase();
        return normalizedUid === 'DEFAULT_ADMIN' || /^DEFAULT_ADMIN_\d+$/.test(normalizedUid);
    }

    function isSeededAdminUser(user) {
        const uid = String(user?.uid || user?.cardUID || '').trim().toUpperCase();
        return isTruthyAdminFlag(user?.isSeededAdmin) || isSeededAdminUID(uid);
    }

    function sanitizeAlphabeticNamePart(value) {
        const cleaned = String(value || '')
            .replace(/[^A-Za-z .'-]+/g, '')
            .replace(/\s{2,}/g, ' ')
            .replace(/^\s+/, '');

        return cleaned;
    }

    function normalizeNamePart(value) {
        return sanitizeAlphabeticNamePart(value).trim();
    }

    function isAlphabeticNamePart(namePart, { required = true } = {}) {
        const trimmed = normalizeNamePart(namePart);
        if (!trimmed) {
            return !required;
        }

        if (!/^[A-Za-z .'-]+$/.test(trimmed)) {
            return false;
        }

        return /[A-Za-z]/.test(trimmed);
    }

    function splitFullName(fullName) {
        const normalized = sanitizeAlphabeticNamePart(fullName).trim();
        if (!normalized) {
            return {
                firstName: '',
                middleName: '',
                lastName: ''
            };
        }

        const parts = normalized.split(/\s+/).filter(Boolean);
        if (parts.length === 1) {
            return {
                firstName: parts[0],
                middleName: '',
                lastName: ''
            };
        }

        return {
            firstName: parts[0],
            middleName: parts.slice(1, -1).join(' '),
            lastName: parts[parts.length - 1]
        };
    }

    function resolveNamePartsFromUser(user) {
        const firstName = normalizeNamePart(user?.firstName || user?.first_name || '');
        const middleName = normalizeNamePart(user?.middleName || user?.middle_name || '');
        const lastName = normalizeNamePart(user?.lastName || user?.last_name || '');

        if (firstName || middleName || lastName) {
            return { firstName, middleName, lastName };
        }

        return splitFullName(user?.name || '');
    }

    function composeFullName({ firstName, middleName, lastName }) {
        return [
            normalizeNamePart(firstName),
            normalizeNamePart(middleName),
            normalizeNamePart(lastName)
        ]
            .filter(Boolean)
            .join(' ')
            .replace(/\s{2,}/g, ' ')
            .trim();
    }

    function normalizeRoleForDisplay(user) {
        const uid = String(user?.uid || user?.cardUID || '').trim().toUpperCase();
        const role = String(user?.type || user?.role || 'user').trim().toLowerCase();
        const isAdminChat = isTruthyAdminFlag(user?.isAdminChat);

        if (role === 'admin' || isSeededAdminUID(uid) || isAdminChat) {
            return 'admin';
        }

        if (role === 'guest') {
            return 'guest';
        }

        return 'user';
    }

    function parsePositiveInteger(value) {
        const parsed = Number(value);
        if (!Number.isFinite(parsed)) {
            return 0;
        }

        const integerValue = Math.floor(parsed);
        return integerValue > 0 ? integerValue : 0;
    }

    function normalizeQueueCategory(value) {
        const normalized = String(value || '').trim().toUpperCase();
        if (normalized === 'ADMIN' || normalized === 'USER') {
            return normalized;
        }

        return '';
    }

    function formatQueueLabel(category, index) {
        const normalizedCategory = category === 'ADMIN' ? 'ADMIN' : 'USER';
        const normalizedIndex = Math.max(1, parsePositiveInteger(index) || 1);
        const labelPrefix = normalizedCategory === 'ADMIN' ? 'Admin' : 'User';
        return `${labelPrefix} ${String(normalizedIndex).padStart(2, '0')}`;
    }

    function resolveQueueMetadata(user, role, queueCounters) {
        const fallbackCategory = role === 'admin' ? 'ADMIN' : 'USER';
        const queueCategory = normalizeQueueCategory(user?.queueCategory || user?.roleQueueCategory)
            || fallbackCategory;

        const providedIndex = parsePositiveInteger(
            user?.queueIndex
            ?? user?.roleQueueIndex
            ?? user?.indexInRole
        );

        let queueIndex = providedIndex;
        if (queueIndex <= 0) {
            if (queueCategory === 'ADMIN') {
                queueCounters.admin += 1;
                queueIndex = queueCounters.admin;
            } else {
                queueCounters.user += 1;
                queueIndex = queueCounters.user;
            }
        } else if (queueCategory === 'ADMIN') {
            queueCounters.admin = Math.max(queueCounters.admin, queueIndex);
        } else {
            queueCounters.user = Math.max(queueCounters.user, queueIndex);
        }

        const queueLabel = String(user?.queueLabel || '').trim() || formatQueueLabel(queueCategory, queueIndex);

        return {
            queueCategory,
            queueIndex,
            queueLabel
        };
    }

    function getDisplayName(user) {
        const nameParts = resolveNamePartsFromUser(user);
        const composed = composeFullName(nameParts);
        if (composed) {
            return composed;
        }

        const fallback = String(user?.name || '').trim();
        return fallback || 'Unknown';
    }

    function getNamePartsFromForm(prefix) {
        if (prefix === 'edit') {
            return {
                firstName: normalizeNamePart(DOM.editUserFirstName?.value || ''),
                middleName: normalizeNamePart(DOM.editUserMiddleName?.value || ''),
                lastName: normalizeNamePart(DOM.editUserLastName?.value || '')
            };
        }

        return {
            firstName: normalizeNamePart(DOM.userFirstName?.value || ''),
            middleName: normalizeNamePart(DOM.userMiddleName?.value || ''),
            lastName: normalizeNamePart(DOM.userLastName?.value || '')
        };
    }

    function validateNameParts(nameParts, prefix) {
        let valid = true;

        const firstNameErrorId = prefix === 'edit' ? 'editUserFirstNameError' : 'userFirstNameError';
        const middleNameErrorId = prefix === 'edit' ? 'editUserMiddleNameError' : 'userMiddleNameError';
        const lastNameErrorId = prefix === 'edit' ? 'editUserLastNameError' : 'userLastNameError';

        if (!nameParts.firstName) {
            setFormError(firstNameErrorId, 'First name is required');
            valid = false;
        } else if (!isAlphabeticNamePart(nameParts.firstName, { required: true })) {
            setFormError(firstNameErrorId, 'First name can include letters, spaces, apostrophes, dots, and hyphens only');
            valid = false;
        }

        if (nameParts.middleName && !isAlphabeticNamePart(nameParts.middleName, { required: false })) {
            setFormError(middleNameErrorId, 'Middle name can include letters, spaces, apostrophes, dots, and hyphens only');
            valid = false;
        }

        if (!nameParts.lastName) {
            setFormError(lastNameErrorId, 'Last name is required');
            valid = false;
        } else if (!isAlphabeticNamePart(nameParts.lastName, { required: true })) {
            setFormError(lastNameErrorId, 'Last name can include letters, spaces, apostrophes, dots, and hyphens only');
            valid = false;
        }

        const fullName = composeFullName(nameParts);
        if (fullName.length > 48) {
            setFormError(lastNameErrorId, 'Combined full name is too long (max 48 characters)');
            valid = false;
        }

        return {
            valid,
            fullName
        };
    }

    function normalizeUsersForDisplay(users) {
        const queueCounters = {
            admin: 0,
            user: 0
        };

        const mappedUsers = (Array.isArray(users) ? users : []).map((user) => {
            const normalized = { ...user };
            const uid = (normalized.uid || normalized.cardUID || '').toString().trim().toUpperCase();

            const nameParts = resolveNamePartsFromUser(normalized);
            const fullName = composeFullName(nameParts);
            const chatId = String(normalized?.telegramChatID || normalized?.chat_id || '').trim();
            const role = normalizeRoleForDisplay(normalized);

            normalized.uid = uid;
            normalized.cardUID = uid;
            normalized.firstName = nameParts.firstName;
            normalized.middleName = nameParts.middleName;
            normalized.lastName = nameParts.lastName;
            normalized.name = fullName || String(normalized.name || '').trim();
            normalized.telegramChatID = chatId;
            normalized.chat_id = chatId;

            const seededAdmin = isSeededAdminUser(normalized);
            const canEdit = resolveOptionalBoolean(normalized?.editable, true);
            const canDelete = resolveOptionalBoolean(normalized?.deletable, role !== 'admin' && !seededAdmin);

            normalized.type = role;
            normalized.role = role;
            normalized.isAdminChat = role === 'admin';
            normalized.isSeededAdmin = seededAdmin;
            normalized.editable = canEdit;
            normalized.deletable = seededAdmin ? false : canDelete;

            const queueMeta = resolveQueueMetadata(normalized, role, queueCounters);
            normalized.queueCategory = queueMeta.queueCategory;
            normalized.queueIndex = queueMeta.queueIndex;
            normalized.roleQueueCategory = queueMeta.queueCategory;
            normalized.roleQueueIndex = queueMeta.queueIndex;
            normalized.queueLabel = queueMeta.queueLabel;

            return normalized;
        });

        mappedUsers.sort((lhs, rhs) => {
            const lhsRole = normalizeRoleForDisplay(lhs);
            const rhsRole = normalizeRoleForDisplay(rhs);
            const lhsAdmin = lhsRole === 'admin';
            const rhsAdmin = rhsRole === 'admin';

            if (lhsAdmin !== rhsAdmin) {
                return lhsAdmin ? -1 : 1;
            }

            const lhsQueueIndex = parsePositiveInteger(lhs?.queueIndex || lhs?.roleQueueIndex);
            const rhsQueueIndex = parsePositiveInteger(rhs?.queueIndex || rhs?.roleQueueIndex);

            if (lhsQueueIndex > 0 && rhsQueueIndex > 0 && lhsQueueIndex !== rhsQueueIndex) {
                return lhsQueueIndex - rhsQueueIndex;
            }

            if (lhsQueueIndex > 0 && rhsQueueIndex <= 0) {
                return -1;
            }

            if (rhsQueueIndex > 0 && lhsQueueIndex <= 0) {
                return 1;
            }

            const lhsDisplayOrder = Number(lhs?.displayOrder);
            const rhsDisplayOrder = Number(rhs?.displayOrder);
            if (Number.isFinite(lhsDisplayOrder) && Number.isFinite(rhsDisplayOrder) && lhsDisplayOrder !== rhsDisplayOrder) {
                return lhsDisplayOrder - rhsDisplayOrder;
            }

            const lhsName = getDisplayName(lhs);
            const rhsName = getDisplayName(rhs);
            const nameCompare = lhsName.localeCompare(rhsName, undefined, {
                sensitivity: 'base',
                numeric: true
            });
            if (nameCompare !== 0) {
                return nameCompare;
            }

            const lhsUid = String(lhs?.uid || lhs?.cardUID || '').trim().toUpperCase();
            const rhsUid = String(rhs?.uid || rhs?.cardUID || '').trim().toUpperCase();
            return lhsUid.localeCompare(rhsUid, undefined, {
                sensitivity: 'base',
                numeric: true
            });
        });

        return mappedUsers;
    }

    function createUserFingerprint(user) {
        const nameParts = resolveNamePartsFromUser(user);
        return [
            String(user?.cardUID || user?.uid || '').trim().toUpperCase(),
            composeFullName(nameParts),
            nameParts.firstName,
            nameParts.middleName,
            nameParts.lastName,
            normalizeRoleForDisplay(user),
            String(user?.queueCategory || user?.roleQueueCategory || ''),
            String(user?.queueIndex || user?.roleQueueIndex || ''),
            String(user?.queueLabel || ''),
            String(user?.telegramChatID || user?.chat_id || '').trim(),
            String(user?.backupPIN || user?.backup_pin || '').trim(),
            isTruthyAdminFlag(user?.isAdminChat) ? '1' : '0',
            isTruthyAdminFlag(user?.isSeededAdmin) ? '1' : '0',
            resolveOptionalBoolean(user?.editable, true) ? '1' : '0',
            resolveOptionalBoolean(user?.deletable, true) ? '1' : '0'
        ].join('|');
    }

    function buildUserCardInnerHTML(user) {
        const role = normalizeRoleForDisplay(user);
        const badgeClass = role === 'admin' ? 'badge-admin'
            : role === 'guest' ? 'badge-guest'
                : 'badge-user';
        const isAdmin = role === 'admin';
        const seededAdmin = isSeededAdminUser(user);
        const uid = String(user.cardUID || user.uid || '').trim().toUpperCase();
        const name = getDisplayName(user);
        const chatId = String(user?.telegramChatID || user?.chat_id || '').trim();
        const queueIndex = parsePositiveInteger(user?.queueIndex || user?.roleQueueIndex) || 1;
        const roleLabel = isAdmin
            ? `ADMIN ${String(queueIndex).padStart(2, '0')}`
            : String(role || 'user').toUpperCase();

        const canEdit = resolveOptionalBoolean(user?.editable, true);
        const canDelete = resolveOptionalBoolean(user?.deletable, role !== 'admin' && !seededAdmin);

        let actionsHtml = '';
        if (canEdit) {
            actionsHtml += `<button class="btn btn-ghost btn-edit" title="Edit user" data-action="edit" data-uid="${escapeHtml(uid)}">
                    <svg viewBox="0 0 24 24"><path d="M3 17.25V21h3.75L17.81 9.94l-3.75-3.75L3 17.25zM20.71 7.04c.39-.39.39-1.02 0-1.41l-2.34-2.34c-.39-.39-1.02-.39-1.41 0l-1.83 1.83 3.75 3.75 1.83-1.83z"/></svg>
                </button>`;
        }

        if (canDelete) {
            actionsHtml += `<button class="btn btn-ghost btn-delete" title="Delete user" data-action="delete" data-uid="${escapeHtml(uid)}">
                    <svg viewBox="0 0 24 24"><path d="M6 19c0 1.1.9 2 2 2h8c1.1 0 2-.9 2-2V7H6v12zM19 4h-3.5l-1-1h-5l-1 1H5v2h14V4z"/></svg>
                </button>`;
        }

        const uidMetaHtml = seededAdmin
            ? ''
            : `<span class="user-uid">${escapeHtml(uid || '--')}</span>`;

        return `<div class="user-info">
                <div class="user-name-row">
                    <span class="user-name">${escapeHtml(name)}</span>
                    <span class="badge user-badge ${badgeClass}">${escapeHtml(roleLabel)}</span>
                </div>
                <div class="user-meta">
                    ${uidMetaHtml}
                    <span class="user-chat">TG: ${escapeHtml(chatId || '--')}</span>
                </div>
            </div>
            <div class="user-actions">
                ${actionsHtml}
            </div>`;
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
        return /^-?\d{6,15}$/.test(v);
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

    function isDuplicateBackupPin(backupPin, excludeUid = '') {
        const normalizedPin = String(backupPin || '').trim();
        if (!/^\d{4}$/.test(normalizedPin)) {
            return false;
        }

        const normalizedExcludeUid = String(excludeUid || '').trim().toUpperCase();
        return Object.entries(state.usersByUid || {}).some(([uid, user]) => {
            const normalizedUid = String(uid || '').trim().toUpperCase();
            if (normalizedUid && normalizedUid === normalizedExcludeUid) {
                return false;
            }

            const userBackupPin = String(user?.backupPIN || user?.backup_pin || user?.pin || '').trim();
            return /^\d{4}$/.test(userBackupPin) && userBackupPin === normalizedPin;
        });
    }

    function isDuplicateUid(uid, excludeUid = '') {
        const normalizedUid = String(uid || '').trim().toUpperCase();
        if (!normalizedUid) {
            return false;
        }

        const normalizedExcludeUid = String(excludeUid || '').trim().toUpperCase();
        return Object.keys(state.usersByUid || {}).some((existingUid) => {
            const normalizedExistingUid = String(existingUid || '').trim().toUpperCase();
            if (!normalizedExistingUid || normalizedExistingUid === normalizedExcludeUid) {
                return false;
            }

            return normalizedExistingUid === normalizedUid;
        });
    }

    function bindNameInputRestrictions() {
        const fields = [
            DOM.userFirstName,
            DOM.userMiddleName,
            DOM.userLastName,
            DOM.editUserFirstName,
            DOM.editUserMiddleName,
            DOM.editUserLastName
        ].filter(Boolean);
        const chatIdFields = [DOM.userChatId, DOM.editUserChatId].filter(Boolean);

        fields.forEach((field) => {
            field.addEventListener('input', () => {
                const sanitized = sanitizeAlphabeticNamePart(field.value);
                if (field.value !== sanitized) {
                    field.value = sanitized;
                }
            });
        });

        chatIdFields.forEach((field) => {
            field.addEventListener('input', () => {
                const rawValue = String(field.value || '').replace(/\s+/g, '');
                const hasLeadingMinus = rawValue.startsWith('-');
                const digits = rawValue.replace(/\D+/g, '').slice(0, 15);
                const normalized = (hasLeadingMinus ? '-' : '') + digits;
                if (field.value !== normalized) {
                    field.value = normalized;
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

    function updateUsersSummary(users) {
        if (!DOM.usersSummary) {
            return;
        }

        const safeUsers = Array.isArray(users) ? users : [];
        const totalProfiles = safeUsers.length;

        if (totalProfiles <= 0) {
            DOM.usersSummary.textContent = 'Profiles: 0 total';
            return;
        }

        let adminProfiles = 0;
        let userProfiles = 0;

        safeUsers.forEach((user) => {
            const role = normalizeRoleForDisplay(user);
            if (role === 'admin') {
                adminProfiles += 1;
                return;
            }

            userProfiles += 1;
        });

        DOM.usersSummary.textContent = `Profiles: ${totalProfiles} total • Admin ${adminProfiles} • Users ${userProfiles}`;
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

    function setResetUsersBusy(isBusy) {
        resetUsersInFlight = Boolean(isBusy);
        if (!DOM.btnResetUsers) {
            return;
        }

        DOM.btnResetUsers.disabled = resetUsersInFlight;
        DOM.btnResetUsers.textContent = resetUsersInFlight
            ? 'Resetting...'
            : RESET_USERS_IDLE_LABEL;
    }

    async function loadUsers() {
        if (state.usersRequestInFlight) {
            return;
        }

        state.usersRequestInFlight = true;

        try {
            const usersTimeoutMs = Math.max(1200, Number(CONFIG.USERS_TIMEOUT_MS || 4200));
            const defaultRetryCount = Math.max(0, Number(CONFIG.API_RETRY_COUNT || 0));
            const retryBaseDelayMs = Math.max(100, Number(CONFIG.API_RETRY_BASE_DELAY_MS || 170));
            const retryMaxDelayMs = Math.max(retryBaseDelayMs, Number(CONFIG.API_RETRY_MAX_DELAY_MS || 700));

            const data = await apiFetch(CONFIG.API.USERS, {
                timeoutMs: usersTimeoutMs,
                retries: defaultRetryCount,
                retryBaseDelayMs,
                retryMaxDelayMs
            });
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
            if (DOM.usersSummary) {
                DOM.usersSummary.textContent = 'Profiles: unavailable';
            }
        } finally {
            state.usersRequestInFlight = false;
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
            updateUsersSummary([]);
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

            const role = normalizeRoleForDisplay(user);
            const queueCategory = normalizeQueueCategory(user?.queueCategory || user?.roleQueueCategory)
                || (role === 'admin' ? 'ADMIN' : 'USER');
            const queueIndex = parsePositiveInteger(user?.queueIndex || user?.roleQueueIndex);

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

            card.dataset.role = role;
            card.dataset.queueCategory = queueCategory;
            card.dataset.queueIndex = queueIndex > 0 ? String(queueIndex) : '';
            card.classList.toggle('user-card-admin', role === 'admin');

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
        updateUsersSummary(users);
    }

    function handleDeleteUser(uid) {
        const key = String(uid || '').trim().toUpperCase();
        const user = state.usersByUid?.[key];
        const seededAdmin = isSeededAdminUser(user);
        const role = normalizeRoleForDisplay(user || {});
        const canDelete = resolveOptionalBoolean(user?.deletable, role !== 'admin' && !seededAdmin);

        if (!canDelete || seededAdmin || role === 'admin') {
            feedback.showToast('Protected admin profile cannot be deleted', 'info');
            return;
        }

        feedback.showModal(
            'Delete User',
            'Are you sure you want to remove this user? This action cannot be undone.',
            'danger',
            async () => {
                try {
                    const data = await apiFetch(`${CONFIG.API.USERS}?uid=${encodeURIComponent(uid)}`, {
                        method: 'DELETE'
                    });
                    if (data?.success) {
                        feedback.showToast('User deleted successfully', 'success');
                        await loadUsers();
                        if (typeof onLogsUpdated === 'function') {
                            await onLogsUpdated();
                        }
                    } else {
                        feedback.showToast(data?.message || 'Failed to delete user', 'error');
                    }
                } catch (error) {
                    const status = Number(error?.status || 0);
                    const errorCode = String(error?.payload?.errorCode || '').trim();

                    if (status === 403 || errorCode === 'PROTECTED_ADMIN_USER') {
                        feedback.showToast(
                            error?.payload?.message || 'Protected admin user cannot be deleted',
                            'info'
                        );
                        return;
                    }

                    if (status === 404) {
                        feedback.showToast(
                            error?.payload?.message || 'User was not found. Refreshing list...',
                            'info'
                        );
                        await loadUsers();
                        return;
                    }

                    feedback.showToast(
                        error?.payload?.message || error?.message || 'Connection error — could not delete user',
                        'error'
                    );
                }
            }
        );
    }

    function handleEditUserRequest(uid) {
        state.editingUserId = uid;
        state.editUserScanRequested = false;
        const key = String(uid || '').trim().toUpperCase();
        const user = state.usersByUid?.[key];

        if (!user) {
            feedback.showToast('User details could not be loaded. Please refresh and try again.', 'error');
            state.editingUserId = null;
            return;
        }

        const seededAdmin = isSeededAdminUser(user);

        const nameParts = resolveNamePartsFromUser(user || {});

        setEditDialogMode(seededAdmin, user);

        DOM.editUserFirstName.value = nameParts.firstName || '';
        DOM.editUserMiddleName.value = nameParts.middleName || '';
        DOM.editUserLastName.value = nameParts.lastName || '';
        DOM.editUserChatId.value = user?.telegramChatID || '';
        DOM.editUserBackupPin.value = seededAdmin ? '' : (user?.backupPIN || '');
        DOM.editUserRfid.value = seededAdmin
            ? 'Managed by system'
            : (user?.cardUID || user?.uid || uid || '');

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

    function getRfidEnrollFlow(targetInput) {
        return targetInput === DOM.editUserRfid ? 'edit-user' : 'add-user';
    }

    function setScanButtonBusy(targetInput, isBusy) {
        const isEditFlow = targetInput === DOM.editUserRfid;
        const button = isEditFlow ? DOM.btnReplaceCard : DOM.btnScanCard;

        if (!button) {
            return;
        }

        const busy = Boolean(isBusy);
        button.disabled = busy;
        button.classList.toggle('scanning', busy);
        button.setAttribute('aria-busy', busy ? 'true' : 'false');

        if (busy) {
            button.innerHTML = 'Scanning Card...';
            return;
        }

        button.innerHTML = isEditFlow ? REPLACE_CARD_IDLE_LABEL : SCAN_CARD_IDLE_LABEL;
    }

    function stopRfidEnrollmentSession(flowName = '') {
        const activeFlow = String(flowName || state.rfidEnrollFlow || '').trim();
        state.rfidEnrollFlow = '';
        state.rfidEnrollActive = false;

        if (!activeFlow || !CONFIG.API?.RFID_ENROLL_STOP) {
            return;
        }

        const rfidTimeoutMs = Math.max(600, Number(CONFIG.RFID_TIMEOUT_MS || 1500));
        const endpoint = `${CONFIG.API.RFID_ENROLL_STOP}?source=${encodeURIComponent(activeFlow)}`;

        apiFetch(endpoint, {
            method: 'POST',
            timeoutMs: rfidTimeoutMs,
            retries: 0
        }).catch(() => {
            // Enrollment stop is best-effort; server-side window auto-expires.
        });
    }

    async function startRfidEnrollmentSession(targetInput) {
        const flow = getRfidEnrollFlow(targetInput);

        if (!CONFIG.API?.RFID_ENROLL_START) {
            return { ok: false, reason: 'unsupported' };
        }

        const requestedWindowMs = Math.max(8000, Number(CONFIG.RFID_ENROLLMENT_WINDOW_MS || 25000));
        const rfidTimeoutMs = Math.max(700, Number(CONFIG.RFID_TIMEOUT_MS || 1500));
        const endpoint = `${CONFIG.API.RFID_ENROLL_START}?source=${encodeURIComponent(flow)}&ttlMs=${encodeURIComponent(String(requestedWindowMs))}`;

        try {
            const data = await apiFetch(endpoint, {
                method: 'POST',
                timeoutMs: rfidTimeoutMs,
                retries: 0
            });

            state.rfidEnrollFlow = String(data?.enrollmentSource || flow).trim() || flow;
            state.rfidEnrollActive = Boolean(data?.enrollmentActive ?? true);

            if (!state.rfidEnrollActive) {
                state.rfidEnrollFlow = '';
                return { ok: false, reason: 'inactive' };
            }

            return { ok: true, reason: '' };
        } catch (error) {
            state.rfidEnrollFlow = '';
            state.rfidEnrollActive = false;

            if (Number(error?.status) === 404) {
                return { ok: false, reason: 'firmware-update-required' };
            }

            return { ok: false, reason: 'network' };
        }
    }

    function stopRfidPoll({ releaseEnrollment = true } = {}) {
        state.rfidPollSession = Number(state.rfidPollSession || 0) + 1;

        if (state.rfidPollTimer) {
            clearTimeout(state.rfidPollTimer);
            state.rfidPollTimer = null;
        }

        setScanButtonBusy(DOM.userRfid, false);
        setScanButtonBusy(DOM.editUserRfid, false);

        if (releaseEnrollment) {
            stopRfidEnrollmentSession();
        }
    }

    async function startRfidPoll(targetInput) {
        if (targetInput === DOM.editUserRfid && state.editingSeededAdmin) {
            feedback.showToast('Seeded admin RFID is managed by system configuration', 'info');
            return;
        }

        const isEditFlow = targetInput === DOM.editUserRfid;
        if (isEditFlow) {
            state.editUserScanRequested = true;
        } else {
            state.addUserScanRequested = true;
        }

        stopRfidPoll();
        const sessionId = Number(state.rfidPollSession || 0);
        targetInput.value = 'Waiting for card tap...';
        targetInput.classList.add('scanning');
        targetInput.classList.remove('scanned', 'input-error');
        let lastScanTimestamp = 0;

        const editingUid = String(state.editingUserId || '').trim().toUpperCase();

        if (targetInput === DOM.userRfid) {
            setFormError('userRfidError', '');
        } else if (targetInput === DOM.editUserRfid) {
            setFormError('editUserRfidError', '');
        }

        setScanButtonBusy(targetInput, true);
        const enrollmentStart = await startRfidEnrollmentSession(targetInput);

        if (sessionId !== Number(state.rfidPollSession || 0)) {
            return;
        }

        if (!enrollmentStart.ok) {
            if (isEditFlow) {
                state.editUserScanRequested = false;
            } else {
                state.addUserScanRequested = false;
            }

            targetInput.value = '';
            targetInput.classList.remove('scanning', 'scanned');
            targetInput.classList.add('input-error');
            setScanButtonBusy(targetInput, false);

            const errorField = isEditFlow ? 'editUserRfidError' : 'userRfidError';
            const formMessage = enrollmentStart.reason === 'firmware-update-required'
                ? 'RFID enrollment mode is unavailable. Update firmware then try again.'
                : 'Unable to start scanner. Check connection and try again.';

            const toastMessage = enrollmentStart.reason === 'firmware-update-required'
                ? 'RFID scanner mode needs latest firmware. Re-upload firmware and retry.'
                : 'Could not start RFID scan mode. Please try again.';

            setFormError(errorField, formMessage);
            feedback.showToast(toastMessage, 'error');
            return;
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
                const rfidTimeoutMs = Math.max(600, Number(CONFIG.RFID_TIMEOUT_MS || 1500));
                const data = await apiFetch(CONFIG.API.RFID_SCAN, {
                    timeoutMs: rfidTimeoutMs,
                    retries: 0
                });

                if (data?.enrollmentActive === false) {
                    targetInput.value = '';
                    targetInput.classList.remove('scanning', 'scanned');
                    targetInput.classList.add('input-error');
                    const timeoutMessage = 'Scanner timed out. Tap Scan Card to start again.';

                    if (isEditFlow) {
                        state.editUserScanRequested = false;
                    } else {
                        state.addUserScanRequested = false;
                    }

                    if (isEditFlow) {
                        setFormError('editUserRfidError', timeoutMessage);
                    } else {
                        setFormError('userRfidError', timeoutMessage);
                    }

                    feedback.showToast('RFID scan window expired. Tap Scan Card again.', 'info');
                    stopRfidPoll({ releaseEnrollment: false });
                    return { captured: true, error: false };
                }

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
                const rfidTimeoutMs = Math.max(600, Number(CONFIG.RFID_TIMEOUT_MS || 1500));
                const baseline = await apiFetch(CONFIG.API.RFID_SCAN, {
                    timeoutMs: rfidTimeoutMs,
                    retries: 0
                });
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
        stopRfidPoll();
        DOM.addUserForm.reset();
        state.addUserScanRequested = false;
        state.rfidEnrollFlow = '';
        state.rfidEnrollActive = false;
        DOM.userRfid.value = '';
        DOM.userRfid.readOnly = true;
        DOM.userRfid.placeholder = 'Press Scan Card to start RFID capture';
        DOM.userRfid.classList.remove('scanned', 'scanning', 'input-error');
        setFormError('userRfidError', '');
        setAddUserBusy(false);
        DOM.addUserModal.showModal();
    }

    function closeAddUserDialog() {
        state.addUserScanRequested = false;
        stopRfidPoll();
        DOM.addUserModal.close();
    }

    function setEditDialogMode(seededAdmin, user = null) {
        const isSeeded = Boolean(seededAdmin);
        state.editingSeededAdmin = isSeeded;

        if (DOM.editUserModal) {
            DOM.editUserModal.dataset.mode = isSeeded ? 'seeded-admin' : 'regular';
        }

        if (DOM.editUserChatId) {
            DOM.editUserChatId.readOnly = isSeeded;
            if (isSeeded && user) {
                DOM.editUserChatId.value = String(user?.telegramChatID || user?.chat_id || '').trim();
            }
        }

        if (DOM.editUserBackupPin) {
            DOM.editUserBackupPin.disabled = isSeeded;
            DOM.editUserBackupPin.required = !isSeeded;
            if (isSeeded) {
                DOM.editUserBackupPin.value = '';
                DOM.editUserBackupPin.placeholder = 'Managed by system';
            } else {
                DOM.editUserBackupPin.placeholder = '1234';
            }
        }

        if (DOM.editUserRfid) {
            DOM.editUserRfid.readOnly = true;
        }

        if (DOM.btnReplaceCard) {
            DOM.btnReplaceCard.disabled = isSeeded;
            DOM.btnReplaceCard.hidden = isSeeded;
            DOM.btnReplaceCard.setAttribute('aria-hidden', isSeeded ? 'true' : 'false');
        }

        if (isSeeded) {
            setFormError('editUserBackupPinError', '');
            setFormError('editUserRfidError', '');
        }
    }

    function closeEditUserDialog() {
        state.editUserScanRequested = false;
        stopRfidPoll();
        setEditDialogMode(false);
        DOM.editUserModal.close();
        state.editingUserId = null;
    }

    function handleResetUsers() {
        if (resetUsersInFlight) {
            return;
        }

        feedback.showModal(
            'Reset All Users',
            'This will remove all users from device storage. Continue?',
            'danger',
            async () => {
                if (resetUsersInFlight) {
                    return;
                }

                try {
                    setResetUsersBusy(true);

                    const result = await apiFetch(CONFIG.API.USERS_RESET, {
                        method: 'POST',
                        body: JSON.stringify({ confirm: 'RESET_ALL_USERS' })
                    });

                    if (result?.success) {
                        state.lastUsersHash = '';
                        state.renderedUserFingerprints = {};
                        state.renderedUserOrder = [];
                        state.usersByUid = {};

                        if (DOM.usersGrid) {
                            DOM.usersGrid.innerHTML = '<p class="users-empty">Refreshing users…</p>';
                        }

                        await loadUsers();

                        if (typeof onLogsUpdated === 'function') {
                            await onLogsUpdated();
                        }

                        const seededRetained = Math.max(0, Number(result?.seededAdminsRetained || 0));
                        const remainingUsers = Math.max(0, Number(result?.remainingUsers || 0));

                        if (seededRetained > 0) {
                            feedback.showToast(
                                `Users reset complete • ${remainingUsers} active profile(s), ${seededRetained} seeded admin retained`,
                                'success'
                            );
                        } else {
                            feedback.showToast('All users were reset successfully', 'success');
                        }

                        return;
                    }

                    feedback.showToast(result?.message || 'Failed to reset users', 'error');
                } catch (error) {
                    feedback.showToast(
                        error?.payload?.message || error?.message || 'Connection error - could not reset users',
                        'error'
                    );
                } finally {
                    setResetUsersBusy(false);
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

        const nameParts = getNamePartsFromForm('add');
        const nameValidation = validateNameParts(nameParts, 'add');
        const name = nameValidation.fullName;
        const chatId = DOM.userChatId.value.trim();
        const backupPin = DOM.userBackupPin.value.trim();
        const rawRfidInput = String(DOM.userRfid.value || '').trim();
        let rfid = sanitizeUID(rawRfidInput);

        // If polling missed a valid tap, recover from latest scan once before blocking submit.
        // This recovery only runs when the user explicitly started scan mode.
        if (state.addUserScanRequested && (!rfid || isWaitingScanText(rawRfidInput))) {
            try {
                const rfidTimeoutMs = Math.max(600, Number(CONFIG.RFID_TIMEOUT_MS || 1500));
                const scan = await apiFetch(CONFIG.API.RFID_SCAN, {
                    timeoutMs: rfidTimeoutMs,
                    retries: 0
                });
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
        if (!nameValidation.valid || !name) {
            valid = false;
        }
        if (!chatId) {
            setFormError('userChatIdError', 'Telegram Chat ID is required');
            valid = false;
        } else if (!isValidChatId(chatId)) {
            setFormError('userChatIdError', TELEGRAM_CHAT_ID_RULE_MESSAGE);
            valid = false;
        } else if (isDuplicateChatId(chatId)) {
            setFormError('userChatIdError', 'Telegram Chat ID already linked to another user');
            valid = false;
        }
        if (!backupPin || !/^\d{4}$/.test(backupPin)) {
            setFormError('userBackupPinError', 'Backup PIN must be exactly 4 digits');
            valid = false;
        } else if (isDuplicateBackupPin(backupPin)) {
            setFormError('userBackupPinError', 'Backup PIN already assigned to another user');
            valid = false;
        }
        if (!rfid) {
            const rfidRequiredMessage = state.addUserScanRequested
                ? 'RFID tag is required - tap a card'
                : 'Tap Scan Card first, then present the RFID card to enroll';

            setFormError('userRfidError', rfidRequiredMessage);
            DOM.userRfid.classList.add('input-error');
            feedback.showToast('Tap Scan Card first, then scan the RFID card.', 'error');
            valid = false;
        } else if (!isValidUid(rfid)) {
            setFormError('userRfidError', 'RFID UID must be 8-20 hex characters (A-F, 0-9)');
            DOM.userRfid.classList.add('input-error');
            feedback.showToast('Invalid RFID format. Scan again or enter a valid UID.', 'error');
            valid = false;
        } else if (isDuplicateUid(rfid)) {
            setFormError('userRfidError', 'RFID already registered. Scan a different card.');
            DOM.userRfid.classList.add('input-error');
            feedback.showToast('RFID already registered — please use another card', 'error');
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
                    firstName: nameParts.firstName,
                    middleName: nameParts.middleName,
                    lastName: nameParts.lastName,
                    first_name: nameParts.firstName,
                    middle_name: nameParts.middleName,
                    last_name: nameParts.lastName,
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
            if (Number(error?.status) === 400) {
                const field = String(error?.payload?.field || '').trim();
                const message = error?.payload?.message || 'Invalid name format';

                if (field === 'firstName' || field === 'first_name') {
                    setFormError('userFirstNameError', message);
                    feedback.showToast(message, 'error');
                    return;
                }

                if (field === 'middleName' || field === 'middle_name') {
                    setFormError('userMiddleNameError', message);
                    feedback.showToast(message, 'error');
                    return;
                }

                if (field === 'lastName' || field === 'last_name') {
                    setFormError('userLastNameError', message);
                    feedback.showToast(message, 'error');
                    return;
                }

                if (field === 'name') {
                    setFormError('userLastNameError', message);
                    feedback.showToast(message, 'error');
                    return;
                }
            }

            if (Number(error?.status) === 400 && error?.payload?.field === 'name') {
                setFormError('userLastNameError', error?.payload?.message || 'Name can include letters, spaces, apostrophes, dots, and hyphens only');
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

            if (Number(error?.status) === 409
                && (error?.payload?.errorCode === 'BACKUP_PIN_ALREADY_REGISTERED'
                    || error?.payload?.field === 'backupPIN')) {
                setFormError(
                    'userBackupPinError',
                    error?.payload?.message || 'Backup PIN already assigned to another user'
                );
                feedback.showToast('Backup PIN already in use — choose another 4-digit code', 'error');
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

        const editingUid = String(state.editingUserId || '').trim().toUpperCase();
        const editingUser = state.usersByUid?.[editingUid] || null;
        const seededAdmin = state.editingSeededAdmin || isSeededAdminUser(editingUser);

        if (!editingUid) {
            feedback.showToast('No user selected for editing', 'error');
            return;
        }

        const nameParts = getNamePartsFromForm('edit');
        const nameValidation = validateNameParts(nameParts, 'edit');
        const name = nameValidation.fullName;
        const chatId = DOM.editUserChatId.value.trim();
        const backupPin = DOM.editUserBackupPin.value.trim();
        const rfid = sanitizeUID(DOM.editUserRfid.value);

        if (seededAdmin) {
            let seededValid = true;

            if (!nameValidation.valid || !name) {
                seededValid = false;
            }

            if (chatId && !isValidChatId(chatId)) {
                setFormError('editUserChatIdError', TELEGRAM_CHAT_ID_RULE_MESSAGE);
                seededValid = false;
            }

            if (!seededValid) {
                return;
            }

            try {
                const body = {
                    uid: editingUid,
                    name,
                    firstName: nameParts.firstName,
                    middleName: nameParts.middleName,
                    lastName: nameParts.lastName,
                    first_name: nameParts.firstName,
                    middle_name: nameParts.middleName,
                    last_name: nameParts.lastName,
                    telegramChatID: chatId,
                    chat_id: chatId,
                    seededAdminProfile: true
                };

                const data = await apiFetch(CONFIG.API.USERS, {
                    method: 'PUT',
                    body: JSON.stringify(body)
                });

                if (data.success) {
                    feedback.showToast('Default admin profile updated', 'success');
                    closeEditUserDialog();
                    await loadUsers();
                    if (typeof onLogsUpdated === 'function') onLogsUpdated();
                } else {
                    feedback.showToast(data.message || 'Failed to update default admin profile', 'error');
                }
            } catch (error) {
                feedback.showToast(
                    error?.payload?.message || error?.message || 'Connection error — could not update default admin profile',
                    'error'
                );
            }

            return;
        }

        if (DOM.editUserRfid.value !== rfid) {
            DOM.editUserRfid.value = rfid;
        }

        let valid = true;
        if (!nameValidation.valid || !name) {
            valid = false;
        }
        if (chatId && !isValidChatId(chatId)) {
            setFormError('editUserChatIdError', TELEGRAM_CHAT_ID_RULE_MESSAGE);
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
        } else if (isDuplicateBackupPin(backupPin, editingUid)) {
            setFormError('editUserBackupPinError', 'Backup PIN already assigned to another user');
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
        } else if (isDuplicateUid(rfid, editingUid)) {
            setFormError('editUserRfidError', 'RFID already belongs to another user. Scan another card.');
            DOM.editUserRfid.classList.add('input-error');
            valid = false;
        }
        if (!valid) return;

        DOM.editUserRfid.classList.remove('input-error');

        try {
            const body = {
                uid: editingUid,
                name,
                firstName: nameParts.firstName,
                middleName: nameParts.middleName,
                lastName: nameParts.lastName,
                first_name: nameParts.firstName,
                middle_name: nameParts.middleName,
                last_name: nameParts.lastName,
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
            if (Number(error?.status) === 400) {
                const field = String(error?.payload?.field || '').trim();
                const message = error?.payload?.message || 'Invalid name format';

                if (field === 'firstName' || field === 'first_name') {
                    setFormError('editUserFirstNameError', message);
                    feedback.showToast(message, 'error');
                    return;
                }

                if (field === 'middleName' || field === 'middle_name') {
                    setFormError('editUserMiddleNameError', message);
                    feedback.showToast(message, 'error');
                    return;
                }

                if (field === 'lastName' || field === 'last_name') {
                    setFormError('editUserLastNameError', message);
                    feedback.showToast(message, 'error');
                    return;
                }

                if (field === 'name') {
                    setFormError('editUserLastNameError', message);
                    feedback.showToast(message, 'error');
                    return;
                }
            }

            if (Number(error?.status) === 400 && error?.payload?.field === 'name') {
                setFormError('editUserLastNameError', error?.payload?.message || 'Name can include letters, spaces, apostrophes, dots, and hyphens only');
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

            if (Number(error?.status) === 409
                && (error?.payload?.errorCode === 'BACKUP_PIN_ALREADY_REGISTERED'
                    || error?.payload?.field === 'backupPIN')) {
                setFormError(
                    'editUserBackupPinError',
                    error?.payload?.message || 'Backup PIN already assigned to another user'
                );
                feedback.showToast('Backup PIN already in use — choose another 4-digit code', 'error');
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

            if (Number(error?.status) === 403 && error?.payload?.errorCode === 'PROTECTED_ADMIN_USER') {
                feedback.showToast(
                    error?.payload?.message || 'Protected admin profile cannot be edited here',
                    'info'
                );
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
        setResetUsersBusy(false);
        setEditDialogMode(false);

        DOM.btnAddUser.addEventListener('click', openAddUserDialog);
        if (DOM.btnResetUsers) {
            DOM.btnResetUsers.addEventListener('click', handleResetUsers);
        }
        DOM.btnCloseDialog.addEventListener('click', closeAddUserDialog);
        DOM.btnCancelAdd.addEventListener('click', closeAddUserDialog);
        DOM.addUserForm.addEventListener('submit', handleAddUser);
        DOM.btnScanCard.addEventListener('click', () => {
            void startRfidPoll(DOM.userRfid);
        });

        DOM.btnCloseEditDialog.addEventListener('click', closeEditUserDialog);
        DOM.btnCancelEdit.addEventListener('click', closeEditUserDialog);
        DOM.editUserForm.addEventListener('submit', handleEditUser);
        DOM.btnReplaceCard.addEventListener('click', () => {
            void startRfidPoll(DOM.editUserRfid);
        });

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
