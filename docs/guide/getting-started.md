# Getting started

## Install

Download the latest package from the [releases page](https://github.com/thiagovespa/clivcut/releases):

- **Linux:** the `.AppImage` (make it executable and run it) or the `.flatpak` (`flatpak install --user clivcut-<version>-x86_64.flatpak`).
- **Windows:** the `.exe` installer, or the portable `.zip`.
- **macOS:** the `.dmg` for your Mac (`arm64` for Apple Silicon, `x86_64` for Intel). The app is not notarized: open it the first time with right-click → Open.

## Create a project

The welcome screen offers canvas formats (landscape 16:9, vertical 9:16, square, feed 4:5, cinema 21:9). Pick one, or import media directly. You can change the canvas later in the inspector when no clip is selected.

![Welcome screen](../screenshots/welcome.png)

## The window

![Editor](../screenshots/editor.png)

- **Panels** (left rail, Alt+1…8): media, text and stickers, captions, filters, transitions, animations, transcript and effects.
- **Player** (center): exactly what will be exported. Drag in it to move the selected clip; Ctrl+wheel resizes it.
- **Inspector** (right): everything about the selected clip, in tabs (Video, Adjust, Mask, Keyframes, Audio, Speed, Advanced). With nothing selected it shows the project settings.
- **Timeline** (bottom): the main track at the bottom, overlay, text and audio tracks above it.

## Import and place media

Click **Import media** or drag files from your file manager into the window. Drag them from the media panel to the timeline, or click the **+** on a thumbnail (or double-click it) to add it to the timeline. Videos above 1080p get a lighter proxy in the background so playback stays smooth (the **Proxy** button on the player toggles it; export always uses the original).

## Save and recover

Ctrl+S saves a `.clivcut` project. While there are unsaved changes, clivcut keeps a recovery copy every 30 seconds and offers it back after a crash.

## Export

Click **Export** (top right), choose the file, resolution, frame rate and quality, and go. See [Exporting](exporting.md) for the other modes.

Next: [Editing on the timeline](editing.md).
