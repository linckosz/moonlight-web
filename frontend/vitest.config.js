/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

/**
 * Vitest config — TNR (non-regression) suite for the frontend.
 *
 * Scope: the deterministic, pure-logic modules written for this project. The
 * 70% coverage gate is enforced ONLY over `coverage.include` — the heavy
 * DOM/WebGPU/WebRTC/WebCodecs glue (StreamView, renderers, transports, audio
 * pipeline) is intentionally out of scope: covering it would require massive
 * brittle mocking that would freeze the architecture instead of protecting it.
 *
 * To bring a new module under the gate: add it to `coverage.include` and ship a
 * matching test/<name>.test.js. See docs/testing.md.
 */
import { defineConfig } from 'vitest/config';

export default defineConfig({
    test: {
        environment: 'jsdom',
        include: ['test/**/*.test.js'],
        // Tests assert observable behavior only (inputs → outputs / emitted
        // bytes / sent messages), never private internals, so they survive
        // refactors of the implementation.
        coverage: {
            provider: 'v8',
            reporter: ['text', 'html', 'json-summary', 'cobertura'],
            reportsDirectory: 'coverage',
            // In-scope, deterministic logic — the only files the gate measures.
            include: [
                'js/util/Av1Utils.js',
                'js/util/Mp4Muxer.js',
                'js/util/BrowserDetect.js',
                'js/util/VersionGuard.js',
                'js/stream/JitterController.js',
                'js/stream/GamepadManager.js',
                'js/stream/renderers/createRenderer.js',
                'js/models/App.js',
                'js/models/Host.js',
                'js/i18n/i18n.js',
                'js/api/BackendClient.js',
                'js/ui/Toast.js',
            ],
            thresholds: {
                lines: 70,
                functions: 70,
                statements: 70,
                branches: 60,
            },
        },
    },
});
