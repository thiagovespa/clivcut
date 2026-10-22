# Editing on the timeline

## Cutting and trimming

| Action | How |
|---|---|
| Split at the playhead | **Ctrl+B** (or the scissors) |
| Delete left / right of the playhead | **Q** / **W** |
| Trim a clip | drag its left or right edge |
| Delete / ripple delete | **Del** / **Shift+Del** (ripple closes the gap) |
| Duplicate | **Ctrl+D** |
| Freeze frame | **Alt+F** (or the snowflake): holds the current frame as a still |

The **main track** is the bottom one. With the **magnet** on (button on the right of the timeline toolbar) it never has gaps; with it off you can leave gaps freely. **Snapping** (N) makes clips stick to edges, the playhead and markers.

## Selecting and moving

Click a clip to select it, Ctrl/Shift-click to add more, or drag on an empty part of the timeline for a rubber-band selection. Selected clips move together. **Ctrl+G** groups clips so they always select and move together (Ctrl+Shift+G ungroups). Right-click a clip for its menu (copy, paste, attributes, group, split, detach audio…).

Copy and paste work across projects: the media comes along. **Copy attributes** (Ctrl+Alt+C) and **paste attributes** (Ctrl+Alt+V) copy the settings of one clip to others.

## Markers and in/out points

**M** adds a marker at the playhead. **I** and **O** set in and out points; the range can be rendered for preview or exported on its own. Beat markers from music are covered in [Audio](audio.md#beat-markers).

## Replacing media

Drop a media file from the Media panel onto a clip to replace its media: position, color, effects, transitions and timing stay (a shorter video shortens the clip).

## Speed

On the **Speed** tab: a constant speed from 0.1× to 10×, reverse, and **speed curves** (Montage, Hero, Bullet, Jump… or your own points). Pitch is kept by default; untick it for the chipmunk effect.

## Keyframes

Every property with a **◆** button can be animated: move the playhead, change the value and press ◆ to add a keyframe. The **Keyframes** tab has a graph editor where you drag keyframes in time and value and pick the curve of each segment (linear, ease in/out, smooth, hold or a custom Bézier).

## Compound clips

A compound clip packs several clips — from several tracks — into one clip.

1. Select the clips and press **Alt+G** (or right-click → Create compound clip).
2. The new clip can be moved, trimmed, split, transformed, color-graded, masked or given effects and transitions as one piece.
3. **Double-click** it (or **Alt+E**) to open its contents in the timeline. A bar above the tracks shows where you are; press **Back** (Alt+↑) to return. Everything you changed inside becomes a single undo step.
4. **Alt+Shift+G** breaks it apart again, at the same timing.

On a magnetic main track, the clips you combine there must be neighbors and cover the whole compound clip, so the rest of the timeline does not move.

## Undo

Ctrl+Z / Ctrl+Shift+Z, without limit. Continuous changes (dragging a slider) count as one step.
