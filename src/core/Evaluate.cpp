// SPDX-License-Identifier: GPL-3.0-or-later
#include "Evaluate.h"

#include "Effects.h"

#include "Keyframes.h"
#include "SpeedCurve.h"

#include <algorithm>
#include <cmath>

namespace clv {

namespace {

double easeOut(double u)
{
    return applyEasing(Easing::EaseOut, std::clamp(u, 0.0, 1.0));
}

/** Aplica uma animação de entrada (p: 0→1) ou saída (p: 1→0) à camada. */
void applyAnimation(VisualLayer &l, AnimationKind kind, double p)
{
    switch (kind) {
    case AnimationKind::None: break;
    case AnimationKind::Fade: l.opacity *= p; break;
    case AnimationKind::Zoom:
        l.scale *= 0.6 + 0.4 * p;
        l.opacity *= p;
        break;
    case AnimationKind::SlideUp: l.y += (1 - p) * 0.5; l.opacity *= p; break;
    case AnimationKind::SlideDown: l.y -= (1 - p) * 0.5; l.opacity *= p; break;
    case AnimationKind::SlideLeft: l.x += (1 - p) * 0.5; l.opacity *= p; break;
    case AnimationKind::SlideRight: l.x -= (1 - p) * 0.5; l.opacity *= p; break;
    }
}

FrameDesc evaluateTracks(const Project &p, const std::vector<Track> &tracks, Micros t, int depth);

VisualLayer resolveLayer(const Project &p, int ti, const Clip &c, Micros rel, int depth)
{
    VisualLayer l;
    l.trackIndex = ti;
    l.clip = &c;
    l.asset = c.hasMedia() ? p.asset(c.assetId) : nullptr;
    l.rel = rel;
    l.sourceTime = c.hasMedia() || c.isCompound() ? sourceTimeAt(p, c, rel) : 0;
    const Micros k = std::clamp<Micros>(rel, 0, c.duration);
    l.x = propAt(c, Prop::X, k);
    l.y = propAt(c, Prop::Y, k);
    l.scale = propAt(c, Prop::Scale, k);
    l.rotation = propAt(c, Prop::Rotation, k);
    l.opacity = propAt(c, Prop::Opacity, k);
    l.flipH = c.transform.flipH;
    l.flipV = c.transform.flipV;
    if (!c.isText()) {
        l.brightness = propAt(c, Prop::Brightness, k);
        l.contrast = propAt(c, Prop::Contrast, k);
        l.saturation = propAt(c, Prop::Saturation, k);
        l.temperature = c.color.temperature;
        l.blend = c.blend;
    }
    if (c.animIn.kind != AnimationKind::None && c.animIn.duration > 0 && rel < c.animIn.duration)
        applyAnimation(l, c.animIn.kind, easeOut(static_cast<double>(rel) / c.animIn.duration));
    const Micros fromEnd = c.duration - rel;
    if (c.animOut.kind != AnimationKind::None && c.animOut.duration > 0 && fromEnd < c.animOut.duration)
        applyAnimation(l, c.animOut.kind, easeOut(static_cast<double>(fromEnd) / c.animOut.duration));
    l.effect = effectShaderIndex(c.effect.id);
    l.effectIntensity = c.effect.intensity;
    l.effectTime = static_cast<double>(rel) / kSecond;
    if (c.stabilize > 0 && l.asset && l.asset->kind == MediaKind::Video)
        if (const auto track = registeredMotion(l.asset->path)) {
            l.stab = stabilizationAt(*track, l.sourceTime, c.stabilize);
            l.stabAspect = track->aspect;
        }
    // Compostos dentro de compostos têm um limite, contra referências circulares.
    if (const Sequence *seq = c.isCompound() && depth < 8 ? p.compound(c.compoundId) : nullptr)
        l.nested = std::make_shared<const FrameDesc>(evaluateTracks(p, seq->tracks, l.sourceTime, depth + 1));
    return l;
}

/** Aplica a transição entre `out` (clipe anterior) e `in` com progresso p ∈ [0,1]. */
void applyTransition(TransitionKind kind, double p, VisualLayer &out, VisualLayer &in)
{
    switch (kind) {
    case TransitionKind::Dissolve: in.opacity *= p; break;
    case TransitionKind::FadeBlack:
        if (p < 0.5) {
            out.opacity *= 1 - 2 * p;
            in.opacity = 0;
        } else {
            out.opacity = 0;
            in.opacity *= 2 * p - 1;
        }
        break;
    case TransitionKind::WipeLeft:
        in.mask = MaskKind::WipeLeft;
        in.maskProgress = p;
        break;
    case TransitionKind::WipeRight:
        in.mask = MaskKind::WipeRight;
        in.maskProgress = p;
        break;
    case TransitionKind::SlideLeft:
        out.x -= 2 * p;
        in.x += 2 * (1 - p);
        break;
    case TransitionKind::SlideRight:
        out.x += 2 * p;
        in.x -= 2 * (1 - p);
        break;
    case TransitionKind::CircleOpen:
        in.mask = MaskKind::Circle;
        in.maskProgress = p;
        break;
    case TransitionKind::ZoomIn:
        out.scale *= 1 + p;
        out.opacity *= 1 - p;
        in.opacity *= p;
        break;
    case TransitionKind::SlideUp:
        out.y -= 2 * p;
        in.y += 2 * (1 - p);
        break;
    case TransitionKind::SlideDown:
        out.y += 2 * p;
        in.y -= 2 * (1 - p);
        break;
    case TransitionKind::WipeUp:
        in.mask = MaskKind::WipeUp;
        in.maskProgress = p;
        break;
    case TransitionKind::WipeDown:
        in.mask = MaskKind::WipeDown;
        in.maskProgress = p;
        break;
    case TransitionKind::Spin:
        // Sai girando e encolhendo; entra girando do outro lado.
        out.rotation += 180 * p;
        out.scale *= 1 - 0.6 * p;
        out.opacity *= 1 - p;
        in.rotation -= 180 * (1 - p);
        in.scale *= 0.4 + 0.6 * p;
        in.opacity *= p;
        break;
    case TransitionKind::ZoomOut:
        in.scale *= 2 - p;
        in.opacity *= p;
        out.opacity *= 1 - p * 0.5;
        break;
    case TransitionKind::FlashWhite: {
        // Clareia até o branco no meio e volta a partir do branco (efeito 13 = branco).
        const double w = 1 - std::abs(2 * p - 1);
        out.effect = in.effect = 13;
        out.effectIntensity = in.effectIntensity = w;
        if (p < 0.5)
            in.opacity = 0;
        else
            out.opacity = 0;
        break;
    }
    case TransitionKind::Glitch: {
        // Interferência forte em volta do corte, que acontece no meio.
        const double w = 1 - std::abs(2 * p - 1);
        out.effect = in.effect = 3;
        out.effectIntensity = in.effectIntensity = 0.4 + 0.6 * w;
        if (p < 0.5)
            in.opacity = 0;
        else
            out.opacity = 0;
        break;
    }
    case TransitionKind::BlurDissolve:
        out.effect = in.effect = 2;
        out.effectIntensity = p;
        in.effectIntensity = 1 - p;
        in.opacity *= p;
        break;
    }
}

} // namespace

Micros sourceTimeAt(const Project &p, const Clip &c, Micros rel)
{
    if (c.isCompound()) {
        const Sequence *seq = p.compound(c.compoundId);
        const Micros len = seq ? seq->duration() : 0;
        return std::clamp<Micros>(c.sourceIn + static_cast<Micros>(rel * c.speed), 0, std::max<Micros>(0, len - 1));
    }
    const MediaAsset *a = p.asset(c.assetId);
    if (!a || a->kind == MediaKind::Image)
        return 0;
    Micros consumed = static_cast<Micros>(rel * c.speed);
    if (c.speedCurve.size() >= 2 && c.duration > 0 && rel >= 0 && rel <= c.duration)
        consumed = static_cast<Micros>(std::llround(c.sourceDuration() * curveProgress(c.speedCurve, static_cast<double>(rel) / c.duration)));
    Micros s = c.reverse ? c.sourceOut() - consumed - 1 : c.sourceIn + consumed;
    if (a->duration > 0)
        s = std::clamp<Micros>(s, 0, a->duration - 1);
    return std::max<Micros>(s, 0);
}

double audioGainAt(const Clip &c, Micros rel)
{
    double g = propAt(c, Prop::Volume, std::clamp<Micros>(rel, 0, c.duration));
    if (c.gainDb != 0)
        g *= std::pow(10.0, c.gainDb / 20.0);
    if (c.fadeIn > 0 && rel < c.fadeIn)
        g *= std::max(0.0, static_cast<double>(rel) / c.fadeIn);
    const Micros fromEnd = c.duration - rel;
    if (c.fadeOut > 0 && fromEnd < c.fadeOut)
        g *= std::max(0.0, static_cast<double>(fromEnd) / c.fadeOut);
    return std::max(0.0, g);
}

bool touchesPrevious(const Project &p, const std::vector<Clip> &clips, int index)
{
    if (index <= 0 || index >= static_cast<int>(clips.size()))
        return false;
    return std::abs(clips[index - 1].end() - clips[index].start) <= p.frameDuration() / 2;
}

FrameDesc evaluate(const Project &p, Micros t)
{
    return evaluateTracks(p, p.tracks, t, 0);
}

namespace {

FrameDesc evaluateTracks(const Project &p, const std::vector<Track> &tracks, Micros t, int depth)
{
    FrameDesc f;
    for (int ti = 0; ti < static_cast<int>(tracks.size()); ++ti) {
        const Track &track = tracks[ti];
        const auto &clips = track.clips;
        for (int ci = 0; ci < static_cast<int>(clips.size()); ++ci) {
            const Clip &c = clips[ci];

            // Transição no corte entre clips[ci-1] e clips[ci] (clipes encostados). O corte é o
            // início do clipe que entra; uma folga de meio quadro fica com o clipe que sai.
            const Clip *next = ci + 1 < static_cast<int>(clips.size()) ? &clips[ci + 1] : nullptr;
            const bool nextHasTransition = next && next->transitionIn && touchesPrevious(p, clips, ci + 1);
            const bool hasTransition = c.transitionIn && touchesPrevious(p, clips, ci);
            const Micros half = hasTransition ? c.transitionIn->duration / 2 : 0;
            const Micros nextHalf = nextHasTransition ? next->transitionIn->duration / 2 : 0;
            const Micros ownEnd = nextHasTransition ? next->start : c.end();

            const bool inside = t >= c.start && t < c.end();
            const bool insideVisual = t >= c.start && t < ownEnd;

            // Uma transição ocupa [corte - d/2, corte + d/2). Cada metade é emitida pelo
            // clipe que contém t; o outro clipe é estendido além da borda (origem extrapolada).
            if (insideVisual && track.isVisual() && !track.hidden && !c.audioOnly) {
                VisualLayer cur = resolveLayer(p, ti, c, t - c.start, depth);
                if (hasTransition && t < c.start + half) {
                    const Clip &prev = clips[ci - 1];
                    VisualLayer out = resolveLayer(p, ti, prev, t - prev.start, depth);
                    const double prog = static_cast<double>(t - (c.start - half)) / c.transitionIn->duration;
                    applyTransition(c.transitionIn->kind, prog, out, cur);
                    f.layers.push_back(out);
                    f.layers.push_back(cur);
                } else if (nextHasTransition && t >= next->start - nextHalf) {
                    VisualLayer in = resolveLayer(p, ti, *next, t - next->start, depth);
                    const double prog = static_cast<double>(t - (next->start - nextHalf)) / next->transitionIn->duration;
                    applyTransition(next->transitionIn->kind, prog, cur, in);
                    f.layers.push_back(cur);
                    f.layers.push_back(in);
                } else {
                    f.layers.push_back(cur);
                }
            }

            if (inside && !track.muted && c.hasMedia() && !c.audioMuted) {
                const MediaAsset *a = p.asset(c.assetId);
                if (a && a->hasAudio && (track.kind == TrackKind::Audio || track.kind == TrackKind::Video))
                    f.audio.push_back({ti, &c, a, t - c.start, sourceTimeAt(p, c, t - c.start)});
            }
        }
    }
    return f;
}

} // namespace

} // namespace clv
