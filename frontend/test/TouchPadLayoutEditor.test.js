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

    const build = (layout = touchPadLayouts(), width = 1180, height = 820) => {
        editor = new TouchPadLayoutEditor(container, {
            layout,
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
});
