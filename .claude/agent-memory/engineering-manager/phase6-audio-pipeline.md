---
name: phase6-audio-pipeline
description: Phase 6 implémentée — Audio Worklet Pipeline, PCM16 vers Float32 via AudioWorklet
metadata:
  type: project
---

## Phase 6 — Audio Pipeline

**Statut**: Complete (2026-05-14)

**Fichiers crees**:
- `frontend/js/audio/audio-processor.js` — AudioWorkletProcessor
- `frontend/js/audio/AudioPipeline.js` — Gestion AudioContext + AudioWorkletNode

**Fichier modifie**:
- `frontend/js/ui/StreamView.js` — Integration AudioPipeline (init, onAudio, close)

**Architecture**:
- PostMessage avec transfer d'ArrayBuffer vers le worklet (pas de SharedArrayBuffer)
- `sample.slice()` extrait les bytes PCM16 du buffer parent qui contient l'en-tete
- Le worklet convertit PCM16 -> Float32 avec de-interleave stereo
- Underrun = silence, Overrun = drop moitie queue (>12 chunks = ~120ms)

**Non implemente**: resampling (sample rate mismatch 44.1kHz vs 48kHz) — TODO futur
**Decision**: gardes _closed dans init() async pour race condition avec close()
**Reference**: [[phase5b-webcodecs-fix]] pour le pipeline video existant
