export function setFormError(elementId, message) {
    const el = document.getElementById(elementId);
    if (el) el.textContent = message;
}

export function clearFormErrors(DOM) {
    document.querySelectorAll('.form-error').forEach(el => {
        el.textContent = '';
    });

    if (DOM.userRfid) {
        DOM.userRfid.classList.remove('input-error');
    }

    if (DOM.editUserRfid) {
        DOM.editUserRfid.classList.remove('input-error');
    }
}

export function escapeHtml(str) {
    const div = document.createElement('div');
    div.appendChild(document.createTextNode(String(str)));
    return div.innerHTML;
}

export function formatLogTime(timeValue, compact) {
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
