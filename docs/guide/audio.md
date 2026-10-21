# Audio

Select a clip with sound and open the inspector's **Audio** tab.

## Volume, fades and loudness

Volume (up to 200%), gain in dB and fade in/out per clip; the volume can be keyframed. **Normalize** measures the loudness (EBU R128) and sets the gain for social media (-14 LUFS), podcast (-16) or broadcast (-23).

## Cleaning up speech

- **Noise reduction:** removes hiss, hum and constant background noise. It is processed in the background and stays in sync.
- **Voice isolation (AI):** keeps only the speech and removes music, noise and room sound (model of 16 MB, downloaded once).
- **Voice effects:** echo, hall, robot, high pitch, deep voice, telephone and old radio.

## Music under speech

On the music clip, raise **Duck under voice**: it gets quieter whenever the other clips have speech and comes back up in the pauses.

## Voiceover

**Alt+R** (or the microphone on the timeline) counts down and records your microphone while the timeline plays, then drops the take on an audio track at that point.

## Removing silences

**Ctrl+Shift+R** finds the pauses in what you hear and marks them in red on the ruler. Adjust the threshold, minimum pause and the margin kept around each phrase, review, and remove them all at once.

## Edit by transcript

![Transcript](../screenshots/transcript.png)

The **Transcript** panel (Alt+7) transcribes the timeline word by word. Click a word to jump there; select words and press **Remove** (or **Select fillers** to pick every "um" and "uh") to cut those moments from the video.

## Beat markers

On a music clip, the **Beats** section finds the tempo and adds markers on every beat, every 2 beats or every bar (the strongest beat). They show as small dots on the ruler and clips snap to them, so your cuts land on the beat. **Clear** removes them (your own markers stay).

## Music separation

The **Separate music (AI)** section splits a song into **vocals, drums, bass and other instruments**, or gives you just the **vocals** or just the **instrumental**. The first time it offers to download the model (166 MB). Separation runs in the background — it takes a while, about twice the length of the song on a typical CPU — and the parts arrive as new audio tracks with the same timing; the original clip is muted, not deleted, so you can compare or go back. Separating the same song again is instant.
