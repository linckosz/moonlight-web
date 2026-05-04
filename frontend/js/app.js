/**
 * Moonlight-Web — Application entry point
 *
 * State machine:
 *   LOADING -> HOST_LIST -> APP_LIST -> STREAMING
 *                        \-> PAIRING -/
 */
import { HostListView } from './ui/HostListView.js';
import { BackendClient } from './api/BackendClient.js';

const MoonlightApp = {
    state: 'loading',
    hostListView: null,

    async init() {
        console.log('[MW] Initializing Moonlight-Web...');

        try {
            const health = await BackendClient.get('/api/health');
            console.log('[MW] Server:', health);
        } catch (err) {
            console.warn('[MW] Server health check failed:', err);
        }

        this.transition('host_list');

        const main = document.getElementById('main-content');
        this.hostListView = new HostListView(main);
        this.hostListView.start();
    },

    transition(newState) {
        console.log(`[MW] State: ${this.state} -> ${newState}`);
        this.state = newState;
    }
};

// Boot
document.addEventListener('DOMContentLoaded', () => MoonlightApp.init());
