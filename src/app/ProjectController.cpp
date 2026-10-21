// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProjectController.h"

#include "AudioOutput.h"
#include "core/DraftImport.h"
#include "core/Captions.h"
#include "core/Clipboard.h"
#include "core/Edit.h"
#include "core/Evaluate.h"
#include "core/Keyframes.h"
#include "core/Lut.h"
#include "core/Serialize.h"
#include "core/SpeedCurve.h"
#include "AudioRecorder.h"
#include "Language.h"
#include "core/Effects.h"
#include "engine/AiModels.h"
#include "media/AudioDecoder.h"
#include "engine/AudioCleanup.h"
#include "engine/Matting.h"
#include "engine/VoiceEffects.h"
#include "engine/VoiceIsolation.h"
#include "engine/Beats.h"
#include "engine/Silence.h"
#include "engine/Stabilization.h"
#include "engine/StemSeparation.h"
#include "engine/Exporter.h"
#include "engine/Proxy.h"
#include "engine/Transcriber.h"
#include "engine/WhisperModels.h"
#include "media/Encoder.h"
#include "media/Loudness.h"
#include "media/MediaProbe.h"
#include "render/OffscreenRhi.h"

#include <QClipboard>
#include <QColor>
#include <QGuiApplication>
#include <QMimeData>
#include <QDirIterator>
#include <QSvgRenderer>
#include <QPainter>
#include <QJSEngine>
#include <QKeySequence>
#include <QQmlEngine>
#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>
#include <QOffscreenSurface>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <thread>

using namespace clv;

namespace {

ProjectController *g_instance = nullptr;

const char *kindName(TrackKind k)
{
    switch (k) {
    case TrackKind::Video: return "video";
    case TrackKind::Audio: return "audio";
    case TrackKind::Text: return "text";
    case TrackKind::Adjust: return "adjust";
    }
    return "video";
}

const char *mediaKindName(MediaKind k)
{
    switch (k) {
    case MediaKind::Video: return "video";
    case MediaKind::Audio: return "audio";
    case MediaKind::Image: return "image";
    }
    return "video";
}

std::optional<Prop> propFromKey(const QString &key)
{
    for (int i = 0; i < kPropCount; ++i)
        if (key == QLatin1String(propName(static_cast<Prop>(i))))
            return static_cast<Prop>(i);
    return std::nullopt;
}

QString localPath(const QUrl &url)
{
    return url.isLocalFile() ? url.toLocalFile() : url.toString();
}

} // namespace

ProjectController::ProjectController(QObject *parent) : QObject(parent)
{
    g_instance = this;
    m_project = createProject(tr("New project"));
    m_snapshot = std::make_shared<const Project>(m_project);
    m_audio = std::make_unique<AudioOutput>();
    m_useProxies = QSettings().value(QStringLiteral("playback/useProxies"), true).toBool();
    // Cópia de recuperação periódica (só quando há alterações novas).
    bool customInterval = false;
    const int interval = qEnvironmentVariableIntValue("CLIVCUT_AUTOSAVE_MS", &customInterval);
    m_autosave.setInterval(customInterval && interval > 0 ? interval : 30'000);
    connect(&m_autosave, &QTimer::timeout, this, &ProjectController::autosaveNow);
    m_autosave.start();
    m_ticker.setInterval(15);
    m_ticker.setTimerType(Qt::PreciseTimer);
    connect(&m_ticker, &QTimer::timeout, this, &ProjectController::tick);
    m_recordTimer.setInterval(100);
    connect(&m_recordTimer, &QTimer::timeout, this, [this] {
        if (!m_recorder)
            return;
        m_recorder->flush();
        // Nível com queda suave, para o medidor não piscar.
        m_recordLevel = std::max(static_cast<double>(m_recorder->takePeak()), m_recordLevel * 0.7);
        emit recordLevelChanged();
    });
    m_cacheTimer.setSingleShot(true);
    m_cacheTimer.setInterval(300);
    connect(&m_cacheTimer, &QTimer::timeout, this, &ProjectController::refreshRenderCache);
}

ProjectController::~ProjectController()
{
    // Saída normal sem alterações pendentes: a cópia de recuperação não é mais necessária.
    if (!modified())
        removeRecovery();
    m_cancelExport = true;
    *m_proxyCancel = true;
    *m_denoiseCancel = true;
    *m_matteCancel = true;
    *m_stabCancel = true;
    *m_stemsCancel = true;
    *m_voiceCancel = true;
    *m_effectCancel = true;
    *m_renderCancel = true;
    if (g_instance == this)
        g_instance = nullptr;
}

ProjectController *ProjectController::instance()
{
    return g_instance;
}

ProjectController *ProjectController::create(QQmlEngine *engine, QJSEngine *)
{
    Q_ASSERT(g_instance);
    g_instance->m_engine = engine;
    QJSEngine::setObjectOwnership(g_instance, QJSEngine::CppOwnership);
    return g_instance;
}

std::shared_ptr<const Project> ProjectController::snapshot() const
{
    std::lock_guard lock(m_snapMutex);
    return m_snapshot;
}

// ---------------------------------------------------------------------------
// Infraestrutura de edição

template <typename F>
bool ProjectController::edit(const QString &label, F &&fn, const QString &coalesceKey)
{
    Project before = m_project;
    if (!fn(m_project)) {
        m_project = std::move(before);
        return false;
    }
    const bool coalesce = !coalesceKey.isEmpty() && coalesceKey == m_lastCoalesceKey && m_coalesceTimer.isValid() &&
                          m_coalesceTimer.elapsed() < 800;
    if (!coalesce)
        m_history.push(before, label);
    m_lastCoalesceKey = coalesceKey;
    m_coalesceTimer.restart();
    commitChange();
    return true;
}

void ProjectController::commitChange()
{
    {
        std::lock_guard lock(m_snapMutex);
        m_snapshot = std::make_shared<const Project>(m_project);
    }
    if (m_playing)
        m_audio->updateProject(snapshot());
    // Seleção só com clipes que ainda existem.
    const int before = m_selection.size();
    m_selection.erase(std::remove_if(m_selection.begin(), m_selection.end(),
                                     [&](const QString &id) { return !edit::findClip(m_project, id); }),
                      m_selection.end());
    if (m_selection.size() != before)
        emit selectionChanged();
    ++m_revisionCounter;
    refreshDenoise();
    refreshMattes();
    refreshStabilize();
    refreshVoice();
    refreshEffects();
    m_cacheTimer.start();
    if (!m_transcript.isEmpty())
        emit transcriptChanged(); // pode ter ficado desatualizada
    emit projectChanged();
    emit selectedClipChanged();
    emit historyChanged();
    emit modifiedChanged();
    emit revisionChanged();
}

void ProjectController::setProject(Project p, const QString &path)
{
    pause();
    if (!m_compoundStack.empty()) {
        m_compoundStack.clear();
        emit compoundPathChanged();
    }
    removeRecovery(); // o projeto anterior foi salvo ou descartado pelo usuário
    m_autosavedRevision = 0;
    m_project = std::move(p);
    m_history.clear();
    m_savedRevision = 0;
    m_filePath = path;
    m_selection.clear();
    m_playhead = 0;
    commitChange(); // atualiza o snapshot antes de o QML pedir miniaturas
    emit fileChanged();
    emit selectionChanged();
    refreshProxies();
    emit assetsChanged();
    emit playheadChanged();
}

void ProjectController::setStatus(const QString &msg)
{
    m_status = msg;
    emit statusMessageChanged();
}

// ---------------------------------------------------------------------------
// Propriedades

void ProjectController::setProjectName(const QString &name)
{
    if (name == m_project.name || name.trimmed().isEmpty())
        return;
    edit(tr("Rename project"), [&](Project &p) {
        p.name = name.trimmed();
        return true;
    });
}

void ProjectController::setMagnet(bool on)
{
    if (on == m_project.mainTrackMagnet)
        return;
    edit(on ? tr("Enable magnet") : tr("Disable magnet"), [&](Project &p) {
        p.mainTrackMagnet = on;
        edit::packMainTrack(p);
        return true;
    });
}

void ProjectController::setSnapping(bool on)
{
    if (on == m_snapping)
        return;
    m_snapping = on;
    emit snappingChanged();
}

QVariantList ProjectController::tracks() const
{
    QVariantList out;
    for (int ti = 0; ti < static_cast<int>(m_project.tracks.size()); ++ti) {
        const Track &t = m_project.tracks[ti];
        QVariantList clips;
        for (const Clip &c : t.clips) {
            const MediaAsset *a = c.hasMedia() ? m_project.asset(c.assetId) : nullptr;
            bool hasKf = false;
            QVariantList kfTimes;
            QSet<Micros> seen;
            for (const auto &list : c.keyframes)
                for (const auto &k : list) {
                    hasKf = true;
                    if (!seen.contains(k.t)) {
                        seen.insert(k.t);
                        kfTimes << static_cast<double>(k.t);
                    }
                }
            QString label;
            if (c.isText()) {
                label = c.text.section(QLatin1Char('\n'), 0, 0);
                label.remove(QLatin1Char('*'));
                if (label.trimmed().isEmpty() && !c.style.shape.isEmpty())
                    label = tr("Shape");
            } else if (c.isAdjust()) {
                // Camada de efeito ou de filtro: mostra o nome do que ela aplica.
                const auto fx = std::find_if(clv::videoEffects().begin(), clv::videoEffects().end(),
                                             [&](const VideoEffect &e) { return e.id == c.effect.id; });
                label = fx != clv::videoEffects().end() ? QCoreApplication::translate("Effects", fx->name.toUtf8())
                        : findBuiltinFilter(c.color.filter) ? QCoreApplication::translate("Filters", findBuiltinFilter(c.color.filter)->name.toUtf8())
                                                            : tr("Adjustment");
            }
            if (c.hasMedia())
                label = a ? a->name : tr("Missing media");
            const Sequence *seq = c.isCompound() ? m_project.compound(c.compoundId) : nullptr;
            if (c.isCompound())
                label = seq ? seq->name : tr("Compound clip");
            if (c.speed <= 0)
                label = tr("Frozen · %1").arg(label);
            else if (c.speedCurve.size() >= 2)
                label = tr("Curve · %1").arg(label);
            else if (c.speed != 1)
                label = QStringLiteral("%1x · %2").arg(c.speed, 0, 'g', 3).arg(label);
            clips << QVariantMap{
                {"id", c.id},
                {"type", c.isText() ? "text" : c.isAdjust() ? "adjust" : "media"},
                {"start", static_cast<double>(c.start)},
                {"duration", static_cast<double>(c.duration)},
                {"end", static_cast<double>(c.end())},
                {"filter", c.color.filter},
                {"audioOnly", c.audioOnly},
                {"audioMuted", c.audioMuted},
                {"groupId", c.groupId},
                {"assetId", c.assetId},
                {"mediaKind", a ? mediaKindName(a->kind) : c.isCompound() ? "compound" : (c.isAdjust() ? "adjust" : "text")},
                {"label", label},
                {"sourceIn", static_cast<double>(c.sourceIn)},
                {"speed", c.speed},
                {"reverse", c.reverse},
                {"assetDuration", a ? static_cast<double>(a->duration) : seq ? static_cast<double>(seq->duration()) : 0.0},
                {"hasAudio", (a && a->hasAudio) || (seq && compoundHasAudio(*seq))},
                {"missing", a && !QFileInfo::exists(a->path)},
                {"transition", c.transitionIn ? transitionName(c.transitionIn->kind) : QString()},
                {"transitionDuration", c.transitionIn ? static_cast<double>(c.transitionIn->duration) : 0.0},
                {"hasKeyframes", hasKf},
                {"keyframeTimes", kfTimes},
                {"fadeIn", static_cast<double>(c.fadeIn)},
                {"fadeOut", static_cast<double>(c.fadeOut)},
                {"selected", m_selection.contains(c.id)},
            };
        }
        out << QVariantMap{
            {"index", ti},
            {"id", t.id},
            {"kind", kindName(t.kind)},
            {"name", t.name},
            {"muted", t.muted},
            {"hidden", t.hidden},
            {"locked", t.locked},
            {"isMain", ti == 0},
            {"clips", clips},
        };
    }
    return out;
}

QVariantList ProjectController::assets() const
{
    std::vector<const MediaAsset *> list;
    for (const auto &[id, a] : m_project.assets)
        list.push_back(&a);
    std::sort(list.begin(), list.end(), [](auto *a, auto *b) { return a->name.localeAwareCompare(b->name) < 0; });
    QVariantList out;
    for (const MediaAsset *a : list) {
        int uses = 0;
        m_project.forEachClip([&](const Clip &c) { uses += c.assetId == a->id; });
        out << QVariantMap{
            {"id", a->id},
            {"name", a->name},
            {"path", a->path},
            {"kind", mediaKindName(a->kind)},
            {"duration", static_cast<double>(a->duration)},
            {"width", a->width},
            {"height", a->height},
            {"hasAudio", a->hasAudio},
            {"missing", !QFileInfo::exists(a->path)},
            {"uses", uses},
            {"proxyState", a->kind != MediaKind::Video ? QStringLiteral("none")
                           : m_proxyBuilding == a->path ? QStringLiteral("building")
                           : m_proxyQueue.contains(a->path) ? QStringLiteral("queued")
                           : !engine::registeredProxy(a->path).isEmpty() ? QStringLiteral("ready")
                                                                          : QStringLiteral("none")},
            {"proxyProgress", m_proxyBuilding == a->path ? m_proxyProgress : 0.0},
        };
    }
    return out;
}

QVariantList ProjectController::markers() const
{
    QVariantList out;
    for (const auto &m : m_project.markers)
        out << QVariantMap{{"id", m.id}, {"t", static_cast<double>(m.t)}, {"label", m.label}, {"color", m.color}, {"beat", m.beat}};
    return out;
}

QString ProjectController::primaryClipId() const
{
    if (m_selection.size() == 1)
        return m_selection.first();
    return m_selection.contains(m_primary) ? m_primary : QString();
}

QVariantMap ProjectController::selectedClip() const
{
    const QString primary = primaryClipId();
    if (primary.isEmpty())
        return {};
    const Clip *c = edit::clipById(m_project, primary);
    if (!c)
        return {};
    const Micros rel = std::clamp<Micros>(m_playhead - c->start, 0, c->duration);
    const bool underPlayhead = m_playhead >= c->start && m_playhead < c->end();
    QVariantMap m{
        {"id", c->id},
        {"type", c->isText() ? "text" : c->isAdjust() ? "adjust" : "media"},
        {"start", static_cast<double>(c->start)},
        {"duration", static_cast<double>(c->duration)},
        {"underPlayhead", underPlayhead},
        {"playheadU", c->duration > 0 ? static_cast<double>(rel) / c->duration : 0.0},
        {"inCaptionTrack", m_project.tracks[edit::findClip(m_project, c->id)->track].captions},
        {"flipH", c->transform.flipH},
        {"flipV", c->transform.flipV},
        {"animIn", animationName(c->animIn.kind)},
        {"animInDuration", static_cast<double>(c->animIn.duration)},
        {"animOut", animationName(c->animOut.kind)},
        {"animOutDuration", static_cast<double>(c->animOut.duration)},
    };
    for (int i = 0; i < kPropCount; ++i) {
        const auto p = static_cast<Prop>(i);
        const QString name = QString::fromLatin1(propName(p));
        m.insert(name, propAt(*c, p, rel));
        m.insert(QStringLiteral("kf_") + name, !c->keyframes[i].empty());
        m.insert(QStringLiteral("kfAt_") + name, keyframeAt(c->keyframes[i], rel, m_project.frameDuration() / 2) != nullptr);
    }
    if (c->isText()) {
        const TextStyle &s = c->style;
        m.insert({{"text", c->text},
                  {"fontFamily", s.fontFamily},
                  {"fontSize", s.fontSize},
                  {"bold", s.bold},
                  {"italic", s.italic},
                  {"color", s.color},
                  {"strokeColor", s.strokeColor},
                  {"strokeWidth", s.strokeWidth},
                  {"background", s.background},
                  {"highlightColor", s.highlightColor},
                  {"curve", s.curve},
                  {"shape", s.shape},
                  {"shapeColor", s.shapeColor},
                  {"shapeWidth", s.shapeWidth},
                  {"shapeHeight", s.shapeHeight},
                  {"align", s.align},
                  {"shadow", s.shadow},
                  {"effect", c->effect.id},
                  {"effectIntensity", c->effect.intensity}});
        return m;
    }
    const ColorAdjust &col = c->color;
    m.insert({{"curves", [&] {
                   QVariantList out;
                   for (const auto &curve : col.curves) {
                       QVariantList pts;
                       for (const auto &p : curve)
                           pts << QVariantMap{{"x", p.x}, {"y", p.y}};
                       out << QVariant(pts);
                   }
                   return out;
               }()},
              {"hsl", [&] {
                   QVariantList out;
                   for (const auto &b : col.hsl)
                       out << QVariantMap{{"hue", b.hue}, {"saturation", b.saturation}, {"luminance", b.luminance}};
                   return out;
               }()},
              {"wheels", QVariantMap{
                   {"lift", QVariantMap{{"x", col.lift.x}, {"y", col.lift.y}, {"luma", col.lift.luma}}},
                   {"gamma", QVariantMap{{"x", col.gamma.x}, {"y", col.gamma.y}, {"luma", col.gamma.luma}}},
                   {"gain", QVariantMap{{"x", col.gain.x}, {"y", col.gain.y}, {"luma", col.gain.luma}}}}},
              {"hasGrade", col.hasGrade()},
              {"effect", c->effect.id},
              {"effectIntensity", c->effect.intensity}});
    m.insert({{"temperature", col.temperature},
              {"exposure", col.exposure},
              {"tint", col.tint},
              {"highlights", col.highlights},
              {"shadows", col.shadows},
              {"vignette", col.vignette},
              {"sharpen", col.sharpen},
              {"filter", col.filter},
              {"filterName", findBuiltinFilter(col.filter) ? QCoreApplication::translate("Filters", findBuiltinFilter(col.filter)->name.toUtf8()) : QString()},
              {"filterIntensity", col.filterIntensity},
              {"lutPath", col.lutPath},
              {"lutName", QFileInfo(col.lutPath).fileName()},
              {"lutIntensity", col.lutIntensity},
              {"maskShape", maskShapeName(c->mask.shape)},
              {"maskX", c->mask.x},
              {"maskY", c->mask.y},
              {"maskWidth", c->mask.width},
              {"maskHeight", c->mask.height},
              {"maskRotation", c->mask.rotation},
              {"maskFeather", c->mask.feather},
              {"maskRoundness", c->mask.roundness},
              {"maskInvert", c->mask.invert}});
    if (c->isAdjust())
        return m;
    const MediaAsset *a = m_project.asset(c->assetId);
    const Sequence *seq = c->isCompound() ? m_project.compound(c->compoundId) : nullptr;
    m.insert({{"assetName", a ? a->name : seq ? seq->name : QString()},
              {"mediaKind", a ? mediaKindName(a->kind) : seq ? "compound" : "video"},
              {"hasAudio", (a && a->hasAudio) || (seq && compoundHasAudio(*seq))},
              {"audioOnly", c->audioOnly},
              {"audioMuted", c->audioMuted},
              {"gainDb", c->gainDb},
              {"denoise", c->denoise},
              {"denoiseState", [&] {
                   if (!a || c->denoise <= 0)
                       return QStringLiteral("off");
                   const QString job = a->path + QLatin1Char('|') + QString::number(engine::quantizedDenoise(c->denoise));
                   if (m_denoiseBuilding == job)
                       return QStringLiteral("processing");
                   if (m_denoiseQueue.contains(job))
                       return QStringLiteral("queued");
                   return engine::registeredDenoised(a->path, c->denoise).isEmpty() ? QStringLiteral("off")
                                                                                     : QStringLiteral("ready");
               }()},
              {"denoiseProgress", m_denoiseProgress},
              {"voiceIsolation", c->voiceIsolation},
              {"ducking", c->ducking},
              {"voiceEffect", c->voiceEffect},
              {"effectState", [&] {
                   if (!a || c->voiceEffect.isEmpty())
                       return QStringLiteral("off");
                   const QString source = audioSourceFor(*c, *a);
                   if (!engine::registeredEffect(source, c->voiceEffect).isEmpty())
                       return QStringLiteral("ready");
                   const QString job = source + QLatin1Char('|') + c->voiceEffect;
                   if (m_effectBuilding == job)
                       return QStringLiteral("processing");
                   return QStringLiteral("queued");
               }()},
              {"effectProgress", m_effectProgress},
              {"voiceState", [&] {
                   if (!a || c->voiceIsolation <= 0)
                       return QStringLiteral("off");
                   if (!engine::voiceIsolationAvailable())
                       return QStringLiteral("unavailable");
                   if (!engine::registeredVoice(a->path, c->voiceIsolation).isEmpty())
                       return QStringLiteral("ready");
                   if (!engine::aiModelInstalled(engine::kVoiceModel))
                       return QStringLiteral("needsModel");
                   const QString job = a->path + QLatin1Char('|') + QString::number(engine::quantizedVoice(c->voiceIsolation));
                   if (m_voiceBuilding == job)
                       return QStringLiteral("processing");
                   return m_voiceQueue.contains(job) ? QStringLiteral("queued") : QStringLiteral("off");
               }()},
              {"voiceProgress", m_voiceProgress},
              {"removeBackground", c->removeBackground},
              {"keepPitch", c->keepPitch},
              {"stemsState", [&] {
                   if (!a || !a->hasAudio)
                       return QStringLiteral("off");
                   if (!engine::stemSeparationAvailable())
                       return QStringLiteral("unavailable");
                   if (m_stemsBuilding == a->path)
                       return QStringLiteral("processing");
                   if (!m_stemsBuilding.isEmpty())
                       return QStringLiteral("busy");
                   if (engine::stemsReady(a->path))
                       return QStringLiteral("ready");
                   return engine::aiModelInstalled(engine::kStemsModel) ? QStringLiteral("off") : QStringLiteral("needsModel");
               }()},
              {"stemsProgress", m_stemsProgress},
              {"stabilize", c->stabilize},
              {"stabilizeState", [&] {
                   if (!a || c->stabilize <= 0)
                       return QStringLiteral("off");
                   if (registeredMotion(a->path))
                       return QStringLiteral("ready");
                   if (m_stabBuilding == a->path)
                       return QStringLiteral("processing");
                   return m_stabQueue.contains(a->path) ? QStringLiteral("queued") : QStringLiteral("off");
               }()},
              {"stabilizeProgress", m_stabProgress},
              {"matteState", [&] {
                   if (!a || !c->removeBackground)
                       return QStringLiteral("off");
                   if (!engine::mattingAvailable() || a->kind != MediaKind::Video)
                       return QStringLiteral("unavailable");
                   if (!engine::registeredMatte(a->path).isEmpty())
                       return QStringLiteral("ready");
                   if (!engine::aiModelInstalled(engine::kMattingModel))
                       return QStringLiteral("needsModel");
                   if (m_matteBuilding == a->path)
                       return QStringLiteral("processing");
                   return m_matteQueue.contains(a->path) ? QStringLiteral("queued") : QStringLiteral("off");
               }()},
              {"matteProgress", m_matteProgress},
              {"speed", c->speed},
              {"speedCurve", [&] {
                   QVariantList pts;
                   for (const auto &pt : c->speedCurve)
                       pts << QVariantMap{{"u", pt.u}, {"v", pt.v}};
                   return pts;
               }()},

              {"reverse", c->reverse},
              {"fadeIn", static_cast<double>(c->fadeIn)},
              {"fadeOut", static_cast<double>(c->fadeOut)},
              {"blend", blendModeName(c->blend)},
              {"chromaEnabled", c->chroma.enabled},
              {"chromaColor", c->chroma.color},
              {"chromaSimilarity", c->chroma.similarity},
              {"chromaSmoothness", c->chroma.smoothness},
              {"cropLeft", c->crop.left},
              {"cropTop", c->crop.top},
              {"cropRight", c->crop.right},
              {"cropBottom", c->crop.bottom},
              {"transition", c->transitionIn ? transitionName(c->transitionIn->kind) : QString()},
              {"transitionDuration", c->transitionIn ? static_cast<double>(c->transitionIn->duration) : 500000.0}});
    return m;
}

QStringList ProjectController::videoCodecs() const
{
    return media::availableVideoCodecs();
}

QStringList ProjectController::hardwareCodecs() const
{
    return media::availableHardwareCodecs();
}

QStringList ProjectController::audioFormats() const
{
    return media::availableAudioFormats();
}

// ---------------------------------------------------------------------------
// Arquivos

void ProjectController::newProject(int width, int height, double fps)
{
    setProject(createProject(tr("New project"), width, height, fps), {});
    setStatus(tr("Project created (%1×%2, %3 fps)").arg(width).arg(height).arg(fps));
}

bool ProjectController::openProject(const QUrl &url)
{
    const QString path = localPath(url);
    if (QFileInfo(path).isDir() || QFileInfo(path).fileName().startsWith(QLatin1String("draft_")))
        return importDraft(url);
    Project p;
    QString err;
    if (!loadProjectFile(path, p, &err)) {
        emit errorOccurred(tr("Could not open the project: %1").arg(err));
        return false;
    }
    setProject(std::move(p), path);
    rememberRecent(path);
    setStatus(tr("Project opened: %1").arg(QFileInfo(path).fileName()));
    return true;
}

bool ProjectController::saveProject()
{
    if (m_filePath.isEmpty())
        return false;
    QString err;
    if (!saveProjectFile(rootProject(), m_filePath, &err)) {
        emit errorOccurred(tr("Save failed: %1").arg(err));
        return false;
    }
    m_savedRevision = m_history.revision();
    for (auto &f : m_compoundStack)
        f.savedRevision = -2;
    removeRecovery();
    rememberRecent(m_filePath);
    emit modifiedChanged();
    setStatus(tr("Project saved"));
    return true;
}

bool ProjectController::saveProjectAs(const QUrl &url)
{
    QString path = localPath(url);
    if (!path.endsWith(QLatin1String(".clivcut")))
        path += QStringLiteral(".clivcut");
    m_filePath = path;
    emit fileChanged();
    return saveProject();
}

bool ProjectController::importDraft(const QUrl &url)
{
    auto r = clv::importDraft(localPath(url));
    if (!r.ok) {
        emit errorOccurred(r.error);
        return false;
    }
    // Completa metadados lendo as mídias que existem.
    for (auto &[id, a] : r.project.assets) {
        MediaAsset probed;
        if (QFileInfo::exists(a.path) && media::probe(a.path, probed)) {
            a.kind = probed.kind;
            a.width = probed.width;
            a.height = probed.height;
            a.hasAudio = probed.hasAudio;
            a.hasVideo = probed.hasVideo;
            a.fps = probed.fps;
            if (a.duration <= 0)
                a.duration = probed.duration;
        }
    }
    setProject(std::move(r.project), {});
    setStatus(r.warnings.isEmpty() ? tr("Draft imported")
                                   : tr("Draft imported with %1 warning(s): %2").arg(r.warnings.size()).arg(r.warnings.first()));
    return true;
}

void ProjectController::importMedia(const QList<QUrl> &urls)
{
    QStringList failed;
    QString lastId;
    Project before = m_project;
    int added = 0;
    for (const QUrl &url : urls) {
        const QString path = localPath(url);
        bool exists = false;
        for (const auto &[id, a] : m_project.assets)
            exists = exists || a.path == path;
        if (exists)
            continue;
        MediaAsset a;
        QString err;
        if (!media::probe(path, a, &err)) {
            failed << QFileInfo(path).fileName();
            continue;
        }
        a.id = newId("ast");
        lastId = a.id;
        m_project.assets.emplace(a.id, a);
        ++added;
    }
    if (added > 0) {
        m_history.push(before, tr("Import media"));
        commitChange();
        refreshProxies();
        emit assetsChanged();
        setStatus(tr("%n file(s) imported", nullptr, added));
    }
    if (!failed.isEmpty())
        emit errorOccurred(tr("Unsupported format: %1").arg(failed.join(QStringLiteral(", "))));
}

void ProjectController::setCanvas(int width, int height)
{
    if (width == m_project.width && height == m_project.height)
        return;
    edit(tr("Canvas aspect ratio"), [&](Project &p) {
        p.width = width;
        p.height = height;
        return true;
    });
}

void ProjectController::setBackground(const QString &color)
{
    edit(tr("Background color"), [&](Project &p) {
        p.background = color;
        return true;
    }, QStringLiteral("bg"));
}

void ProjectController::setBackgroundBlur(double amount)
{
    amount = std::clamp(amount, 0.0, 1.0);
    edit(tr("Background blur"), [&](Project &p) {
        if (p.backgroundBlur == amount)
            return false;
        p.backgroundBlur = amount;
        return true;
    }, QStringLiteral("bgblur"));
}

void ProjectController::relinkAsset(const QString &assetId, const QUrl &url)
{
    auto it = m_project.assets.find(assetId);
    if (it == m_project.assets.end())
        return;
    MediaAsset probed;
    const QString path = localPath(url);
    if (!media::probe(path, probed)) {
        emit errorOccurred(tr("Could not read %1").arg(path));
        return;
    }
    edit(tr("Relink media"), [&](Project &p) {
        MediaAsset &a = p.assets[assetId];
        probed.id = a.id;
        a = probed;
        return true;
    });
    emit assetsChanged();
}

// ---------------------------------------------------------------------------
// Timeline

void ProjectController::addAssetToTimeline(const QString &assetId, int trackIndex, double start)
{
    const MediaAsset *a = m_project.asset(assetId);
    if (!a)
        return;
    QString newClip;
    edit(tr("Add to timeline"), [&](Project &p) {
        Micros at = start >= 0 ? static_cast<Micros>(start) : m_playhead;
        Clip c = createMediaClip(*a, at);
        if (trackIndex < 0 && start < 0 && a->kind != MediaKind::Audio) {
            // Duplo clique: entra na faixa principal, no fim.
            c.start = p.tracks[0].clips.empty() ? 0 : p.tracks[0].clips.back().end();
            trackIndex = 0;
        }
        newClip = edit::insertClip(p, std::move(c), trackIndex);
        return !newClip.isEmpty();
    });
    if (!newClip.isEmpty()) {
        m_selection = {newClip};
        emit selectionChanged();
        emit selectedClipChanged();
        emit assetsChanged();
    }
}

QString ProjectController::defaultTextFor(const QString &preset) const
{
    if (preset == QLatin1String("title"))
        return tr("Title");
    if (preset == QLatin1String("subtitle"))
        return tr("Subtitle");
    return tr("Text");
}

void ProjectController::addShape(const QString &kind)
{
    QString id;
    edit(tr("Add shape"), [&](Project &p) {
        Clip c = createTextClip(QString(), m_playhead);
        c.style.shape = kind;
        c.style.strokeWidth = 0;
        if (kind == QLatin1String("bubble")) {
            // Balão já vem com texto escuro, pronto para escrever a fala.
            c.style.shapeColor = QStringLiteral("#ffffff");
            c.style.color = QStringLiteral("#111111");
            c.style.fontSize = 56;
            c.text = tr("Hello!");
        }
        id = edit::insertClip(p, std::move(c), -1);
        return !id.isEmpty();
    });
    if (!id.isEmpty()) {
        m_selection = {id};
        emit selectionChanged();
        emit selectedClipChanged();
    }
}

void ProjectController::addText(const QString &text, const QString &preset)
{
    QString id;
    edit(tr("Add text"), [&](Project &p) {
        Clip c = createTextClip(text.isEmpty() ? defaultTextFor(preset) : text, m_playhead);
        if (preset == QLatin1String("title")) {
            c.style.fontSize = 120;
            c.transform.y = 0;
        } else if (preset == QLatin1String("subtitle")) {
            c.style.fontSize = 56;
            c.style.bold = false;
            c.style.background = QStringLiteral("#99000000");
            c.style.strokeWidth = 0;
            c.transform.y = 0.75;
        } else if (preset == QLatin1String("highlight")) {
            c.style.fontSize = 90;
            c.style.color = QStringLiteral("#ffe14d");
            c.style.strokeWidth = 6;
            c.transform.y = 0;
            c.animIn = {AnimationKind::Zoom, 400'000};
        }
        id = edit::insertClip(p, std::move(c), -1);
        return !id.isEmpty();
    });
    if (!id.isEmpty()) {
        m_selection = {id};
        emit selectionChanged();
        emit selectedClipChanged();
    }
}

void ProjectController::splitAtPlayhead()
{
    const Micros t = m_project.snapToFrame(m_playhead);
    edit(tr("Split"), [&](Project &p) { return edit::splitAt(p, t, m_selection) > 0 || edit::splitAt(p, t) > 0; });
}

void ProjectController::deleteSelection(bool ripple)
{
    if (m_selection.isEmpty())
        return;
    const QStringList ids = m_selection;
    edit(tr("Delete"), [&](Project &p) { return edit::deleteClips(p, ids, ripple); });
}

void ProjectController::deleteLeft()
{
    const Micros t = m_project.snapToFrame(m_playhead);
    QStringList targets = m_selection;
    if (targets.isEmpty())
        for (const auto &c : m_project.tracks[0].clips)
            if (t > c.start && t < c.end())
                targets << c.id;
    edit(tr("Delete left"), [&](Project &p) {
        bool any = false;
        for (const auto &id : targets)
            any = edit::trimToPlayhead(p, id, t, edit::Edge::Start, true) || any;
        return any;
    });
    // Na faixa principal magnética o clipe recua: o cursor vai para o novo início.
    if (!targets.isEmpty())
        if (const Clip *c = edit::clipById(m_project, targets.first()))
            seek(static_cast<double>(c->start));
}

void ProjectController::deleteRight()
{
    const Micros t = m_project.snapToFrame(m_playhead);
    QStringList targets = m_selection;
    if (targets.isEmpty())
        for (const auto &c : m_project.tracks[0].clips)
            if (t > c.start && t < c.end())
                targets << c.id;
    edit(tr("Delete right"), [&](Project &p) {
        bool any = false;
        for (const auto &id : targets)
            any = edit::trimToPlayhead(p, id, t, edit::Edge::End, true) || any;
        return any;
    });
}

void ProjectController::duplicateSelection()
{
    QStringList created;
    edit(tr("Duplicate"), [&](Project &p) {
        for (const auto &id : m_selection)
            if (auto n = edit::duplicateClip(p, id))
                created << *n;
        return !created.isEmpty();
    });
    if (!created.isEmpty()) {
        m_selection = created;
        emit selectionChanged();
        emit selectedClipChanged();
    }
}

bool ProjectController::moveClip(const QString &clipId, int trackIndex, double start)
{
    // Vários selecionados (ou grupo): move todos no tempo, cada um na sua faixa.
    if (m_selection.size() > 1 && m_selection.contains(clipId)) {
        const Clip *c = edit::clipById(m_project, clipId);
        if (!c)
            return false;
        const Micros delta = m_project.snapToFrame(static_cast<Micros>(start)) - c->start;
        const QStringList ids = m_selection;
        return edit(tr("Move clips"), [&](Project &p) { return edit::moveClips(p, ids, delta); });
    }
    return edit(tr("Move clip"), [&](Project &p) {
        return edit::moveClip(p, clipId, trackIndex, p.snapToFrame(static_cast<Micros>(start)));
    });
}

bool ProjectController::trimClip(const QString &clipId, bool startEdge, double time)
{
    return edit(tr("Trim clip"), [&](Project &p) {
        return edit::trimClip(p, clipId, startEdge ? edit::Edge::Start : edit::Edge::End,
                              p.snapToFrame(static_cast<Micros>(time)));
    });
}

double ProjectController::snapTime(double t, const QStringList &ignore, double thresholdUs) const
{
    if (!m_snapping)
        return t;
    return static_cast<double>(edit::snap(m_project, static_cast<Micros>(t), static_cast<Micros>(thresholdUs), ignore,
                                          m_playhead));
}

void ProjectController::setTrackFlag(int trackIndex, const QString &flag, bool value)
{
    if (trackIndex < 0 || trackIndex >= static_cast<int>(m_project.tracks.size()))
        return;
    edit(tr("Change track"), [&](Project &p) {
        Track &t = p.tracks[trackIndex];
        if (flag == QLatin1String("muted"))
            t.muted = value;
        else if (flag == QLatin1String("hidden"))
            t.hidden = value;
        else if (flag == QLatin1String("locked"))
            t.locked = value;
        else
            return false;
        return true;
    });
}

void ProjectController::addMarker()
{
    edit(tr("Add marker"), [&](Project &p) {
        for (const auto &m : p.markers)
            if (std::llabs(m.t - m_playhead) < p.frameDuration())
                return false;
        p.markers.push_back({newId("mrk"), m_project.snapToFrame(m_playhead), QString(), QStringLiteral("#f5c242")});
        return true;
    });
}

void ProjectController::markIn()
{
    const Micros t = m_project.snapToFrame(m_playhead);
    edit(tr("Mark in"), [&](Project &p) {
        if (p.inPoint == t)
            return false;
        p.inPoint = t;
        if (p.outPoint > 0 && p.outPoint <= t)
            p.outPoint = -1;
        return true;
    });
}

void ProjectController::markOut()
{
    const Micros t = m_project.snapToFrame(m_playhead);
    if (t <= 0)
        return;
    edit(tr("Mark out"), [&](Project &p) {
        if (p.outPoint == t)
            return false;
        p.outPoint = t;
        if (p.inPoint >= t)
            p.inPoint = -1;
        return true;
    });
}

void ProjectController::clearInOut()
{
    edit(tr("Clear in/out"), [&](Project &p) {
        if (!p.hasRange())
            return false;
        p.inPoint = p.outPoint = -1;
        return true;
    });
}

void ProjectController::jumpToInOut(bool out)
{
    const Micros t = out ? m_project.outPoint : m_project.inPoint;
    if (t >= 0)
        seek(static_cast<double>(t));
}

void ProjectController::detachAudio()
{
    QStringList targets = m_selection;
    if (targets.isEmpty())
        for (const auto &c : m_project.tracks[0].clips)
            if (m_playhead >= c.start && m_playhead < c.end())
                targets << c.id;
    QStringList created;
    edit(tr("Detach audio"), [&](Project &p) {
        for (const auto &id : targets)
            if (auto a = edit::detachAudio(p, id))
                created << *a;
        return !created.isEmpty();
    });
    if (created.isEmpty())
        setStatus(tr("Select a video clip with audio"));
    else
        setStatus(tr("Audio detached from %n clip(s)", nullptr, static_cast<int>(created.size())));
}

void ProjectController::freezeFrame()
{
    QString target;
    for (const auto &id : m_selection)
        target = id;
    if (target.isEmpty())
        for (const auto &c : m_project.tracks[0].clips)
            if (m_playhead >= c.start && m_playhead < c.end())
                target = c.id;
    auto ref = edit::findClip(m_project, target);
    if (!ref || ref->track != 0) {
        setStatus(tr("Select a video clip on the main track to freeze the frame"));
        return;
    }
    const Micros t = m_project.snapToFrame(m_playhead);
    edit(tr("Freeze frame"), [&](Project &p) {
        const Clip original = p.tracks[0].clips[ref->index];
        if (original.isText())
            return false;
        Clip freeze = original;
        freeze.id = newId("clp");
        freeze.sourceIn = sourceTimeAt(p, original, t - original.start);
        freeze.speed = 0;
        freeze.reverse = false;
        freeze.volume = 0;
        freeze.start = t;
        freeze.duration = 3 * kSecond;
        freeze.fadeIn = freeze.fadeOut = 0;
        freeze.transitionIn.reset();
        freeze.animIn = freeze.animOut = {};
        for (auto &list : freeze.keyframes)
            list.clear();
        if (t > original.start && t < original.end())
            edit::splitClip(p, original.id, t);
        return !edit::insertClip(p, std::move(freeze), 0).isEmpty();
    });
}

// ---------------------------------------------------------------------------
// Seleção e propriedades

void ProjectController::select(const QString &clipId, bool additive)
{
    if (clipId.isEmpty()) {
        clearSelection();
        return;
    }
    // Clicar num clipe de grupo seleciona o grupo inteiro.
    m_primary = clipId;
    const QStringList group = edit::expandToGroups(m_project, {clipId});
    if (additive) {
        if (m_selection.contains(clipId)) {
            for (const auto &id : group)
                m_selection.removeAll(id);
        } else {
            for (const auto &id : group)
                if (!m_selection.contains(id))
                    m_selection << id;
        }
    } else {
        if (m_selection == group) {
            emit selectedClipChanged(); // o clipe principal pode ter mudado dentro do grupo
            return;
        }
        m_selection = group;
    }
    emit selectionChanged();
    emit selectedClipChanged();
    emit projectChanged();
}

void ProjectController::clearSelection()
{
    if (m_selection.isEmpty())
        return;
    m_selection.clear();
    emit selectionChanged();
    emit selectedClipChanged();
    emit projectChanged();
}

namespace {

/** Aplica uma propriedade ao clipe; animáveis com keyframes viram keyframe em `rel`. */
bool applyClipProperty(Project &p, Clip *c, Micros rel, const QString &key, const QVariant &value)
{
    if (auto prop = propFromKey(key)) {
        const double v = value.toDouble();
        auto &kfs = c->keyframes[static_cast<int>(*prop)];
        if (!kfs.empty())
            upsertKeyframe(kfs, {rel, v, Easing::Linear}, p.frameDuration() / 2);
        else
            setBaseValue(*c, *prop, v);
        return true;
    }
    TextStyle &s = c->style;
    if (key == QLatin1String("flipH")) c->transform.flipH = value.toBool();
    else if (key == QLatin1String("flipV")) c->transform.flipV = value.toBool();
    else if (key == QLatin1String("text")) c->text = value.toString();
    else if (key == QLatin1String("fontFamily")) s.fontFamily = value.toString();
    else if (key == QLatin1String("fontSize")) s.fontSize = value.toDouble();
    else if (key == QLatin1String("bold")) s.bold = value.toBool();
    else if (key == QLatin1String("italic")) s.italic = value.toBool();
    else if (key == QLatin1String("color")) s.color = value.toString();
    else if (key == QLatin1String("strokeColor")) s.strokeColor = value.toString();
    else if (key == QLatin1String("strokeWidth")) s.strokeWidth = value.toDouble();
    else if (key == QLatin1String("background")) s.background = value.toString();
    else if (key == QLatin1String("align")) s.align = value.toInt();
    else if (key == QLatin1String("shadow")) s.shadow = value.toBool();
    else if (key == QLatin1String("highlightColor")) s.highlightColor = value.toString();
    else if (key == QLatin1String("curve")) s.curve = std::clamp(value.toDouble(), -1.0, 1.0);
    else if (key == QLatin1String("shape")) s.shape = value.toString();
    else if (key == QLatin1String("shapeColor")) s.shapeColor = value.toString();
    else if (key == QLatin1String("shapeWidth")) s.shapeWidth = std::clamp(value.toDouble(), 4.0, 4000.0);
    else if (key == QLatin1String("shapeHeight")) s.shapeHeight = std::clamp(value.toDouble(), 4.0, 4000.0);
    else if (key == QLatin1String("fadeIn")) c->fadeIn = std::clamp<Micros>(static_cast<Micros>(value.toDouble()), 0, c->duration);
    else if (key == QLatin1String("fadeOut")) c->fadeOut = std::clamp<Micros>(static_cast<Micros>(value.toDouble()), 0, c->duration);
    else if (key == QLatin1String("reverse")) c->reverse = value.toBool();
    else if (key == QLatin1String("audioMuted")) c->audioMuted = value.toBool();
    else if (key == QLatin1String("gainDb")) c->gainDb = std::clamp(value.toDouble(), -24.0, 24.0);
    else if (key == QLatin1String("denoise")) c->denoise = engine::quantizedDenoise(value.toDouble());
    else if (key == QLatin1String("removeBackground")) c->removeBackground = value.toBool();
    else if (key == QLatin1String("stabilize")) c->stabilize = std::clamp(std::round(value.toDouble() * 20) / 20, 0.0, 1.0);
    else if (key == QLatin1String("effect")) c->effect.id = effectShaderIndex(value.toString()) > 0 ? value.toString() : QString();
    else if (key == QLatin1String("effectIntensity")) c->effect.intensity = std::clamp(value.toDouble(), 0.0, 1.0);
    else if (key == QLatin1String("voiceEffect")) c->voiceEffect = engine::findVoiceEffect(value.toString()) ? value.toString() : QString();
    else if (key == QLatin1String("ducking")) c->ducking = std::clamp(value.toDouble(), 0.0, 1.0);
    else if (key == QLatin1String("voiceIsolation")) c->voiceIsolation = engine::quantizedVoice(value.toDouble());
    else if (key == QLatin1String("keepPitch")) c->keepPitch = value.toBool();
    else if (key == QLatin1String("temperature")) c->color.temperature = value.toDouble();
    else if (key == QLatin1String("maskShape")) c->mask.shape = maskShapeFromName(value.toString());
    else if (key == QLatin1String("maskX")) c->mask.x = value.toDouble();
    else if (key == QLatin1String("maskY")) c->mask.y = value.toDouble();
    else if (key == QLatin1String("maskWidth")) c->mask.width = std::max(0.01, value.toDouble());
    else if (key == QLatin1String("maskHeight")) c->mask.height = std::max(0.01, value.toDouble());
    else if (key == QLatin1String("maskRotation")) c->mask.rotation = value.toDouble();
    else if (key == QLatin1String("maskFeather")) c->mask.feather = std::clamp(value.toDouble(), 0.0, 1.0);
    else if (key == QLatin1String("maskRoundness")) c->mask.roundness = std::clamp(value.toDouble(), 0.0, 1.0);
    else if (key == QLatin1String("maskInvert")) c->mask.invert = value.toBool();
    else if (key == QLatin1String("exposure")) c->color.exposure = value.toDouble();
    else if (key == QLatin1String("tint")) c->color.tint = value.toDouble();
    else if (key == QLatin1String("highlights")) c->color.highlights = value.toDouble();
    else if (key == QLatin1String("shadows")) c->color.shadows = value.toDouble();
    else if (key == QLatin1String("vignette")) c->color.vignette = value.toDouble();
    else if (key == QLatin1String("sharpen")) c->color.sharpen = value.toDouble();
    else if (key == QLatin1String("filter")) c->color.filter = value.toString();
    else if (key == QLatin1String("filterIntensity")) c->color.filterIntensity = std::clamp(value.toDouble(), 0.0, 1.0);
    else if (key == QLatin1String("lutPath")) c->color.lutPath = value.toString();
    else if (key == QLatin1String("lutIntensity")) c->color.lutIntensity = std::clamp(value.toDouble(), 0.0, 1.0);
    else if (key == QLatin1String("blend")) c->blend = blendModeFromName(value.toString());
    else if (key == QLatin1String("chromaEnabled")) c->chroma.enabled = value.toBool();
    else if (key == QLatin1String("chromaColor")) c->chroma.color = value.toString();
    else if (key == QLatin1String("chromaSimilarity")) c->chroma.similarity = value.toDouble();
    else if (key == QLatin1String("chromaSmoothness")) c->chroma.smoothness = value.toDouble();
    else if (key == QLatin1String("cropLeft")) c->crop.left = std::clamp(value.toDouble(), 0.0, 0.95 - c->crop.right);
    else if (key == QLatin1String("cropRight")) c->crop.right = std::clamp(value.toDouble(), 0.0, 0.95 - c->crop.left);
    else if (key == QLatin1String("cropTop")) c->crop.top = std::clamp(value.toDouble(), 0.0, 0.95 - c->crop.bottom);
    else if (key == QLatin1String("cropBottom")) c->crop.bottom = std::clamp(value.toDouble(), 0.0, 0.95 - c->crop.top);
    else return false;
    return true;
}

} // namespace

void ProjectController::setClipProperty(const QString &clipId, const QString &key, const QVariant &value)
{
    setClipProperties(clipId, QVariantMap{{key, value}});
}

void ProjectController::setClipProperties(const QString &clipId, const QVariantMap &values)
{
    const Clip *existing = edit::clipById(m_project, clipId);
    if (!existing || values.isEmpty())
        return;
    const Micros rel = std::clamp<Micros>(m_playhead - existing->start, 0, existing->duration);
    const QStringList keys = values.keys();
    edit(tr("Change %1").arg(keys.join(QStringLiteral(", "))), [&](Project &p) {
        Clip *c = edit::clipById(p, clipId);
        if (!c)
            return false;
        bool any = false;
        for (auto it = values.begin(); it != values.end(); ++it)
            any = applyClipProperty(p, c, rel, it.key(), it.value()) || any;
        return any;
    }, clipId + keys.join(QLatin1Char('|')));
}

void ProjectController::toggleKeyframe(const QString &clipId, const QString &prop)
{
    const auto p = propFromKey(prop);
    const Clip *c = edit::clipById(m_project, clipId);
    if (!p || !c)
        return;
    const Micros rel = m_playhead - c->start;
    if (rel < 0 || rel > c->duration) {
        setStatus(tr("Place the playhead over the clip to create keyframes"));
        return;
    }
    edit(tr("Keyframe"), [&](Project &proj) {
        Clip *cc = edit::clipById(proj, clipId);
        auto &kfs = cc->keyframes[static_cast<int>(*p)];
        const Micros tol = proj.frameDuration() / 2;
        const double current = propAt(*cc, *p, rel);
        if (!removeKeyframeAt(kfs, rel, tol))
            upsertKeyframe(kfs, {rel, current, Easing::Linear}, tol);
        else if (kfs.empty())
            setBaseValue(*cc, *p, current); // último keyframe removido: o valor visível vira o base
        return true;
    });
}

void ProjectController::setTransition(const QString &clipId, const QString &kind, double durationUs)
{
    edit(tr("Transition"), [&](Project &p) {
        auto ref = edit::findClip(p, clipId);
        if (!ref || ref->index == 0)
            return false;
        Clip &c = p.tracks[ref->track].clips[ref->index];
        if (kind.isEmpty() || kind == QLatin1String("none")) {
            c.transitionIn.reset();
            return true;
        }
        const Clip &prev = p.tracks[ref->track].clips[ref->index - 1];
        const Micros maxD = std::min(prev.duration, c.duration);
        c.transitionIn = Transition{transitionFromName(kind), std::clamp<Micros>(static_cast<Micros>(durationUs), 100'000, maxD)};
        return true;
    }, clipId + QStringLiteral("transition"));
}

void ProjectController::setAnimation(const QString &clipId, bool in, const QString &kind, double durationUs)
{
    edit(tr("Animation"), [&](Project &p) {
        Clip *c = edit::clipById(p, clipId);
        if (!c)
            return false;
        ClipAnimation &a = in ? c->animIn : c->animOut;
        a.kind = animationFromName(kind);
        a.duration = std::clamp<Micros>(static_cast<Micros>(durationUs), 100'000, c->duration);
        return true;
    }, clipId + (in ? QStringLiteral("animIn") : QStringLiteral("animOut")));
}

namespace {
std::vector<SpeedPoint> pointsFrom(const QVariantList &list)
{
    std::vector<SpeedPoint> out;
    for (const QVariant &v : list) {
        const QVariantMap m = v.toMap();
        out.push_back({m.value("u").toDouble(), m.value("v").toDouble()});
    }
    return out;
}
} // namespace

QVariantList ProjectController::speedPresets() const
{
    QVariantList out;
    for (const auto &p : clv::speedPresets()) {
        QVariantList pts;
        for (const auto &pt : p.points)
            pts << QVariantMap{{"u", pt.u}, {"v", pt.v}};
        out << QVariantMap{{"id", p.id}, {"name", QCoreApplication::translate("SpeedCurves", p.name.toUtf8())}, {"points", pts}};
    }
    return out;
}

void ProjectController::setSpeedCurve(const QString &clipId, const QVariantList &points)
{
    const auto pts = pointsFrom(points);
    edit(tr("Speed curve"), [&](Project &p) { return edit::setSpeedCurve(p, clipId, pts); },
         clipId + QStringLiteral("curve"));
}

void ProjectController::applySpeedPreset(const QString &clipId, const QString &presetId)
{
    for (const auto &preset : clv::speedPresets())
        if (preset.id == presetId) {
            edit(tr("Curve: %1").arg(QCoreApplication::translate("SpeedCurves", preset.name.toUtf8())),
                 [&](Project &p) { return edit::setSpeedCurve(p, clipId, preset.points); });
            return;
        }
}

void ProjectController::setSpeed(const QString &clipId, double speed)
{
    edit(tr("Speed"), [&](Project &p) { return edit::setSpeed(p, clipId, speed); }, clipId + QStringLiteral("speed"));
}

void ProjectController::resetClipProperties(const QString &clipId, const QString &group)
{
    edit(tr("Reset"), [&](Project &p) {
        Clip *c = edit::clipById(p, clipId);
        if (!c)
            return false;
        auto clearKf = [&](std::initializer_list<Prop> props) {
            for (Prop pr : props)
                c->keyframes[static_cast<int>(pr)].clear();
        };
        if (group == QLatin1String("transform")) {
            const bool text = c->isText();
            c->transform = Transform{};
            if (text)
                c->transform.y = 0.6;
            c->crop = Crop{};
            clearKf({Prop::X, Prop::Y, Prop::Scale, Prop::Rotation, Prop::Opacity});
        } else if (group == QLatin1String("color")) {
            const ColorAdjust old = c->color;
            c->color = ColorAdjust{};
            c->color.filter = old.filter; // filtro e LUT têm seus próprios botões de remover
            c->color.filterIntensity = old.filterIntensity;
            c->color.lutPath = old.lutPath;
            c->color.lutIntensity = old.lutIntensity;
            clearKf({Prop::Brightness, Prop::Contrast, Prop::Saturation});
        } else if (group == QLatin1String("mask")) {
            c->mask = Mask{};
        } else if (group == QLatin1String("audio")) {
            c->volume = 1;
            c->fadeIn = c->fadeOut = 0;
            clearKf({Prop::Volume});
        } else {
            return false;
        }
        return true;
    });
}

// ---------------------------------------------------------------------------
// Histórico

void ProjectController::undo()
{
    if (!m_history.undo(m_project))
        return;
    m_lastCoalesceKey.clear();
    commitChange();
    emit assetsChanged();
}

void ProjectController::redo()
{
    if (!m_history.redo(m_project))
        return;
    m_lastCoalesceKey.clear();
    commitChange();
    emit assetsChanged();
}

// ---------------------------------------------------------------------------
// Reprodução

void ProjectController::seek(double t)
{
    const Micros clamped = std::clamp<Micros>(static_cast<Micros>(t), 0, std::max<Micros>(0, m_project.duration()));
    if (clamped == m_playhead && !m_playing)
        return;
    m_playhead = clamped;
    if (m_playing) {
        m_playStart = m_playhead;
        m_clock.restart();
        if (m_audio->available())
            m_audio->start(snapshot(), m_playhead);
    }
    emit playheadChanged();
    emit selectedClipChanged();
}

void ProjectController::play()
{
    if (m_playing || m_project.duration() <= 0)
        return;
    if (m_playhead >= m_project.duration() - m_project.frameDuration())
        m_playhead = 0;
    m_playing = true;
    m_playStart = m_playhead;
    m_clock.restart();
    if (m_audio->available())
        m_audio->start(snapshot(), m_playhead);
    m_ticker.start();
    emit playingChanged();
}

void ProjectController::pause()
{
    if (!m_playing)
        return;
    m_playing = false;
    m_ticker.stop();
    m_audio->stop();
    m_playhead = m_project.snapToFrame(m_playhead);
    emit playingChanged();
    emit playheadChanged();
    emit selectedClipChanged();
}

void ProjectController::togglePlay()
{
    m_playing ? pause() : play();
}

void ProjectController::tick()
{
    Micros pos = m_audio->available() ? m_audio->position() : m_playStart + m_clock.nsecsElapsed() / 1000;
    const Micros end = m_project.duration();
    if (pos >= end) {
        pos = end;
        m_playhead = pos;
        pause();
        return;
    }
    m_playhead = pos;
    emit playheadChanged();
}

void ProjectController::stepFrames(int n)
{
    pause();
    seek(static_cast<double>(m_project.snapToFrame(m_playhead) + n * m_project.frameDuration()));
}

void ProjectController::jumpToEdge(bool end)
{
    seek(end ? static_cast<double>(m_project.duration()) : 0.0);
}

// ---------------------------------------------------------------------------
// Export

void ProjectController::exportVideo(const QUrl &url, int height, double fps, const QString &codec, int crf,
                                    bool rangeOnly, bool hardware)
{
    if (m_exporting)
        return;
    const media::VideoCodec vc = media::videoCodecFromName(codec);
    QString path = localPath(url);
    if (QFileInfo(path).suffix().isEmpty())
        path += QLatin1Char('.') + media::containerFor(vc);

    engine::ExportOptions opts;
    opts.outputPath = path;
    const double aspect = static_cast<double>(m_project.width) / m_project.height;
    // A "altura" escolhida vale para o lado menor (vídeos verticais: 1080p = 1080 de largura).
    if (m_project.width >= m_project.height) {
        opts.encode.height = height;
        opts.encode.width = static_cast<int>(std::lround(height * aspect));
    } else {
        opts.encode.width = height;
        opts.encode.height = static_cast<int>(std::lround(height / aspect));
    }
    opts.encode.fps = fps > 0 ? fps : m_project.fps;
    opts.encode.crf = crf;
    opts.encode.codec = vc;
    opts.encode.hardware = hardware;
    if (rangeOnly && m_project.hasRange()) {
        opts.rangeStart = m_project.rangeStart();
        opts.rangeEnd = m_project.rangeEnd();
    }
    startExport(opts);
}

void ProjectController::exportAudio(const QUrl &url, bool rangeOnly)
{
    if (m_exporting)
        return;
    QString path = localPath(url);
    if (!media::availableAudioFormats().contains(QFileInfo(path).suffix().toLower()))
        path += QStringLiteral(".wav");
    engine::ExportOptions opts;
    opts.outputPath = path;
    opts.encode.video = false;
    if (rangeOnly && m_project.hasRange()) {
        opts.rangeStart = m_project.rangeStart();
        opts.rangeEnd = m_project.rangeEnd();
    }
    startExport(opts);
}

void ProjectController::startExport(const engine::ExportOptions &opts)
{
    pause();
    // A pasta padrão (ex.: ~/Videos) pode ainda não existir.
    QDir().mkpath(QFileInfo(opts.outputPath).absolutePath());
    m_exporting = true;
    m_exportProgress = 0;
    m_cancelExport = false;
    emit exportChanged();

    // A superfície GL (fallback) precisa nascer na thread da GUI.
    auto *surface = render::OffscreenRhi::createFallbackSurface();
    // Com um composto aberto, exporta o projeto inteiro (não só o conteúdo dele).
    auto snap = m_compoundStack.empty() ? snapshot() : std::make_shared<const Project>(rootProject());
    std::thread([this, snap, opts, surface]() {
        QString err;
        engine::Exporter exporter;
        auto progress = [this](double f) {
            QMetaObject::invokeMethod(this, [this, f] {
                m_exportProgress = f;
                emit exportChanged();
            }, Qt::QueuedConnection);
            return !m_cancelExport.load();
        };
        bool ok;
        if (opts.encode.video) {
            render::OffscreenRhi rhi;
            ok = rhi.create(&err, surface) && exporter.run(*snap, opts, progress, &err, &rhi);
        } else {
            ok = exporter.run(*snap, opts, progress, &err);
        }
        QMetaObject::invokeMethod(this, [this, ok, err, path = opts.outputPath, surface, encoder = exporter.encoderName()] {
            delete surface;
            m_exporting = false;
            emit exportChanged();
            emit exportFinished(ok, ok ? path : err);
            setStatus(ok ? tr("Exported: %1 (%2)").arg(path, encoder) : err);
        }, Qt::QueuedConnection);
    }).detach();
}

void ProjectController::exportFrame(const QUrl &url)
{
    if (m_exporting)
        return;
    pause();
    QString path = localPath(url);
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext != QLatin1String("png") && ext != QLatin1String("jpg") && ext != QLatin1String("jpeg"))
        path += QStringLiteral(".png");
    QDir().mkpath(QFileInfo(path).absolutePath());
    m_exporting = true;
    m_exportProgress = 0;
    emit exportChanged();
    auto *surface = render::OffscreenRhi::createFallbackSurface();
    auto snap = snapshot();
    const Micros t = m_playhead;
    std::thread([this, snap, path, surface, t]() {
        QString err;
        render::OffscreenRhi rhi;
        bool ok = rhi.create(&err, surface);
        if (ok) {
            const QImage img = engine::Exporter::renderStill(*snap, t, {}, &rhi, &err);
            // JPEG não tem alfa: achata sobre o fundo opaco do projeto (já composto).
            ok = !img.isNull() && img.convertToFormat(QImage::Format_RGB888).save(path, nullptr, 95);
            if (!ok && err.isEmpty())
                err = tr("Could not write %1").arg(path);
        }
        QMetaObject::invokeMethod(this, [this, ok, err, path, surface] {
            delete surface;
            m_exporting = false;
            emit exportChanged();
            emit exportFinished(ok, ok ? path : err);
            setStatus(ok ? tr("Frame saved: %1").arg(path) : err);
        }, Qt::QueuedConnection);
    }).detach();
}

void ProjectController::cancelExport()
{
    m_cancelExport = true;
}

// ---------------------------------------------------------------------------
// Utilitários

QString ProjectController::formatTime(double us) const
{
    const Micros t = static_cast<Micros>(us);
    const Micros totalFrames = static_cast<Micros>(std::floor(static_cast<double>(t) * m_project.fps / kSecond + 1e-6));
    const int fpsInt = std::max(1, static_cast<int>(std::lround(m_project.fps)));
    const Micros secs = t / kSecond;
    return QStringLiteral("%1:%2:%3")
        .arg(secs / 60, 2, 10, QLatin1Char('0'))
        .arg(secs % 60, 2, 10, QLatin1Char('0'))
        .arg(totalFrames % fpsInt, 2, 10, QLatin1Char('0'));
}

QString ProjectController::assetPath(const QString &assetId) const
{
    const MediaAsset *a = m_project.asset(assetId);
    return a ? a->path : QString();
}

// ---------------------------------------------------------------------------
// Legendas

bool ProjectController::transcriptionAvailable() const
{
    return engine::transcriptionAvailable();
}

QVariantList ProjectController::captionModels() const
{
    QVariantList out;
    for (const auto &m : engine::whisperModels())
        out << QVariantMap{
            {"id", m.id},
            {"label", m.label},
            {"sizeMb", static_cast<int>(m.sizeBytes / 1'000'000)},
            {"description", QCoreApplication::translate("Models", m.description.toUtf8())},
            {"installed", engine::whisperModelInstalled(m.id)},
        };
    return out;
}

QVariantList ProjectController::audioTracks() const
{
    QVariantList out;
    for (int i = 0; i < static_cast<int>(m_project.tracks.size()); ++i) {
        const Track &t = m_project.tracks[i];
        bool hasAudio = false;
        for (const Clip &c : t.clips)
            if (const MediaAsset *a = c.isText() ? nullptr : m_project.asset(c.assetId))
                hasAudio = hasAudio || a->hasAudio;
        if (hasAudio)
            out << QVariantMap{{"index", i}, {"name", i == 0 ? tr("Main") : t.name}};
    }
    return out;
}

void ProjectController::downloadModel(const QString &id)
{
    const engine::AiModel *model = engine::findAiModel(id);
    if (m_modelReply || !model || engine::aiModelInstalled(id))
        return;
    if (!m_network)
        m_network = new QNetworkAccessManager(this);
    QDir().mkpath(engine::whisperModelsDir());
    const QString finalPath = engine::aiModelPath(id);
    auto *file = new QFile(finalPath + QStringLiteral(".part"), this);
    if (!file->open(QIODevice::WriteOnly)) {
        emit errorOccurred(tr("Could not write to %1").arg(file->fileName()));
        delete file;
        return;
    }
    QNetworkRequest req(model->url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    m_modelReply = m_network->get(req);
    m_downloadingModel = id;
    m_modelDownloadProgress = 0;
    emit captionModelsChanged();
    emit modelDownloadProgressChanged();

    connect(m_modelReply, &QNetworkReply::readyRead, file, [this, file] { file->write(m_modelReply->readAll()); });
    connect(m_modelReply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        m_modelDownloadProgress = total > 0 ? static_cast<double>(got) / total : 0;
        emit modelDownloadProgressChanged();
    });
    connect(m_modelReply, &QNetworkReply::finished, this, [this, file, finalPath, id] {
        file->write(m_modelReply->readAll());
        file->close();
        bool ok = m_modelReply->error() == QNetworkReply::NoError && file->size() > 1'000'000;
        QString err = m_modelReply->errorString();
        // Conferência do conteúdo baixado (modelos com hash conhecido).
        const engine::AiModel *m = engine::findAiModel(id);
        if (ok && m && !m->sha256.isEmpty() && file->open(QIODevice::ReadOnly)) {
            QCryptographicHash h(QCryptographicHash::Sha256);
            h.addData(file);
            file->close();
            if (QString::fromLatin1(h.result().toHex()) != m->sha256) {
                ok = false;
                err = tr("the downloaded file is corrupted (checksum mismatch)");
            }
        }
        const bool cancelled = m_modelReply->error() == QNetworkReply::OperationCanceledError;
        if (ok) {
            QFile::remove(finalPath);
            file->rename(finalPath);
        } else {
            file->remove();
        }
        file->deleteLater();
        m_modelReply->deleteLater();
        m_modelReply = nullptr;
        m_downloadingModel.clear();
        emit captionModelsChanged();
        if (ok) {
            setStatus(tr("Model %1 installed").arg(id));
            if (id == engine::kMattingModel)
                refreshMattes();
            if (id == engine::kVoiceModel)
                refreshVoice();
            emit selectedClipChanged();
        }
        else if (!cancelled)
            emit errorOccurred(tr("Model download failed: %1").arg(err));
    });
}

void ProjectController::cancelModelDownload()
{
    if (m_modelReply)
        m_modelReply->abort();
}

void ProjectController::generateCaptions(const QString &modelId, const QString &language, int trackIndex, bool replace)
{
    if (m_transcribing)
        return;
    if (!engine::whisperModelInstalled(modelId)) {
        emit errorOccurred(tr("Download the model before generating captions"));
        return;
    }
    pause();
    engine::TranscribeOptions opts;
    opts.modelPath = engine::whisperModelPath(modelId);
    opts.language = language.isEmpty() ? QStringLiteral("auto") : language;
    opts.trackIndex = trackIndex;
    m_transcribing = true;
    m_transcribeProgress = 0;
    m_cancelTranscription = false;
    emit transcribeChanged();

    auto snap = snapshot();
    std::thread([this, snap, opts, replace] {
        std::vector<CaptionSegment> segs;
        QString err;
        const bool ok = engine::transcribeProject(
            *snap, opts,
            [this](double f) {
                QMetaObject::invokeMethod(this, [this, f] {
                    m_transcribeProgress = f;
                    emit transcribeChanged();
                }, Qt::QueuedConnection);
                return !m_cancelTranscription.load();
            },
            segs, &err);
        QMetaObject::invokeMethod(this, [this, ok, err, segs = std::move(segs), replace] {
            m_transcribing = false;
            emit transcribeChanged();
            if (!ok) {
                if (!m_cancelTranscription)
                    emit errorOccurred(err);
                return;
            }
            if (segs.empty()) {
                emit errorOccurred(tr("No speech recognized"));
                return;
            }
            edit(tr("Auto captions"), [&](Project &p) {
                if (replace)
                    for (int i = static_cast<int>(p.tracks.size()) - 1; i >= 1; --i)
                        if (p.tracks[i].captions)
                            p.tracks.erase(p.tracks.begin() + i);
                addCaptionTrack(p, segs);
                return true;
            });
            setStatus(tr("%n caption(s) generated", nullptr, static_cast<int>(segs.size())));
        }, Qt::QueuedConnection);
    }).detach();
}

void ProjectController::cancelTranscription()
{
    m_cancelTranscription = true;
}

bool ProjectController::importSubtitles(const QUrl &url)
{
    QFile f(localPath(url));
    if (!f.open(QIODevice::ReadOnly)) {
        emit errorOccurred(tr("Could not open %1").arg(f.fileName()));
        return false;
    }
    const auto segs = reflowCaptions(parseSubtitles(QString::fromUtf8(f.readAll())), 80);
    if (segs.empty()) {
        emit errorOccurred(tr("No valid captions in %1").arg(QFileInfo(f).fileName()));
        return false;
    }
    edit(tr("Import captions"), [&](Project &p) {
        addCaptionTrack(p, segs, QFileInfo(f).completeBaseName());
        return true;
    });
    setStatus(tr("%n caption(s) imported", nullptr, static_cast<int>(segs.size())));
    return true;
}

bool ProjectController::exportSubtitles(const QUrl &url)
{
    // Faixa escolhida: a do texto selecionado; senão a primeira faixa de legendas.
    const Track *track = nullptr;
    if (m_selection.size() == 1)
        if (auto ref = edit::findClip(m_project, m_selection.first()))
            if (m_project.tracks[ref->track].kind == TrackKind::Text)
                track = &m_project.tracks[ref->track];
    for (const auto &t : m_project.tracks)
        if (!track && t.captions)
            track = &t;
    if (!track) {
        emit errorOccurred(tr("There is no caption track to export"));
        return false;
    }
    QString path = localPath(url);
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".srt");
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(toSrt(captionsFromTrack(*track)).toUtf8()) < 0 || !f.commit()) {
        emit errorOccurred(tr("Failed to save %1").arg(path));
        return false;
    }
    setStatus(tr("Captions exported: %1").arg(path));
    return true;
}

void ProjectController::applyStyleToTrack(const QString &clipId)
{
    auto ref = edit::findClip(m_project, clipId);
    if (!ref)
        return;
    edit(tr("Apply style to captions"), [&](Project &p) {
        Track &t = p.tracks[ref->track];
        const Clip src = t.clips[ref->index];
        if (!src.isText())
            return false;
        for (Clip &c : t.clips) {
            if (!c.isText())
                continue;
            c.style = src.style;
            c.transform = src.transform;
            c.animIn = src.animIn;
            c.animOut = src.animOut;
        }
        return true;
    });
}

// ---------------------------------------------------------------------------
// Filtros e ajustes

QVariantList ProjectController::filters() const
{
    QVariantList out;
    for (const auto &f : builtinFilters())
        out << QVariantMap{{"id", f.id}, {"name", QCoreApplication::translate("Filters", f.name.toUtf8())}, {"category", f.category}};
    return out;
}

void ProjectController::applyFilter(const QString &clipId, const QString &filterId)
{
    edit(tr("Filter"), [&](Project &p) {
        Clip *c = edit::clipById(p, clipId);
        if (!c || c->isText())
            return false;
        if (!filterId.isEmpty() && !findBuiltinFilter(filterId))
            return false;
        c->color.filter = filterId;
        if (c->color.filterIntensity <= 0)
            c->color.filterIntensity = 0.8;
        return true;
    });
}

void ProjectController::addFilterClip(const QString &filterId)
{
    QString id;
    edit(filterId.isEmpty() ? tr("Add adjustment") : tr("Add filter"), [&](Project &p) {
        // Cobre do cursor até o fim do clipe principal sob ele (ou 3 s).
        Micros duration = 3 * kSecond;
        for (const auto &c : p.tracks[0].clips)
            if (m_playhead >= c.start && m_playhead < c.end())
                duration = std::max(p.frameDuration(), c.end() - m_playhead);
        id = edit::insertClip(p, createAdjustClip(p.snapToFrame(m_playhead), duration, filterId), -1);
        return !id.isEmpty();
    });
    if (!id.isEmpty()) {
        m_selection = {id};
        emit selectionChanged();
        emit selectedClipChanged();
    }
}

bool ProjectController::loadLut(const QString &clipId, const QUrl &url)
{
    const QString path = localPath(url);
    QString err;
    if (!loadCube(path, &err).isValid()) {
        emit errorOccurred(tr("Invalid LUT (%1): %2").arg(QFileInfo(path).fileName(), err));
        return false;
    }
    setClipProperties(clipId, {{QStringLiteral("lutPath"), path}, {QStringLiteral("lutIntensity"), 1.0}});
    return true;
}

QString ProjectController::previewFrameRef() const
{
    auto ref = [&](const Clip &c) {
        const Micros rel = std::clamp<Micros>(m_playhead - c.start, 0, std::max<Micros>(0, c.duration - 1));
        return QStringLiteral("%1/%2").arg(c.assetId).arg(sourceTimeAt(m_project, c, rel));
    };
    // Clipe selecionado, senão o clipe visual sob o cursor, senão o primeiro clipe de vídeo/imagem.
    if (m_selection.size() == 1)
        if (const Clip *c = edit::clipById(m_project, m_selection.first()); c && c->hasMedia())
            if (const MediaAsset *a = m_project.asset(c->assetId); a && a->hasVideo)
                return ref(*c);
    const Clip *fallback = nullptr;
    for (const auto &t : m_project.tracks)
        for (const auto &c : t.clips) {
            const MediaAsset *a = c.hasMedia() ? m_project.asset(c.assetId) : nullptr;
            if (!a || !a->hasVideo)
                continue;
            if (m_playhead >= c.start && m_playhead < c.end())
                return ref(c);
            if (!fallback)
                fallback = &c;
        }
    return fallback ? ref(*fallback) : QString();
}

// ---------------------------------------------------------------------------
// Proxies

void ProjectController::setUseProxies(bool on)
{
    if (on == m_useProxies)
        return;
    m_useProxies = on;
    QSettings().setValue(QStringLiteral("playback/useProxies"), on);
    emit proxiesChanged();
    ++m_revisionCounter;
    emit revisionChanged(); // redesenha o preview com a outra fonte
}

void ProjectController::refreshProxies()
{
    for (const auto &[id, a] : m_project.assets) {
        if (a.kind != MediaKind::Video || !QFileInfo::exists(a.path))
            continue;
        if (!engine::registeredProxy(a.path).isEmpty() || engine::discoverProxy(a.path))
            continue;
        if (engine::proxyRecommended(a))
            enqueueProxy(a.path);
    }
}

void ProjectController::generateProxy(const QString &assetId)
{
    const MediaAsset *a = m_project.asset(assetId);
    if (!a || a->kind != MediaKind::Video)
        return;
    if (!engine::registeredProxy(a->path).isEmpty()) {
        setStatus(tr("The proxy for %1 already exists").arg(a->name));
        return;
    }
    enqueueProxy(a->path);
}

void ProjectController::enqueueProxy(const QString &mediaPath)
{
    if (m_proxyBuilding == mediaPath || m_proxyQueue.contains(mediaPath))
        return;
    m_proxyQueue << mediaPath;
    emit proxiesChanged();
    emit assetsChanged();
    startNextProxy();
}

void ProjectController::cancelProxies()
{
    m_proxyQueue.clear();
    *m_proxyCancel = true;
    m_proxyBuilding.clear();
    emit proxiesChanged();
    emit assetsChanged();
}

void ProjectController::startNextProxy()
{
    if (!m_proxyBuilding.isEmpty() || m_proxyQueue.isEmpty())
        return;
    m_proxyBuilding = m_proxyQueue.takeFirst();
    m_proxyProgress = 0;
    m_proxyCancel = std::make_shared<std::atomic<bool>>(false);
    emit proxiesChanged();
    emit assetsChanged();

    const QString path = m_proxyBuilding;
    auto cancel = m_proxyCancel;
    std::thread([this, path, cancel] {
        QString err;
        const QString out = engine::proxyPathFor(path);
        double lastSent = 0;
        const bool ok = engine::generateProxy(
            path, out,
            [&](double f) {
                // Atualiza a interface a cada 2%.
                if (f - lastSent >= 0.02 || f >= 1.0) {
                    lastSent = f;
                    if (!*cancel)
                        QMetaObject::invokeMethod(this, [this, f] {
                            m_proxyProgress = f;
                            emit assetsChanged();
                        }, Qt::QueuedConnection);
                }
                return !*cancel;
            },
            &err);
        if (*cancel && !ok)
            return; // controlador pode estar sendo destruído; nada a publicar
        QMetaObject::invokeMethod(this, [this, ok, err, path, out] {
            m_proxyBuilding.clear();
            if (ok) {
                engine::registerProxy(path, out);
                setStatus(tr("Proxy ready: %1").arg(QFileInfo(path).fileName()));
                ++m_revisionCounter;
                emit revisionChanged();
            } else {
                emit errorOccurred(tr("Failed to generate proxy for %1: %2").arg(QFileInfo(path).fileName(), err));
            }
            emit proxiesChanged();
            emit assetsChanged();
            startNextProxy();
        }, Qt::QueuedConnection);
    }).detach();
}

// ---------------------------------------------------------------------------
// Área de transferência e grupos

bool ProjectController::canPaste() const
{
    if (!m_clipboard.empty())
        return true;
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    return mime && mime->hasFormat(QString::fromLatin1(kClipboardMime));
}

void ProjectController::copySelection()
{
    if (m_selection.isEmpty())
        return;
    m_clipboard = copyClips(m_project, m_selection);
    auto *mime = new QMimeData;
    mime->setData(QString::fromLatin1(kClipboardMime), clipboardToJson(m_clipboard));
    QGuiApplication::clipboard()->setMimeData(mime);
    emit clipboardChanged();
    setStatus(tr("%n clip(s) copied", nullptr, static_cast<int>(m_clipboard.items.size())));
}

void ProjectController::cutSelection()
{
    if (m_selection.isEmpty())
        return;
    copySelection();
    const QStringList ids = m_selection;
    edit(tr("Cut"), [&](Project &p) { return edit::deleteClips(p, ids, false); });
}

void ProjectController::paste()
{
    // Prefere a área de transferência do sistema (permite colar entre janelas/projetos).
    Clipboard cb = m_clipboard;
    if (const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
        mime && mime->hasFormat(QString::fromLatin1(kClipboardMime))) {
        Clipboard sys;
        if (clipboardFromJson(mime->data(QString::fromLatin1(kClipboardMime)), sys))
            cb = std::move(sys);
    }
    if (cb.empty())
        return;
    QStringList created;
    edit(tr("Paste"), [&](Project &p) {
        created = pasteClips(p, cb, p.snapToFrame(m_playhead));
        return !created.isEmpty();
    });
    if (!created.isEmpty()) {
        m_selection = created;
        emit selectionChanged();
        emit selectedClipChanged();
        emit assetsChanged();
        refreshProxies();
    }
}

void ProjectController::copyAttributes()
{
    const QString primary = primaryClipId();
    if (primary.isEmpty())
        return;
    if (const Clip *c = edit::clipById(m_project, primary)) {
        m_attributeSource = *c;
        emit clipboardChanged();
        setStatus(tr("Attributes copied"));
    }
}

void ProjectController::pasteAttributesToSelection()
{
    if (!m_attributeSource || m_selection.isEmpty())
        return;
    const QStringList ids = m_selection;
    edit(tr("Paste attributes"), [&](Project &p) {
        bool any = false;
        for (const auto &id : ids)
            if (Clip *c = edit::clipById(p, id); c && c->id != m_attributeSource->id) {
                pasteAttributes(*c, *m_attributeSource);
                any = true;
            }
        return any;
    });
}

bool ProjectController::compoundHasAudio(const Sequence &seq) const
{
    for (const auto &t : seq.tracks)
        for (const auto &c : t.clips) {
            if (c.audioMuted || t.muted)
                continue;
            if (const Sequence *inner = c.isCompound() ? m_project.compound(c.compoundId) : nullptr) {
                if (inner != &seq && compoundHasAudio(*inner))
                    return true;
            } else if (const MediaAsset *a = c.hasMedia() ? m_project.asset(c.assetId) : nullptr; a && a->hasAudio) {
                return true;
            }
        }
    return false;
}

void ProjectController::createCompound()
{
    const QStringList ids = edit::expandToGroups(m_project, m_selection);
    if (ids.isEmpty())
        return;
    edit::CompoundError err = edit::CompoundError::None;
    QString id;
    const QString name = tr("Compound clip %1").arg(m_project.compounds.size() + 1);
    edit(tr("Create compound clip"), [&](Project &p) {
        id = edit::createCompound(p, ids, name, &err);
        return !id.isEmpty();
    });
    if (id.isEmpty()) {
        setStatus(err == edit::CompoundError::Locked ? tr("Unlock the tracks to create a compound clip")
                  : err == edit::CompoundError::MainTrackGap
                      ? tr("On the main track, select neighboring clips that cover the whole compound clip")
                      : tr("Select the clips to combine"));
        return;
    }
    select(id);
    setStatus(tr("%n clip(s) combined into %1", nullptr, static_cast<int>(ids.size())).arg(name));
}

void ProjectController::breakCompound()
{
    const QString id = primaryClipId();
    const Clip *c = edit::clipById(m_project, id);
    if (!c || !c->isCompound())
        return;
    QStringList back;
    edit(tr("Break compound clip"), [&](Project &p) {
        back = edit::breakCompound(p, id);
        return !back.isEmpty();
    });
    if (!back.isEmpty()) {
        m_selection = back;
        m_primary = back.first();
        emit selectionChanged();
        emit selectedClipChanged();
    }
}

Project ProjectController::rootProject() const
{
    Project cur = m_project;
    for (auto it = m_compoundStack.rbegin(); it != m_compoundStack.rend(); ++it) {
        Project outer = it->outer;
        edit::mergeCompoundWorkspace(outer, it->seqId, cur);
        cur = std::move(outer);
    }
    return cur;
}

QVariantList ProjectController::compoundPath() const
{
    QVariantList out;
    for (const auto &f : m_compoundStack)
        out << QVariantMap{{"name", f.name}};
    return out;
}

void ProjectController::openCompound(const QString &clipId)
{
    const Clip *c = edit::clipById(m_project, clipId);
    const Sequence *seq = c && c->isCompound() ? m_project.compound(c->compoundId) : nullptr;
    auto work = seq ? edit::compoundWorkspace(m_project, seq->id) : std::nullopt;
    if (!work)
        return;
    pause();
    int uses = 0;
    m_project.forEachClip([&](const Clip &x) { uses += x.isCompound() && x.compoundId == seq->id; });
    CompoundFrame f;
    f.clipId = clipId;
    f.seqId = seq->id;
    f.name = seq->name;
    f.playhead = m_playhead;
    f.selection = m_selection;
    f.savedRevision = m_savedRevision;
    const bool outerModified = modified();
    const Micros inner = std::clamp<Micros>(c->sourceIn + (m_playhead - c->start), 0, work->duration());
    f.outer = std::move(m_project);
    f.history = std::move(m_history);
    m_compoundStack.push_back(std::move(f));
    m_project = std::move(*work);
    m_history = History();
    m_savedRevision = outerModified ? -1 : 0;
    m_lastCoalesceKey.clear();
    m_selection.clear();
    m_playhead = m_project.snapToFrame(inner);
    commitChange();
    emit selectionChanged();
    emit playheadChanged();
    emit compoundPathChanged();
    setStatus(uses > 1 ? tr("Editing %1: changes apply to all %2 copies of it").arg(m_compoundStack.back().name).arg(uses)
                       : tr("Editing %1").arg(m_compoundStack.back().name));
}

void ProjectController::closeCompound()
{
    if (m_compoundStack.empty())
        return;
    pause();
    CompoundFrame f = std::move(m_compoundStack.back());
    m_compoundStack.pop_back();
    const bool innerClean = m_history.revision() == m_savedRevision;
    Project merged = f.outer;
    edit::mergeCompoundWorkspace(merged, f.seqId, m_project);
    const bool changed = serializeProject(merged) != serializeProject(f.outer);
    m_history = std::move(f.history);
    if (changed)
        m_history.push(f.outer, tr("Edit compound clip"));
    // Salvo lá dentro: continua "salvo" se nada mudou depois do save.
    m_savedRevision = f.savedRevision == -2 ? (innerClean ? m_history.revision() : -1) : f.savedRevision;
    m_project = std::move(merged);
    m_lastCoalesceKey.clear();
    m_selection = edit::clipById(m_project, f.clipId) ? QStringList{f.clipId} : QStringList{};
    m_primary = f.clipId;
    m_playhead = std::min(f.playhead, m_project.duration());
    commitChange();
    emit selectionChanged();
    emit playheadChanged();
    emit compoundPathChanged();
    emit modifiedChanged();
}

void ProjectController::groupSelection()
{
    if (m_selection.size() < 2)
        return;
    const QStringList ids = m_selection;
    edit(tr("Group"), [&](Project &p) { return !edit::groupClips(p, ids).isEmpty(); });
}

void ProjectController::ungroupSelection()
{
    const QStringList ids = m_selection;
    edit(tr("Ungroup"), [&](Project &p) { return edit::ungroupClips(p, ids); });
}

void ProjectController::selectInRange(double t0, double t1, const QVariantList &trackIndices, bool additive)
{
    const Micros a = static_cast<Micros>(std::min(t0, t1)), b = static_cast<Micros>(std::max(t0, t1));
    QStringList found;
    for (const QVariant &v : trackIndices) {
        const int ti = v.toInt();
        if (ti < 0 || ti >= static_cast<int>(m_project.tracks.size()) || m_project.tracks[ti].locked)
            continue;
        for (const Clip &c : m_project.tracks[ti].clips)
            if (c.start < b && c.end() > a)
                found << c.id;
    }
    found = edit::expandToGroups(m_project, found);
    QStringList next = additive ? m_selection : QStringList{};
    for (const auto &id : found)
        if (!next.contains(id))
            next << id;
    if (next == m_selection)
        return;
    m_selection = next;
    emit selectionChanged();
    emit selectedClipChanged();
    emit projectChanged();
}

void ProjectController::selectAll()
{
    QStringList all;
    for (const auto &t : m_project.tracks)
        if (!t.locked)
            for (const auto &c : t.clips)
                all << c.id;
    m_selection = all;
    emit selectionChanged();
    emit selectedClipChanged();
    emit projectChanged();
}

// ---------------------------------------------------------------------------
// Recuperação

QString ProjectController::recoveryDir()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("recovery"));
}

QString ProjectController::recoveryFileFor(const QString &projectId) const
{
    return QDir(recoveryDir()).filePath(projectId + QStringLiteral(".clivcut"));
}

void ProjectController::removeRecovery()
{
    m_recoveryLock.reset();
    QFile::remove(recoveryFileFor(m_project.id));
    QFile::remove(recoveryFileFor(m_project.id) + QStringLiteral(".json"));
}

void ProjectController::autosaveNow()
{
    if (!modified() || m_revisionCounter == m_autosavedRevision || m_project.id.isEmpty())
        return;
    QDir().mkpath(recoveryDir());
    const QString file = recoveryFileFor(m_project.id);
    if (!m_recoveryLock) {
        m_recoveryLock = std::make_unique<QLockFile>(file + QStringLiteral(".lock"));
        m_recoveryLock->setStaleLockTime(0); // só o PID decide se está vivo
        m_recoveryLock->tryLock(0);
    }
    QString err;
    if (!saveProjectFile(rootProject(), file, &err)) {
        qWarning("clivcut: recovery copy failed: %s", qPrintable(err));
        return;
    }
    // Metadados: de onde veio o projeto e quando foi gravado.
    QSaveFile meta(file + QStringLiteral(".json"));
    if (meta.open(QIODevice::WriteOnly)) {
        meta.write(QJsonDocument(QJsonObject{{"originalPath", m_filePath},
                                             {"name", m_project.name},
                                             {"savedAt", QDateTime::currentDateTime().toString(Qt::ISODate)},
                                             {"duration", static_cast<double>(m_project.duration())}})
                       .toJson());
        meta.commit();
    }
    m_autosavedRevision = m_revisionCounter;
}

QVariantList ProjectController::recoverableProjects() const
{
    QVariantList out;
    const QDir dir(recoveryDir());
    for (const QFileInfo &fi : dir.entryInfoList({QStringLiteral("*.clivcut")}, QDir::Files, QDir::Time)) {
        if (fi.completeBaseName() == m_project.id)
            continue; // a sessão atual
        QLockFile lock(fi.absoluteFilePath() + QStringLiteral(".lock"));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0))
            continue; // outra janela do clivcut ainda está editando este projeto
        lock.unlock();
        QJsonObject meta;
        QFile mf(fi.absoluteFilePath() + QStringLiteral(".json"));
        if (mf.open(QIODevice::ReadOnly))
            meta = QJsonDocument::fromJson(mf.readAll()).object();
        out << QVariantMap{
            {"file", fi.absoluteFilePath()},
            {"name", meta.value("name").toString(fi.completeBaseName())},
            {"originalPath", meta.value("originalPath").toString()},
            {"savedAt", QDateTime::fromString(meta.value("savedAt").toString(), Qt::ISODate)
                            .toString(QStringLiteral("dd/MM/yyyy HH:mm"))},
            {"duration", meta.value("duration").toDouble()},
        };
    }
    return out;
}

bool ProjectController::recoverProject(const QString &recoveryFile)
{
    Project p;
    QString err;
    if (!loadProjectFile(recoveryFile, p, &err)) {
        emit errorOccurred(tr("Could not recover: %1").arg(err));
        return false;
    }
    QString original;
    QFile mf(recoveryFile + QStringLiteral(".json"));
    if (mf.open(QIODevice::ReadOnly))
        original = QJsonDocument::fromJson(mf.readAll()).object().value("originalPath").toString();
    setProject(std::move(p), original);
    // O conteúdo recuperado ainda não foi salvo: fica marcado como modificado
    // (um passo de histórico vazio) e a cópia continua até salvar.
    m_history.push(m_project, tr("Recover project"));
    m_autosavedRevision = 0;
    commitChange();
    autosaveNow();
    if (recoveryFile != recoveryFileFor(m_project.id)) {
        QFile::remove(recoveryFile + QStringLiteral(".lock"));
        QFile::remove(recoveryFile);
        QFile::remove(recoveryFile + QStringLiteral(".json"));
    }
    setStatus(tr("Project recovered: save to keep the changes"));
    return true;
}

void ProjectController::discardRecovery(const QString &recoveryFile)
{
    QFile::remove(recoveryFile + QStringLiteral(".lock"));
    QFile::remove(recoveryFile);
    QFile::remove(recoveryFile + QStringLiteral(".json"));
}

void ProjectController::discardChanges()
{
    removeRecovery();
    m_savedRevision = m_history.revision();
    emit modifiedChanged();
}

// ---------------------------------------------------------------------------
// Editor de keyframes

namespace {
QVariantMap keyframeToMap(const Keyframe &k)
{
    return {{"t", static_cast<double>(k.t)}, {"v", k.v}, {"ease", easingName(k.ease)},
            {"bx1", k.bx1}, {"by1", k.by1}, {"bx2", k.bx2}, {"by2", k.by2}};
}
} // namespace

QVariantList ProjectController::keyframes(const QString &clipId, const QString &prop) const
{
    const Clip *c = edit::clipById(m_project, clipId);
    const auto p = propFromKey(prop);
    QVariantList out;
    if (c && p)
        for (const auto &k : c->keyframes[static_cast<int>(*p)])
            out << keyframeToMap(k);
    return out;
}

QVariantList ProjectController::sampleKeyframes(const QString &clipId, const QString &prop, int samples) const
{
    const Clip *c = edit::clipById(m_project, clipId);
    const auto p = propFromKey(prop);
    QVariantList out;
    if (!c || !p || samples < 2)
        return out;
    for (int i = 0; i < samples; ++i)
        out << propAt(*c, *p, c->duration * i / (samples - 1));
    return out;
}

void ProjectController::moveKeyframe(const QString &clipId, const QString &prop, double fromT, double toT, double value)
{
    const auto p = propFromKey(prop);
    if (!p)
        return;
    edit(tr("Move keyframe"), [&](Project &proj) {
        Clip *c = edit::clipById(proj, clipId);
        if (!c)
            return false;
        auto &list = c->keyframes[static_cast<int>(*p)];
        const Micros tol = proj.frameDuration() / 2;
        auto it = std::find_if(list.begin(), list.end(),
                               [&](const Keyframe &k) { return std::llabs(k.t - static_cast<Micros>(fromT)) <= tol; });
        if (it == list.end())
            return false;
        Keyframe k = *it;
        list.erase(it);
        k.t = std::clamp<Micros>(proj.snapToFrame(static_cast<Micros>(toT)), 0, c->duration);
        k.v = value;
        upsertKeyframe(list, k, tol); // substitui se cair sobre outro keyframe
        return true;
    }, clipId + prop + QStringLiteral("kfmove"));
}

void ProjectController::setKeyframeEasing(const QString &clipId, const QString &prop, double t, const QString &ease)
{
    const auto p = propFromKey(prop);
    if (!p)
        return;
    edit(tr("Keyframe easing"), [&](Project &proj) {
        Clip *c = edit::clipById(proj, clipId);
        if (!c)
            return false;
        for (auto &k : c->keyframes[static_cast<int>(*p)])
            if (std::llabs(k.t - static_cast<Micros>(t)) <= proj.frameDuration() / 2) {
                k.ease = easingFromName(ease);
                return true;
            }
        return false;
    });
}

void ProjectController::setKeyframeBezier(const QString &clipId, const QString &prop, double t, double x1, double y1,
                                          double x2, double y2)
{
    const auto p = propFromKey(prop);
    if (!p)
        return;
    edit(tr("Keyframe curve"), [&](Project &proj) {
        Clip *c = edit::clipById(proj, clipId);
        if (!c)
            return false;
        for (auto &k : c->keyframes[static_cast<int>(*p)])
            if (std::llabs(k.t - static_cast<Micros>(t)) <= proj.frameDuration() / 2) {
                k.ease = Easing::Bezier;
                k.bx1 = std::clamp(x1, 0.0, 1.0);
                k.by1 = std::clamp(y1, -1.0, 2.0);
                k.bx2 = std::clamp(x2, 0.0, 1.0);
                k.by2 = std::clamp(y2, -1.0, 2.0);
                return true;
            }
        return false;
    }, clipId + prop + QStringLiteral("bezier"));
}

void ProjectController::setEasingForAll(const QString &clipId, const QString &prop, const QString &ease)
{
    const auto p = propFromKey(prop);
    if (!p)
        return;
    edit(tr("Keyframes easing"), [&](Project &proj) {
        Clip *c = edit::clipById(proj, clipId);
        if (!c || c->keyframes[static_cast<int>(*p)].empty())
            return false;
        for (auto &k : c->keyframes[static_cast<int>(*p)])
            k.ease = easingFromName(ease);
        return true;
    });
}

// ---------------------------------------------------------------------------
// Volume (loudness) e redução de ruído

void ProjectController::normalizeLoudness(double targetLufs)
{
    if (m_normalizing)
        return;
    struct Job {
        QString clipId, path;
        Micros start, duration;
    };
    std::vector<Job> jobs;
    const QStringList ids = m_selection.isEmpty() ? QStringList{} : m_selection;
    for (const auto &id : ids) {
        const Clip *c = edit::clipById(m_project, id);
        const MediaAsset *a = c && c->hasMedia() ? m_project.asset(c->assetId) : nullptr;
        if (a && a->hasAudio && !c->audioMuted && c->speed > 0)
            jobs.push_back({c->id, a->path, c->sourceIn, c->sourceDuration()});
    }
    if (jobs.empty()) {
        setStatus(tr("Select clips with audio to normalize"));
        return;
    }
    m_normalizing = true;
    emit audioJobsChanged();
    std::thread([this, jobs, targetLufs] {
        QVariantMap gains;
        for (const Job &j : jobs) {
            const double lufs = media::measureLoudness(j.path, j.start, j.duration);
            if (std::isfinite(lufs))
                gains.insert(j.clipId, std::clamp(targetLufs - lufs, -24.0, 24.0));
        }
        QMetaObject::invokeMethod(this, [this, gains, targetLufs] {
            m_normalizing = false;
            emit audioJobsChanged();
            if (gains.isEmpty()) {
                emit errorOccurred(tr("There is no audible audio to measure"));
                return;
            }
            edit(tr("Normalize loudness"), [&](Project &p) {
                bool any = false;
                for (auto it = gains.begin(); it != gains.end(); ++it)
                    if (Clip *c = edit::clipById(p, it.key())) {
                        c->gainDb = it.value().toDouble();
                        any = true;
                    }
                return any;
            });
            setStatus(tr("Loudness normalized to %1 LUFS (%n clip(s))", nullptr, static_cast<int>(gains.size()))
                          .arg(targetLufs, 0, 'f', 0));
        }, Qt::QueuedConnection);
    }).detach();
}

void ProjectController::refreshDenoise()
{
    bool changed = false;
    m_project.forEachClip([&](const Clip &c) {
        const MediaAsset *a = c.hasMedia() && c.denoise > 0 ? m_project.asset(c.assetId) : nullptr;
        if (!a || !a->hasAudio || !QFileInfo::exists(a->path))
            return;
        if (!engine::registeredDenoised(a->path, c.denoise).isEmpty() || engine::discoverDenoised(a->path, c.denoise))
            return;
        const QString job = a->path + QLatin1Char('|') + QString::number(engine::quantizedDenoise(c.denoise));
        if (job != m_denoiseBuilding && !m_denoiseQueue.contains(job)) {
            m_denoiseQueue << job;
            changed = true;
        }
    });
    if (changed) {
        emit audioJobsChanged();
        startNextDenoise();
    }
}

void ProjectController::startNextDenoise()
{
    if (!m_denoiseBuilding.isEmpty() || m_denoiseQueue.isEmpty())
        return;
    m_denoiseBuilding = m_denoiseQueue.takeFirst();
    m_denoiseProgress = 0;
    m_denoiseCancel = std::make_shared<std::atomic<bool>>(false);
    const QString path = m_denoiseBuilding.section(QLatin1Char('|'), 0, -2);
    const double strength = m_denoiseBuilding.section(QLatin1Char('|'), -1).toDouble();
    auto cancel = m_denoiseCancel;
    emit selectedClipChanged();
    std::thread([this, path, strength, cancel] {
        const QString out = engine::denoisedPathFor(path, strength);
        QString err;
        double lastSent = 0;
        const bool ok = engine::generateDenoised(
            path, out, strength,
            [&](double f) {
                if (f - lastSent >= 0.03 && !*cancel) {
                    lastSent = f;
                    QMetaObject::invokeMethod(this, [this, f] {
                        m_denoiseProgress = f;
                        emit selectedClipChanged();
                    }, Qt::QueuedConnection);
                }
                return !*cancel;
            },
            &err);
        if (*cancel)
            return;
        QMetaObject::invokeMethod(this, [this, ok, err, path, strength, out] {
            m_denoiseBuilding.clear();
            if (ok) {
                engine::registerDenoised(path, strength, out);
                refreshEffects(); // o efeito passa a usar o áudio limpo
                setStatus(tr("Noise reduced: %1").arg(QFileInfo(path).fileName()));
                if (m_playing)
                    m_audio->updateProject(snapshot());
            } else {
                emit errorOccurred(tr("Noise reduction failed: %1").arg(err));
            }
            emit audioJobsChanged();
            emit selectedClipChanged();
            startNextDenoise();
        }, Qt::QueuedConnection);
    }).detach();
}

// ---------------------------------------------------------------------------
// Cache de renderização da pré-visualização

void ProjectController::setPreviewResolution(int side)
{
    side = std::clamp(side, 320, 4096);
    if (side == m_previewResolution)
        return;
    m_previewResolution = side;
    refreshRenderCache();
}

std::shared_ptr<const engine::RenderCacheView> ProjectController::renderCacheView() const
{
    std::lock_guard lock(m_snapMutex);
    return m_cacheView;
}

QVariantList ProjectController::cachedRanges() const
{
    return m_cachedRanges;
}

void ProjectController::refreshRenderCache()
{
    auto view = std::make_shared<engine::RenderCacheView>();
    view->longSide = m_previewResolution;
    const std::vector<QString> keys = engine::renderChunkKeys(m_project);
    view->files.resize(keys.size());
    QVariantList ranges;
    const Micros end = m_project.duration();
    for (size_t i = 0; i < keys.size(); ++i) {
        const QString file = engine::renderCacheFile(keys[i], m_previewResolution);
        if (!QFileInfo::exists(file))
            continue;
        view->files[i] = file;
        const double a = static_cast<double>(i * engine::kRenderChunk);
        const double b = static_cast<double>(std::min<Micros>((i + 1) * engine::kRenderChunk, end));
        // Junta trechos vizinhos numa faixa só.
        if (!ranges.isEmpty() && ranges.last().toMap().value("end").toDouble() == a) {
            QVariantMap last = ranges.last().toMap();
            last["end"] = b;
            ranges.last() = last;
        } else {
            ranges << QVariantMap{{"start", a}, {"end", b}};
        }
    }
    {
        std::lock_guard lock(m_snapMutex);
        m_cacheView = view;
    }
    m_cachedRanges = ranges;
    emit renderCacheChanged();
}

void ProjectController::renderPreview()
{
    if (m_rendering || m_project.duration() <= 0)
        return;
    const Micros from = m_project.hasRange() ? m_project.rangeStart() : 0;
    const Micros to = m_project.hasRange() ? m_project.rangeEnd() : m_project.duration();
    std::vector<int> chunks;
    for (int i = static_cast<int>(from / engine::kRenderChunk); i * engine::kRenderChunk < to; ++i)
        if (i < engine::renderChunkCount(m_project))
            chunks.push_back(i);
    if (chunks.empty())
        return;
    m_rendering = true;
    m_renderProgress = 0;
    m_renderCancel = std::make_shared<std::atomic<bool>>(false);
    emit renderCacheChanged();
    setStatus(tr("Rendering preview…"));

    auto *surface = render::OffscreenRhi::createFallbackSurface();
    auto snap = snapshot();
    auto cancel = m_renderCancel;
    const int side = m_previewResolution;
    std::thread([this, snap, chunks, side, surface, cancel] {
        QString err;
        render::OffscreenRhi rhi;
        bool ok = rhi.create(&err, surface);
        if (ok)
            ok = engine::renderChunks(
                *snap, chunks, side, &rhi,
                [this, cancel](double f) {
                    if (!*cancel)
                        QMetaObject::invokeMethod(this, [this, f] {
                            m_renderProgress = f;
                            emit renderCacheChanged();
                        }, Qt::QueuedConnection);
                    return !*cancel;
                },
                [this, cancel](int) {
                    if (!*cancel)
                        QMetaObject::invokeMethod(this, [this] { refreshRenderCache(); }, Qt::QueuedConnection);
                },
                &err);
        if (*cancel) {
            // Controlador pode estar sendo destruído: só libera a superfície (na thread da GUI).
            QMetaObject::invokeMethod(this, [surface] { delete surface; }, Qt::QueuedConnection);
            return;
        }
        QMetaObject::invokeMethod(this, [this, ok, err, surface] {
            delete surface;
            m_rendering = false;
            refreshRenderCache();
            if (ok)
                setStatus(tr("Preview rendered"));
            else
                emit errorOccurred(err);
        }, Qt::QueuedConnection);
    }).detach();
}

void ProjectController::cancelRenderPreview()
{
    if (!m_rendering)
        return;
    *m_renderCancel = true;
    m_rendering = false;
    refreshRenderCache();
    setStatus(tr("Render cancelled"));
}

void ProjectController::clearRenderCache()
{
    cancelRenderPreview();
    const qint64 freed = engine::clearRenderCache();
    refreshRenderCache();
    setStatus(tr("Render cache cleared (%1 MB)").arg(static_cast<double>(freed) / 1e6, 0, 'f', 1));
}

// ---------------------------------------------------------------------------
// Remoção de silêncios e cortes por trecho de tempo

void ProjectController::analyzeSilences(double thresholdDb, double minPauseMs, double paddingMs)
{
    if (m_analyzingSilences || m_project.duration() <= 0)
        return;
    engine::SilenceOptions opts;
    opts.thresholdDb = std::clamp(thresholdDb, -80.0, -10.0);
    opts.minSilence = static_cast<Micros>(std::max(50.0, minPauseMs) * 1000);
    opts.padding = static_cast<Micros>(std::clamp(paddingMs, 0.0, 2000.0) * 1000);
    if (m_project.hasRange()) {
        opts.rangeStart = m_project.rangeStart();
        opts.rangeEnd = m_project.rangeEnd();
    }
    m_analyzingSilences = true;
    m_silenceProgress = 0;
    m_silences.clear();
    emit silencesChanged();
    auto snap = snapshot();
    const int revision = m_revisionCounter;
    std::thread([this, snap, opts, revision] {
        const auto ranges = engine::detectSilences(*snap, opts, [this](double f) {
            QMetaObject::invokeMethod(this, [this, f] {
                m_silenceProgress = f;
                emit silencesChanged();
            }, Qt::QueuedConnection);
            return true;
        });
        QVariantList list;
        Micros total = 0;
        for (const auto &[a, b] : ranges) {
            list << QVariantMap{{"start", static_cast<double>(a)}, {"end", static_cast<double>(b)}};
            total += b - a;
        }
        QMetaObject::invokeMethod(this, [this, list, total, revision] {
            m_analyzingSilences = false;
            m_silences = list;
            m_silenceRevision = revision;
            emit silencesChanged();
            setStatus(list.isEmpty() ? tr("No pauses found")
                                     : tr("%n pause(s) found (%1)", nullptr, static_cast<int>(list.size()))
                                           .arg(formatTime(static_cast<double>(total))));
        }, Qt::QueuedConnection);
    }).detach();
}

bool ProjectController::hasBeatMarkers() const
{
    return std::any_of(m_project.markers.begin(), m_project.markers.end(), [](const Marker &m) { return m.beat; });
}

void ProjectController::detectBeats(int every)
{
    if (m_detectingBeats)
        return;
    const QString clipId = primaryClipId();
    const auto loc = edit::findClip(m_project, clipId);
    const Clip *c = edit::clipById(m_project, clipId);
    const MediaAsset *a = c && c->hasMedia() ? m_project.asset(c->assetId) : nullptr;
    if (!loc || !a || !a->hasAudio) {
        setStatus(tr("Select a clip with music to detect its beats"));
        return;
    }
    m_detectingBeats = true;
    emit beatsChanged();
    setStatus(tr("Detecting beats…"));
    auto snap = snapshot();
    const int track = loc->track;
    const Micros start = c->start, end = c->end();
    every = std::clamp(every, 1, 8);
    std::thread([this, snap, track, start, end, every, clipId] {
        const engine::BeatResult r = engine::detectBeats(*snap, track, start, end);
        const std::vector<Micros> beats = engine::everyNthBeat(r, every);
        QMetaObject::invokeMethod(this, [this, beats, bpm = r.bpm, start, end, clipId] {
            m_detectingBeats = false;
            emit beatsChanged();
            // O clipe mudou de lugar enquanto analisava: as batidas não valem mais.
            const Clip *now = edit::clipById(m_project, clipId);
            if (!now || now->start != start || now->end() != end) {
                setStatus(tr("The clip changed during the analysis; detect the beats again"));
                return;
            }
            if (beats.empty()) {
                setStatus(tr("No beats found in this clip"));
                return;
            }
            edit(tr("Mark beats"), [&](Project &p) {
                std::erase_if(p.markers, [&](const Marker &m) { return m.beat && m.t >= start && m.t <= end; });
                for (const Micros t : beats)
                    p.markers.push_back({newId("mrk"), p.snapToFrame(t), QString(), QStringLiteral("#22d3ee"), true});
                std::sort(p.markers.begin(), p.markers.end(), [](const Marker &x, const Marker &y) { return x.t < y.t; });
                return true;
            });
            setStatus(tr("%n beat(s) marked · %1 BPM", nullptr, static_cast<int>(beats.size())).arg(std::lround(bpm)));
        }, Qt::QueuedConnection);
    }).detach();
}

void ProjectController::clearBeatMarkers()
{
    edit(tr("Clear beat markers"), [](Project &p) {
        return std::erase_if(p.markers, [](const Marker &m) { return m.beat; }) > 0;
    });
}

void ProjectController::removeSilences()
{
    if (m_silences.isEmpty() || m_analyzingSilences)
        return;
    // A análise vale para o projeto como estava; editado depois disso, os tempos não batem mais.
    if (m_silenceRevision != m_revisionCounter) {
        emit errorOccurred(tr("The project changed after the analysis; analyze again"));
        return;
    }
    const int n = static_cast<int>(m_silences.size());
    if (removeTimeRanges(m_silences, tr("Remove silences")))
        setStatus(tr("%n pause(s) removed", nullptr, n));
    clearSilences();
}

void ProjectController::clearSilences()
{
    if (m_silences.isEmpty())
        return;
    m_silences.clear();
    emit silencesChanged();
}

bool ProjectController::removeTimeRanges(const QVariantList &ranges, const QString &label)
{
    std::vector<std::pair<Micros, Micros>> list;
    for (const auto &v : ranges) {
        const QVariantMap m = v.toMap();
        list.emplace_back(static_cast<Micros>(m.value("start").toDouble()), static_cast<Micros>(m.value("end").toDouble()));
    }
    return edit(label.isEmpty() ? tr("Remove ranges") : label,
                [&](Project &p) { return edit::removeRanges(p, list) > 0; });
}

// ---------------------------------------------------------------------------
// Edição pela transcrição

void ProjectController::generateTranscript(const QString &modelId, const QString &language)
{
    if (m_transcribing)
        return;
    if (!engine::whisperModelInstalled(modelId)) {
        emit errorOccurred(tr("Download the model before transcribing"));
        return;
    }
    pause();
    engine::TranscribeOptions opts;
    opts.modelPath = engine::whisperModelPath(modelId);
    opts.language = language.isEmpty() ? QStringLiteral("auto") : language;
    opts.maxChars = 1; // um segmento por palavra
    m_transcribing = true;
    m_transcribeProgress = 0;
    m_cancelTranscription = false;
    emit transcribeChanged();

    auto snap = snapshot();
    const int revision = m_revisionCounter;
    std::thread([this, snap, opts, revision] {
        std::vector<CaptionSegment> words;
        QString err;
        const bool ok = engine::transcribeProject(
            *snap, opts,
            [this](double f) {
                QMetaObject::invokeMethod(this, [this, f] {
                    m_transcribeProgress = f;
                    emit transcribeChanged();
                }, Qt::QueuedConnection);
                return !m_cancelTranscription.load();
            },
            words, &err);
        QMetaObject::invokeMethod(this, [this, ok, err, words = std::move(words), revision] {
            m_transcribing = false;
            emit transcribeChanged();
            if (!ok) {
                if (!m_cancelTranscription)
                    emit errorOccurred(err);
                return;
            }
            if (words.empty()) {
                emit errorOccurred(tr("No speech recognized"));
                return;
            }
            m_transcript.clear();
            for (const auto &w : words)
                m_transcript << QVariantMap{{"start", static_cast<double>(w.start)},
                                            {"end", static_cast<double>(w.end)},
                                            {"text", w.text}};
            m_transcriptRevision = revision;
            emit transcriptChanged();
            setStatus(tr("%n word(s) transcribed", nullptr, static_cast<int>(words.size())));
        }, Qt::QueuedConnection);
    }).detach();
}

bool ProjectController::removeTranscriptWords(const QVariantList &indices)
{
    if (m_transcript.isEmpty() || transcriptStale())
        return false;
    QList<int> idx;
    for (const auto &v : indices)
        if (v.toInt() >= 0 && v.toInt() < m_transcript.size())
            idx << v.toInt();
    std::sort(idx.begin(), idx.end());
    idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
    if (idx.isEmpty())
        return false;
    // Palavras consecutivas viram um corte só, do início da primeira ao início da seguinte
    // (leva junto a pausa entre elas e evita sobrar meia sílaba).
    std::vector<std::pair<Micros, Micros>> ranges;
    auto wordAt = [&](int i) { return m_transcript.at(i).toMap(); };
    for (int k = 0; k < idx.size();) {
        const int first = idx[k];
        int last = first;
        while (k + 1 < idx.size() && idx[k + 1] == last + 1)
            last = idx[++k];
        ++k;
        const auto a = static_cast<Micros>(wordAt(first).value("start").toDouble());
        const auto b = static_cast<Micros>(last + 1 < m_transcript.size() ? wordAt(last + 1).value("start").toDouble()
                                                                         : wordAt(last).value("end").toDouble());
        ranges.emplace_back(a, std::max(a, b));
    }
    QVariantList rangeList;
    for (const auto &[a, b] : ranges)
        rangeList << QVariantMap{{"start", static_cast<double>(a)}, {"end", static_cast<double>(b)}};
    if (!removeTimeRanges(rangeList, tr("Remove words")))
        return false;

    // Atualiza a transcrição sem transcrever de novo: tira as palavras e recua as seguintes.
    QVariantList updated;
    for (int i = 0; i < m_transcript.size(); ++i) {
        if (idx.contains(i))
            continue;
        QVariantMap w = m_transcript.at(i).toMap();
        Micros shift = 0;
        for (const auto &[a, b] : ranges)
            if (static_cast<Micros>(w.value("start").toDouble()) >= b)
                shift += b - a;
        w["start"] = w.value("start").toDouble() - static_cast<double>(shift);
        w["end"] = w.value("end").toDouble() - static_cast<double>(shift);
        updated << w;
    }
    m_transcript = updated;
    m_transcriptRevision = m_revisionCounter;
    emit transcriptChanged();
    setStatus(tr("%n word(s) removed", nullptr, static_cast<int>(idx.size())));
    return true;
}

QVariantList ProjectController::fillerWordIndices() const
{
    // Hesitações comuns em inglês, português e espanhol (sem pontuação, minúsculas).
    static const QSet<QString> fillers{
        QStringLiteral("um"),  QStringLiteral("uh"),  QStringLiteral("uhm"), QStringLiteral("umm"), QStringLiteral("uhh"),
        QStringLiteral("er"),  QStringLiteral("erm"), QStringLiteral("ah"),  QStringLiteral("ahh"), QStringLiteral("eh"),
        QStringLiteral("hmm"), QStringLiteral("hm"),  QStringLiteral("mm"),  QStringLiteral("mhm"), QStringLiteral("é"),
        QStringLiteral("éé"),  QStringLiteral("ééé"), QStringLiteral("ã"),   QStringLiteral("ãh"),  QStringLiteral("ahn"),
        QStringLiteral("hum"), QStringLiteral("eee"), QStringLiteral("ehh")};
    QVariantList out;
    for (int i = 0; i < m_transcript.size(); ++i) {
        QString w = m_transcript.at(i).toMap().value("text").toString().toLower();
        w.remove(QRegularExpression(QStringLiteral("[^\\p{L}]")));
        if (fillers.contains(w))
            out << i;
    }
    return out;
}

void ProjectController::clearTranscript()
{
    m_transcript.clear();
    emit transcriptChanged();
}

// ---------------------------------------------------------------------------
// Correção de cor avançada (curvas, HSL, rodas)

void ProjectController::setCurve(const QString &clipId, int channel, const QVariantList &points)
{
    if (channel < 0 || channel > 3)
        return;
    std::vector<CurvePoint> pts;
    for (const auto &v : points) {
        const QVariantMap m = v.toMap();
        pts.push_back({std::clamp(m.value("x").toDouble(), 0.0, 1.0), std::clamp(m.value("y").toDouble(), 0.0, 1.0)});
    }
    std::sort(pts.begin(), pts.end(), [](const CurvePoint &a, const CurvePoint &b) { return a.x < b.x; });
    // Só os cantos na identidade = curva vazia (neutra).
    if (pts.size() <= 2 && std::all_of(pts.begin(), pts.end(), [](const CurvePoint &p) { return p.x == p.y && (p.x == 0 || p.x == 1); }))
        pts.clear();
    edit(tr("Curves"), [&](Project &p) {
        Clip *c = edit::clipById(p, clipId);
        if (!c || c->color.curves[channel] == pts)
            return false;
        c->color.curves[channel] = pts;
        return true;
    }, QStringLiteral("curve:%1:%2").arg(clipId).arg(channel));
}

void ProjectController::setHsl(const QString &clipId, int band, double hue, double saturation, double luminance)
{
    if (band < 0 || band >= kHslBands)
        return;
    const HslAdjust value{std::clamp(hue, -1.0, 1.0), std::clamp(saturation, -1.0, 1.0), std::clamp(luminance, -1.0, 1.0)};
    edit(tr("HSL"), [&](Project &p) {
        Clip *c = edit::clipById(p, clipId);
        if (!c || c->color.hsl[band] == value)
            return false;
        c->color.hsl[band] = value;
        return true;
    }, QStringLiteral("hsl:%1:%2").arg(clipId).arg(band));
}

void ProjectController::setColorWheel(const QString &clipId, const QString &which, double x, double y, double luma)
{
    // Ponto fora do disco volta para a borda.
    const double r = std::hypot(x, y);
    if (r > 1) {
        x /= r;
        y /= r;
    }
    const ColorWheel value{x, y, std::clamp(luma, -1.0, 1.0)};
    edit(tr("Color wheels"), [&](Project &p) {
        Clip *c = edit::clipById(p, clipId);
        if (!c)
            return false;
        ColorWheel *w = which == QLatin1String("lift") ? &c->color.lift
                        : which == QLatin1String("gamma") ? &c->color.gamma
                        : which == QLatin1String("gain") ? &c->color.gain : nullptr;
        if (!w || *w == value)
            return false;
        *w = value;
        return true;
    }, QStringLiteral("wheel:%1:%2").arg(clipId, which));
}

void ProjectController::resetGrade(const QString &clipId)
{
    edit(tr("Reset color grading"), [&](Project &p) {
        Clip *c = edit::clipById(p, clipId);
        if (!c || !c->color.hasGrade())
            return false;
        for (auto &curve : c->color.curves)
            curve.clear();
        c->color.hsl = {};
        c->color.lift = c->color.gamma = c->color.gain = {};
        return true;
    });
}

// ---------------------------------------------------------------------------
// Escopos

void ProjectController::setScopesEnabled(bool on)
{
    if (on == m_scopesEnabled)
        return;
    m_scopesEnabled = on;
    emit scopesChanged();
}

void ProjectController::setScopeFrame(const QImage &img)
{
    {
        std::lock_guard lock(m_scopeMutex);
        m_scopeFrame = img;
    }
    QMetaObject::invokeMethod(this, &ProjectController::scopeFrameChanged, Qt::QueuedConnection);
}

QImage ProjectController::scopeFrame() const
{
    std::lock_guard lock(m_scopeMutex);
    return m_scopeFrame;
}

// ---------------------------------------------------------------------------
// Atalhos

QString ProjectController::keySequence(int key, int modifiers) const
{
    const auto mods = Qt::KeyboardModifiers(modifiers) & ~Qt::KeypadModifier;
    return QKeySequence(QKeyCombination(mods, Qt::Key(key))).toString(QKeySequence::PortableText);
}

QString ProjectController::shortcutText(const QString &sequence) const
{
    return QKeySequence(sequence, QKeySequence::PortableText).toString(QKeySequence::NativeText);
}

bool ProjectController::sameShortcut(const QString &a, const QString &b) const
{
    return QKeySequence(a, QKeySequence::PortableText) == QKeySequence(b, QKeySequence::PortableText);
}

// ---------------------------------------------------------------------------
// Idioma da interface

QString ProjectController::language() const
{
    return app::savedLanguage();
}

void ProjectController::setLanguage(const QString &code)
{
    if (code == app::savedLanguage())
        return;
    app::saveLanguage(code);
    app::installLanguage(code);
    if (m_engine)
        m_engine->retranslate();
    emit languageChanged();
    // Textos montados em C++ (nomes de filtros, modelos, rótulos de clipes).
    emit projectChanged();
    emit assetsChanged();
    emit selectedClipChanged();
    emit captionModelsChanged();
}

QString ProjectController::activeLanguage() const
{
    return app::activeLanguage();
}

QVariantList ProjectController::languages() const
{
    return app::availableLanguages();
}

// ---------------------------------------------------------------------------
// Narração (gravação do microfone)

bool ProjectController::recording() const
{
    return m_recorder && m_recorder->recording();
}

double ProjectController::recordElapsed() const
{
    return m_recorder ? static_cast<double>(m_recorder->frames()) * kSecond / media::kSampleRate : 0.0;
}

QStringList ProjectController::inputDevices() const
{
    return AudioRecorder::devices();
}

bool ProjectController::startRecording(const QString &device)
{
    if (recording())
        return false;
    // Ao lado do projeto salvo; sem projeto salvo, na pasta de dados do app.
    const QString dir = m_filePath.isEmpty()
                            ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("recordings"))
                            : QDir(QFileInfo(m_filePath).absolutePath()).filePath(QFileInfo(m_filePath).completeBaseName() + QStringLiteral(" recordings"));
    m_recordPath = QDir(dir).filePath(QStringLiteral("voiceover-%1.wav").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    if (!m_recorder)
        m_recorder = std::make_unique<AudioRecorder>();
    QString err;
    if (!m_recorder->start(m_recordPath, device, &err)) {
        emit errorOccurred(err);
        return false;
    }
    m_recordStart = m_project.snapToFrame(m_playhead);
    m_recordLevel = 0;
    m_recordTimer.start();
    emit recordingChanged();
    play(); // narra por cima do vídeo
    setStatus(tr("Recording voiceover…"));
    return true;
}

void ProjectController::stopRecording()
{
    if (!recording())
        return;
    m_recordTimer.stop();
    const std::int64_t frames = m_recorder->stop();
    pause();
    m_recordLevel = 0;
    emit recordingChanged();
    emit recordLevelChanged();
    if (frames < media::kSampleRate / 10) { // menos de 0,1 s: descarta
        QFile::remove(m_recordPath);
        return;
    }
    importMedia({QUrl::fromLocalFile(m_recordPath)});
    for (const auto &[id, a] : m_project.assets)
        if (a.path == m_recordPath) {
            addAssetToTimeline(id, -1, static_cast<double>(m_recordStart));
            break;
        }
    setStatus(tr("Voiceover recorded (%1)").arg(formatTime(static_cast<double>(frames) * kSecond / media::kSampleRate)));
}

// ---------------------------------------------------------------------------
// Efeitos de voz (em segundo plano)

QVariantList ProjectController::videoEffects() const
{
    QVariantList out;
    for (const auto &e : clv::videoEffects())
        out << QVariantMap{{"id", e.id}, {"name", QCoreApplication::translate("Effects", e.name.toUtf8())}};
    return out;
}

QVariantList ProjectController::stickers() const
{
    QVariantList out;
    QDirIterator it(QStringLiteral(":/qt/qml/Clivcut/stickers"), {QStringLiteral("*.svg")}, QDir::Files);
    QStringList names;
    while (it.hasNext())
        names << QFileInfo(it.next()).completeBaseName();
    names.sort();
    for (const auto &n : names)
        out << QVariantMap{{"id", n}, {"source", QStringLiteral("qrc:/qt/qml/Clivcut/stickers/%1.svg").arg(n)}};
    return out;
}

void ProjectController::addSticker(const QString &id)
{
    // O SVG vira um PNG com transparência (uma vez) e entra como imagem comum.
    const QString svg = QStringLiteral(":/qt/qml/Clivcut/stickers/%1.svg").arg(id);
    if (!QFileInfo::exists(svg))
        return;
    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("stickers"));
    const QString png = QDir(dir).filePath(id + QStringLiteral(".png"));
    if (!QFileInfo::exists(png)) {
        QDir().mkpath(dir);
        QSvgRenderer renderer(svg);
        QImage img(512, 512, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter painter(&img);
        renderer.render(&painter, QRectF(16, 16, 480, 480));
        painter.end();
        if (!img.save(png)) {
            emit errorOccurred(tr("Could not write %1").arg(png));
            return;
        }
    }
    importMedia({QUrl::fromLocalFile(png)});
    QString assetId;
    for (const auto &[aid, a] : m_project.assets)
        if (a.path == png)
            assetId = aid;
    if (assetId.isEmpty())
        return;
    QString clipId;
    edit(tr("Add sticker"), [&](Project &p) {
        const MediaAsset *a = p.asset(assetId);
        if (!a)
            return false;
        Clip c = createMediaClip(*a, m_project.snapToFrame(m_playhead));
        c.duration = 3 * kSecond;
        c.transform.scale = 0.35;
        // Faixa de cima (nunca a principal), para ficar sobre o vídeo.
        const int track = edit::ensureTrackFor(p, TrackKind::Video, c.start, c.duration, 1);
        clipId = edit::insertClip(p, std::move(c), std::max(1, track));
        return !clipId.isEmpty();
    });
    if (!clipId.isEmpty()) {
        m_selection = {clipId};
        emit selectionChanged();
        emit selectedClipChanged();
    }
}

void ProjectController::addEffectClip(const QString &effectId)
{
    if (effectShaderIndex(effectId) == 0)
        return;
    addFilterClip({});
    if (m_selection.size() == 1) {
        const QString id = m_selection.first();
        edit(tr("Add effect"), [&](Project &p) {
            Clip *c = edit::clipById(p, id);
            if (!c)
                return false;
            c->effect = {effectId, 0.6};
            return true;
        }, QStringLiteral("addEffect:") + id);
        emit selectedClipChanged();
    }
}

QVariantList ProjectController::voiceEffects() const
{
    QVariantList out;
    for (const auto &e : engine::voiceEffects())
        out << QVariantMap{{"id", e.id}, {"name", QCoreApplication::translate("VoiceEffects", e.name.toUtf8())}};
    return out;
}

QString ProjectController::audioSourceFor(const Clip &c, const MediaAsset &a) const
{
    if (c.voiceIsolation > 0) {
        const QString voice = engine::registeredVoice(a.path, c.voiceIsolation);
        if (!voice.isEmpty())
            return voice;
    }
    if (c.denoise > 0) {
        const QString clean = engine::registeredDenoised(a.path, c.denoise);
        if (!clean.isEmpty())
            return clean;
    }
    return a.path;
}

void ProjectController::refreshEffects()
{
    bool changed = false;
    m_project.forEachClip([&](const Clip &c) {
        const MediaAsset *a = c.hasMedia() && !c.voiceEffect.isEmpty() ? m_project.asset(c.assetId) : nullptr;
        if (!a || !a->hasAudio || !QFileInfo::exists(a->path))
            return;
        const QString source = audioSourceFor(c, *a);
        if (!engine::registeredEffect(source, c.voiceEffect).isEmpty() || engine::discoverEffect(source, c.voiceEffect))
            return;
        const QString job = source + QLatin1Char('|') + c.voiceEffect;
        if (job != m_effectBuilding && !m_effectQueue.contains(job)) {
            m_effectQueue << job;
            changed = true;
        }
    });
    if (changed) {
        emit selectedClipChanged();
        startNextEffect();
    }
}

void ProjectController::startNextEffect()
{
    if (!m_effectBuilding.isEmpty() || m_effectQueue.isEmpty())
        return;
    m_effectBuilding = m_effectQueue.takeFirst();
    m_effectProgress = 0;
    m_effectCancel = std::make_shared<std::atomic<bool>>(false);
    const QString source = m_effectBuilding.section(QLatin1Char('|'), 0, -2);
    const QString effect = m_effectBuilding.section(QLatin1Char('|'), -1);
    auto cancel = m_effectCancel;
    emit selectedClipChanged();
    std::thread([this, source, effect, cancel] {
        const QString out = engine::effectPathFor(source, effect);
        QString err;
        double lastSent = 0;
        const bool ok = engine::generateEffect(
            source, effect, out,
            [&](double f) {
                if (f - lastSent >= 0.02 && !*cancel) {
                    lastSent = f;
                    QMetaObject::invokeMethod(this, [this, f] {
                        m_effectProgress = f;
                        emit selectedClipChanged();
                    }, Qt::QueuedConnection);
                }
                return !*cancel;
            },
            &err);
        if (*cancel)
            return;
        QMetaObject::invokeMethod(this, [this, ok, err, source, effect, out] {
            m_effectBuilding.clear();
            if (ok) {
                engine::registerEffect(source, effect, out);
                if (m_playing)
                    m_audio->updateProject(snapshot());
            } else {
                emit errorOccurred(tr("Voice effect failed: %1").arg(err));
            }
            emit selectedClipChanged();
            startNextEffect();
        }, Qt::QueuedConnection);
    }).detach();
}

// ---------------------------------------------------------------------------
// Isolamento de voz (IA, em segundo plano)

void ProjectController::separateStems(int mode)
{
    const QString clipId = primaryClipId();
    const Clip *c = edit::clipById(m_project, clipId);
    const MediaAsset *a = c && c->hasMedia() ? m_project.asset(c->assetId) : nullptr;
    if (!a || !a->hasAudio || !QFileInfo::exists(a->path) || !engine::stemSeparationAvailable())
        return;
    mode = std::clamp(mode, 0, 2);
    if (engine::stemsReady(a->path)) {
        placeStems(clipId, mode);
        return;
    }
    if (!engine::aiModelInstalled(engine::kStemsModel)) {
        setStatus(tr("Download the music separation model first"));
        return;
    }
    if (!m_stemsBuilding.isEmpty()) {
        if (m_stemsBuilding == a->path)
            m_stemRequests.push_back({clipId, mode});
        else
            setStatus(tr("Wait for the current music separation to finish"));
        return;
    }
    m_stemsBuilding = a->path;
    m_stemsProgress = 0;
    m_stemRequests = {{clipId, mode}};
    m_stemsCancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = m_stemsCancel;
    const QString path = a->path;
    const QString model = engine::aiModelPath(engine::kStemsModel);
    emit selectedClipChanged();
    setStatus(tr("Separating the music in %1…").arg(QFileInfo(path).fileName()));
    std::thread([this, path, model, cancel] {
        QString err;
        double lastSent = 0;
        const bool ok = engine::separateStems(
            path, model,
            [&](double f) {
                if (f - lastSent >= 0.01 && !*cancel) {
                    lastSent = f;
                    QMetaObject::invokeMethod(this, [this, f] {
                        m_stemsProgress = f;
                        emit selectedClipChanged();
                    }, Qt::QueuedConnection);
                }
                return !*cancel;
            },
            &err);
        if (*cancel)
            return;
        QMetaObject::invokeMethod(this, [this, ok, err] {
            m_stemsBuilding.clear();
            const auto requests = std::move(m_stemRequests);
            m_stemRequests.clear();
            if (ok) {
                for (const auto &r : requests)
                    placeStems(r.clipId, r.mode);
            } else {
                emit errorOccurred(tr("Music separation failed: %1").arg(err));
            }
            emit selectedClipChanged();
        }, Qt::QueuedConnection);
    }).detach();
}

void ProjectController::placeStems(const QString &clipId, int mode)
{
    const Clip *orig = edit::clipById(m_project, clipId);
    const MediaAsset *src = orig && orig->hasMedia() ? m_project.asset(orig->assetId) : nullptr;
    if (!src || !engine::stemsReady(src->path)) {
        setStatus(tr("The clip was removed before the separation finished"));
        return;
    }
    using engine::Stem;
    const std::vector<Stem> stems = mode == 1 ? std::vector<Stem>{Stem::Vocals}
                                    : mode == 2 ? std::vector<Stem>{Stem::Instrumental}
                                                : std::vector<Stem>{Stem::Vocals, Stem::Drums, Stem::Bass, Stem::Other};
    const QString srcName = src->name;
    QStringList created;
    edit(tr("Separate music"), [&](Project &p) {
        Clip *c = edit::clipById(p, clipId);
        if (!c)
            return false;
        const Clip base = *c;
        for (const Stem s : stems) {
            const QString path = engine::stemPathFor(src->path, s);
            // A mesma faixa já importada é reaproveitada.
            QString assetId;
            for (const auto &[id, a] : p.assets)
                if (a.path == path)
                    assetId = id;
            if (assetId.isEmpty()) {
                MediaAsset a;
                if (!media::probe(path, a))
                    continue;
                a.id = newId("ast");
                a.name = QStringLiteral("%1 · %2").arg(srcName, engine::stemName(s));
                assetId = a.id;
                p.assets.emplace(a.id, a);
            }
            // Mesmo trecho, tempo e volume do original; processamentos de voz não se repetem.
            Clip sc = base;
            sc.id = newId("clp");
            sc.groupId.clear();
            sc.assetId = assetId;
            sc.audioOnly = false;
            sc.audioMuted = false;
            sc.denoise = 0;
            sc.voiceIsolation = 0;
            sc.voiceEffect.clear();
            sc.removeBackground = false;
            sc.stabilize = 0;
            sc.transitionIn.reset();
            Track t = createTrack(TrackKind::Audio, engine::stemName(s));
            t.clips.push_back(sc);
            p.tracks.push_back(std::move(t));
            created << sc.id;
        }
        if (created.isEmpty())
            return false;
        c = edit::clipById(p, clipId);
        c->audioMuted = true; // o original continua lá, sem som (para comparar ou voltar)
        return true;
    });
    if (created.isEmpty())
        return;
    refreshProxies();
    emit assetsChanged();
    m_selection = created;
    m_primary = created.first();
    emit selectionChanged();
    emit selectedClipChanged();
    setStatus(tr("Music separated into %n track(s)", nullptr, static_cast<int>(created.size())));
}

void ProjectController::refreshVoice()
{
    if (!engine::voiceIsolationAvailable() || !engine::aiModelInstalled(engine::kVoiceModel))
        return;
    bool changed = false;
    m_project.forEachClip([&](const Clip &c) {
        const MediaAsset *a = c.hasMedia() && c.voiceIsolation > 0 ? m_project.asset(c.assetId) : nullptr;
        if (!a || !a->hasAudio || !QFileInfo::exists(a->path))
            return;
        if (!engine::registeredVoice(a->path, c.voiceIsolation).isEmpty() || engine::discoverVoice(a->path, c.voiceIsolation))
            return;
        const QString job = a->path + QLatin1Char('|') + QString::number(engine::quantizedVoice(c.voiceIsolation));
        if (job != m_voiceBuilding && !m_voiceQueue.contains(job)) {
            m_voiceQueue << job;
            changed = true;
        }
    });
    if (changed) {
        emit selectedClipChanged();
        startNextVoice();
    }
}

void ProjectController::startNextVoice()
{
    if (!m_voiceBuilding.isEmpty() || m_voiceQueue.isEmpty())
        return;
    m_voiceBuilding = m_voiceQueue.takeFirst();
    m_voiceProgress = 0;
    m_voiceCancel = std::make_shared<std::atomic<bool>>(false);
    const QString path = m_voiceBuilding.section(QLatin1Char('|'), 0, -2);
    const double strength = m_voiceBuilding.section(QLatin1Char('|'), -1).toDouble();
    const QString model = engine::aiModelPath(engine::kVoiceModel);
    auto cancel = m_voiceCancel;
    emit selectedClipChanged();
    setStatus(tr("Isolating voice in %1…").arg(QFileInfo(path).fileName()));
    std::thread([this, path, strength, model, cancel] {
        const QString out = engine::voicePathFor(path, strength);
        QString err;
        double lastSent = 0;
        const bool ok = engine::generateVoiceIsolated(
            path, model, out, strength,
            [&](double f) {
                if (f - lastSent >= 0.01 && !*cancel) {
                    lastSent = f;
                    QMetaObject::invokeMethod(this, [this, f] {
                        m_voiceProgress = f;
                        emit selectedClipChanged();
                    }, Qt::QueuedConnection);
                }
                return !*cancel;
            },
            &err);
        if (*cancel)
            return;
        QMetaObject::invokeMethod(this, [this, ok, err, path, strength, out] {
            m_voiceBuilding.clear();
            if (ok) {
                engine::registerVoice(path, strength, out);
                refreshEffects();
                setStatus(tr("Voice isolated: %1").arg(QFileInfo(path).fileName()));
                if (m_playing)
                    m_audio->updateProject(snapshot());
            } else {
                emit errorOccurred(tr("Voice isolation failed: %1").arg(err));
            }
            emit selectedClipChanged();
            startNextVoice();
        }, Qt::QueuedConnection);
    }).detach();
}

// ---------------------------------------------------------------------------
// Remoção de fundo (matte por IA, em segundo plano)

void ProjectController::refreshMattes()
{
    if (!engine::mattingAvailable() || !engine::aiModelInstalled(engine::kMattingModel))
        return;
    bool changed = false;
    m_project.forEachClip([&](const Clip &c) {
        const MediaAsset *a = c.hasMedia() && c.removeBackground ? m_project.asset(c.assetId) : nullptr;
        if (!a || a->kind != MediaKind::Video || !QFileInfo::exists(a->path))
            return;
        if (!engine::registeredMatte(a->path).isEmpty() || engine::discoverMatte(a->path))
            return;
        if (a->path != m_matteBuilding && !m_matteQueue.contains(a->path)) {
            m_matteQueue << a->path;
            changed = true;
        }
    });
    if (changed) {
        emit selectedClipChanged();
        startNextMatte();
    }
}

void ProjectController::startNextMatte()
{
    if (!m_matteBuilding.isEmpty() || m_matteQueue.isEmpty())
        return;
    m_matteBuilding = m_matteQueue.takeFirst();
    m_matteProgress = 0;
    m_matteCancel = std::make_shared<std::atomic<bool>>(false);
    const QString path = m_matteBuilding;
    const QString model = engine::aiModelPath(engine::kMattingModel);
    auto cancel = m_matteCancel;
    emit selectedClipChanged();
    setStatus(tr("Removing background from %1…").arg(QFileInfo(path).fileName()));
    std::thread([this, path, model, cancel] {
        const QString out = engine::mattePathFor(path);
        QString err;
        double lastSent = 0;
        const bool ok = engine::generateMatte(
            path, model, out,
            [&](double f) {
                if (f - lastSent >= 0.01 && !*cancel) {
                    lastSent = f;
                    QMetaObject::invokeMethod(this, [this, f] {
                        m_matteProgress = f;
                        emit selectedClipChanged();
                    }, Qt::QueuedConnection);
                }
                return !*cancel;
            },
            &err);
        if (*cancel)
            return;
        QMetaObject::invokeMethod(this, [this, ok, err, path, out] {
            m_matteBuilding.clear();
            if (ok) {
                engine::registerMatte(path, out);
                setStatus(tr("Background removed: %1").arg(QFileInfo(path).fileName()));
                ++m_revisionCounter;
                emit revisionChanged();
            } else {
                emit errorOccurred(tr("Background removal failed: %1").arg(err));
            }
            emit selectedClipChanged();
            startNextMatte();
        }, Qt::QueuedConnection);
    }).detach();
}

void ProjectController::refreshStabilize()
{
    bool changed = false;
    m_project.forEachClip([&](const Clip &c) {
        const MediaAsset *a = c.hasMedia() && c.stabilize > 0 ? m_project.asset(c.assetId) : nullptr;
        if (!a || a->kind != MediaKind::Video || !QFileInfo::exists(a->path))
            return;
        if (registeredMotion(a->path) || engine::discoverMotion(a->path))
            return;
        if (a->path != m_stabBuilding && !m_stabQueue.contains(a->path)) {
            m_stabQueue << a->path;
            changed = true;
        }
    });
    if (changed) {
        emit selectedClipChanged();
        startNextStabilize();
    }
}

void ProjectController::startNextStabilize()
{
    if (!m_stabBuilding.isEmpty() || m_stabQueue.isEmpty())
        return;
    m_stabBuilding = m_stabQueue.takeFirst();
    m_stabProgress = 0;
    m_stabCancel = std::make_shared<std::atomic<bool>>(false);
    const QString path = m_stabBuilding;
    auto cancel = m_stabCancel;
    emit selectedClipChanged();
    setStatus(tr("Analyzing camera motion in %1…").arg(QFileInfo(path).fileName()));
    std::thread([this, path, cancel] {
        QString err;
        double lastSent = 0;
        const bool ok = engine::analyzeMotion(
            path, engine::motionPathFor(path),
            [&](double f) {
                if (f - lastSent >= 0.01 && !*cancel) {
                    lastSent = f;
                    QMetaObject::invokeMethod(this, [this, f] {
                        m_stabProgress = f;
                        emit selectedClipChanged();
                    }, Qt::QueuedConnection);
                }
                return !*cancel;
            },
            &err);
        if (*cancel)
            return;
        QMetaObject::invokeMethod(this, [this, ok, err, path] {
            m_stabBuilding.clear();
            if (ok) {
                setStatus(tr("Stabilization ready: %1").arg(QFileInfo(path).fileName()));
                commitChange(); // o quadro e as chaves do cache de render mudam
            } else {
                emit errorOccurred(tr("Stabilization failed: %1").arg(err));
            }
            emit selectedClipChanged();
            startNextStabilize();
        }, Qt::QueuedConnection);
    }).detach();
}

// ---------------------------------------------------------------------------
// Projetos recentes

QVariantList ProjectController::recentProjects() const
{
    QVariantList out;
    const QStringList paths = QSettings().value(QStringLiteral("recent/projects")).toStringList();
    for (const QString &p : paths) {
        const QFileInfo fi(p);
        if (!fi.exists())
            continue;
        out << QVariantMap{{"path", p},
                           {"name", fi.completeBaseName()},
                           {"folder", fi.absolutePath()},
                           {"modified", fi.lastModified().toString(QStringLiteral("dd/MM/yyyy HH:mm"))}};
    }
    return out;
}

void ProjectController::rememberRecent(const QString &path)
{
    if (path.isEmpty() || !path.endsWith(QLatin1String(".clivcut")))
        return;
    QSettings s;
    QStringList paths = s.value(QStringLiteral("recent/projects")).toStringList();
    paths.removeAll(path);
    paths.prepend(path);
    while (paths.size() > 10)
        paths.removeLast();
    s.setValue(QStringLiteral("recent/projects"), paths);
    emit recentProjectsChanged();
}
