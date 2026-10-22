// SPDX-License-Identifier: GPL-3.0-or-later
#include "Edit.h"

#include "Keyframes.h"
#include "SpeedCurve.h"

#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>

namespace clv::edit {

namespace {

bool isMainMagnet(const Project &p, int trackIndex)
{
    return trackIndex == 0 && p.mainTrackMagnet;
}

void shiftAllKeyframes(Clip &c, Micros delta)
{
    for (auto &list : c.keyframes)
        shiftKeyframes(list, delta);
}

void clampAllKeyframes(Clip &c)
{
    for (auto &list : c.keyframes)
        clampKeyframes(list, c.duration);
}

/** Limite inferior de start na faixa (fim do clipe anterior). */
Micros prevEnd(const Track &t, int index)
{
    return index > 0 ? t.clips[index - 1].end() : 0;
}

/** Limite superior de end na faixa (início do clipe seguinte). */
Micros nextStart(const Track &t, int index)
{
    return index + 1 < static_cast<int>(t.clips.size()) ? t.clips[index + 1].start
                                                          : std::numeric_limits<Micros>::max();
}

} // namespace

std::optional<ClipRef> findClip(const Project &p, const QString &clipId)
{
    for (int ti = 0; ti < static_cast<int>(p.tracks.size()); ++ti) {
        const auto &clips = p.tracks[ti].clips;
        for (int ci = 0; ci < static_cast<int>(clips.size()); ++ci)
            if (clips[ci].id == clipId)
                return ClipRef{ti, ci};
    }
    return std::nullopt;
}

Clip *clipById(Project &p, const QString &clipId)
{
    auto r = findClip(p, clipId);
    return r ? &p.tracks[r->track].clips[r->index] : nullptr;
}

const Clip *clipById(const Project &p, const QString &clipId)
{
    auto r = findClip(p, clipId);
    return r ? &p.tracks[r->track].clips[r->index] : nullptr;
}

TrackKind trackKindFor(const Project &p, const Clip &c)
{
    if (c.isText())
        return TrackKind::Text;
    if (c.isAdjust())
        return TrackKind::Adjust;
    if (c.audioOnly)
        return TrackKind::Audio;
    if (c.isCompound()) {
        // Composto só de áudio vai para faixa de áudio; com qualquer imagem, para faixa de vídeo.
        const Sequence *seq = p.compound(c.compoundId);
        if (!seq)
            return TrackKind::Video;
        for (const auto &t : seq->tracks)
            for (const auto &inner : t.clips)
                if (trackKindFor(p, inner) != TrackKind::Audio)
                    return TrackKind::Video;
        return TrackKind::Audio;
    }
    const MediaAsset *a = p.asset(c.assetId);
    if (a && a->kind == MediaKind::Audio)
        return TrackKind::Audio;
    return TrackKind::Video;
}

bool fitsInTrack(const Track &t, Micros start, Micros duration, const QString &ignoreId)
{
    if (start < 0)
        return false;
    const Micros end = start + duration;
    for (const auto &c : t.clips) {
        if (c.id == ignoreId)
            continue;
        if (start < c.end() && c.start < end)
            return false;
    }
    return true;
}

void sortTrack(Track &t)
{
    std::stable_sort(t.clips.begin(), t.clips.end(), [](const Clip &a, const Clip &b) { return a.start < b.start; });
}

void packMainTrack(Project &p)
{
    if (p.tracks.empty() || !p.mainTrackMagnet)
        return;
    auto &main = p.tracks[0];
    sortTrack(main);
    Micros cursor = 0;
    for (auto &c : main.clips) {
        c.start = cursor;
        cursor = c.end();
    }
}

void removeEmptyTracks(Project &p)
{
    if (p.tracks.size() <= 1)
        return;
    for (int i = static_cast<int>(p.tracks.size()) - 1; i >= 1; --i)
        if (p.tracks[i].clips.empty())
            p.tracks.erase(p.tracks.begin() + i);
}

int ensureTrackFor(Project &p, TrackKind kind, Micros start, Micros duration, int preferred)
{
    auto usable = [&](int i) {
        const auto &t = p.tracks[i];
        return t.kind == kind && !t.locked && !isMainMagnet(p, i) && fitsInTrack(t, start, duration);
    };
    if (preferred >= 0 && preferred < static_cast<int>(p.tracks.size())) {
        if (isMainMagnet(p, preferred) && kind == TrackKind::Video)
            return preferred;
        if (usable(preferred))
            return preferred;
    }
    // Faixa principal vazia aceita o primeiro clipe de vídeo.
    if (kind == TrackKind::Video && !p.tracks.empty() && p.tracks[0].clips.empty() && preferred < 0)
        return 0;
    const int from = std::max(preferred, 1);
    for (int i = from; i < static_cast<int>(p.tracks.size()); ++i)
        if (usable(i))
            return i;
    for (int i = 1; i < from && i < static_cast<int>(p.tracks.size()); ++i)
        if (usable(i))
            return i;
    p.tracks.push_back(createTrack(kind));
    return static_cast<int>(p.tracks.size()) - 1;
}

Micros maxDuration(const Project &p, const Clip &c)
{
    if (c.isCompound()) {
        const Sequence *seq = p.compound(c.compoundId);
        return seq && c.speed > 0 ? static_cast<Micros>((seq->duration() - c.sourceIn) / c.speed) : -1;
    }
    if (!c.hasMedia())
        return -1;
    const MediaAsset *a = p.asset(c.assetId);
    if (!a || a->kind == MediaKind::Image || a->duration <= 0 || c.speed <= 0)
        return -1;
    return static_cast<Micros>((a->duration - c.sourceIn) / c.speed);
}

QString insertClip(Project &p, Clip c, int trackIndex)
{
    if (c.duration <= 0)
        return {};
    const TrackKind kind = trackKindFor(p, c);
    if (trackIndex >= static_cast<int>(p.tracks.size()))
        trackIndex = -1;
    if (trackIndex >= 0 && p.tracks[trackIndex].kind != kind)
        trackIndex = -1;
    if (trackIndex >= 0 && p.tracks[trackIndex].locked)
        return {};

    if (trackIndex == 0 && p.mainTrackMagnet) {
        auto &main = p.tracks[0];
        // Posição de inserção: antes do primeiro clipe cujo ponto médio passa de start.
        auto it = std::find_if(main.clips.begin(), main.clips.end(),
                               [&](const Clip &o) { return c.start < o.start + o.duration / 2; });
        const QString id = c.id;
        main.clips.insert(it, std::move(c));
        // A ordem de inserção define a ordem; reatribui starts sequenciais.
        Micros cursor = 0;
        for (auto &o : main.clips) {
            o.start = cursor;
            cursor = o.end();
        }
        return id;
    }

    const int ti = ensureTrackFor(p, kind, c.start, c.duration, trackIndex);
    if (ti == 0 && p.mainTrackMagnet)
        return insertClip(p, std::move(c), 0);
    const QString id = c.id;
    p.tracks[ti].clips.push_back(std::move(c));
    sortTrack(p.tracks[ti]);
    return id;
}

std::optional<QString> splitClip(Project &p, const QString &clipId, Micros t)
{
    auto ref = findClip(p, clipId);
    if (!ref)
        return std::nullopt;
    Track &track = p.tracks[ref->track];
    if (track.locked)
        return std::nullopt;
    Clip &c = track.clips[ref->index];
    const Micros rel = t - c.start;
    const Micros minLen = p.frameDuration() / 2;
    if (rel < minLen || c.duration - rel < minLen)
        return std::nullopt;

    const Micros origIn = c.sourceIn;
    const Micros origDuration = c.duration;
    const Micros origSrc = c.sourceDuration();
    Clip right = c;
    right.id = newId(c.isText() ? "txt" : c.isAdjust() ? "adj" : c.isCompound() ? "cmp" : "clp");
    right.start = t;
    right.duration = c.duration - rel;
    Clip &left = c;
    left.duration = rel;

    // Com curva: cada metade fica com seu pedaço da curva e sua velocidade média,
    // e a origem é dividida pela integral da curva (não linearmente).
    const bool curved = left.speedCurve.size() >= 2 && left.hasMedia() && left.speed > 0;
    if (curved) {
        const double a = static_cast<double>(rel) / origDuration;
        const Micros leftSrc = static_cast<Micros>(std::llround(origSrc * curveProgress(left.speedCurve, a)));
        const Micros rightSrc = origSrc - leftSrc;
        right.speedCurve = sliceCurve(left.speedCurve, a, 1);
        left.speedCurve = sliceCurve(left.speedCurve, 0, a);
        left.speed = static_cast<double>(leftSrc) / rel;
        right.speed = static_cast<double>(rightSrc) / right.duration;
        if (left.reverse) {
            left.sourceIn = origIn + rightSrc;
            right.sourceIn = origIn;
        } else {
            right.sourceIn = origIn + leftSrc;
        }
    } else if (left.hasMedia() || left.isCompound()) {
        const Micros leftSrc = static_cast<Micros>(rel * c.speed);
        if (c.reverse) {
            // Em reverso, o início da timeline mostra o fim do trecho de origem.
            left.sourceIn = origIn + right.sourceDuration();
            right.sourceIn = origIn;
        } else {
            right.sourceIn = origIn + leftSrc;
        }
    }
    // Fades/animações/transição ficam nas extremidades originais.
    left.fadeOut = 0;
    left.animOut = {};
    right.fadeIn = 0;
    right.animIn = {};
    right.transitionIn.reset();

    for (int i = 0; i < kPropCount; ++i) {
        auto [l, r] = splitKeyframes(c.keyframes[i], rel);
        left.keyframes[i] = std::move(l);
        right.keyframes[i] = std::move(r);
    }
    const QString rightId = right.id;
    track.clips.insert(track.clips.begin() + ref->index + 1, std::move(right));
    return rightId;
}

int splitAt(Project &p, Micros t, const QStringList &onlyIds)
{
    QStringList targets;
    for (const auto &track : p.tracks) {
        if (track.locked)
            continue;
        for (const auto &c : track.clips)
            if (t > c.start && t < c.end() && (onlyIds.isEmpty() || onlyIds.contains(c.id)))
                targets << c.id;
    }
    int n = 0;
    for (const auto &id : targets)
        if (splitClip(p, id, t))
            ++n;
    return n;
}

Micros removeRanges(Project &p, std::vector<std::pair<Micros, Micros>> ranges)
{
    // Normaliza: ordena, descarta vazios e junta sobrepostos.
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<Micros, Micros>> merged;
    for (auto [a, b] : ranges) {
        a = std::max<Micros>(0, a);
        if (b <= a)
            continue;
        if (!merged.empty() && a <= merged.back().second)
            merged.back().second = std::max(merged.back().second, b);
        else
            merged.emplace_back(a, b);
    }
    Micros total = 0;
    // De trás para frente: cada corte não desloca os trechos ainda por cortar.
    for (auto it = merged.rbegin(); it != merged.rend(); ++it) {
        const auto [a, b] = *it;
        splitAt(p, a);
        splitAt(p, b);
        const Micros len = b - a;
        for (Track &track : p.tracks) {
            if (track.locked)
                continue;
            track.clips.erase(std::remove_if(track.clips.begin(), track.clips.end(),
                                             [&](const Clip &c) { return c.start >= a && c.end() <= b; }),
                              track.clips.end());
            for (Clip &c : track.clips)
                if (c.start >= b)
                    c.start -= len;
        }
        p.markers.erase(std::remove_if(p.markers.begin(), p.markers.end(),
                                       [&](const Marker &m) { return m.t >= a && m.t < b; }),
                        p.markers.end());
        for (Marker &m : p.markers)
            if (m.t >= b)
                m.t -= len;
        auto shift = [&](Micros &t) {
            if (t >= b)
                t -= len;
            else if (t > a)
                t = a;
        };
        if (p.inPoint >= 0)
            shift(p.inPoint);
        if (p.outPoint > 0)
            shift(p.outPoint);
        total += len;
    }
    if (total > 0)
        removeEmptyTracks(p);
    return total;
}

bool deleteClips(Project &p, const QStringList &ids, bool ripple)
{
    bool any = false;
    for (int ti = 0; ti < static_cast<int>(p.tracks.size()); ++ti) {
        Track &track = p.tracks[ti];
        if (track.locked)
            continue;
        // Remove de trás para frente para que o ripple use posições originais.
        for (int ci = static_cast<int>(track.clips.size()) - 1; ci >= 0; --ci) {
            if (!ids.contains(track.clips[ci].id))
                continue;
            const Micros gap = track.clips[ci].duration;
            track.clips.erase(track.clips.begin() + ci);
            any = true;
            if (ripple && !isMainMagnet(p, ti))
                for (int k = ci; k < static_cast<int>(track.clips.size()); ++k)
                    track.clips[k].start -= gap;
        }
    }
    if (!any)
        return false;
    packMainTrack(p);
    removeEmptyTracks(p);
    return true;
}

bool trimClip(Project &p, const QString &clipId, Edge edge, Micros newTime)
{
    auto ref = findClip(p, clipId);
    if (!ref)
        return false;
    Track &track = p.tracks[ref->track];
    if (track.locked)
        return false;
    Clip &c = track.clips[ref->index];
    const Micros minLen = p.frameDuration();
    const bool magnet = isMainMagnet(p, ref->track);
    const MediaAsset *a = c.hasMedia() ? p.asset(c.assetId) : nullptr;
    const bool limited = (a && a->kind != MediaKind::Image && a->duration > 0 && c.speed > 0)
                         || (c.isCompound() && c.speed > 0 && p.compound(c.compoundId));

    if (edge == Edge::End) {
        Micros newEnd = std::max(newTime, c.start + minLen);
        if (limited) {
            if (c.reverse) {
                // Em reverso, estender o fim consome origem antes de sourceIn.
                const Micros maxExtra = static_cast<Micros>(c.sourceIn / c.speed);
                newEnd = std::min(newEnd, c.end() + maxExtra);
            } else {
                newEnd = std::min(newEnd, c.start + maxDuration(p, c));
            }
        }
        if (!magnet)
            newEnd = std::min(newEnd, nextStart(track, ref->index));
        const Micros newDur = newEnd - c.start;
        if (newDur < minLen)
            return false;
        if (c.reverse && limited)
            c.sourceIn -= static_cast<Micros>((newDur - c.duration) * c.speed);
        c.duration = newDur;
        clampAllKeyframes(c);
    } else {
        Micros newStart = std::min(newTime, c.end() - minLen);
        if (limited && !c.reverse)
            newStart = std::max(newStart, c.start - static_cast<Micros>(c.sourceIn / c.speed));
        if (limited && c.reverse) {
            const Micros tail = a->duration - c.sourceOut();
            newStart = std::max(newStart, c.start - static_cast<Micros>(tail / c.speed));
        }
        if (!magnet)
            newStart = std::max(newStart, prevEnd(track, ref->index));
        newStart = std::max<Micros>(newStart, 0);
        const Micros delta = newStart - c.start;
        if (c.duration - delta < minLen)
            return false;
        if ((c.hasMedia() || c.isCompound()) && !c.reverse)
            c.sourceIn += static_cast<Micros>(delta * c.speed);
        c.start = newStart;
        c.duration -= delta;
        shiftAllKeyframes(c, -delta);
        clampAllKeyframes(c);
        if (c.sourceIn < 0)
            c.sourceIn = 0;
    }
    packMainTrack(p);
    return true;
}

bool trimToPlayhead(Project &p, const QString &clipId, Micros t, Edge removeSide, bool ripple)
{
    auto ref = findClip(p, clipId);
    if (!ref)
        return false;
    const Clip &c = p.tracks[ref->track].clips[ref->index];
    if (t <= c.start || t >= c.end())
        return false;
    if (removeSide == Edge::End)
        return trimClip(p, clipId, Edge::End, t);
    const Micros removed = t - c.start;
    if (!trimClip(p, clipId, Edge::Start, t))
        return false;
    if (ripple && !isMainMagnet(p, ref->track)) {
        auto &clips = p.tracks[ref->track].clips;
        for (int k = ref->index; k < static_cast<int>(clips.size()); ++k)
            clips[k].start -= removed;
    }
    return true;
}

bool moveClip(Project &p, const QString &clipId, int targetTrack, Micros newStart)
{
    auto ref = findClip(p, clipId);
    if (!ref || targetTrack < 0 || targetTrack >= static_cast<int>(p.tracks.size()))
        return false;
    if (p.tracks[ref->track].locked || p.tracks[targetTrack].locked)
        return false;
    Clip c = p.tracks[ref->track].clips[ref->index];
    if (p.tracks[targetTrack].kind != trackKindFor(p, c))
        return false;
    newStart = std::max<Micros>(newStart, 0);

    if (!isMainMagnet(p, targetTrack) && !fitsInTrack(p.tracks[targetTrack], newStart, c.duration, c.id)) {
        // Sem espaço: cai na próxima faixa livre (ou numa nova).
        Project probe = p;
        probe.tracks[ref->track].clips.erase(probe.tracks[ref->track].clips.begin() + ref->index);
        const int ti = ensureTrackFor(probe, p.tracks[targetTrack].kind, newStart, c.duration, targetTrack);
        if (ti == 0 && p.mainTrackMagnet)
            return false;
        c.start = newStart;
        probe.tracks[ti].clips.push_back(c);
        sortTrack(probe.tracks[ti]);
        packMainTrack(probe);
        removeEmptyTracks(probe);
        p = std::move(probe);
        return true;
    }

    p.tracks[ref->track].clips.erase(p.tracks[ref->track].clips.begin() + ref->index);
    c.start = newStart;
    if (isMainMagnet(p, targetTrack)) {
        insertClip(p, std::move(c), 0);
    } else {
        p.tracks[targetTrack].clips.push_back(std::move(c));
        sortTrack(p.tracks[targetTrack]);
    }
    packMainTrack(p);
    removeEmptyTracks(p);
    return true;
}

bool setSpeed(Project &p, const QString &clipId, double speed)
{
    auto ref = findClip(p, clipId);
    if (!ref || speed < 0.1 || speed > 100)
        return false;
    Track &track = p.tracks[ref->track];
    Clip &c = track.clips[ref->index];
    if (!c.hasMedia() || c.speed <= 0)
        return false;
    const Micros src = c.sourceDuration();
    c.speedCurve.clear(); // velocidade normal substitui a curva
    Micros newDur = static_cast<Micros>(std::llround(src / speed));
    if (!isMainMagnet(p, ref->track)) {
        newDur = std::min(newDur, nextStart(track, ref->index) - c.start);
    }
    if (newDur < p.frameDuration())
        return false;
    const double ratio = static_cast<double>(newDur) / static_cast<double>(c.duration);
    for (auto &list : c.keyframes)
        for (auto &k : list)
            k.t = static_cast<Micros>(std::llround(k.t * ratio));
    c.speed = speed;
    c.duration = newDur;
    packMainTrack(p);
    return true;
}

bool setSpeedCurve(Project &p, const QString &clipId, std::vector<SpeedPoint> curve)
{
    auto ref = findClip(p, clipId);
    if (!ref)
        return false;
    Track &track = p.tracks[ref->track];
    Clip &c = track.clips[ref->index];
    if (!c.hasMedia() || c.speed <= 0 || track.locked)
        return false;
    const Micros src = c.sourceDuration();
    if (curve.size() < 2) {
        // Sem curva: volta à velocidade constante média atual.
        c.speedCurve.clear();
        return true;
    }
    curve = normalizedCurve(std::move(curve));
    const double mean = curveMean(curve);
    Micros newDur = static_cast<Micros>(std::llround(src / mean));
    if (!isMainMagnet(p, ref->track))
        newDur = std::min(newDur, nextStart(track, ref->index) - c.start);
    if (newDur < p.frameDuration())
        return false;
    const double ratio = static_cast<double>(newDur) / static_cast<double>(c.duration);
    for (auto &list : c.keyframes)
        for (auto &k : list)
            k.t = static_cast<Micros>(std::llround(k.t * ratio));
    c.speedCurve = std::move(curve);
    c.duration = newDur;
    c.speed = mean; // se a faixa limitou a duração, consome menos origem com a mesma forma
    packMainTrack(p);
    return true;
}

std::optional<QString> duplicateClip(Project &p, const QString &clipId)
{
    auto ref = findClip(p, clipId);
    if (!ref)
        return std::nullopt;
    Clip c = p.tracks[ref->track].clips[ref->index];
    c.id = newId(c.isText() ? "txt" : c.isAdjust() ? "adj" : "clp");
    c.groupId.clear();
    c.start = c.end();
    const QString id = insertClip(p, std::move(c), ref->track);
    if (id.isEmpty())
        return std::nullopt;
    return id;
}

QStringList expandToGroups(const Project &p, const QStringList &ids)
{
    QStringList groups;
    for (const auto &t : p.tracks)
        for (const auto &c : t.clips)
            if (ids.contains(c.id) && !c.groupId.isEmpty() && !groups.contains(c.groupId))
                groups << c.groupId;
    QStringList out = ids;
    if (groups.isEmpty())
        return out;
    for (const auto &t : p.tracks)
        for (const auto &c : t.clips)
            if (groups.contains(c.groupId) && !out.contains(c.id))
                out << c.id;
    return out;
}

QString groupClips(Project &p, const QStringList &ids)
{
    if (ids.size() < 2)
        return {};
    const QString gid = newId("grp");
    int n = 0;
    for (auto &t : p.tracks)
        for (auto &c : t.clips)
            if (ids.contains(c.id)) {
                c.groupId = gid;
                ++n;
            }
    return n >= 2 ? gid : QString();
}

bool ungroupClips(Project &p, const QStringList &ids)
{
    const QStringList all = expandToGroups(p, ids);
    bool any = false;
    for (auto &t : p.tracks)
        for (auto &c : t.clips)
            if (all.contains(c.id) && !c.groupId.isEmpty()) {
                c.groupId.clear();
                any = true;
            }
    return any;
}

bool moveClips(Project &p, const QStringList &ids, Micros delta)
{
    if (ids.isEmpty() || delta == 0)
        return false;
    // Não deixa nenhum clipe ir para antes de zero.
    Micros minStart = std::numeric_limits<Micros>::max();
    for (const auto &t : p.tracks)
        for (const auto &c : t.clips)
            if (ids.contains(c.id)) {
                if (t.locked)
                    return false;
                minStart = std::min(minStart, c.start);
            }
    if (minStart == std::numeric_limits<Micros>::max())
        return false;
    delta = std::max(delta, -minStart);
    if (delta == 0)
        return false;

    struct Moving {
        int track;
        Clip clip;
    };
    std::vector<Moving> moving;
    for (int ti = 0; ti < static_cast<int>(p.tracks.size()); ++ti) {
        auto &clips = p.tracks[ti].clips;
        for (auto it = clips.begin(); it != clips.end();) {
            if (ids.contains(it->id)) {
                moving.push_back({ti, *it});
                it = clips.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Reinsere na ordem do tempo (importante na faixa principal magnética).
    std::sort(moving.begin(), moving.end(), [](const Moving &a, const Moving &b) { return a.clip.start < b.clip.start; });
    for (auto &m : moving) {
        m.clip.start += delta;
        const bool main = m.track == 0;
        if (main && p.mainTrackMagnet) {
            insertClip(p, std::move(m.clip), 0);
            continue;
        }
        Track &t = p.tracks[m.track];
        if (fitsInTrack(t, m.clip.start, m.clip.duration)) {
            t.clips.push_back(std::move(m.clip));
            sortTrack(t);
        } else {
            insertClip(p, std::move(m.clip), -1);
        }
    }
    packMainTrack(p);
    removeEmptyTracks(p);
    return true;
}

std::optional<QString> detachAudio(Project &p, const QString &clipId)
{
    auto ref = findClip(p, clipId);
    if (!ref)
        return std::nullopt;
    Clip &v = p.tracks[ref->track].clips[ref->index];
    const MediaAsset *a = v.hasMedia() ? p.asset(v.assetId) : nullptr;
    if (!a || a->kind != MediaKind::Video || !a->hasAudio || v.audioOnly || v.audioMuted || v.speed <= 0 ||
        p.tracks[ref->track].locked)
        return std::nullopt;
    Clip audio = v;
    audio.id = newId("clp");
    audio.groupId.clear();
    audio.audioOnly = true;
    // Só o que faz sentido para áudio.
    audio.transform = Transform{};
    audio.crop = Crop{};
    audio.color = ColorAdjust{};
    audio.mask = Mask{};
    audio.chroma = ChromaKey{};
    audio.blend = BlendMode::Normal;
    audio.transitionIn.reset();
    audio.animIn = audio.animOut = {};
    for (int i = 0; i < kPropCount; ++i)
        if (static_cast<Prop>(i) != Prop::Volume)
            audio.keyframes[i].clear();
    // O vídeo fica mudo; volume/fades passam para o clipe de áudio.
    v.audioMuted = true;
    v.keyframes[static_cast<int>(Prop::Volume)].clear();
    const int ti = ensureTrackFor(p, TrackKind::Audio, audio.start, audio.duration, -1);
    const QString id = audio.id;
    p.tracks[ti].clips.push_back(std::move(audio));
    sortTrack(p.tracks[ti]);
    return id;
}

Micros snap(const Project &p, Micros t, Micros threshold, const QStringList &ignoreClips, Micros playhead)
{
    Micros best = t;
    Micros bestDist = threshold + 1;
    auto consider = [&](Micros cand) {
        const Micros d = std::llabs(cand - t);
        if (d < bestDist) {
            bestDist = d;
            best = cand;
        }
    };
    consider(0);
    if (playhead >= 0)
        consider(playhead);
    for (const auto &m : p.markers)
        consider(m.t);
    for (const auto &track : p.tracks)
        for (const auto &c : track.clips) {
            if (ignoreClips.contains(c.id))
                continue;
            consider(c.start);
            consider(c.end());
        }
    return bestDist <= threshold ? best : t;
}


QString createCompound(Project &p, const QStringList &ids, const QString &name, CompoundError *error)
{
    auto fail = [&](CompoundError e) {
        if (error)
            *error = e;
        return QString();
    };
    if (error)
        *error = CompoundError::None;
    // Faixas envolvidas, de baixo para cima, e o trecho total.
    std::map<int, QStringList> byTrack;
    Micros t0 = std::numeric_limits<Micros>::max(), t1 = 0;
    for (const QString &id : ids) {
        const auto ref = findClip(p, id);
        if (!ref)
            continue;
        if (p.tracks[ref->track].locked)
            return fail(CompoundError::Locked);
        const Clip &c = p.tracks[ref->track].clips[ref->index];
        byTrack[ref->track].push_back(id);
        t0 = std::min(t0, c.start);
        t1 = std::max(t1, c.end());
    }
    if (byTrack.empty() || t1 <= t0)
        return fail(CompoundError::NoClips);
    const int lowest = byTrack.begin()->first;
    const bool onMain = isMainMagnet(p, lowest);
    int firstMainIndex = -1;
    if (onMain) {
        // Os clipes da principal precisam ser vizinhos e começar/terminar o trecho todo.
        const auto &main = p.tracks[lowest].clips;
        const auto &sel = byTrack[lowest];
        std::vector<int> idx;
        for (int i = 0; i < static_cast<int>(main.size()); ++i)
            if (sel.contains(main[i].id))
                idx.push_back(i);
        for (size_t k = 1; k < idx.size(); ++k)
            if (idx[k] != idx[k - 1] + 1)
                return fail(CompoundError::MainTrackGap);
        if (main[idx.front()].start != t0 || main[idx.back()].end() != t1)
            return fail(CompoundError::MainTrackGap);
        firstMainIndex = idx.front();
    }

    Sequence seq;
    seq.id = newId("seq");
    seq.name = name;
    for (const auto &[ti, list] : byTrack) {
        Track &src = p.tracks[ti];
        Track inner = src;
        inner.id = newId("trk");
        inner.clips.clear();
        inner.captions = false;
        for (auto it = src.clips.begin(); it != src.clips.end();) {
            if (list.contains(it->id)) {
                Clip c = std::move(*it);
                c.start -= t0;
                inner.clips.push_back(std::move(c));
                it = src.clips.erase(it);
            } else {
                ++it;
            }
        }
        seq.tracks.push_back(std::move(inner));
    }

    Clip cc;
    cc.id = newId("cmp");
    cc.type = ClipType::Compound;
    cc.compoundId = seq.id;
    cc.start = t0;
    cc.duration = t1 - t0;
    p.compounds.push_back(std::move(seq));

    const TrackKind kind = trackKindFor(p, cc);
    int target = lowest;
    if (onMain) {
        auto &main = p.tracks[lowest].clips;
        main.insert(main.begin() + firstMainIndex, cc);
    } else {
        if (p.tracks[target].kind != kind || !fitsInTrack(p.tracks[target], cc.start, cc.duration))
            target = ensureTrackFor(p, kind, cc.start, cc.duration, std::max(lowest, 1));
        p.tracks[target].clips.push_back(cc);
        sortTrack(p.tracks[target]);
    }
    removeEmptyTracks(p);
    return cc.id;
}

QStringList breakCompound(Project &p, const QString &clipId)
{
    const auto ref = findClip(p, clipId);
    if (!ref || p.tracks[ref->track].locked)
        return {};
    const Clip cc = p.tracks[ref->track].clips[ref->index];
    const Sequence *seqPtr = cc.isCompound() ? p.compound(cc.compoundId) : nullptr;
    if (!seqPtr)
        return {};
    const Sequence seq = *seqPtr;
    const int baseTrack = ref->track;
    const bool onMain = isMainMagnet(p, baseTrack);
    auto &baseClips = p.tracks[baseTrack].clips;
    baseClips.erase(baseClips.begin() + ref->index);

    // Só o trecho visível do composto, recortado como faria um corte nas bordas.
    const Micros winStart = cc.sourceIn, winEnd = cc.sourceIn + cc.duration;
    const Micros offset = cc.start - cc.sourceIn;
    QStringList out;
    int below = baseTrack; // cada faixa interna fica acima da anterior
    for (size_t k = 0; k < seq.tracks.size(); ++k) {
        std::vector<Clip> clips;
        for (Clip c : seq.tracks[k].clips) {
            if (c.end() <= winStart || c.start >= winEnd)
                continue;
            if (c.start < winStart) {
                const Micros delta = winStart - c.start;
                if ((c.hasMedia() || c.isCompound()) && !c.reverse)
                    c.sourceIn += static_cast<Micros>(delta * c.speed);
                c.start = winStart;
                c.duration -= delta;
                c.transitionIn.reset();
            }
            if (c.end() > winEnd)
                c.duration = winEnd - c.start;
            c.start += offset;
            clips.push_back(std::move(c));
        }
        if (clips.empty())
            continue;
        const TrackKind kind = seq.tracks[k].kind;
        auto fitsAll = [&](int ti) {
            if (p.tracks[ti].kind != kind || p.tracks[ti].locked)
                return false;
            for (const Clip &c : clips)
                if (!fitsInTrack(p.tracks[ti], c.start, c.duration))
                    return false;
            return true;
        };
        int ti = -1;
        if (k == 0 && (onMain ? kind == TrackKind::Video : fitsAll(baseTrack)))
            ti = baseTrack;
        else if (below + 1 < static_cast<int>(p.tracks.size()) && fitsAll(below + 1))
            ti = below + 1;
        else {
            // Faixa nova logo acima, para manter a ordem de empilhamento.
            Track t = createTrack(kind, seq.tracks[k].name);
            ti = std::max(below + 1, 1);
            p.tracks.insert(p.tracks.begin() + std::min<size_t>(ti, p.tracks.size()), std::move(t));
        }
        for (Clip &c : clips) {
            out << c.id;
            if (onMain && ti == baseTrack) {
                // Na principal magnética, entra no lugar do composto, em ordem.
                auto &main = p.tracks[ti].clips;
                const auto pos = std::find_if(main.begin(), main.end(), [&](const Clip &m) { return m.start >= c.start; });
                main.insert(pos, std::move(c));
            } else {
                p.tracks[ti].clips.push_back(std::move(c));
            }
        }
        sortTrack(p.tracks[ti]);
        below = ti;
    }
    if (onMain)
        packMainTrack(p);
    pruneCompounds(p);
    removeEmptyTracks(p);
    return out;
}

void pruneCompounds(Project &p)
{
    // Repete até estabilizar: uma sequência pode ser usada só por outra que também sumiu.
    for (bool changed = true; changed;) {
        changed = false;
        QSet<QString> used;
        p.forEachClip([&](const Clip &c) {
            if (c.isCompound())
                used.insert(c.compoundId);
        });
        const auto before = p.compounds.size();
        std::erase_if(p.compounds, [&](const Sequence &s) { return !used.contains(s.id); });
        changed = p.compounds.size() != before;
    }
}


std::optional<Project> compoundWorkspace(const Project &outer, const QString &seqId)
{
    const Sequence *seq = outer.compound(seqId);
    if (!seq)
        return std::nullopt;
    Project w = outer;
    w.tracks = seq->tracks;
    if (w.tracks.empty() || w.tracks.front().kind != TrackKind::Video)
        w.tracks.insert(w.tracks.begin(), createTrack(TrackKind::Video));
    w.markers.clear();
    w.inPoint = -1;
    w.outPoint = -1;
    w.mainTrackMagnet = false;
    return w;
}

void mergeCompoundWorkspace(Project &outer, const QString &seqId, const Project &inner)
{
    // Ajustes do projeto valem para tudo, mesmo mudados de dentro de um composto.
    outer.name = inner.name;
    outer.width = inner.width;
    outer.height = inner.height;
    outer.fps = inner.fps;
    outer.background = inner.background;
    outer.backgroundBlur = inner.backgroundBlur;
    // Mídias e sequências: atualiza ou acrescenta pelo id, sem apagar o que só é usado fora.
    for (const auto &[id, a] : inner.assets)
        outer.assets[id] = a;
    for (const Sequence &s : inner.compounds) {
        auto it = std::find_if(outer.compounds.begin(), outer.compounds.end(), [&](const Sequence &o) { return o.id == s.id; });
        if (it == outer.compounds.end())
            outer.compounds.push_back(s);
        else if (s.id != seqId)
            *it = s;
    }
    auto seq = std::find_if(outer.compounds.begin(), outer.compounds.end(), [&](const Sequence &o) { return o.id == seqId; });
    if (seq == outer.compounds.end())
        return;
    seq->tracks.clear();
    for (const Track &t : inner.tracks)
        if (!t.clips.empty())
            seq->tracks.push_back(t);
    const Micros len = seq->duration();

    // Clipes compostos que mostram esta sequência cabem no novo conteúdo.
    const Micros minLen = outer.frameDuration();
    auto fit = [&](std::vector<Track> &tracks) {
        for (Track &t : tracks) {
            std::erase_if(t.clips, [&](const Clip &c) { return c.isCompound() && c.compoundId == seqId && len <= 0; });
            for (Clip &c : t.clips) {
                if (!c.isCompound() || c.compoundId != seqId)
                    continue;
                c.sourceIn = std::clamp<Micros>(c.sourceIn, 0, std::max<Micros>(0, len - minLen));
                c.duration = std::clamp<Micros>(c.duration, std::min(minLen, len), len - c.sourceIn);
            }
        }
    };
    fit(outer.tracks);
    for (Sequence &s : outer.compounds)
        if (s.id != seqId)
            fit(s.tracks);
    packMainTrack(outer);
    removeEmptyTracks(outer);
    pruneCompounds(outer);
}


bool setClipMedia(Project &p, const QString &clipId, const MediaAsset &asset)
{
    const auto ref = findClip(p, clipId);
    if (!ref || p.tracks[ref->track].locked)
        return false;
    Clip &c = p.tracks[ref->track].clips[ref->index];
    if (!c.hasMedia())
        return false;
    if (!p.assets.count(asset.id))
        p.assets.emplace(asset.id, asset);
    c.assetId = asset.id;
    c.slot.clear();
    c.sourceIn = 0;
    c.reverse = false;
    c.audioOnly = asset.kind == MediaKind::Audio ? false : c.audioOnly;
    const bool timed = asset.kind != MediaKind::Image && asset.duration > 0;
    if (timed && c.speed > 0) {
        const Micros fit = static_cast<Micros>(asset.duration / c.speed);
        if (fit < c.duration)
            c.duration = std::max(p.frameDuration(), fit);
    }
    clampAllKeyframes(c);
    packMainTrack(p);
    return true;
}

QStringList emptySlots(const Project &p)
{
    struct Ref {
        Micros start;
        int track;
        QString id;
    };
    std::vector<Ref> refs;
    for (int ti = 0; ti < static_cast<int>(p.tracks.size()); ++ti)
        for (const Clip &c : p.tracks[ti].clips)
            if (c.isEmptySlot())
                refs.push_back({c.start, ti, c.id});
    std::sort(refs.begin(), refs.end(), [](const Ref &a, const Ref &b) {
        return a.start != b.start ? a.start < b.start : a.track < b.track;
    });
    QStringList out;
    for (const Ref &r : refs)
        out << r.id;
    return out;
}

void makeTemplate(Project &p, const std::function<QString(MediaKind, int)> &label)
{
    std::map<MediaKind, int> counters;
    auto convert = [&](std::vector<Track> &tracks) {
        // Numeração em ordem de tempo, como o usuário vê.
        std::vector<Clip *> clips;
        for (Track &t : tracks)
            for (Clip &c : t.clips)
                if (c.hasMedia() && !c.assetId.isEmpty())
                    clips.push_back(&c);
        std::stable_sort(clips.begin(), clips.end(), [](const Clip *a, const Clip *b) { return a->start < b->start; });
        for (Clip *c : clips) {
            const MediaAsset *a = p.asset(c->assetId);
            const MediaKind kind = a ? a->kind : MediaKind::Video;
            c->slot = label(kind, ++counters[kind]);
            c->assetId.clear();
            c->sourceIn = 0;
            c->denoise = 0;
            c->voiceIsolation = 0;
            c->removeBackground = false;
            c->stabilize = 0;
        }
    };
    convert(p.tracks);
    for (Sequence &s : p.compounds)
        convert(s.tracks);
    p.assets.clear();
    p.markers.clear();
    p.inPoint = p.outPoint = -1;
}

} // namespace clv::edit
