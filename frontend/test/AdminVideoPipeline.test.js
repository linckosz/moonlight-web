/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

// The admin's "Advanced" section: the picture chain of a native session —
// Auto / D3D11 / D3D12 on Windows, Auto / VA-API / Vulkan on Linux, as the
// server lists them. Shown only where the server says the choice means
// something, saved the moment it changes, and never left showing a value the
// server refused to store.
vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getStreamingSettings: vi.fn(), saveStreamingSettings: vi.fn() },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn() },
}));

import { AdminView } from '../js/ui/AdminView.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

describe('AdminView — video pipeline (Advanced)', () => {
    let view;

    const select = () => document.querySelector('#select-video-pipeline');
    const mount = () => {
        document.body.innerHTML = `<div>${view._renderAdvanced()}</div>`;
        view.container = document.body;
        view.bindEvents();
    };

    beforeEach(() => {
        document.body.innerHTML = '<div></div>';
        view = new AdminView(document.body, () => {});
        BackendClient.getStreamingSettings.mockReset();
        BackendClient.saveStreamingSettings.mockReset();
        Toast.success.mockReset();
        Toast.error.mockReset();
    });

    afterEach(() => {
        view.destroy();
    });

    it('reads the stored choice and whether this machine has one to make', async () => {
        BackendClient.getStreamingSettings.mockResolvedValue({
            native_video_pipeline: 'd3d12',
            native_video_pipeline_supported: true,
        });
        await view._loadStreamingState();
        expect(view._videoPipeline).toBe('d3d12');
        expect(view._videoPipelineSupported).toBe(true);
    });

    it('defaults to auto and hidden when the server says nothing (an older backend)', async () => {
        BackendClient.getStreamingSettings.mockResolvedValue({});
        await view._loadStreamingState();
        expect(view._videoPipeline).toBe('auto');
        expect(view._videoPipelineSupported).toBe(false);
        // The section stays for the diagnostics; the chain's menu does not.
        const html = view._renderAdvanced();
        expect(html).not.toContain('select-video-pipeline');
        expect(html).toContain('btn-download-logs');
        expect(html).toContain('chk-debug-mode');
    });

    it('keeps its defaults when the settings cannot be read', async () => {
        BackendClient.getStreamingSettings.mockRejectedValue(new Error('backend busy'));
        await view._loadStreamingState();
        expect(view._videoPipeline).toBe('auto');
        expect(view._videoPipelineSupported).toBe(false);
    });

    it('offers Auto, D3D11 and D3D12, the stored one selected', () => {
        view._videoPipelineSupported = true;
        view._videoPipeline = 'd3d11';
        mount();
        const values = Array.from(select().options).map((o) => o.value);
        expect(values).toEqual(['auto', 'd3d11', 'd3d12']);
        expect(select().value).toBe('d3d11');
    });

    it("offers a Linux host's chains when the server lists them, with Linux's hint", async () => {
        BackendClient.getStreamingSettings.mockResolvedValue({
            native_video_pipeline: 'vulkan',
            native_video_pipeline_supported: true,
            native_video_pipeline_options: ['auto', 'vaapi', 'vulkan'],
        });
        await view._loadStreamingState();
        mount();
        const values = Array.from(select().options).map((o) => o.value);
        expect(values).toEqual(['auto', 'vaapi', 'vulkan']);
        expect(select().value).toBe('vulkan');
        expect(document.body.innerHTML).toContain('admin.videoPipelineHintLinux');
    });

    it('shows Auto for a stored value this OS does not list (it runs as Auto)', async () => {
        BackendClient.getStreamingSettings.mockResolvedValue({
            native_video_pipeline: 'd3d12',
            native_video_pipeline_supported: true,
            native_video_pipeline_options: ['auto', 'vaapi', 'vulkan'],
        });
        await view._loadStreamingState();
        mount();
        expect(select().value).toBe('auto');
    });

    it('saves a change at once and says so', async () => {
        BackendClient.saveStreamingSettings.mockResolvedValue({ native_video_pipeline: 'd3d12' });
        view._videoPipelineSupported = true;
        mount();
        select().value = 'd3d12';
        select().dispatchEvent(new Event('change'));
        await vi.waitFor(() => expect(Toast.success).toHaveBeenCalled());
        expect(BackendClient.saveStreamingSettings).toHaveBeenCalledWith({
            native_video_pipeline: 'd3d12',
        });
        expect(view._videoPipeline).toBe('d3d12');
    });

    it('puts the stored value back when the server refuses the change', async () => {
        BackendClient.saveStreamingSettings.mockRejectedValue(new Error('400'));
        view._videoPipelineSupported = true;
        view._videoPipeline = 'auto';
        mount();
        select().value = 'd3d12';
        select().dispatchEvent(new Event('change'));
        await vi.waitFor(() => expect(Toast.error).toHaveBeenCalled());
        expect(view._videoPipeline).toBe('auto');
        expect(select().value).toBe('auto');
        expect(Toast.success).not.toHaveBeenCalled();
    });
});

// The codec of a native session beside it: Auto, or PyroWave for a wired LAN,
// offered only where the server says it can run (Windows, the D3D12 route).
describe('AdminView — video codec (Advanced)', () => {
    let view;

    const select = () => document.querySelector('#select-video-codec');
    const mount = () => {
        document.body.innerHTML = `<div>${view._renderAdvanced()}</div>`;
        view.container = document.body;
        view.bindEvents();
    };

    beforeEach(() => {
        document.body.innerHTML = '<div></div>';
        view = new AdminView(document.body, () => {});
        BackendClient.getStreamingSettings.mockReset();
        BackendClient.saveStreamingSettings.mockReset();
        Toast.success.mockReset();
        Toast.error.mockReset();
    });

    afterEach(() => {
        view.destroy();
    });

    it('is hidden unless the server offers it', async () => {
        BackendClient.getStreamingSettings.mockResolvedValue({ native_video_codec: 'pyrowave' });
        await view._loadStreamingState();
        expect(view._videoCodecSupported).toBe(false);
        expect(view._renderAdvanced()).not.toContain('select-video-codec');
    });

    it('offers Auto and PyroWave for a wired LAN, the stored one selected', async () => {
        BackendClient.getStreamingSettings.mockResolvedValue({
            native_video_codec: 'pyrowave',
            native_video_codec_supported: true,
        });
        await view._loadStreamingState();
        mount();
        const values = Array.from(select().options).map((o) => o.value);
        expect(values).toEqual(['auto', 'pyrowave']);
        expect(select().value).toBe('pyrowave');
        expect(select().options[1].textContent).toBe('admin.videoCodecPyrowave');
        expect(document.body.innerHTML).toContain('admin.videoCodecHint');
    });

    it('saves a change at once, and puts the stored value back on a refusal', async () => {
        BackendClient.saveStreamingSettings.mockResolvedValueOnce({
            native_video_codec: 'pyrowave',
        });
        view._videoCodecSupported = true;
        mount();
        select().value = 'pyrowave';
        select().dispatchEvent(new Event('change'));
        await vi.waitFor(() => expect(Toast.success).toHaveBeenCalled());
        expect(BackendClient.saveStreamingSettings).toHaveBeenCalledWith({
            native_video_codec: 'pyrowave',
        });
        expect(view._videoCodec).toBe('pyrowave');

        BackendClient.saveStreamingSettings.mockRejectedValueOnce(new Error('400'));
        select().value = 'auto';
        select().dispatchEvent(new Event('change'));
        await vi.waitFor(() => expect(Toast.error).toHaveBeenCalled());
        expect(view._videoCodec).toBe('pyrowave');
        expect(select().value).toBe('pyrowave');
    });
});
