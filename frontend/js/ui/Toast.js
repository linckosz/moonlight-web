/**
 * Moonlight-Web — Toast notification utility
 */
export class Toast {
    static _ensureContainer() {
        let container = document.getElementById('toast-container');
        if (!container) {
            container = document.createElement('div');
            container.id = 'toast-container';
            document.body.appendChild(container);
        }
        return container;
    }

    /**
     * Show a toast notification.
     * @param {string} message
     * @param {'success'|'error'|'info'} type
     */
    static show(message, type = 'info') {
        const container = this._ensureContainer();

        const toast = document.createElement('div');
        toast.className = `toast toast-${type}`;
        toast.textContent = message;

        container.appendChild(toast);

        // Auto-remove after animation
        const remove = () => {
            if (toast.parentNode) {
                toast.classList.add('toast-exit');
                setTimeout(() => {
                    if (toast.parentNode) toast.remove();
                }, 300);
            }
        };

        // Dismiss on click
        toast.addEventListener('click', remove);

        // Auto-fade after 4s
        setTimeout(remove, 4000);
    }

    static success(message) { this.show(message, 'success'); }
    static error(message)   { this.show(message, 'error'); }
    static warning(message) { this.show(message, 'warning'); }
    static info(message)    { this.show(message, 'info'); }
}
