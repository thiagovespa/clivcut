# Text, captions and stickers

![Text panel](../screenshots/text.png)

## Titles and text

In the **Text** panel (Alt+2), pick a preset (default, title, subtitle, highlight); it lands at the playhead on a text track. On the inspector's **Text** tab you change the content, font, size, color, outline, shadow, background box and alignment.

- **Highlighted words:** write words between asterisks — `*like this*` — and they take the highlight color. Use `\*` for a literal asterisk.
- **Curved text:** the *Curve* slider bends the text into an arc, up or down.
- **Shapes:** rectangle, rounded, circle, triangle, star, arrow, heart and speech bubble, alone or behind the text, with their own color and size.

## Stickers

The bottom of the Text panel has a sticker library. Click one to drop it at the playhead, above the video; move and resize it like any clip. To make a sticker or text follow something that moves, see [motion tracking](color-and-effects.md#motion-tracking).

## Animations

The **Animations** panel (Alt+6) adds in and out animations (fade, zoom, slide) to the selected clip.

## Automatic captions

The **Captions** panel (Alt+3) transcribes the timeline on your computer with whisper.cpp, in 90+ languages. The first time, pick a model (Tiny is fast, Large v3 Turbo is the most accurate) and it is downloaded once. Each caption becomes an editable text clip on a *Captions* track; style one and use **Apply to all** to copy that style to the rest. Captions can be imported from SRT/VTT and exported to SRT.

To cut a video by editing its words, see [Edit by transcript](audio.md#edit-by-transcript).
