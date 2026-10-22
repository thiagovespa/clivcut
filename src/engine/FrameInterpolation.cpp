// SPDX-License-Identifier: GPL-3.0-or-later
#include "FrameInterpolation.h"

#include "Proxy.h"
#include "core/Evaluate.h"
#include "media/Encoder.h"
#include "media/MediaProbe.h"
#include "media/VideoDecoder.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#ifdef CLIVCUT_HAVE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace clv::engine {

namespace {

struct Entry {
    Micros start = 0, end = 0; ///< tempos da mídia cobertos pelo arquivo
    int factor = 1;
    QString path;
};

QMutex g_mutex;
QHash<QString, QList<Entry>> g_entries; ///< por mídia

/// A rede trabalha em blocos de 128 px (cinco níveis de fluxo, o mais grosso a 1/16).
constexpr int kAlign = 128;

QString cacheDir()
{
    return QDir(QFileInfo(proxyCacheDir()).absolutePath()).filePath(QStringLiteral("interpolated"));
}

/** Prefixo dos arquivos de uma mídia: nome + hash do caminho, tamanho e data. */
QString filePrefix(const QString &mediaPath)
{
    const QFileInfo fi(mediaPath);
    const QByteArray k = fi.absoluteFilePath().toUtf8() + '|' + QByteArray::number(fi.size()) + '|' +
                         QByteArray::number(fi.lastModified().toMSecsSinceEpoch()) + "|rife426";
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(k, QCryptographicHash::Sha1).toHex().left(16));
    return QStringLiteral("%1_%2").arg(fi.completeBaseName().left(40), hash);
}

double mediaFps(const MediaAsset *a)
{
    return a && a->fps > 1 ? a->fps : 30.0;
}

/** Índice do quadro da mídia em vigor no tempo `t` (o último que começa até t). */
std::int64_t frameIndex(Micros t, double fps)
{
    return static_cast<std::int64_t>(std::floor(static_cast<double>(t) * fps / kSecond + 1e-6));
}

Micros frameTime(std::int64_t i, double fps)
{
    return static_cast<Micros>(std::llround(static_cast<double>(i) * kSecond / fps));
}

} // namespace

bool interpolationAvailable()
{
#ifdef CLIVCUT_HAVE_ONNX
    return true;
#else
    return false;
#endif
}

InterpolationSpan interpolationSpanFor(const Project &p, const Clip &c)
{
    InterpolationSpan s;
    const MediaAsset *a = c.hasMedia() ? p.asset(c.assetId) : nullptr;
    if (!a || a->kind != MediaKind::Video || c.duration <= 0)
        return s;
    // Velocidade mínima (curvas de velocidade variam ao longo do clipe) e o trecho da mídia usado.
    constexpr int kSamples = 200;
    double minRate = 1e9;
    Micros lo = std::numeric_limits<Micros>::max(), hi = std::numeric_limits<Micros>::min();
    Micros prev = sourceTimeAt(p, c, 0);
    for (int i = 0; i <= kSamples; ++i) {
        const Micros rel = c.duration * i / kSamples;
        const Micros src = sourceTimeAt(p, c, rel);
        lo = std::min(lo, src);
        hi = std::max(hi, src);
        if (i > 0) {
            const double rate = std::abs(static_cast<double>(src - prev)) / (static_cast<double>(c.duration) / kSamples);
            if (rate > 1e-3) // quadro congelado não conta
                minRate = std::min(minRate, rate);
        }
        prev = src;
    }
    if (minRate >= 1e9)
        return s;
    const double fps = mediaFps(a);
    // Quantos quadros novos por quadro da mídia para cada quadro do projeto ter o seu.
    const double need = p.fps / (fps * minRate);
    s.factor = std::clamp(static_cast<int>(std::ceil(need - 0.05)), 1, kInterpolationMaxFactor);
    const Micros frame = frameTime(1, fps);
    s.in = std::max<Micros>(0, lo - frame);
    s.out = a->duration > 0 ? std::min(a->duration, hi + frame) : hi + frame;
    return s;
}

QString interpolationPathFor(const QString &mediaPath, const InterpolationSpan &span)
{
    return QDir(cacheDir()).filePath(QStringLiteral("%1_x%2_%3_%4.mp4")
                                         .arg(filePrefix(mediaPath))
                                         .arg(span.factor)
                                         .arg(span.in)
                                         .arg(span.out));
}

void registerInterpolation(const QString &mediaPath, const InterpolationSpan &span, Micros fileStart, const QString &path)
{
    QMutexLocker lock(&g_mutex);
    auto &list = g_entries[mediaPath];
    for (const Entry &e : list)
        if (e.path == path)
            return;
    list.append({fileStart, span.out, span.factor, path});
}

QString findInterpolation(const QString &mediaPath, const InterpolationSpan &span, Micros *fileStart, int *factor)
{
    QMutexLocker lock(&g_mutex);
    const auto it = g_entries.constFind(mediaPath);
    if (it == g_entries.constEnd())
        return {};
    const Entry *best = nullptr;
    for (const Entry &e : *it)
        if (e.start <= span.in && e.end >= span.out && e.factor >= span.factor && (!best || e.factor > best->factor))
            best = &e;
    if (!best)
        return {};
    if (fileStart)
        *fileStart = best->start;
    if (factor)
        *factor = best->factor;
    return best->path;
}

void discoverInterpolations(const QString &mediaPath)
{
    // Nome: <prefixo>_x<fator>_<início pedido>_<fim>.mp4; o arquivo começa no quadro em vigor no início.
    const QString prefix = filePrefix(mediaPath);
    static const QRegularExpression re(QStringLiteral("_x(\\d+)_(\\d+)_(\\d+)\\.mp4$"));
    MediaAsset info;
    bool probed = false;
    for (const QFileInfo &fi : QDir(cacheDir()).entryInfoList({prefix + QStringLiteral("_x*.mp4")}, QDir::Files)) {
        const auto m = re.match(fi.fileName());
        if (!m.hasMatch() || fi.size() <= 0)
            continue;
        if (!probed) {
            probed = true;
            if (!media::probe(mediaPath, info))
                return;
        }
        InterpolationSpan s;
        s.factor = m.captured(1).toInt();
        s.in = m.captured(2).toLongLong();
        s.out = m.captured(3).toLongLong();
        registerInterpolation(mediaPath, s, frameTime(frameIndex(s.in, mediaFps(&info)), mediaFps(&info)),
                              fi.absoluteFilePath());
    }
}

bool generateInterpolation(const QString &mediaPath, const InterpolationSpan &span, const QString &modelPath,
                           const QString &outPath, Micros *fileStart, const std::function<bool(double)> &progress,
                           QString *error)
{
    auto fail = [&](const QString &m) {
        if (error)
            *error = m;
        return false;
    };
#ifndef CLIVCUT_HAVE_ONNX
    (void)mediaPath;
    (void)span;
    (void)modelPath;
    (void)outPath;
    (void)fileStart;
    (void)progress;
    return fail(QCoreApplication::translate("Engine", "This build was compiled without ONNX Runtime"));
#else
    if (!QFileInfo::exists(modelPath))
        return fail(QCoreApplication::translate("Engine", "Interpolation model not found: %1").arg(modelPath));
    if (span.factor < 2 || span.out <= span.in)
        return fail(QCoreApplication::translate("Engine", "Nothing to interpolate"));
    MediaAsset info;
    if (!media::probe(mediaPath, info, error))
        return false;
    if (info.kind != MediaKind::Video)
        return fail(QCoreApplication::translate("Engine", "Frame interpolation only applies to videos"));
    media::VideoDecoder dec;
    if (!dec.open(mediaPath, QSize(kInterpolationMaxSide, kInterpolationMaxSide), error, true))
        return false;
    const QSize size = dec.outputSize();
    const int W = size.width(), H = size.height();
    const int PW = (W + kAlign - 1) / kAlign * kAlign, PH = (H + kAlign - 1) / kAlign * kAlign;
    const double fps = mediaFps(&info);
    const int k = span.factor;
    const std::int64_t first = frameIndex(span.in, fps);
    const std::int64_t last = std::max(first + 1, frameIndex(span.out, fps));
    if (fileStart)
        *fileStart = frameTime(first, fps);

    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "clivcut");
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(std::max(1, QThread::idealThreadCount()));
        opts.AddConfigEntry("session.intra_op.allow_spinning", "0");
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef _WIN32
        const std::wstring model = modelPath.toStdWString();
#else
        const std::string model = modelPath.toStdString();
#endif
        Ort::Session session(env, model.c_str(), opts);
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const std::array<const char *, 3> inNames{"img0", "img1", "timestep"};
        const std::array<const char *, 1> outNames{"frame"};
        const std::array<int64_t, 4> imgShape{1, 3, PH, PW};
        const std::array<int64_t, 4> tShape{1, 1, 1, 1};

        QDir().mkpath(QFileInfo(outPath).absolutePath());
        const QString tmp = outPath + QStringLiteral(".part.mp4");
        media::EncodeSettings es;
        es.width = W;
        es.height = H;
        es.fps = fps * k;
        es.crf = 16;
        es.preset = QStringLiteral("veryfast");
        es.gopFrames = std::max(1, static_cast<int>(es.fps / 2));
        es.audio = false;
        media::Encoder enc;
        QString err;
        if (!enc.open(tmp, es, &err))
            return fail(err);

        // Quadro → tensor RGB planar 0..1, com borda zerada até o múltiplo de 128.
        const auto toTensor = [&](const QImage &img, std::vector<float> &t) {
            t.assign(static_cast<size_t>(3) * PW * PH, 0.f);
            const QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
            for (int y = 0; y < H; ++y) {
                const uchar *p = rgba.constScanLine(y);
                for (int x = 0; x < W; ++x) {
                    const size_t o = static_cast<size_t>(y) * PW + x;
                    t[o] = p[x * 4] / 255.f;
                    t[static_cast<size_t>(PW) * PH + o] = p[x * 4 + 1] / 255.f;
                    t[static_cast<size_t>(2) * PW * PH + o] = p[x * 4 + 2] / 255.f;
                }
            }
        };
        const auto fromTensor = [&](const float *t) {
            QImage img(W, H, QImage::Format_RGBA8888);
            for (int y = 0; y < H; ++y) {
                uchar *q = img.scanLine(y);
                for (int x = 0; x < W; ++x) {
                    const size_t o = static_cast<size_t>(y) * PW + x;
                    for (int c = 0; c < 3; ++c)
                        q[x * 4 + c] = static_cast<uchar>(
                            std::lround(std::clamp(t[static_cast<size_t>(c) * PW * PH + o], 0.f, 1.f) * 255));
                    q[x * 4 + 3] = 255;
                }
            }
            return img;
        };
        const auto decode = [&](std::int64_t i) {
            QImage img = dec.frameAt(frameTime(i, fps));
            return img.size() == size ? img : QImage();
        };

        QImage a = decode(first);
        if (a.isNull())
            return fail(QCoreApplication::translate("Engine", "Could not read the video"));
        std::vector<float> ta, tb;
        toTensor(a, ta);
        const std::int64_t pairs = last - first;
        for (std::int64_t i = first; i < last; ++i) {
            if (!enc.writeVideo(a))
                return fail(QCoreApplication::translate("Engine", "Failed to write the interpolated video"));
            QImage b = decode(i + 1);
            if (b.isNull())
                b = a; // fim do arquivo: o último quadro fica parado
            const bool same = b.cacheKey() == a.cacheKey();
            if (!same)
                toTensor(b, tb);
            for (int j = 1; j < k; ++j) {
                if (same) {
                    enc.writeVideo(a);
                    continue;
                }
                float t = static_cast<float>(j) / k;
                std::array<Ort::Value, 3> inputs{
                    Ort::Value::CreateTensor<float>(mem, ta.data(), ta.size(), imgShape.data(), imgShape.size()),
                    Ort::Value::CreateTensor<float>(mem, tb.data(), tb.size(), imgShape.data(), imgShape.size()),
                    Ort::Value::CreateTensor<float>(mem, &t, 1, tShape.data(), tShape.size())};
                auto out = session.Run(Ort::RunOptions{nullptr}, inNames.data(), inputs.data(), inputs.size(),
                                       outNames.data(), outNames.size());
                if (!enc.writeVideo(fromTensor(out[0].GetTensorData<float>())))
                    return fail(QCoreApplication::translate("Engine", "Failed to write the interpolated video"));
                if (progress && !progress((static_cast<double>(i - first) + static_cast<double>(j) / k) / pairs)) {
                    enc.finish();
                    QFile::remove(tmp);
                    return fail(QCoreApplication::translate("Engine", "Frame interpolation cancelled"));
                }
            }
            a = b;
            if (!same)
                std::swap(ta, tb);
            if (progress && !progress(static_cast<double>(i + 1 - first) / pairs)) {
                enc.finish();
                QFile::remove(tmp);
                return fail(QCoreApplication::translate("Engine", "Frame interpolation cancelled"));
            }
        }
        if (!enc.writeVideo(a) || !enc.finish(&err)) {
            QFile::remove(tmp);
            return fail(err.isEmpty() ? QCoreApplication::translate("Engine", "Failed to write the interpolated video") : err);
        }
        QFile::remove(outPath);
        if (!QFile::rename(tmp, outPath))
            return fail(QCoreApplication::translate("Engine", "Could not write %1").arg(outPath));
        return true;
    } catch (const Ort::Exception &e) {
        return fail(QCoreApplication::translate("Engine", "AI model error: %1").arg(QString::fromUtf8(e.what())));
    }
#endif
}

} // namespace clv::engine
