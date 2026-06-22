import js from '@eslint/js';
import globals from 'globals';

/**
 * Flat ESLint config (ESLint 9) for the Vanilla-JS frontend.
 * No build step: plain ES modules running in the browser, plus Worker /
 * AudioWorklet contexts and a couple of Node-side scripts.
 */
export default [
    {
        // Vendored / minified code is never linted.
        ignores: ['js/vendor/**', '**/*.min.js', 'build/**', 'node_modules/**'],
    },

    js.configs.recommended,

    // Browser ES modules (the bulk of the app).
    {
        files: ['js/**/*.js'],
        languageOptions: {
            ecmaVersion: 2023,
            sourceType: 'module',
            globals: {
                ...globals.browser,
                // WebCodecs / WebGPU / media APIs not always in globals.browser
                VideoDecoder: 'readonly',
                VideoEncoder: 'readonly',
                EncodedVideoChunk: 'readonly',
                AudioDecoder: 'readonly',
                AudioData: 'readonly',
                EncodedAudioChunk: 'readonly',
                GPUBufferUsage: 'readonly',
                GPUTextureUsage: 'readonly',
                GPUShaderStage: 'readonly',
                GPUMapMode: 'readonly',
            },
        },
        rules: {
            'no-unused-vars': [
                'warn',
                {
                    args: 'none',
                    caughtErrors: 'none', // intentional error swallows are common
                    varsIgnorePattern: '^_',
                    ignoreRestSiblings: true,
                },
            ],
            'no-empty': ['warn', { allowEmptyCatch: true }],
            'no-constant-condition': ['error', { checkLoops: false }],
            'prefer-const': 'warn',
            eqeqeq: ['warn', 'smart'],
        },
    },

    // Worker / AudioWorklet context (no DOM, has worker globals).
    {
        files: ['js/audio/audio-processor.js', 'js/stream/VideoDecodeWorker.js'],
        languageOptions: {
            globals: {
                ...globals.worker,
                registerProcessor: 'readonly',
                AudioWorkletProcessor: 'readonly',
                currentTime: 'readonly',
                sampleRate: 'readonly',
            },
        },
    },

    // Node-side helper scripts.
    {
        files: ['scripts/**/*.{js,cjs}'],
        languageOptions: {
            sourceType: 'commonjs',
            globals: { ...globals.node },
        },
    },
];
