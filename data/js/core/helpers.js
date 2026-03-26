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

export function formatLogTime(logOrTimeValue, compact) {
    if (!logOrTimeValue) return '--';

    const isObject = typeof logOrTimeValue === 'object' && logOrTimeValue !== null;
    const rawTime = isObject ? logOrTimeValue.time : logOrTimeValue;
    const rawEpoch = isObject ? Number(logOrTimeValue.epochMs || 0) : 0;

    let date = null;
    if (Number.isFinite(rawEpoch) && rawEpoch > 0) {
        date = new Date(rawEpoch);
    } else {
        const timeText = String(rawTime || '').trim();
        if (timeText) {
            // Handle strict ISO strings and legacy "... UTC" strings.
            const normalized = timeText.endsWith(' UTC')
                ? `${timeText.replace(' UTC', '').replace(' ', 'T')}Z`
                : timeText;
            const parsed = new Date(normalized);
            if (!Number.isNaN(parsed.getTime())) {
                date = parsed;
            }
        }
    }

    if (date) {
        const now = Date.now();
        const deltaMs = now - date.getTime();

        if (Number.isFinite(deltaMs) && deltaMs >= 0) {
            const deltaSec = Math.floor(deltaMs / 1000);
            const deltaMin = Math.floor(deltaSec / 60);
            const deltaHr = Math.floor(deltaMin / 60);

            if (compact) {
                if (deltaSec < 60) return `${deltaSec}s ago`;
                if (deltaMin < 60) return `${deltaMin}m ago`;
                if (deltaHr < 24) return `${deltaHr}h ago`;
            } else {
                if (deltaSec < 60) return `${date.toLocaleTimeString()} (${deltaSec}s ago)`;
                if (deltaMin < 60) return `${date.toLocaleTimeString()} (${deltaMin}m ago)`;
                if (deltaHr < 24) return `${date.toLocaleString()} (${deltaHr}h ago)`;
            }
        }

        if (compact) {
            return date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
        }
        return date.toLocaleString();
    }

    if (!compact) return String(rawTime || '--');

    const matched = String(rawTime || '').match(/(\d{2}:\d{2})(?::\d{2})?/);
    if (matched && matched[1]) {
        return matched[1];
    }

    return String(rawTime || '--').length > 8
        ? String(rawTime).slice(0, 8)
        : String(rawTime || '--');
}
