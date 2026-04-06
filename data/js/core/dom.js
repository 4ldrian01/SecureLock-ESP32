export function getDOM() {
    return {
        statusBadge: document.getElementById('statusBadge'),
        statusText: document.getElementById('statusText'),
        btnLogout: document.getElementById('btnLogout'),

        authOverlay: document.getElementById('authOverlay'),
        adminLoginForm: document.getElementById('adminLoginForm'),
        adminLoginUsername: document.getElementById('adminLoginUsername'),
        adminLoginPassword: document.getElementById('adminLoginPassword'),
        adminLoginError: document.getElementById('adminLoginError'),
        adminLockoutMessage: document.getElementById('adminLockoutMessage'),
        btnAdminLogin: document.getElementById('btnAdminLogin'),

        lockVisual: document.getElementById('lockVisual'),
        lockStatusLabel: document.getElementById('lockStatusLabel'),
        lockStatusSub: document.getElementById('lockStatusSub'),
        diagStatus: document.getElementById('diagStatus'),
        btnEmergency: document.getElementById('btnEmergency'),

        pinCode: document.getElementById('pinCode'),
        pinTimer: document.getElementById('pinTimer'),

        logsSection: document.querySelector('.logs-section'),
        usersSection: document.querySelector('.users-section'),

        logsTableBody: document.getElementById('logsTableBody'),
        logsPagination: document.getElementById('logsPagination'),
        logsPrevPage: document.getElementById('logsPrevPage'),
        logsNextPage: document.getElementById('logsNextPage'),
        logsPageInfo: document.getElementById('logsPageInfo'),
        btnClearLogs: document.getElementById('btnClearLogs'),

        usersGrid: document.getElementById('usersGrid'),
        btnAddUser: document.getElementById('btnAddUser'),
        btnResetUsers: document.getElementById('btnResetUsers'),

        modalOverlay: document.getElementById('modalOverlay'),
        modalTitle: document.getElementById('modalTitle'),
        modalMessage: document.getElementById('modalMessage'),
        modalIcon: document.getElementById('modalIcon'),
        btnModalCancel: document.getElementById('btnModalCancel'),
        btnModalConfirm: document.getElementById('btnModalConfirm'),

        addUserModal: document.getElementById('addUserModal'),
        addUserForm: document.getElementById('addUserForm'),
        userName: document.getElementById('userName'),
        userChatId: document.getElementById('userChatId'),
        userBackupPin: document.getElementById('userBackupPin'),
        userRfid: document.getElementById('userRfid'),
        btnSubmitAdd: document.getElementById('btnSubmitAdd'),
        btnScanCard: document.getElementById('btnScanCard'),
        btnCloseDialog: document.getElementById('btnCloseDialog'),
        btnCancelAdd: document.getElementById('btnCancelAdd'),

        editUserModal: document.getElementById('editUserModal'),
        editUserForm: document.getElementById('editUserForm'),
        editUserName: document.getElementById('editUserName'),
        editUserChatId: document.getElementById('editUserChatId'),
        editUserBackupPin: document.getElementById('editUserBackupPin'),
        editUserRfid: document.getElementById('editUserRfid'),
        btnReplaceCard: document.getElementById('btnReplaceCard'),
        btnCloseEditDialog: document.getElementById('btnCloseEditDialog'),
        btnCancelEdit: document.getElementById('btnCancelEdit'),

        toast: document.getElementById('toast'),
        toastMessage: document.getElementById('toastMessage')
    };
}
