/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

// The admin's "MoonlightWeb Host" section: the picture chain of a native session —
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

describe('AdminView — video pipeline (MoonlightWeb Host)', () => {
    let view;

    const select = () => document.querySelector('#select-video-pipeline');
    const mount = () => {
        document.body.innerHTML = `<div>${view._renderNativeHost()}</div>`;
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
        // The chain's menu is not offered, and Advanced keeps the diagnostics.
        expect(view._renderNativeHost()).not.toContain('select-video-pipeline');
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

// PyroWave left this page for each browser's Settings (SettingsPyrowave.test.js):
// the admin offers no codec, even with a D3D12 chain to choose.
describe('AdminView — no video codec (Advanced)', () => {
    it('has no codec menu beside the picture chain', async () => {
        document.body.innerHTML = '<div></div>';
        const view = new AdminView(document.body, () => {});
        BackendClient.getStreamingSettings.mockResolvedValue({
            native_video_pipeline: 'auto',
            native_video_pipeline_supported: true,
            native_video_codec: 'pyrowave',
            native_video_codec_supported: true,
        });
        await view._loadStreamingState();
        const html = view._renderNativeHost();
        expect(html).toContain('select-video-pipeline');
        expect(html).not.toContain('select-video-codec');
        view.destroy();
    });
});
