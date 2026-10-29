/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The touch keyboard bar sits right above the soft keyboard, whichever way the
 * browser makes room for it.
 *
 * Safari on an iPhone keeps the layout viewport and hides its bottom; Chrome on
 * an iPhone (27/10/2026) shrinks the layout viewport with the visible area.
 * The bar was parked by its bottom, from the layout viewport's: there it went
 * to the top of the screen, over Stop, or never showed at all.
 */
import { describe, it, expect, vi, afterEach } from 'vitest';

vi.mock('../js/ui/Toast.js', () => ({ Toast: { warning: () => {} } }));
vi.mock('../js/i18n/i18n.js', () => ({ t: (key) => key, applyTranslations: () => {} }));

const { StreamViewKeyboard } = await import('../js/ui/StreamViewKeyboard.js');
const { StreamViewTouch } = await import('../js/ui/StreamViewTouch.js');

const BAR = 48;

/** The bar and the stream element, on a stand-in for the StreamView. */
function view() {
    const v = Object.create(StreamViewKeyboard.prototype);
    v._rootEl = document.createElement('div');
    v._sendKeyEvent = () => {};
    v._buildKbToolbar();
    Object.defineProperty(v._kbToolbar, 'offsetHeight', { get: () => BAR });
    v.streamEl = document.createElement('div');
    v._positionStatsOverlay = () => {};
    return v;
}

/** The page as the browser shows it: layout height, visible height and offset. */
function viewport(innerHeight, height, offsetTop = 0) {
    vi.spyOn(window, 'innerHeight', 'get').mockReturnValue(innerHeight);
    Object.defineProperty(window, 'visualViewport', {
        configurable: true,
        value: { height, offsetTop },
    });
}

const shown = (v) => v._kbToolbar.classList.contains('visible');

afterEach(() => {
    vi.restoreAllMocks();
    document.documentElement.style.removeProperty('--kbd-inset');
});

describe('touch keyboard bar placement', () => {
    it('sits above a keyboard that hides the bottom of the layout viewport', () => {
        const v = view();
        viewport(800, 800);
        v._handleViewportResize();
        v._kbdVisible = true;
        viewport(800, 450);
        v._handleViewportResize();
        expect(shown(v)).toBe(true);
        expect(v._kbToolbar.style.top).toBe(450 - BAR + 'px');
        expect(v.streamEl.style.height).toBe(450 - BAR + 'px');
        expect(document.documentElement.style.getPropertyValue('--kbd-inset')).toBe(
            350 + BAR + 'px',
        );
    });

    it('sits above a keyboard that shrinks the layout viewport with it', () => {
        const v = view();
        viewport(800, 800);
        v._handleViewportResize();
        v._kbdVisible = true;
        viewport(450, 450);
        v._handleViewportResize();
        expect(shown(v)).toBe(true);
        expect(v._kbToolbar.style.top).toBe(450 - BAR + 'px');
        // Nothing of the layout viewport is hidden: toasts only clear the bar.
        expect(document.documentElement.style.getPropertyValue('--kbd-inset')).toBe(BAR + 'px');
    });

    it('rests the picture a few pixels above the bar, and back when the keyboard closes', () => {
        const v = view();
        v.canvasArea = document.createElement('div');
        Object.defineProperty(v.canvasArea, 'offsetWidth', { get: () => 390 });
        Object.defineProperty(v.canvasArea, 'offsetHeight', { get: () => 400 });
        v._displayEl = () => null;
        v._surfaceIntrinsic = () => [1920, 1080];
        v._invalidateMediaRect = vi.fn();
        viewport(800, 800);
        v._handleViewportResize();
        v._kbdVisible = true;
        viewport(450, 450);
        v._handleViewportResize();
        // 1920×1080 across 390 px is 219 px tall: centred, 90 px of black
        // would sit under it; seated, only 4.
        expect(v.canvasArea.style.top).toBe('86px');
        // Never by a transform: that would make the area the containing block
        // of the fixed trackpad pointer, drawn that much lower (28/10/2026).
        expect(v.canvasArea.style.transform).toBe('');
        expect(v._invalidateMediaRect).toHaveBeenCalled();
        v._kbdVisible = false;
        viewport(800, 800);
        v._handleViewportResize();
        expect(v.canvasArea.style.top).toBe('');
    });

    /** The stand-in with a 2560×1440 canvas and the real zoom, in an area
     *  390 px wide: 266 px tall over the key bar, 640 without it. */
    function zoomedView(zoom, panY) {
        const v = view();
        v.canvasArea = document.createElement('div');
        const areaH = () => (shown(v) ? 266 : 640);
        Object.defineProperty(v.canvasArea, 'offsetWidth', { get: () => 390 });
        Object.defineProperty(v.canvasArea, 'offsetHeight', { get: areaH });
        v.canvasArea.getBoundingClientRect = () => ({ width: 390, height: areaH() });
        v.canvas = { width: 2560, height: 1440, style: {} };
        v._displayEl = () => v.canvas;
        v._videoIsDisplay = () => false;
        v._surfaceIntrinsic = () => [2560, 1440];
        v._invalidateMediaRect = () => {};
        v._placeClientCursor = () => {};
        v._applyZoomTransform = StreamViewTouch.prototype._applyZoomTransform;
        v._zoom = zoom;
        v._panX = 0;
        v._panY = panY;
        return v;
    }

    function openKeyboard(v) {
        viewport(800, 800);
        v._handleViewportResize();
        v._kbdVisible = true;
        viewport(450, 450);
        v._handleViewportResize();
    }

    it('keeps a zoomed picture taller than the area down on the bar (iPhone, 27/10)', () => {
        // 2560×1440 across 390 px is 219 px tall; at ×3.5, 768: 251 px of it
        // above or below the area. Pushed up past its edge before the keyboard.
        const v = zoomedView(3.5, -351);
        openKeyboard(v);
        expect(v._panY).toBeCloseTo(-251.1, 0);
        expect(v.canvasArea.style.top).toBe('');
    });

    it('seats a zoomed picture still shorter than the area at its zoom', () => {
        // ×1.1: 241 px tall in 266, so no pan and 12 px of black under it, 4 left.
        const v = zoomedView(1.1, -40);
        openKeyboard(v);
        expect(Math.abs(v._panY)).toBe(0);
        expect(v.canvasArea.style.top).toBe('8px');
    });

    function closeKeyboard(v) {
        v._kbdVisible = false;
        viewport(800, 800);
        v._handleViewportResize();
    }

    it('puts a zoomed picture back where it was once the keyboard closes (iPhone, 27/10)', () => {
        const v = zoomedView(3.5, -351);
        openKeyboard(v);
        closeKeyboard(v);
        expect(v._panY).toBe(-351);
        expect(v._panX).toBe(0);
        expect(v.canvasArea.style.top).toBe('');
        expect(v.canvas.style.transform).toBe('translate(0px, -351px) scale(3.5)');
    });

    it('puts a picture at zoom 1 back in the middle once the keyboard closes', () => {
        const v = zoomedView(1, 0);
        openKeyboard(v);
        // 219 px tall in 266: 23 px of black under it, 4 left.
        expect(v.canvasArea.style.top).toBe('19px');
        closeKeyboard(v);
        expect(v.canvasArea.style.top).toBe('');
        expect(Math.abs(v._panY)).toBe(0);
    });

    it('keeps a pan made while typing, axis by axis (iPhone, 29/10)', () => {
        const v = zoomedView(3.5, -351);
        v._panX = 200;
        openKeyboard(v);
        // The user drags the desktop sideways to read its other half.
        v._panX = -300;
        closeKeyboard(v);
        expect(v._panX).toBe(-300);
        // Up and down, only the clamp moved it: that part goes back.
        expect(v._panY).toBe(-351);
    });

    it('keeps a pan made while typing through a later resize of the keyboard', () => {
        const v = zoomedView(3.5, -351);
        openKeyboard(v);
        v._panY = -100;
        // The suggestion bar comes up: the keyboard grows, the branch runs again.
        viewport(450, 420);
        v._handleViewportResize();
        closeKeyboard(v);
        expect(v._panY).toBe(-100);
    });

    it('keeps the pan of a zoom changed while typing', () => {
        const v = zoomedView(3.5, -351);
        openKeyboard(v);
        v._zoom = 2;
        v._panY = -50;
        closeKeyboard(v);
        expect(v._panY).toBe(-50);
    });

    it('goes when the keyboard closes, and a new page height is learnt', () => {
        const v = view();
        viewport(800, 800);
        v._handleViewportResize();
        v._kbdVisible = true;
        viewport(450, 450);
        v._handleViewportResize();
        v._kbdVisible = false;
        viewport(390, 390); // turned to landscape, keyboard down
        v._handleViewportResize();
        expect(shown(v)).toBe(false);
    });
});
