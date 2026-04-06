import { initApp } from '../app/index.js';

const deferredStylesheet = document.getElementById('dashboardStyle');
if (deferredStylesheet && deferredStylesheet.media !== 'all') {
    deferredStylesheet.media = 'all';
}

initApp();
