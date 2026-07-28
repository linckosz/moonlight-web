/**
 * Type stub for the vendored `opus-decoder` UMD bundle (eshaz/opus-decoder).
 *
 * The bundle is minified third-party code: `tsconfig.json` excludes
 * `js/vendor/**`, but `exclude` only filters the `include` globs — a file
 * reached through an `import` is still pulled into the program and checked.
 * TypeScript prefers a sibling `.d.ts` over the `.js` when resolving, so this
 * file is what keeps ~28 errors from minified variable reuse out of the report.
 *
 * `opusWasm.js` imports it purely for its side effect: the bundle registers the
 * decoder on `globalThis['opus-decoder']`, from which the real wrapper reads.
 * It therefore exports nothing.
 */

export {};
