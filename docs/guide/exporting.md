# Exporting

![Export](../screenshots/export.png)

Click **Export** (Ctrl+E). The dialog has three modes:

- **Video:** file name, resolution (480p to 4K), frame rate (24 to 60 fps), codec (H.264, HEVC, AV1, ProRes and the transparent ones, depending on your FFmpeg) and quality. **Encode on the GPU** uses VA-API, NVENC, Quick Sync, VideoToolbox or AMF when available and falls back to the CPU otherwise. Tick the in/out option to export only that range.
- **Audio only:** WAV, FLAC, AAC, Opus or MP3.
- **Current frame:** a PNG or JPG of what the player shows.

## Transparent video

ProRes 4444 (`.mov`) and VP9 (`.webm`) keep the alpha channel: areas without clips stay transparent. Useful for titles and lower thirds to use in other editors.

## Smooth playback of heavy parts

**Ctrl+R** pre-renders the in/out range (or the whole timeline). Rendered parts show as a green bar on the ruler; an edit only invalidates the parts it touches.

## Command line

`clivcut-render` renders a project without opening the window:

```sh
clivcut-render project.clivcut out.mp4 --codec hevc --height 2160
clivcut-render --transcribe --model small --language en video.mp4 captions.srt
```

Stabilization and background removal are applied there too (a missing stabilization analysis is done on the spot).
