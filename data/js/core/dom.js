export function getDOM() {
    return {
        statusBadge: document.getElementById('statusBadge'),
        statusText: document.getElementById('statusText'),

        lockVisual: document.getElementById('lockVisual'),
        lockStatusLabel: document.getElementById('lockStatusLabel'),
        lockStatusSub: document.getElementById('lockStatusSub'),
        btnEmergency: document.getElementById('btnEmergency'),

        pinCode: document.getElementById('pinCode'),
        pinTimer: document.getElementById('pinTimer'),

        logsTableBody: document.getElementById('logsTableBody'),
        logsPagination: document.getElementById('logsPagination'),
        logsPrevPage: document.getElementById('logsPrevPage'),
        logsNextPage: document.getElementById('logsNextPage'),
        logsPageInfo: document.getElementById('logsPageInfo'),
        btnClearLogs: document.getElementById('btnClearLogs'),

        usersGrid: document.getElementById('usersGrid'),
        btnAddUser: document.getElementById('btnAddUser'),

        modalOverlay: document.getElementById('modalOverlay'),
        modalTitle: document.getElementById('modalTitle'),
        modalMessage: document.getElementById('modalMessage'),
        modalIcon: document.getElementById('modalIcon'),
        btnModalCancel: document.getElementById('btnModalCancel'),
        btnModalConfirm: document.getElementById('btnModalConfirm'),

        addUserModal: document.getElementById('addUserModal'),
        addUserForm: document.getElementById('addUserForm'),
        userName: document.getElementById('userName'),
        userPin: document.getElementById('userPin'),
        userRfid: document.getElementById('userRfid'),
        btnSubmitAdd: document.getElementById('btnSubmitAdd'),
        btnScanCard: document.getElementById('btnScanCard'),
        btnCloseDialog: document.getElementById('btnCloseDialog'),
        btnCancelAdd: document.getElementById('btnCancelAdd'),

        editUserModal: document.getElementById('editUserModal'),
        editUserForm: document.getElementById('editUserForm'),
        editUserName: document.getElementById('editUserName'),
        editUserPin: document.getElementById('editUserPin'),
        editUserRfid: document.getElementById('editUserRfid'),
        btnReplaceCard: document.getElementById('btnReplaceCard'),
        btnCloseEditDialog: document.getElementById('btnCloseEditDialog'),
        btnCancelEdit: document.getElementById('btnCancelEdit'),

        toast: document.getElementById('toast'),
        toastMessage: document.getElementById('toastMessage')
    };
}
