# Roadmap

✅ done · 🟡 partial · ⬜ pending

## MVP (core editing flow)

| # | Feature | Status | Notes |
|---|---|---|---|
| 1 | Projects: create, open, save, recovery | ✅ | recovery backup every 30 s, recover after a crash, confirm on close with unsaved changes |
| 2 | Media import, thumbnails, waveforms, relinking | ✅ | whole-folder import pending |
| 3 | GPU player with preview quality | ✅ | fullscreen pending |
| 4 | Multi-track timeline (main, overlay, text, audio), mute/hide/lock | ✅ | |
| 5 | Split, delete left/right, trim, ripple | ✅ | |
| 6 | Main-track magnet and snapping | ✅ | |
| 7 | Multi-selection, copy/paste, attributes, groups | ✅ | rubber-band selection and group dragging |
| 8 | Undo/redo | ✅ | |
| 9 | Zoom, scroll, fit, markers, in/out points | ✅ | in/out range (I/O) saved with the project and exportable on its own |
| 10 | Transform, crop, flip, opacity | ✅ | on-player transform handles missing |
| 11 | Canvas aspect ratios, background color and blurred background | ✅ | |
| 12 | Speed, curves, reverse, freeze | ✅ | pitch preserved by a built-in WSOLA time stretcher (optional per clip); reversed clips play their audio backwards |
| 13 | Keyframes with easing | ✅ | graph editor with custom Bézier; dragging keyframes directly on the timeline missing |
| 14 | Text and animations | ✅ | per-character/per-word animations (typewriter, karaoke) and spoken-word highlighting in automatic captions missing |
| 15 | Captions: SRT/VTT import, editing, export, automatic captions (whisper.cpp) | ✅ | caption list editor and local translation missing |
| 16 | Audio: volume, fades, detach audio, normalize, noise reduction, voice isolation | ✅ | voice isolation with DeepFilterNet3 (local, on demand) |
| 17 | Color adjustments and .cube LUTs | ✅ | exposure, brightness, contrast, saturation, highlights, shadows, temperature, tint, vignette and sharpness; 3D/1D LUTs on the GPU |
| 18 | Filters (built-in LUTs) | ✅ | 16 built-in filters, current-frame preview and adjustment layers on the timeline; curves, HSL and color wheels missing (Phase 2) |
| 19 | Transitions | ✅ | 8 types |
| 20 | In and out animations | ✅ | looping (combo) animations missing |
| 21 | Masks and chroma key | ✅ | mask keyframes, freeform path masks and on-player edit handles missing |
| 22 | Blend modes | ✅ | 10 modes |
| 23 | Export | ✅ | GPU encoding with software fallback, audio-only, current frame and alpha video (ProRes 4444 / VP9 WebM) |
| 24 | Proxies and render cache | ✅ | automatic proxies, hardware decoding and a render cache (2 s chunks keyed by content, drawn as a green bar on the ruler) |
| 25 | Customizable shortcuts | ✅ | editor with search, key capture and conflict handling; tooltips, menus and the command palette show the current keys |
| 26 | Interface languages | ✅ | English (source), Português (Brasil) and Español; live switching |

## Phase 2

| # | Feature | Status | Notes |
|---|---|---|---|
| 1 | Zero-copy hardware decoding and frame prefetching | 🟡 | VA-API decoding done (MVP #24); zero-copy (VA-API → dmabuf → Vulkan/GL texture) and prefetching on a thread missing |
| 2 | Compound clips | ✅ | create, break apart, edit the contents in place (nested), trim/split, render, mix and copy as one clip |
| 3 | Speed curves and smooth slow motion | 🟡 | speed curves done (MVP #12); frame interpolation (RIFE) missing |
| 4 | Advanced color: curves, HSL, color wheels, adjustment layers | ✅ | grading baked into a per-clip 3D LUT (also on adjustment layers); keyframing grading values missing |
| 5 | Scopes (histogram, waveform, vectorscope) | ✅ | read back from the composited frame (what gets exported), up to 10 times per second; RGB parade missing |
| 6 | Local background removal | 🟡 | RobustVideoMatting on ONNX Runtime: matte generated in the background, cached and applied as clip alpha in preview and export; model downloaded on demand from the Inspector. Interactive cutout (SAM 2) missing |
| 7 | Object tracking | ⬜ | |
| 8 | Stabilization | ✅ | own motion analysis (phase correlation + robust similarity fit), live smoothing with auto zoom; no rolling-shutter correction |
| 9 | Audio cleanup | 🟡 | noise reduction (afftdn), loudness normalization (EBU R128) and AI voice isolation (DeepFilterNet3) done; music/stem separation (Demucs) missing |
| 10 | Transcript-based editing and silence removal | ✅ | word-level transcript panel (whisper.cpp) where deleting words cuts the video with ripple, filler-word selection, and silence removal with a preview on the ruler |
| 11 | Local TTS (Piper) | ⬜ | |
| 12 | Stickers and shapes, curved text, per-character styles | ✅ | shapes, curved text, highlighted words (*markup*) and a built-in sticker library (Fluent Emoji, MIT) |
| 13 | Shader-based video effects (glitch, shake, blur, VHS, glow, particles) and more transitions | ✅ | 12 effects in the layer shader (per clip or as an effect layer) and 17 transitions; effect keyframes and multiple effects per clip missing |
| 14 | Audio: music ducking under voice, voice effects (echo, reverb, voice changer), voiceover recording | ✅ | ducking in real time in the mixer, voice effects as cached FFmpeg filter chains, voiceover recorded with SDL capture |
| 15 | Music beat detection with automatic markers | ✅ | spectral-flux onsets, autocorrelation tempo and dynamic-programming beat tracking; every beat, 2 beats or bar |
| 16 | Project templates with media slots and ready-made text templates | ⬜ | |
| 17 | Packaging: AppImage | ✅ | tested |
| 18 | Packaging: Flatpak | 🟡 | builds in CI on every push (KDE 6.11, FFmpeg 8.0.3 with x264/VA-API/NVENC, whisper.cpp, ONNX Runtime) and uploads a .flatpak bundle; publishing on Flathub missing |
| 19 | Packaging: Windows (MSVC/vcpkg/NSIS) and macOS (Homebrew/DMG) | 🟡 | CMake and CI ready, not yet run (no Windows/Mac machine and no active CI) |

## Phase 3

| # | Feature | Status | Notes |
|---|---|---|---|
| 1 | Automatic reframing | ⬜ | |
| 2 | Face retouching | ⬜ | |
| 3 | Super-resolution (Real-ESRGAN) | ⬜ | |
| 4 | Generative AI plugins with configurable providers (local or remote), always optional | ⬜ | |
| 5 | Screen and webcam recording via PipeWire / portal | ⬜ | |
| 6 | Library of freely licensed music and sound effects | ⬜ | |
| 7 | Mobile: Android | ⬜ | see plan below |
| 8 | Mobile: iOS | ⬜ | after Android; see plan below |

Mobile plan:

- Build FFmpeg, ONNX Runtime (AAR / xcframework) and whisper.cpp for arm64; replace SDL2 with `QAudioSink`.
- Hardware decoding with MediaCodec on Android (VideoToolbox already covers iOS).
- Dedicated QML touch UI reusing `ProjectController`: portrait layout, pinch on the timeline, long-press to drag clips.
- File access: `content://` URIs on Android, sandbox and photo library on iOS; memory limits during export.
- iOS: LGPL-only, dynamically linked FFmpeg build (App Store compatible), signing with an Apple Developer account.
