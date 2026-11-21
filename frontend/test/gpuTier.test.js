/**
 * BrowserDetect — the tier of a GPU by its name (gpuTierOf), and with it the
 * mode of mwsr a device takes (mwsrQualityWanted, pickAutoEnhancer): quality,
 * the kernel and a second pass, on a computer with a dedicated graphics card;
 * performance, the kernel alone, everywhere else and whenever the name says
 * nothing. The names are the ones browsers give (WebGL's unmasked renderer).
 */
import { describe, it, expect, vi, afterEach } from 'vitest';
import { gpuTierOf } from '../js/util/BrowserDetect.js';

const angle = (vendor, name) =>
    `ANGLE (${vendor}, ${name} (0x00002D04) Direct3D11 vs_5_0 ps_5_0, D3D11)`;

const DEDICATED = [
    angle('NVIDIA', 'NVIDIA GeForce RTX 5060 Ti'),
    angle('NVIDIA', 'NVIDIA GeForce RTX 3060 Laptop GPU'),
    angle('NVIDIA', 'NVIDIA GeForce GTX 1650'),
    angle('NVIDIA', 'NVIDIA GeForce GTX 980'), // the name Firefox rounds many cards to
    angle('NVIDIA', 'NVIDIA RTX A2000'),
    angle('NVIDIA', 'NVIDIA Quadro P2000'),
    angle('NVIDIA', 'NVIDIA TITAN Xp'),
    'NVIDIA GeForce RTX 4070/PCIe/SSE2',
    angle('AMD', 'AMD Radeon RX 7800 XT'),
    angle('AMD', 'AMD Radeon RX 6600M'),
    angle('AMD', 'Radeon RX 580 Series'),
    angle('AMD', 'Radeon RX550/550 Series'),
    angle('AMD', 'AMD Radeon RX 9070 XT'),
    angle('AMD', 'Radeon (TM) Pro WX 7100 Graphics'),
    angle('AMD', 'AMD Radeon R9 200 Series'),
    angle('AMD', 'Radeon RX Vega 64'),
    'ANGLE (AMD, ANGLE Metal Renderer: AMD Radeon Pro 5500M, Unspecified Version)',
    'AMD Radeon RX 7800 XT (radeonsi, navi32, LLVM 17.0.6, DRM 3.57, 6.8.0)',
    angle('Intel', 'Intel(R) Arc(TM) A770 Graphics'),
    angle('Intel', 'Intel(R) Arc(TM) A380 Graphics'),
    angle('Intel', 'Intel(R) Arc(TM) A350M Graphics'),
    angle('Intel', 'Intel(R) Arc(TM) B580 Graphics'),
    angle('Intel', 'Intel(R) Arc(TM) Pro A60 Graphics'),
    // Apple's M Pro, M Max and M Ultra: integrated, at a card's level
    'ANGLE (Apple, ANGLE Metal Renderer: Apple M1 Pro, Unspecified Version)',
    'ANGLE (Apple, ANGLE Metal Renderer: Apple M3 Max, Unspecified Version)',
    'ANGLE (Apple, ANGLE Metal Renderer: Apple M4 Pro, Unspecified Version)',
    'ANGLE (Apple, ANGLE Metal Renderer: Apple M2 Ultra, Unspecified Version)',
];

const LIGHT = [
    // integrated, Intel
    angle('Intel', 'Intel(R) UHD Graphics 770'),
    angle('Intel', 'Intel(R) UHD Graphics'),
    angle('Intel', 'Intel(R) HD Graphics 620'),
    angle('Intel', 'Intel(R) Iris(R) Xe Graphics'),
    angle('Intel', 'Intel(R) Iris(R) Xe MAX Graphics'),
    // a Core Ultra's own GPU carries the Arc name, without a letter before the number
    angle('Intel', 'Intel(R) Arc(TM) Graphics'),
    angle('Intel', 'Intel(R) Arc(TM) 140V GPU (16GB)'),
    angle('Intel', 'Intel(R) Arc(TM) 130T GPU'),
    'Mesa Intel(R) UHD Graphics 770 (ADL-S GT1)',
    // integrated, AMD
    angle('AMD', 'AMD Radeon(TM) Graphics'),
    angle('AMD', 'AMD Radeon 780M Graphics'),
    angle('AMD', 'AMD Radeon(TM) 890M Graphics'),
    angle('AMD', 'AMD Radeon(TM) 8060S Graphics'),
    angle('AMD', 'AMD Radeon(TM) Vega 8 Graphics'),
    angle('AMD', 'AMD Radeon(TM) RX Vega 11 Graphics'), // an APU, despite the RX
    angle('AMD', 'AMD Radeon(TM) R7 Graphics'),
    'AMD Radeon Graphics (radeonsi, phoenix, LLVM 17.0.6, DRM 3.57, 6.8.0)',
    // the small dedicated cards, at an integrated GPU's level
    angle('NVIDIA', 'NVIDIA GeForce MX450'),
    angle('NVIDIA', 'NVIDIA GeForce GT 1030'),
    angle('NVIDIA', 'NVIDIA GeForce 940MX'),
    angle('AMD', 'AMD Radeon R5 M430'),
    // Apple, and the phones' and tablets' GPUs
    'ANGLE (Apple, ANGLE Metal Renderer: Apple M3, Unspecified Version)',
    'ANGLE (Apple, ANGLE Metal Renderer: Apple M1, Unspecified Version)',
    'Apple GPU', // Safari's masked name: no chip, no pass
    'ANGLE (Qualcomm, Qualcomm(R) Adreno(TM) 618 GPU (0x41333830) Direct3D11 vs_5_0 ps_5_0, D3D11)',
    'Adreno (TM) 740',
    'Mali-G57 MC2',
    'PowerVR Rogue GE8320',
    'ANGLE (Samsung Xclipse 940) on Vulkan 1.3.279',
    // no GPU at all
    'ANGLE (Google, Vulkan 1.3.0 (SwiftShader Device (Subzero) (0x0000C0DE)), SwiftShader driver)',
    'llvmpipe (LLVM 17.0.6, 256 bits)',
    angle('Microsoft', 'Microsoft Basic Render Driver'),
];

const UNKNOWN = ['', null, undefined, 'masked', 'WebKit WebGL', 'NV137'];

describe('BrowserDetect — the tier of a GPU by its name', () => {
    it('reads a graphics card of its own as dedicated', () => {
        for (const name of DEDICATED) expect(gpuTierOf(name), name).toBe('dedicated');
    });

    it('reads an integrated GPU, and what stands at its level, as light', () => {
        for (const name of LIGHT) expect(gpuTierOf(name), name).toBe('light');
    });

    it('does not guess: an empty, masked or unheard-of name is unknown', () => {
        for (const name of UNKNOWN) expect(gpuTierOf(name), String(name)).toBe('unknown');
    });
});

describe('BrowserDetect — the mode of mwsr a device takes', () => {
    const UA = {
        desktop:
            'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36',
        android:
            'Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Mobile Safari/537.36',
    };
    const RTX = angle('NVIDIA', 'NVIDIA GeForce RTX 5060 Ti');
    const IGPU = angle('AMD', 'AMD Radeon 780M Graphics');

    /**
     * BrowserDetect on a device of that user agent, whose WebGL contexts name
     * `renderer` — or, given an object, one name per powerPreference.
     */
    async function fresh(ua, renderer, storage = {}) {
        vi.resetModules();
        vi.stubGlobal('navigator', { userAgent: ua, maxTouchPoints: ua === UA.android ? 5 : 0 });
        vi.stubGlobal('localStorage', { getItem: (k) => storage[k] ?? null });
        const UNMASKED = 0x9246;
        const asked = [];
        vi.spyOn(document, 'createElement').mockImplementation(() => ({
            getContext: (kind, attrs) => {
                const power = (attrs && attrs.powerPreference) || 'default';
                asked.push(power);
                const name = typeof renderer === 'string' ? renderer : renderer[power];
                return {
                    RENDERER: 0x1f01,
                    getExtension: (n) =>
                        n === 'WEBGL_debug_renderer_info'
                            ? { UNMASKED_RENDERER_WEBGL: UNMASKED }
                            : null,
                    getParameter: (p) => (p === UNMASKED ? name : 'masked'),
                };
            },
        }));
        const m = await import('../js/util/BrowserDetect.js');
        return { m, asked };
    }

    afterEach(() => {
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
    });

    it('takes quality on a computer with a dedicated card, performance on an integrated GPU', async () => {
        let { m } = await fresh(UA.desktop, RTX);
        expect(m.mwsrQualityWanted()).toBe(true);
        expect(m.pickAutoEnhancer()).toBe('gl-mwsr-q');
        ({ m } = await fresh(UA.desktop, IGPU));
        expect(m.mwsrQualityWanted()).toBe(false);
        expect(m.pickAutoEnhancer()).toBe('gl-mwsr');
    });

    it('takes performance when the name says nothing', async () => {
        for (const name of ['', 'masked', 'Apple GPU']) {
            const { m } = await fresh(UA.desktop, name);
            expect(m.mwsrQualityWanted(), name).toBe(false);
            expect(m.pickAutoEnhancer(), name).toBe('gl-mwsr');
        }
    });

    it('keeps the kernel alone on an HDR stream, whatever the card', async () => {
        const { m } = await fresh(UA.desktop, RTX);
        expect(m.pickAutoEnhancer({ hdr: true })).toBe('mwsr');
    });

    it('takes performance on a phone, whatever its GPU is called', async () => {
        const { m } = await fresh(UA.android, RTX);
        expect(m.IS_MOBILE_OR_TABLET).toBe(true);
        expect(m.mwsrQualityWanted()).toBe(false);
        expect(m.pickAutoEnhancer()).toBe('gl-mwsr');
    });

    it("asks the GPU of the stream's own context: the high-performance one", async () => {
        // a laptop whose browser gives each preference its card
        const { m, asked } = await fresh(UA.desktop, { default: IGPU, 'high-performance': RTX });
        expect(m.mwsrQualityWanted()).toBe(true);
        expect(asked).toContain('high-performance');
        // probed once
        const n = asked.length;
        m.mwsrQualityWanted();
        expect(asked.length).toBe(n);
        // and the other way round
        const other = await fresh(UA.desktop, { default: RTX, 'high-performance': IGPU });
        expect(other.m.mwsrQualityWanted()).toBe(false);
        // the default context still answers the older questions
        expect(other.m.gpuRendererString()).toBe(RTX);
    });

    it('obeys the lab key over the device, both ways, and ignores any other value', async () => {
        let { m } = await fresh(UA.desktop, IGPU, { mw_mwsr_mode: 'quality' });
        expect(m.mwsrModeWanted()).toBe('quality');
        expect(m.pickAutoEnhancer()).toBe('gl-mwsr-q');
        expect(m.pickAutoEnhancer({ hdr: true })).toBe('mwsr');
        ({ m } = await fresh(UA.desktop, RTX, { mw_mwsr_mode: 'performance' }));
        expect(m.pickAutoEnhancer()).toBe('gl-mwsr');
        ({ m } = await fresh(UA.android, 'Mali-G715', { mw_mwsr_mode: 'quality' }));
        expect(m.pickAutoEnhancer()).toBe('gl-mwsr-q');
        ({ m } = await fresh(UA.desktop, RTX, { mw_mwsr_mode: 'on' }));
        expect(m.mwsrModeWanted()).toBeNull();
        expect(m.pickAutoEnhancer()).toBe('gl-mwsr-q');
    });
});
