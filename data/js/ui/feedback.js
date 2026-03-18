export function createFeedback({ DOM, CONFIG }) {
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

    function bindCoreModalEvents() {
        DOM.btnModalCancel.addEventListener('click', hideModal);
        DOM.btnModalConfirm.addEventListener('click', () => {
            if (typeof modalConfirmCallback === 'function') {
                modalConfirmCallback();
            }
            hideModal();
        });

        DOM.modalOverlay.addEventListener('click', (e) => {
            if (e.target === DOM.modalOverlay) hideModal();
        });

        document.addEventListener('keydown', (e) => {
            if (e.key === 'Escape' && DOM.modalOverlay.dataset.visible === 'true') {
                hideModal();
            }
        });
    }

    return {
        showModal,
        hideModal,
        showToast,
        bindCoreModalEvents
    };
}
