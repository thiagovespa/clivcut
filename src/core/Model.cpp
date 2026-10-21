// SPDX-License-Identifier: GPL-3.0-or-later
#include "Model.h"

#include "Serialize.h"

#include <QJsonDocument>

#include <QCoreApplication>

#include <QRandomGenerator>

#include <algorithm>
#include <cmath>

namespace clv {

Micros Project::duration() const
{
    Micros end = 0;
    for (const auto &t : tracks)
        for (const auto &c : t.clips)
            end = std::max(end, c.end());
    return end;
}

Micros Sequence::duration() const
{
    Micros end = 0;
    for (const auto &t : tracks)
        for (const auto &c : t.clips)
            end = std::max(end, c.end());
    return end;
}

const Sequence *Project::compound(const QString &id) const
{
    if (id.isEmpty())
        return nullptr;
    auto it = std::find_if(compounds.begin(), compounds.end(), [&](const Sequence &s) { return s.id == id; });
    return it == compounds.end() ? nullptr : &*it;
}

const MediaAsset *Project::asset(const QString &id) const
{
    auto it = assets.find(id);
    return it == assets.end() ? nullptr : &it->second;
}

Micros Project::snapToFrame(Micros t) const
{
    const double frames = std::round(static_cast<double>(t) * fps / kSecond);
    return static_cast<Micros>(std::llround(frames * kSecond / fps));
}

QString newId(const char *prefix)
{
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    QString s = QString::fromLatin1(prefix) + QLatin1Char('_');
    auto *rng = QRandomGenerator::global();
    for (int i = 0; i < 12; ++i)
        s += QLatin1Char(alphabet[rng->bounded(36)]);
    return s;
}

Project createProject(const QString &name, int width, int height, double fps)
{
    Project p;
    p.id = newId("prj");
    p.name = name;
    p.width = width;
    p.height = height;
    p.fps = fps;
    p.tracks.push_back(createTrack(TrackKind::Video, QCoreApplication::translate("Track", "Main")));
    return p;
}

Track createTrack(TrackKind kind, const QString &name)
{
    Track t;
    t.id = newId("trk");
    t.kind = kind;
    if (!name.isEmpty()) {
        t.name = name;
    } else {
        switch (kind) {
        case TrackKind::Video: t.name = QCoreApplication::translate("Track", "Video"); break;
        case TrackKind::Audio: t.name = QCoreApplication::translate("Track", "Audio"); break;
        case TrackKind::Text: t.name = QCoreApplication::translate("Track", "Text"); break;
        case TrackKind::Adjust: t.name = QCoreApplication::translate("Track", "Filters"); break;
        }
    }
    return t;
}

Clip createMediaClip(const MediaAsset &asset, Micros start)
{
    Clip c;
    c.id = newId("clp");
    c.type = ClipType::Media;
    c.assetId = asset.id;
    c.start = start;
    c.duration = (asset.kind == MediaKind::Image || asset.duration <= 0) ? kImageDefaultDuration : asset.duration;
    return c;
}

Clip createTextClip(const QString &text, Micros start, Micros duration)
{
    Clip c;
    c.id = newId("txt");
    c.type = ClipType::Text;
    c.text = text;
    c.start = start;
    c.duration = duration;
    c.transform.y = 0.6;
    return c;
}

Clip createAdjustClip(Micros start, Micros duration, const QString &filter)
{
    Clip c;
    c.id = newId("adj");
    c.type = ClipType::Adjust;
    c.start = start;
    c.duration = duration;
    c.color.filter = filter;
    return c;
}

bool ColorAdjust::hasGrade() const
{
    for (const auto &c : curves)
        if (!c.empty())
            return true;
    for (const auto &b : hsl)
        if (!b.isZero())
            return true;
    return !lift.isZero() || !gamma.isZero() || !gain.isZero();
}

QString ColorAdjust::gradeKey() const
{
    if (!hasGrade())
        return {};
    return QStringLiteral("grade:") + QString::fromUtf8(QJsonDocument(gradeToJson(*this)).toJson(QJsonDocument::Compact));
}

} // namespace clv
