import { StreamViewKeyboard } from '../ui/StreamViewKeyboard.js';
import { StreamViewTouch } from '../ui/StreamViewTouch.js';
import { StreamViewFullscreen } from '../ui/StreamViewFullscreen.js';

/**
 * `StreamView` is assembled at runtime: the bottom of StreamView.js copies the
 * keyboard, touch and fullscreen prototypes onto `StreamView.prototype`. The
 * mixin methods declare `@this {StreamView}`, so this merge is what closes the
 * loop in both directions — it tells the checker the instance really does carry
 * that surface.
 *
 * State owned by a mixin is declared here too: it is only ever assigned from a
 * mixin method, whose `this` is a StreamView, so there is no assignment for the
 * checker to infer the property from.
 */
declare module '../ui/StreamView.js' {
    interface StreamView extends StreamViewKeyboard, StreamViewTouch, StreamViewFullscreen {
        /**
         * Latched toolbar modifiers on the mobile soft keyboard. The index
         * signature is load-bearing: `_setMod`/`_resetMods` address entries
         * through `Object.keys()`.
         */
        _heldMods: {
            ctrl?: boolean;
            shift?: boolean;
            alt?: boolean;
            meta?: boolean;
            [name: string]: boolean;
        };

        /** Fired once when the first decoded frame reaches the canvas. */
        onFirstFrame: (() => void) | null;
        /** Fired when the user initiates a quit, before teardown starts. */
        onQuitStart: (() => void) | null;
        /** Fired on every individual congestion hint (IDR/PLI/drops). */
        onCongestionSignal: (() => void) | null;
        /** Suppress the one-shot gesture-hint slide on the next activation. */
        _suppressShortcutsSlide: boolean;
    }
}
