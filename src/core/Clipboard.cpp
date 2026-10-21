// SPDX-License-Identifier: GPL-3.0-or-later
#include "Clipboard.h"

#include "Edit.h"
#include "Keyframes.h"
#include "Serialize.h"

#include <QHash>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <functional>

namespace clv {

namespace {

const char *trackKindName(TrackKind k)
{
    switch (k) {
    case TrackKind::Video: return "video";
    case TrackKind::Audio: return "audio";
    case TrackKind::Text: return "text";
    case TrackKind::Adjust: return "adjust";
    }
    return "video";
}

TrackKind trackKindFromName(const QString &s)
{
    if (s == QLatin1String("audio"))
        return TrackKind::Audio;
    if (s == QLatin1String("text"))
        return TrackKind::Text;
    if (s == QLatin1String("adjust"))
        return TrackKind::Adjust;
    return TrackKind::Video;
}

} // namespace

Clipboard copyClips(const Project &p, const QStringList &ids)
{
    Clipboard c;
    QSet<QString> assetIds, seqIds;
    // Mídias e sequências usadas, descendo por compostos dentro de compostos.
    std::function<void(const Clip &)> collect = [&](const Clip &clip) {
        if (clip.hasMedia())
            assetIds.insert(clip.assetId);
        const Sequence *seq = clip.isCompound() && !seqIds.contains(clip.compoundId) ? p.compound(clip.compoundId) : nullptr;
        if (!seq)
            return;
        seqIds.insert(seq->id);
        c.compounds.push_back(*seq);
        for (const auto &t : seq->tracks)
            for (const auto &inner : t.clips)
                collect(inner);
    };
    for (int ti = 0; ti < static_cast<int>(p.tracks.size()); ++ti)
        for (const Clip &clip : p.tracks[ti].clips)
            if (ids.contains(clip.id)) {
                c.items.push_back({ti, p.tracks[ti].kind, clip});
                collect(clip);
            }
    for (const QString &id : assetIds)
        if (const MediaAsset *a = p.asset(id))
            c.assets.push_back(*a);
    return c;
}

QByteArray clipboardToJson(const Clipboard &c)
{
    QJsonArray items, assets;
    for (const auto &it : c.items)
        items.append(QJsonObject{{"track", it.track}, {"kind", trackKindName(it.kind)}, {"clip", clipToJson(it.clip)}});
    for (const auto &a : c.assets)
        assets.append(assetToJson(a));
    QJsonArray compounds;
    for (const auto &s : c.compounds)
        compounds.append(sequenceToJson(s));
    return QJsonDocument(QJsonObject{{"format", "clivcut-clips"}, {"items", items}, {"assets", assets}, {"compounds", compounds}})
        .toJson(QJsonDocument::Compact);
}

bool clipboardFromJson(const QByteArray &data, Clipboard &out)
{
    const auto doc = QJsonDocument::fromJson(data);
    if (!doc.isObject() || doc.object().value("format").toString() != QLatin1String("clivcut-clips"))
        return false;
    Clipboard c;
    for (const auto &v : doc.object().value("items").toArray()) {
        const auto o = v.toObject();
        c.items.push_back({o.value("track").toInt(), trackKindFromName(o.value("kind").toString()),
                           clipFromJson(o.value("clip").toObject())});
    }
    for (const auto &v : doc.object().value("assets").toArray())
        c.assets.push_back(assetFromJson(v.toObject()));
    for (const auto &v : doc.object().value("compounds").toArray())
        c.compounds.push_back(sequenceFromJson(v.toObject()));
    out = std::move(c);
    return !out.empty();
}

QStringList pasteClips(Project &p, const Clipboard &c, Micros at)
{
    if (c.empty())
        return {};
    // Mídias: reaproveita pelo id ou pelo caminho; senão adiciona.
    QHash<QString, QString> assetMap;
    for (const MediaAsset &a : c.assets) {
        if (p.assets.count(a.id)) {
            assetMap.insert(a.id, a.id);
            continue;
        }
        QString found;
        for (const auto &[id, existing] : p.assets)
            if (existing.path == a.path)
                found = id;
        if (found.isEmpty()) {
            MediaAsset copy = a;
            copy.id = newId("ast");
            found = copy.id;
            p.assets.emplace(found, copy);
        }
        assetMap.insert(a.id, found);
    }

    // Sequências: as que o projeto já tem (mesmo id) são reaproveitadas; as outras entram
    // com as mídias internas remapeadas.
    for (Sequence s : c.compounds) {
        if (p.compound(s.id))
            continue;
        for (auto &t : s.tracks)
            for (auto &inner : t.clips)
                if (inner.hasMedia())
                    inner.assetId = assetMap.value(inner.assetId, inner.assetId);
        p.compounds.push_back(std::move(s));
    }

    Micros origin = c.items.front().clip.start;
    for (const auto &it : c.items)
        origin = std::min(origin, it.clip.start);

    // Ordem: faixa e tempo, para a faixa principal magnética inserir em sequência.
    std::vector<ClipboardItem> items = c.items;
    std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
        return a.track != b.track ? a.track < b.track : a.clip.start < b.clip.start;
    });

    QHash<QString, QString> groupMap;
    QStringList created;
    for (ClipboardItem it : items) {
        Clip clip = it.clip;
        clip.id = newId(clip.isText() ? "txt" : clip.isAdjust() ? "adj" : "clp");
        clip.start = std::max<Micros>(0, at + (clip.start - origin));
        if (clip.hasMedia())
            clip.assetId = assetMap.value(clip.assetId, clip.assetId);
        if (!clip.groupId.isEmpty()) {
            if (!groupMap.contains(clip.groupId))
                groupMap.insert(clip.groupId, newId("grp"));
            clip.groupId = groupMap.value(clip.groupId);
        }
        // Mesma faixa se for do mesmo tipo e tiver espaço; senão onde couber.
        int target = -1;
        if (it.track < static_cast<int>(p.tracks.size()) && p.tracks[it.track].kind == it.kind &&
            (it.track == 0 ? p.mainTrackMagnet || edit::fitsInTrack(p.tracks[0], clip.start, clip.duration)
                           : edit::fitsInTrack(p.tracks[it.track], clip.start, clip.duration)))
            target = it.track;
        if (target < 0 && it.track == 0 && it.kind == TrackKind::Video)
            target = 0; // vídeo da faixa principal volta para a principal (o ímã abre espaço)
        const QString id = edit::insertClip(p, std::move(clip), target);
        if (!id.isEmpty())
            created << id;
    }
    return created;
}

void pasteAttributes(Clip &dst, const Clip &src)
{
    // Transformação e animações valem para qualquer clipe visual.
    if (!dst.isAdjust() && !src.isAdjust()) {
        dst.transform = src.transform;
        dst.animIn = src.animIn;
        dst.animOut = src.animOut;
        for (Prop p : {Prop::X, Prop::Y, Prop::Scale, Prop::Rotation, Prop::Opacity}) {
            auto kfs = src.keyframes[static_cast<int>(p)];
            clampKeyframes(kfs, dst.duration);
            dst.keyframes[static_cast<int>(p)] = kfs;
        }
    }
    if (dst.isText() && src.isText()) {
        dst.style = src.style;
        return;
    }
    if (dst.isText() || src.isText())
        return;
    // Mídia/ajuste: cor, filtro, LUT, máscara, mesclagem, chroma, crop, áudio.
    dst.color = src.color;
    dst.mask = src.mask;
    for (Prop p : {Prop::Brightness, Prop::Contrast, Prop::Saturation}) {
        auto kfs = src.keyframes[static_cast<int>(p)];
        clampKeyframes(kfs, dst.duration);
        dst.keyframes[static_cast<int>(p)] = kfs;
    }
    if (dst.hasMedia() && src.hasMedia()) {
        dst.blend = src.blend;
        dst.chroma = src.chroma;
        dst.crop = src.crop;
        dst.volume = src.volume;
        dst.fadeIn = std::min(src.fadeIn, dst.duration);
        dst.fadeOut = std::min(src.fadeOut, dst.duration);
    }
}

} // namespace clv
