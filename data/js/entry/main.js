import { initApp } from '../app/index.js?v=20260418r7';

function activateDeferredStylesheet() {
    const deferredStylesheet = document.getElementById('dashboardStyle');
    if (deferredStylesheet && deferredStylesheet.media !== 'all') {
        deferredStylesheet.media = 'all';
    }
}

function isEditableElement(node) {
    if (!(node instanceof HTMLElement)) {
        return false;
    }

    const tag = node.tagName.toLowerCase();
    return tag === 'input'
        || tag === 'textarea'
        || tag === 'select'
        || node.isContentEditable;
}

function setupMobileViewportUX() {
    let viewportRaf = null;

    const applyViewportMetrics = () => {
        const vv = window.visualViewport;
        const viewportHeight = vv ? vv.height : window.innerHeight;
        const safeHeight = Math.max(320, Math.round(viewportHeight || window.innerHeight || 0));

        document.documentElement.style.setProperty('--app-vh', `${safeHeight}px`);

        if (document.body) {
            const keyboardLikelyOpen = Boolean(vv)
                && ((window.innerHeight - vv.height) > 140);
            document.body.dataset.keyboard = keyboardLikelyOpen ? 'open' : 'closed';
        }
    };

    const scheduleViewportUpdate = () => {
        if (viewportRaf) {
            return;
        }

        viewportRaf = requestAnimationFrame(() => {
            viewportRaf = null;
            applyViewportMetrics();
        });
    };

    applyViewportMetrics();

    window.addEventListener('resize', scheduleViewportUpdate, { passive: true });
    window.addEventListener('orientationchange', () => {
        setTimeout(scheduleViewportUpdate, 140);
    });

    if (window.visualViewport) {
        window.visualViewport.addEventListener('resize', scheduleViewportUpdate);
        window.visualViewport.addEventListener('scroll', scheduleViewportUpdate);
    }

    document.addEventListener('focusin', (event) => {
        const target = event.target;
        if (!isEditableElement(target)) {
            return;
        }

        scheduleViewportUpdate();

        if (!window.matchMedia('(max-width: 1024px)').matches) {
            return;
        }

        setTimeout(() => {
            if (target instanceof HTMLElement) {
                target.scrollIntoView({
                    block: 'center',
                    inline: 'nearest',
                    behavior: 'smooth'
                });
            }
        }, 180);
    });

    document.addEventListener('focusout', () => {
        setTimeout(scheduleViewportUpdate, 120);
    });
}

function markAppReady() {
    if (!document.body) {
        return;
    }

    // Wait for one full paint cycle so startup layout settles first.
    requestAnimationFrame(() => {
        requestAnimationFrame(() => {
            document.body.dataset.appReady = 'true';
        });
    });
}

function installAuthPendingFailsafe() {
    window.setTimeout(() => {
        if (!document.body || document.body.dataset.authenticated !== 'pending') {
            return;
        }

        document.body.dataset.authenticated = 'false';

        const overlay = document.getElementById('authOverlay');
        if (overlay) {
            overlay.dataset.visible = 'true';
            overlay.dataset.mode = 'login';
            overlay.hidden = false;
            overlay.setAttribute('aria-busy', 'false');
        }

        const checking = document.getElementById('authChecking');
        if (checking) {
            checking.textContent = 'Session check timed out. Please login.';
        }

        const loginError = document.getElementById('adminLoginError');
        if (loginError && !String(loginError.textContent || '').trim()) {
            loginError.textContent = 'Session verification timed out. Please login.';
        }
    }, 9000);
}

function boot() {
    activateDeferredStylesheet();
    setupMobileViewportUX();
    installAuthPendingFailsafe();
    initApp();
    markAppReady();
}

if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot, { once: true });
} else {
    boot();
}
