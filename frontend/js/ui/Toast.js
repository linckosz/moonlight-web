/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

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
    /** Max simultaneously visible toasts (mobile vs desktop). */
    static _maxVisible() {
        const isMobile = window.matchMedia && window.matchMedia('(max-width: 768px)').matches;
        return isMobile ? 3 : 5;
    }

    /**
     * Remove the oldest toasts so that no more than `max` remain after a new
     * one is appended. The oldest (top-most) toasts slide out first.
     */
    static _enforceLimit(container) {
        const max = this._maxVisible();
        // Only count toasts that are not already exiting.
        const toasts = container.querySelectorAll('.toast:not(.toast-exit)');
        const excess = toasts.length - (max - 1);
        for (let i = 0; i < excess; i++) {
            const toast = toasts[i];
            if (toast && toast.parentNode) {
                toast.classList.add('toast-exit');
                setTimeout(() => {
                    if (toast.parentNode) toast.remove();
                }, 300);
            }
        }
    }

    static show(message, type = 'info') {
        const container = this._ensureContainer();

        // Trim oldest toasts before adding the new one (max 5 PC / 3 mobile).
        this._enforceLimit(container);

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

    static success(message) {
        this.show(message, 'success');
    }
    static error(message) {
        this.show(message, 'error');
    }
    static warning(message) {
        this.show(message, 'warning');
    }
    static info(message) {
        this.show(message, 'info');
    }

    /**
     * Dismiss all visible toasts with a 500ms fade-out animation.
     * Returns a Promise that resolves after the animation completes,
     * so callers can await it before showing a new toast (e.g.:
     *   await Toast.dismissAll();
     *   Toast.success('New message');
     * ).
     *
     * The 500ms delay gives the user time to briefly read the old
     * message before it is replaced.
     */
    static dismissAll() {
        return new Promise((resolve) => {
            const container = document.getElementById('toast-container');
            if (!container) {
                resolve();
                return;
            }
            const toasts = container.querySelectorAll('.toast');
            if (toasts.length === 0) {
                resolve();
                return;
            }
            for (const toast of toasts) {
                if (toast.parentNode) {
                    toast.classList.add('toast-exit');
                    setTimeout(() => {
                        if (toast.parentNode) toast.remove();
                    }, 300);
                }
            }
            // Resolve after the exit animation completes (500ms total)
            setTimeout(resolve, 500);
        });
    }
}
