/**
 * Moonlight-Web — Application entry point
 *
 * State machine:
 *   LOADING -> HOST_LIST -> APP_LIST -> STREAMING
 *                        \-> PAIRING -/
 */

const MoonlightApp = {
    state: 'loading',
    hosts: [],

    async init() {
        console.log('[MW] Initializing Moonlight-Web...');
        this.checkServerHealth();
        this.showView('welcome');
        console.log('[MW] Ready. State:', this.state);
    },

    async checkServerHealth() {
        try {
            const resp = await fetch('/api/health');
            if (resp.ok) {
                const data = await resp.json();
                console.log('[MW] Server health:', data);
            }
        } catch (err) {
            console.warn('[MW] Server health check failed:', err);
        }
    },

    showView(viewName) {
        document.querySelectorAll('.view').forEach(v => v.classList.remove('active'));
        const view = document.getElementById(`view-${viewName}`);
        if (view) {
            view.classList.add('active');
        }
    },

    transition(newState) {
        console.log(`[MW] State: ${this.state} -> ${newState}`);
        this.state = newState;
    }
};

// Boot
document.addEventListener('DOMContentLoaded', () => MoonlightApp.init());
