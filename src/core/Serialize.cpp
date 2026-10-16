// SPDX-License-Identifier: GPL-3.0-or-later
#include "Serialize.h"

#include "Keyframes.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include <array>
#include <utility>

namespace clv {

namespace {

template <typename E, std::size_t N>
QString enumToName(const std::array<std::pair<E, const char *>, N> &table, E v)
{
    for (const auto &[e, n] : table)
        if (e == v)
            return QString::fromLatin1(n);
    return QString::fromLatin1(table[0].second);
}

template <typename E, std::size_t N>
E enumFromName(const std::array<std::pair<E, const char *>, N> &table, const QString &s)
{
    for (const auto &[e, n] : table)
        if (s == QLatin1String(n))
            return e;
    return table[0].first;
}

constexpr std::array<std::pair<Easing, const char *>, 6> kEasings{{
    {Easing::Linear, "linear"},
    {Easing::EaseIn, "easeIn"},
    {Easing::EaseOut, "easeOut"},
    {Easing::EaseInOut, "easeInOut"},
    {Easing::Hold, "hold"},
    {Easing::Bezier, "bezier"},
}};

constexpr std::array<std::pair<BlendMode, const char *>, 10> kBlends{{
    {BlendMode::Normal, "normal"},
    {BlendMode::Multiply, "multiply"},
    {BlendMode::Screen, "screen"},
    {BlendMode::Overlay, "overlay"},
    {BlendMode::Darken, "darken"},
    {BlendMode::Lighten, "lighten"},
    {BlendMode::ColorDodge, "colorDodge"},
    {BlendMode::ColorBurn, "colorBurn"},
    {BlendMode::SoftLight, "softLight"},
    {BlendMode::Difference, "difference"},
}};

constexpr std::array<std::pair<TransitionKind, const char *>, 17> kTransitions{{
    {TransitionKind::Dissolve, "dissolve"},
    {TransitionKind::FadeBlack, "fadeBlack"},
    {TransitionKind::WipeLeft, "wipeLeft"},
    {TransitionKind::WipeRight, "wipeRight"},
    {TransitionKind::SlideLeft, "slideLeft"},
    {TransitionKind::SlideRight, "slideRight"},
    {TransitionKind::CircleOpen, "circleOpen"},
    {TransitionKind::ZoomIn, "zoomIn"},
    {TransitionKind::SlideUp, "slideUp"},
    {TransitionKind::SlideDown, "slideDown"},
    {TransitionKind::WipeUp, "wipeUp"},
    {TransitionKind::WipeDown, "wipeDown"},
    {TransitionKind::Spin, "spin"},
    {TransitionKind::ZoomOut, "zoomOut"},
    {TransitionKind::FlashWhite, "flashWhite"},
    {TransitionKind::Glitch, "glitch"},
    {TransitionKind::BlurDissolve, "blurDissolve"},
}};

constexpr std::array<std::pair<AnimationKind, const char *>, 7> kAnimations{{
    {AnimationKind::None, "none"},
    {AnimationKind::Fade, "fade"},
    {AnimationKind::Zoom, "zoom"},
    {AnimationKind::SlideUp, "slideUp"},
    {AnimationKind::SlideDown, "slideDown"},
    {AnimationKind::SlideLeft, "slideLeft"},
    {AnimationKind::SlideRight, "slideRight"},
}};

constexpr std::array<std::pair<MediaKind, const char *>, 3> kMediaKinds{{
    {MediaKind::Video, "video"},
    {MediaKind::Audio, "audio"},
    {MediaKind::Image, "image"},
}};

constexpr std::array<std::pair<TrackKind, const char *>, 4> kTrackKinds{{
    {TrackKind::Video, "video"},
    {TrackKind::Audio, "audio"},
    {TrackKind::Text, "text"},
    {TrackKind::Adjust, "adjust"},
}};

constexpr std::array<std::pair<ClipType, const char *>, 3> kClipTypes{{
    {ClipType::Media, "media"},
    {ClipType::Text, "text"},
    {ClipType::Adjust, "adjust"},
}};

constexpr std::array<std::pair<MaskShape, const char *>, 5> kMaskShapes{{
    {MaskShape::None, "none"},
    {MaskShape::Linear, "linear"},
    {MaskShape::Mirror, "mirror"},
    {MaskShape::Circle, "circle"},
    {MaskShape::Rectangle, "rectangle"},
}};

QJsonObject maskToJson(const Mask &m)
{
    return {{"shape", enumToName(kMaskShapes, m.shape)}, {"x", m.x}, {"y", m.y}, {"width", m.width},
            {"height", m.height}, {"rotation", m.rotation}, {"feather", m.feather}, {"roundness", m.roundness},
            {"invert", m.invert}};
}

Mask maskFromJson(const QJsonObject &o)
{
    Mask m;
    m.shape = enumFromName(kMaskShapes, o.value("shape").toString());
    m.x = o.value("x").toDouble(0.5);
    m.y = o.value("y").toDouble(0.5);
    m.width = o.value("width").toDouble(0.5);
    m.height = o.value("height").toDouble(0.5);
    m.rotation = o.value("rotation").toDouble();
    m.feather = o.value("feather").toDouble(0.05);
    m.roundness = o.value("roundness").toDouble();
    m.invert = o.value("invert").toBool();
    return m;
}

} // namespace

QJsonObject gradeToJson(const ColorAdjust &c)
{
    QJsonObject o;
    static const char *names[] = {"master", "red", "green", "blue"};
    QJsonObject curves;
    for (int ch = 0; ch < 4; ++ch) {
        if (c.curves[ch].empty())
            continue;
        QJsonArray pts;
        for (const auto &p : c.curves[ch])
            pts.append(QJsonArray{p.x, p.y});
        curves.insert(QLatin1String(names[ch]), pts);
    }
    if (!curves.isEmpty())
        o.insert("curves", curves);
    bool anyHsl = false;
    QJsonArray hsl;
    for (const auto &b : c.hsl) {
        hsl.append(QJsonArray{b.hue, b.saturation, b.luminance});
        anyHsl = anyHsl || !b.isZero();
    }
    if (anyHsl)
        o.insert("hsl", hsl);
    QJsonObject wheels;
    const std::pair<const char *, const ColorWheel *> ws[] = {{"lift", &c.lift}, {"gamma", &c.gamma}, {"gain", &c.gain}};
    for (const auto &[name, w] : ws)
        if (!w->isZero())
            wheels.insert(QLatin1String(name), QJsonArray{w->x, w->y, w->luma});
    if (!wheels.isEmpty())
        o.insert("wheels", wheels);
    return o;
}

void gradeFromJson(const QJsonObject &o, ColorAdjust &c)
{
    static const char *names[] = {"master", "red", "green", "blue"};
    const QJsonObject curves = o.value("curves").toObject();
    for (int ch = 0; ch < 4; ++ch) {
        c.curves[ch].clear();
        for (const auto &v : curves.value(QLatin1String(names[ch])).toArray()) {
            const QJsonArray p = v.toArray();
            c.curves[ch].push_back({std::clamp(p.at(0).toDouble(), 0.0, 1.0), std::clamp(p.at(1).toDouble(), 0.0, 1.0)});
        }
    }
    const QJsonArray hsl = o.value("hsl").toArray();
    for (int i = 0; i < kHslBands; ++i) {
        const QJsonArray b = hsl.at(i).toArray();
        c.hsl[i] = {std::clamp(b.at(0).toDouble(), -1.0, 1.0), std::clamp(b.at(1).toDouble(), -1.0, 1.0),
                    std::clamp(b.at(2).toDouble(), -1.0, 1.0)};
    }
    const QJsonObject wheels = o.value("wheels").toObject();
    const std::pair<const char *, ColorWheel *> ws[] = {{"lift", &c.lift}, {"gamma", &c.gamma}, {"gain", &c.gain}};
    for (const auto &[name, w] : ws) {
        const QJsonArray a = wheels.value(QLatin1String(name)).toArray();
        *w = {a.at(0).toDouble(), a.at(1).toDouble(), std::clamp(a.at(2).toDouble(), -1.0, 1.0)};
    }
}

namespace {

QJsonObject colorToJson(const ColorAdjust &c)
{
    QJsonObject o{{"brightness", c.brightness}, {"contrast", c.contrast}, {"saturation", c.saturation},
                  {"temperature", c.temperature}, {"exposure", c.exposure}, {"tint", c.tint},
                  {"highlights", c.highlights}, {"shadows", c.shadows}, {"vignette", c.vignette},
                  {"sharpen", c.sharpen}};
    if (!c.filter.isEmpty()) {
        o.insert("filter", c.filter);
        o.insert("filterIntensity", c.filterIntensity);
    }
    if (!c.lutPath.isEmpty()) {
        o.insert("lut", c.lutPath);
        o.insert("lutIntensity", c.lutIntensity);
    }
    const QJsonObject grade = gradeToJson(c);
    for (auto it = grade.begin(); it != grade.end(); ++it)
        o.insert(it.key(), it.value());
    return o;
}

ColorAdjust colorFromJson(const QJsonObject &col)
{
    ColorAdjust c;
    c.brightness = col.value("brightness").toDouble();
    c.contrast = col.value("contrast").toDouble();
    c.saturation = col.value("saturation").toDouble();
    c.temperature = col.value("temperature").toDouble();
    c.exposure = col.value("exposure").toDouble();
    c.tint = col.value("tint").toDouble();
    c.highlights = col.value("highlights").toDouble();
    c.shadows = col.value("shadows").toDouble();
    c.vignette = col.value("vignette").toDouble();
    c.sharpen = col.value("sharpen").toDouble();
    c.filter = col.value("filter").toString();
    c.filterIntensity = col.value("filterIntensity").toDouble(0.8);
    c.lutPath = col.value("lut").toString();
    c.lutIntensity = col.value("lutIntensity").toDouble(1);
    gradeFromJson(col, c);
    return c;
}

// Micros como double no JSON: exato até 2^53 µs (~285 anos).
QJsonValue us(Micros v) { return QJsonValue(static_cast<double>(v)); }
Micros toUs(const QJsonValue &v, Micros def = 0) { return v.isDouble() ? static_cast<Micros>(v.toDouble()) : def; }

QJsonObject transformToJson(const Transform &t)
{
    return {{"x", t.x}, {"y", t.y}, {"scale", t.scale}, {"rotation", t.rotation},
            {"opacity", t.opacity}, {"flipH", t.flipH}, {"flipV", t.flipV}};
}

Transform transformFromJson(const QJsonObject &o)
{
    Transform t;
    t.x = o.value("x").toDouble(0);
    t.y = o.value("y").toDouble(0);
    t.scale = o.value("scale").toDouble(1);
    t.rotation = o.value("rotation").toDouble(0);
    t.opacity = o.value("opacity").toDouble(1);
    t.flipH = o.value("flipH").toBool();
    t.flipV = o.value("flipV").toBool();
    return t;
}

QJsonObject animToJson(const ClipAnimation &a)
{
    return {{"kind", animationName(a.kind)}, {"duration", us(a.duration)}};
}

ClipAnimation animFromJson(const QJsonValue &v)
{
    ClipAnimation a;
    const auto o = v.toObject();
    a.kind = animationFromName(o.value("kind").toString());
    a.duration = toUs(o.value("duration"), a.duration);
    return a;
}

} // namespace

QJsonObject clipToJson(const Clip &c)
{
    QJsonObject o{
        {"id", c.id},
        {"type", enumToName(kClipTypes, c.type)},
        {"group", c.groupId},
        {"start", us(c.start)},
        {"duration", us(c.duration)},
        {"transform", transformToJson(c.transform)},
    };
    QJsonObject kfs;
    for (int i = 0; i < kPropCount; ++i) {
        const auto &list = c.keyframes[i];
        if (list.empty())
            continue;
        QJsonArray arr;
        for (const auto &k : list) {
            QJsonObject ko{{"t", us(k.t)}, {"v", k.v}, {"ease", easingName(k.ease)}};
            if (k.ease == Easing::Bezier)
                ko.insert("bezier", QJsonArray{k.bx1, k.by1, k.bx2, k.by2});
            arr.append(ko);
        }
        kfs.insert(QString::fromLatin1(propName(static_cast<Prop>(i))), arr);
    }
    if (!kfs.isEmpty())
        o.insert("keyframes", kfs);
    if (c.animIn.kind != AnimationKind::None)
        o.insert("animIn", animToJson(c.animIn));
    if (c.animOut.kind != AnimationKind::None)
        o.insert("animOut", animToJson(c.animOut));

    if (c.mask.active())
        o.insert("mask", maskToJson(c.mask));
    if (c.isAdjust()) {
        o.insert("color", colorToJson(c.color));
        return o;
    }
    if (c.isText()) {
        const auto &s = c.style;
        o.insert("text", c.text);
        o.insert("style", QJsonObject{
                              {"fontFamily", s.fontFamily},
                              {"fontSize", s.fontSize},
                              {"bold", s.bold},
                              {"italic", s.italic},
                              {"color", s.color},
                              {"strokeColor", s.strokeColor},
                              {"strokeWidth", s.strokeWidth},
                              {"background", s.background},
                              {"align", s.align},
                              {"shadow", s.shadow},
                              {"highlightColor", s.highlightColor},
                              {"curve", s.curve},
                              {"shape", s.shape},
                              {"shapeColor", s.shapeColor},
                              {"shapeWidth", s.shapeWidth},
                              {"shapeHeight", s.shapeHeight},
                          });
        return o;
    }

    o.insert("assetId", c.assetId);
    o.insert("sourceIn", us(c.sourceIn));
    o.insert("speed", c.speed);
    if (c.speedCurve.size() >= 2) {
        QJsonArray curve;
        for (const auto &pt : c.speedCurve)
            curve.append(QJsonArray{pt.u, pt.v});
        o.insert("speedCurve", curve);
    }
    o.insert("reverse", c.reverse);
    o.insert("volume", c.volume);
    if (c.audioMuted)
        o.insert("audioMuted", true);
    if (c.gainDb != 0)
        o.insert("gainDb", c.gainDb);
    if (c.denoise > 0)
        o.insert("denoise", c.denoise);
    if (c.removeBackground)
        o.insert("removeBackground", true);
    if (!c.keepPitch)
        o.insert("keepPitch", false);
    if (c.voiceIsolation > 0)
        o.insert("voiceIsolation", c.voiceIsolation);
    if (c.ducking > 0)
        o.insert("ducking", c.ducking);
    if (!c.voiceEffect.isEmpty())
        o.insert("voiceEffect", c.voiceEffect);
    if (!c.effect.id.isEmpty())
        o.insert("effect", QJsonObject{{"id", c.effect.id}, {"intensity", c.effect.intensity}});
    if (c.audioOnly)
        o.insert("audioOnly", true);
    o.insert("fadeIn", us(c.fadeIn));
    o.insert("fadeOut", us(c.fadeOut));
    o.insert("blend", blendModeName(c.blend));
    if (!c.crop.isIdentity())
        o.insert("crop", QJsonObject{{"left", c.crop.left}, {"top", c.crop.top},
                                     {"right", c.crop.right}, {"bottom", c.crop.bottom}});
    o.insert("color", colorToJson(c.color));
    if (c.chroma.enabled)
        o.insert("chroma", QJsonObject{{"color", c.chroma.color},
                                       {"similarity", c.chroma.similarity},
                                       {"smoothness", c.chroma.smoothness}});
    if (c.transitionIn)
        o.insert("transitionIn", QJsonObject{{"kind", transitionName(c.transitionIn->kind)},
                                             {"duration", us(c.transitionIn->duration)}});
    return o;
}

Clip clipFromJson(const QJsonObject &o)
{
    Clip c;
    c.id = o.value("id").toString();
    c.type = enumFromName(kClipTypes, o.value("type").toString());
    c.groupId = o.value("group").toString();
    c.start = toUs(o.value("start"));
    c.duration = toUs(o.value("duration"));
    c.transform = transformFromJson(o.value("transform").toObject());
    const auto kfs = o.value("keyframes").toObject();
    for (int i = 0; i < kPropCount; ++i) {
        const auto arr = kfs.value(QString::fromLatin1(propName(static_cast<Prop>(i)))).toArray();
        for (const auto &v : arr) {
            const auto k = v.toObject();
            Keyframe kf{toUs(k.value("t")), k.value("v").toDouble(), easingFromName(k.value("ease").toString())};
            const auto b = k.value("bezier").toArray();
            if (b.size() == 4) {
                kf.bx1 = b[0].toDouble();
                kf.by1 = b[1].toDouble();
                kf.bx2 = b[2].toDouble();
                kf.by2 = b[3].toDouble();
            }
            c.keyframes[i].push_back(kf);
        }
    }
    if (o.contains("animIn"))
        c.animIn = animFromJson(o.value("animIn"));
    if (o.contains("animOut"))
        c.animOut = animFromJson(o.value("animOut"));

    if (o.contains("mask"))
        c.mask = maskFromJson(o.value("mask").toObject());
    if (c.isAdjust()) {
        c.color = colorFromJson(o.value("color").toObject());
        return c;
    }
    if (c.isText()) {
        c.text = o.value("text").toString();
        const auto s = o.value("style").toObject();
        TextStyle d;
        c.style.fontFamily = s.value("fontFamily").toString(d.fontFamily);
        c.style.fontSize = s.value("fontSize").toDouble(d.fontSize);
        c.style.bold = s.value("bold").toBool(d.bold);
        c.style.italic = s.value("italic").toBool(d.italic);
        c.style.color = s.value("color").toString(d.color);
        c.style.strokeColor = s.value("strokeColor").toString(d.strokeColor);
        c.style.strokeWidth = s.value("strokeWidth").toDouble(d.strokeWidth);
        c.style.background = s.value("background").toString();
        c.style.align = s.value("align").toInt(d.align);
        c.style.shadow = s.value("shadow").toBool(d.shadow);
        c.style.highlightColor = s.value("highlightColor").toString(d.highlightColor);
        c.style.curve = std::clamp(s.value("curve").toDouble(), -1.0, 1.0);
        c.style.shape = s.value("shape").toString();
        c.style.shapeColor = s.value("shapeColor").toString(d.shapeColor);
        c.style.shapeWidth = std::max(4.0, s.value("shapeWidth").toDouble(d.shapeWidth));
        c.style.shapeHeight = std::max(4.0, s.value("shapeHeight").toDouble(d.shapeHeight));
        return c;
    }

    c.assetId = o.value("assetId").toString();
    c.sourceIn = toUs(o.value("sourceIn"));
    c.speed = o.value("speed").toDouble(1);
    for (const auto &v : o.value("speedCurve").toArray()) {
        const auto pt = v.toArray();
        c.speedCurve.push_back({pt.at(0).toDouble(), pt.at(1).toDouble(1)});
    }
    c.reverse = o.value("reverse").toBool();
    c.volume = o.value("volume").toDouble(1);
    c.audioMuted = o.value("audioMuted").toBool();
    c.gainDb = o.value("gainDb").toDouble();
    c.denoise = o.value("denoise").toDouble();
    c.removeBackground = o.value("removeBackground").toBool();
    c.keepPitch = o.value("keepPitch").toBool(true);
    c.voiceIsolation = std::clamp(o.value("voiceIsolation").toDouble(), 0.0, 1.0);
    c.ducking = std::clamp(o.value("ducking").toDouble(), 0.0, 1.0);
    c.voiceEffect = o.value("voiceEffect").toString();
    c.effect.id = o.value("effect").toObject().value("id").toString();
    c.effect.intensity = std::clamp(o.value("effect").toObject().value("intensity").toDouble(0.6), 0.0, 1.0);
    c.audioOnly = o.value("audioOnly").toBool();
    c.fadeIn = toUs(o.value("fadeIn"));
    c.fadeOut = toUs(o.value("fadeOut"));
    c.blend = blendModeFromName(o.value("blend").toString());
    const auto crop = o.value("crop").toObject();
    c.crop = {crop.value("left").toDouble(), crop.value("top").toDouble(), crop.value("right").toDouble(),
              crop.value("bottom").toDouble()};
    c.color = colorFromJson(o.value("color").toObject());
    if (o.contains("chroma")) {
        const auto ch = o.value("chroma").toObject();
        c.chroma.enabled = true;
        c.chroma.color = ch.value("color").toString(c.chroma.color);
        c.chroma.similarity = ch.value("similarity").toDouble(c.chroma.similarity);
        c.chroma.smoothness = ch.value("smoothness").toDouble(c.chroma.smoothness);
    }
    if (o.contains("transitionIn")) {
        const auto tr = o.value("transitionIn").toObject();
        c.transitionIn = Transition{transitionFromName(tr.value("kind").toString()), toUs(tr.value("duration"), 500'000)};
    }
    return c;
}

QJsonObject assetToJson(const MediaAsset &a)
{
    return QJsonObject{
        {"id", a.id},
        {"kind", enumToName(kMediaKinds, a.kind)},
        {"path", a.path},
        {"name", a.name},
        {"duration", us(a.duration)},
        {"width", a.width},
        {"height", a.height},
        {"fps", a.fps},
        {"hasAudio", a.hasAudio},
        {"hasVideo", a.hasVideo},
    };
}

MediaAsset assetFromJson(const QJsonObject &a)
{
    MediaAsset m;
    m.id = a.value("id").toString();
    m.kind = enumFromName(kMediaKinds, a.value("kind").toString());
    m.path = a.value("path").toString();
    m.name = a.value("name").toString();
    m.duration = toUs(a.value("duration"));
    m.width = a.value("width").toInt();
    m.height = a.value("height").toInt();
    m.fps = a.value("fps").toDouble();
    m.hasAudio = a.value("hasAudio").toBool();
    m.hasVideo = a.value("hasVideo").toBool(m.kind != MediaKind::Audio);
    return m;
}

QString easingName(Easing e) { return enumToName(kEasings, e); }
Easing easingFromName(const QString &s) { return enumFromName(kEasings, s); }
QString blendModeName(BlendMode m) { return enumToName(kBlends, m); }
BlendMode blendModeFromName(const QString &s) { return enumFromName(kBlends, s); }
QString transitionName(TransitionKind k) { return enumToName(kTransitions, k); }
TransitionKind transitionFromName(const QString &s) { return enumFromName(kTransitions, s); }
QString animationName(AnimationKind k) { return enumToName(kAnimations, k); }
QString maskShapeName(MaskShape s) { return enumToName(kMaskShapes, s); }
MaskShape maskShapeFromName(const QString &s) { return enumFromName(kMaskShapes, s); }
AnimationKind animationFromName(const QString &s) { return enumFromName(kAnimations, s); }

QJsonObject toJson(const Project &p)
{
    QJsonArray assets;
    for (const auto &[id, a] : p.assets)
        assets.append(assetToJson(a));
    QJsonArray tracks;
    for (const auto &t : p.tracks) {
        QJsonArray clips;
        for (const auto &c : t.clips)
            clips.append(clipToJson(c));
        tracks.append(QJsonObject{
            {"id", t.id},
            {"kind", enumToName(kTrackKinds, t.kind)},
            {"name", t.name},
            {"muted", t.muted},
            {"hidden", t.hidden},
            {"locked", t.locked},
            {"captions", t.captions},
            {"clips", clips},
        });
    }
    QJsonArray markers;
    for (const auto &m : p.markers)
        markers.append(QJsonObject{{"id", m.id}, {"t", us(m.t)}, {"label", m.label}, {"color", m.color}});
    QJsonObject out{
        {"format", "clivcut"},
        {"version", kProjectFormatVersion},
        {"id", p.id},
        {"name", p.name},
        {"width", p.width},
        {"height", p.height},
        {"fps", p.fps},
        {"background", p.background},
        {"mainTrackMagnet", p.mainTrackMagnet},
        {"assets", assets},
        {"tracks", tracks},
        {"markers", markers},
    };
    if (p.backgroundBlur > 0)
        out.insert("backgroundBlur", p.backgroundBlur);
    if (p.inPoint >= 0)
        out.insert("inPoint", us(p.inPoint));
    if (p.outPoint > 0)
        out.insert("outPoint", us(p.outPoint));
    return out;
}

QByteArray serializeProject(const Project &p)
{
    return QJsonDocument(toJson(p)).toJson(QJsonDocument::Indented);
}

bool parseProject(const QByteArray &data, Project &out, QString *error)
{
    QJsonParseError pe;
    const auto doc = QJsonDocument::fromJson(data, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QCoreApplication::translate("Core", "Invalid JSON: %1").arg(pe.errorString());
        return false;
    }
    const auto o = doc.object();
    if (o.value("format").toString() != QLatin1String("clivcut")) {
        if (error)
            *error = QCoreApplication::translate("Core", "The file is not a clivcut project");
        return false;
    }
    if (o.value("version").toInt() > kProjectFormatVersion) {
        if (error)
            *error = QCoreApplication::translate("Core", "Project created by a newer version of clivcut");
        return false;
    }
    Project p;
    p.id = o.value("id").toString();
    p.name = o.value("name").toString(p.name);
    p.width = o.value("width").toInt(p.width);
    p.height = o.value("height").toInt(p.height);
    p.fps = o.value("fps").toDouble(p.fps);
    p.background = o.value("background").toString(p.background);
    p.mainTrackMagnet = o.value("mainTrackMagnet").toBool(true);
    p.backgroundBlur = std::clamp(o.value("backgroundBlur").toDouble(), 0.0, 1.0);
    p.inPoint = static_cast<Micros>(o.value("inPoint").toDouble(-1));
    p.outPoint = static_cast<Micros>(o.value("outPoint").toDouble(-1));
    for (const auto &v : o.value("assets").toArray()) {
        const MediaAsset m = assetFromJson(v.toObject());
        p.assets.emplace(m.id, m);
    }
    for (const auto &v : o.value("tracks").toArray()) {
        const auto t = v.toObject();
        Track tr;
        tr.id = t.value("id").toString();
        tr.kind = enumFromName(kTrackKinds, t.value("kind").toString());
        tr.name = t.value("name").toString();
        tr.muted = t.value("muted").toBool();
        tr.hidden = t.value("hidden").toBool();
        tr.locked = t.value("locked").toBool();
        tr.captions = t.value("captions").toBool();
        for (const auto &cv : t.value("clips").toArray())
            tr.clips.push_back(clipFromJson(cv.toObject()));
        p.tracks.push_back(std::move(tr));
    }
    if (p.tracks.empty())
        p.tracks.push_back(createTrack(TrackKind::Video, QCoreApplication::translate("Track", "Main")));
    for (const auto &v : o.value("markers").toArray()) {
        const auto m = v.toObject();
        p.markers.push_back({m.value("id").toString(), toUs(m.value("t")), m.value("label").toString(),
                             m.value("color").toString(QStringLiteral("#f5c242"))});
    }
    out = std::move(p);
    return true;
}

bool saveProjectFile(const Project &p, const QString &path, QString *error)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    f.write(serializeProject(p));
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

bool loadProjectFile(const QString &path, Project &out, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return parseProject(f.readAll(), out, error);
}

} // namespace clv
