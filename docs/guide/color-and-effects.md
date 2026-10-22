# Color, effects and motion

## Filters and color grading

![Color](../screenshots/color.png)

- **Filters** panel (Alt+4): 16 looks previewed on the current frame. Click to apply to the selected clip, or add an **adjustment layer** that affects everything below it.
- **Adjust** tab: exposure, brightness, contrast, saturation, highlights, shadows, temperature, tint, vignette, sharpness and `.cube` LUTs; further down, **tone curves** (luma and R/G/B), **HSL** per color range and **lift/gamma/gain** wheels.
- **Scopes** (Ctrl+Shift+V): histogram, waveform and vectorscope over the player.

## Masks, blending and keying

The **Mask** tab has linear, mirror, circle and rounded-rectangle masks with feather and invert (also on adjustment layers, to grade one region). The **Advanced** tab has blend modes, chroma key and **background removal**, with a local AI model and no green screen:

- **People (automatic)** cuts out the people in the video.
- **Pick an object** cuts out anything you choose. Click **Click the object on the player**, then click the object in the player; Alt+click (or right-click) marks parts that do not belong to it. A preview of that frame shows the selection after each click. When it looks right, **Cut out in the whole clip** follows the object through the clip, before and after that frame. It runs in the background and takes a few seconds per frame; the model (SAM 2, 195 MB) is downloaded once.

## Effects and transitions

![Effects](../screenshots/effects.png)

- **Effects** panel (Alt+8): shake, blur, glitch, RGB split, VHS, glow, film grain, pixelate, mirror, snow, zoom pulse and strobe. With a clip selected, the effect goes on that clip; otherwise it becomes an effect layer from the playhead.
- **Transitions** panel (Alt+5): click a transition with a clip selected to put it on the cut before that clip.

## Stabilization

On the **Video** tab of a shaky clip, raise **Stabilization → Strength**. The camera motion is analyzed once in the background; after that, changing the strength is instant. More strength smooths more and zooms in a little more to hide the moving edges.

## Motion tracking

Make a text, sticker or image follow something that moves in the video below it:

1. Select the clip that should follow and move the playhead to a moment where the object is visible.
2. On the **Video** tab, click **Follow the video below**.
3. Drag a box around the object on the player (right-click or Esc cancels).

The tracking runs from the playhead in both directions and the clip gets position keyframes, which you can fine-tune on the Keyframes tab. **Remove** in the Tracking section takes them away.
