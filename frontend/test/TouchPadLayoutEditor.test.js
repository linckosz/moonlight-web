/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The layout editor: the controller drawn as a stream draws it, handles over
 * it, and the places a drag writes as shares of the stage.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/i18n/i18n.js', () => ({
    t: (key) => 'text:' + key,
}));

import { TouchPadLayoutEditor } from '../js/ui/TouchPadLayoutEditor.js';
import { touchPadLayouts } from '../js/stream/TouchPadState.js';

describe('TouchPadLayoutEditor', () => {
    let container;
    let editor;
    let onChange;
    let stage;
    /**
     * The stage, as a screen: jsdom measures everything at zero, so the editor
     * is given the shape it would have on a tablet.
     */
    const size = (width, height, left = 0, top = 0) => {
        stage = container.querySelector('.tp-layout-editor-stage');
        const rect = () => ({
            left,
            top,
            width,
            height,
            right: left + width,
            bottom: top + height,
        });
        stage.getBoundingClientRect = rect;
        const layer = stage.querySelector('.touch-pad');
        if (layer) layer.getBoundingClientRect = rect;
        if (editor._view) editor._view.relayout();
        editor._refresh();
        // What a handle measures, from the place it was given: jsdom lays
        // nothing out, and a grab reads the element it is on.
        for (const el of stage.querySelectorAll('.tp-layout-editor-handle')) {
            el.getBoundingClientRect = () => {
                const x = left + parseFloat(el.style.left);
                const y = top + parseFloat(el.style.top);
                const w = parseFloat(el.style.width);
                const h = parseFloat(el.style.height);
                return { left: x, top: y, width: w, height: h, right: x + w, bottom: y + h };
            };
        }
    };
    const handle = (id) =>
        container.querySelector(`.tp-layout-editor-handle[data-control="${id}"]`);
    const centreOf = (el) => {
        const style = el.style;
        return [
            parseFloat(style.left) + parseFloat(style.width) / 2,
            parseFloat(style.top) + parseFloat(style.height) / 2,
        ];
    };
    const pointer = (clientX, clientY, pointerId = 1) => ({
        button: 0,
        pointerId,
        clientX,
        clientY,
        preventDefault: () => {},
        stopPropagation: () => {},
    });

    /** Grab a handle and take it to the given place, as a finger would. */
    const dragTo = (id, clientX, clientY) => {
        const el = handle(id);
        const [cx, cy] = centreOf(el);
        editor._grab(pointer(cx, cy), el, id);
        editor._dragTo(pointer(clientX, clientY));
        editor._drop();
    };

    const build = (layout = touchPadLayouts(), width = 1180, height = 820, look = {}) => {
        editor = new TouchPadLayoutEditor(container, {
            layout,
            ...look,
            onChange,
            onClosed: () => {},
        });
        editor.open();
        size(width, height);
        return editor;
    };

    beforeEach(() => {
        container = document.createElement('div');
        document.body.appendChild(container);
        onChange = vi.fn();
    });

    afterEach(() => {
        if (editor) editor.close();
        editor = null;
        container.remove();
    });

    it('shows the controller and a handle over every control it may move', () => {
        build();
        expect(container.querySelector('.touch-pad')).not.toBeNull();
        const ids = [
            'lt',
            'lb',
            'rt',
            'rb',
            'x',
            'y',
            'a',
            'b',
            'dpad',
            'l3',
            'r3',
            'view',
            'menu',
            'stick-left',
            'stick-right',
        ];
        for (const id of ids) expect(handle(id)).not.toBeNull();
        // The button that hides the controller is not one of them: it follows
        // the two pills.
        expect(container.querySelectorAll('.tp-layout-editor-handle').length).toBe(ids.length);
    });

    it('lays its handles over what the layer drew', () => {
        build();
        const a = handle('a');
        const drawn = container.querySelector('[data-control="a"]');
        expect(a.style.left).toBe(drawn.style.left);
        expect(a.style.top).toBe(drawn.style.top);
        expect(a.style.width).toBe(drawn.style.width);
    });

    it('is deaf to the fingers it arranges', () => {
        build();
        expect(container.querySelector('.touch-pad').classList.contains('is-editing')).toBe(true);
    });

    it("names the controls as printed, or in the viewer's language", () => {
        build();
        expect(handle('a').getAttribute('aria-label')).toBe('A');
        expect(handle('lb').getAttribute('aria-label')).toBe('LB');
        expect(handle('dpad').getAttribute('aria-label')).toBe('text:settings.touchPadControlDpad');
        expect(handle('stick-right').getAttribute('aria-label')).toBe(
            'text:settings.touchPadControlStickRight',
        );
    });

    it('writes a drag as a share of the stage, and follows the finger', () => {
        build();
        dragTo('a', 590, 410);
        const a = handle('a');
        expect(parseFloat(a.style.left) + parseFloat(a.style.width) / 2).toBeCloseTo(590, 0);
        expect(parseFloat(a.style.top) + parseFloat(a.style.height) / 2).toBeCloseTo(410, 0);
        const saved = editor._draft.landscape.a;
        expect(saved.x).toBeCloseTo(0.5, 2);
        expect(saved.y).toBeCloseTo(0.5, 2);
    });

    it('keeps what it wrote when Done is pressed, and hands it over', () => {
        build();
        dragTo('a', 590, 410);
        container.querySelector('[data-act="done"]').click();
        expect(onChange).toHaveBeenCalledTimes(1);
        const layout = onChange.mock.calls[0][0];
        expect(layout.landscape.a.x).toBeCloseTo(0.5, 2);
        expect(editor.isOpen()).toBe(false);
    });

    it('saves nothing on Cancel, and nothing on Escape', () => {
        build();
        dragTo('a', 590, 410);
        container.querySelector('[data-act="cancel"]').click();
        expect(onChange).not.toHaveBeenCalled();
        expect(editor.isOpen()).toBe(false);

        build();
        dragTo('a', 590, 410);
        document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
        expect(onChange).not.toHaveBeenCalled();
        expect(editor.isOpen()).toBe(false);
    });

    it('gives the controls back on Reset, keeping the preset', () => {
        build(touchPadLayouts({ preset: 'left', landscape: { a: { x: 0.2, y: 0.2, s: 1.2 } } }));
        expect(handle('a').style.width).not.toBe('');
        container.querySelector('[data-act="reset"]').click();
        expect(editor._draft.landscape).toEqual({});
        expect(editor._draft.preset).toBe('left');
        // The mirror is still on: the face buttons are on the left.
        expect(
            parseFloat(handle('a').style.left) + parseFloat(handle('a').style.width) / 2,
        ).toBeLessThan(1180 / 2);
    });

    it('sizes the selected control without moving it', () => {
        build();
        const el = handle('a');
        const [cx, cy] = centreOf(el);
        editor._grab(pointer(cx, cy), el, 'a');
        editor._drop();
        const slider = container.querySelector('.tp-layout-editor-size input');
        expect(slider.disabled).toBe(false);
        slider.value = '1.2';
        slider.dispatchEvent(new Event('input'));
        const saved = editor._draft.landscape.a;
        expect(saved.s).toBeCloseTo(1.2, 5);
        expect(saved.x).toBeCloseTo(cx / 1180, 3);
        expect(saved.y).toBeCloseTo(cy / 820, 3);
        expect(parseFloat(handle('a').style.width)).toBeGreaterThan(
            parseFloat(el.style.width) * 0.99,
        );
    });

    it('has a dead size slider until a control is picked', () => {
        build();
        expect(container.querySelector('.tp-layout-editor-size input').disabled).toBe(true);
    });

    it('keeps portrait and landscape apart', () => {
        build();
        dragTo('a', 590, 410);
        expect(editor._draft.landscape.a).toBeDefined();
        size(820, 1180);
        expect(editor._draft.portrait.a).toBeUndefined();
        dragTo('a', 410, 900);
        expect(editor._draft.portrait.a.y).toBeCloseTo(900 / 1180, 2);
        expect(editor._draft.landscape.a.y).toBeCloseTo(0.5, 2);
    });

    it('names the orientation the stage is in', () => {
        build();
        const label = () => container.querySelector('.tp-layout-editor-orientation').textContent;
        expect(label()).toBe('text:settings.touchPadEditorLandscape');
        size(820, 1180);
        expect(label()).toBe('text:settings.touchPadEditorPortrait');
    });

    it('takes the ring of a stick, never its zone', () => {
        build();
        const stick = handle('stick-right');
        expect(stick.classList.contains('is-stick')).toBe(true);
        dragTo('stick-right', 300, 400);
        const saved = editor._draft.landscape['stick-right'];
        expect(saved.x).toBeLessThan(0.5);
        // Clamped inside its own side: the right zone starts at 58 % of the width.
        expect(parseFloat(stick.style.left)).toBeGreaterThan(1180 * 0.58 - 1);
    });

    it('leaves the page as it found it', () => {
        build();
        expect(container.querySelector('.tp-layout-editor')).not.toBeNull();
        editor.close();
        expect(container.querySelector('.tp-layout-editor')).toBeNull();
        expect(editor.isOpen()).toBe(false);
    });

    describe('its floating bar', () => {
        const bar = () => container.querySelector('.tp-layout-editor-bar');
        const input = (cls) => container.querySelector(`.${cls} input`);
        const slide = (cls, value) => {
            input(cls).value = String(value);
            input(cls).dispatchEvent(new Event('input'));
        };
        const centreX = (id) =>
            parseFloat(handle(id).style.left) + parseFloat(handle(id).style.width) / 2;

        it('opens unfolded, folds to a pill and unfolds again', () => {
            build();
            expect(bar().classList.contains('is-folded')).toBe(false);
            container.querySelector('[data-act="fold"]').click();
            expect(bar().classList.contains('is-folded')).toBe(true);
            container.querySelector('[data-act="unfold"]').click();
            expect(bar().classList.contains('is-folded')).toBe(false);
        });

        it('keeps Done at hand while folded', () => {
            build();
            container.querySelector('[data-act="fold"]').click();
            container.querySelector('.tp-bar-done-folded').click();
            expect(onChange).toHaveBeenCalledTimes(1);
        });

        it('tries a preset at once, dropping the places it replaces, and keeps it only on Done', () => {
            build(touchPadLayouts({ landscape: { a: { x: 0.9, y: 0.5, s: 1 } } }));
            container.querySelector('[data-preset="left"]').click();
            expect(editor._draft).toEqual({ preset: 'left', landscape: {}, portrait: {} });
            expect(centreX('a')).toBeLessThan(1180 / 2);
            expect(
                container.querySelector('[data-preset="left"]').getAttribute('aria-pressed'),
            ).toBe('true');
            container.querySelector('[data-act="cancel"]').click();
            expect(onChange).not.toHaveBeenCalled();
        });

        it('starts from the size and opacity saved, and hands back what was tried', () => {
            build(touchPadLayouts(), 1180, 820, { scale: 0.9, opacity: 0.6 });
            expect(input('tp-layout-editor-scale').value).toBe('90');
            expect(input('tp-layout-editor-opacity').value).toBe('60');
            const before = parseFloat(handle('a').style.width);
            slide('tp-layout-editor-scale', 120);
            expect(parseFloat(handle('a').style.width)).toBeGreaterThan(before);
            slide('tp-layout-editor-opacity', 40);
            const layer = container.querySelector('.touch-pad');
            expect(layer.style.getPropertyValue('--touch-pad-opacity')).toBe('0.4');
            container.querySelector('[data-act="done"]').click();
            expect(onChange.mock.calls[0][1]).toEqual({ scale: 1.2, opacity: 0.4, style: 'xbox' });
        });

        it('renames the buttons after the pad picked, and hands the style back on Done', () => {
            build(touchPadLayouts(), 1180, 820, { style: 'ps' });
            const select = container.querySelector('.tp-layout-editor-style select');
            expect([...select.options].map((o) => o.value)).toEqual(['xbox', 'ps', 'switch']);
            expect(select.value).toBe('ps');
            const drawn = () =>
                container.querySelector('.touch-pad [data-control="a"]').textContent;
            expect(drawn()).toBe('✕');
            expect(handle('a').getAttribute('aria-label')).toBe('✕');
            select.value = 'switch';
            select.dispatchEvent(new Event('change'));
            expect(drawn()).toBe('B');
            expect(handle('lt').getAttribute('aria-label')).toBe('ZL');
            // Only the names: no control moved.
            expect(editor._draft.landscape).toEqual({});
            container.querySelector('[data-act="done"]').click();
            expect(onChange.mock.calls[0][1].style).toBe('switch');
        });

        it('falls back to the Xbox names for a style it does not know', () => {
            build(touchPadLayouts(), 1180, 820, { style: 'gamecube' });
            expect(container.querySelector('.tp-layout-editor-style select').value).toBe('xbox');
            expect(handle('a').getAttribute('aria-label')).toBe('A');
        });

        it('shows three backgrounds, a preview that is never handed over', () => {
            build();
            const stageEl = container.querySelector('.tp-layout-editor-stage');
            expect(stageEl.classList.contains('tp-bg-default')).toBe(true);
            const ids = [...container.querySelectorAll('[data-bg]')].map((b) => b.dataset.bg);
            expect(ids).toEqual(['default', 'night', 'day']);
            container.querySelector('[data-bg="day"]').click();
            expect(stageEl.classList.contains('tp-bg-day')).toBe(true);
            expect(stageEl.classList.contains('tp-bg-default')).toBe(false);
            container.querySelector('[data-act="done"]').click();
            expect(Object.keys(onChange.mock.calls[0][1]).sort()).toEqual([
                'opacity',
                'scale',
                'style',
            ]);
            expect(onChange.mock.calls[0][0]).not.toHaveProperty('background');
        });

        it('says how it is used until a control is touched, then which one', () => {
            build();
            const text = () => container.querySelector('.tp-layout-editor-size-text').textContent;
            expect(text()).toBe('text:settings.touchPadEditorHint');
            const el = handle('a');
            const [cx, cy] = centreOf(el);
            editor._grab(pointer(cx, cy), el, 'a');
            editor._drop();
            expect(text()).toBe('text:settings.touchPadEditorControlSize');
        });

        it('goes where its grip takes it, never off the screen', () => {
            build();
            const room = { left: 0, top: 0, width: 1180, height: 820, right: 1180, bottom: 820 };
            editor._el.getBoundingClientRect = () => room;
            bar().getBoundingClientRect = () => {
                const x = parseFloat(bar().style.left) || 0;
                const y = parseFloat(bar().style.top) || 0;
                return {
                    left: x,
                    top: y,
                    width: 380,
                    height: 200,
                    right: x + 380,
                    bottom: y + 200,
                };
            };
            editor._placeBar();
            // In the middle the first time.
            expect(parseFloat(bar().style.left)).toBe(400);
            expect(parseFloat(bar().style.top)).toBe(310);
            const grip = container.querySelector('.tp-bar-grip');
            editor._grabBar({ ...pointer(410, 320), currentTarget: grip });
            editor._barDrag.onMove(pointer(60, 70));
            expect(parseFloat(bar().style.left)).toBe(50);
            expect(parseFloat(bar().style.top)).toBe(60);
            editor._barDrag.onMove(pointer(5000, 5000));
            expect(parseFloat(bar().style.left)).toBe(1180 - 380);
            expect(parseFloat(bar().style.top)).toBe(820 - 200);
        });
    });
});
