// SPDX-License-Identifier: GPL-3.0-or-later
#include "Matting.h"

#include "Proxy.h"
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
#include <QImage>
#include <QMutex>
#include <QThread>

#include <algorithm>
#include <array>
#include <cmath>

#ifdef CLIVCUT_HAVE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace clv::engine {

namespace {
QMutex g_mutex;
QHash<QString, QString> g_mattes;
} // namespace

bool mattingAvailable()
{
#ifdef CLIVCUT_HAVE_ONNX
    return true;
#else
    return false;
#endif
}

QString mattePathFor(const QString &mediaPath)
{
    const QFileInfo fi(mediaPath);
    const QByteArray k = fi.absoluteFilePath().toUtf8() + '|' + QByteArray::number(fi.size()) + '|' +
                         QByteArray::number(fi.lastModified().toMSecsSinceEpoch()) + "|rvm1";
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(k, QCryptographicHash::Sha1).toHex().left(16));
    const QString dir = QDir(QFileInfo(proxyCacheDir()).absolutePath()).filePath(QStringLiteral("mattes"));
    return QDir(dir).filePath(QStringLiteral("%1_%2.mp4").arg(fi.completeBaseName().left(40), hash));
}

bool generateMatte(const QString &mediaPath, const QString &modelPath, const QString &outPath,
                   const std::function<bool(double)> &progress, QString *error)
{
    auto fail = [&](const QString &m) {
        if (error)
            *error = m;
        return false;
    };
#ifndef CLIVCUT_HAVE_ONNX
    (void)mediaPath;
    (void)modelPath;
    (void)outPath;
    (void)progress;
    return fail(QCoreApplication::translate("Engine", "This build was compiled without ONNX Runtime"));
#else
    if (!QFileInfo::exists(modelPath))
        return fail(QCoreApplication::translate("Engine", "Cutout model not found: %1").arg(modelPath));
    MediaAsset info;
    if (!media::probe(mediaPath, info, error))
        return false;
    if (info.kind == MediaKind::Audio)
        return fail(QCoreApplication::translate("Engine", "Background removal only applies to videos and images"));

    media::VideoDecoder dec;
    if (!dec.open(mediaPath, QSize(kMatteMaxSide, kMatteMaxSide), error, true))
        return false;
    const QSize size = dec.outputSize();
    const int W = size.width(), H = size.height();
    const bool still = info.kind == MediaKind::Image;
    const double fps = still ? 1.0 : (info.fps > 1 ? std::min(info.fps, 60.0) : 30.0);
    const Micros duration = still ? kSecond : std::max<Micros>(dec.duration(), info.duration);
    const std::int64_t total = std::max<std::int64_t>(1, std::llround(duration * fps / kSecond));

    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "clivcut");
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(std::max(1, QThread::idealThreadCount()));
        // Sem espera girando: com a CPU ocupada (um build, uma exportação), as threads ociosas
        // roubam tempo das que trabalham e tudo fica bem mais lento.
        opts.AddConfigEntry("session.intra_op.allow_spinning", "0");
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef _WIN32
        const std::wstring model = modelPath.toStdWString();
#else
        const std::string model = modelPath.toStdString();
#endif
        Ort::Session session(env, model.c_str(), opts);
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        const std::array<const char *, 6> inNames{"src", "r1i", "r2i", "r3i", "r4i", "downsample_ratio"};
        const std::array<const char *, 5> outNames{"pha", "r1o", "r2o", "r3o", "r4o"};
        // A rede trabalha internamente em ~512 px no lado maior.
        float ratio = std::min(1.0f, 512.0f / static_cast<float>(std::max(W, H)));
        const std::array<int64_t, 1> ratioShape{1};
        const std::array<int64_t, 4> srcShape{1, 3, H, W};
        std::vector<float> src(static_cast<size_t>(3) * W * H);
        float zero = 0.f;
        const std::array<int64_t, 4> zeroShape{1, 1, 1, 1};
        std::vector<Ort::Value> rec; // estados recorrentes r1..r4 do quadro anterior

        QDir().mkpath(QFileInfo(outPath).absolutePath());
        const QString tmp = outPath + QStringLiteral(".part.mp4");
        media::EncodeSettings es;
        es.width = W;
        es.height = H;
        es.fps = fps;
        es.crf = 16;
        es.preset = QStringLiteral("veryfast");
        es.gopFrames = std::max(1, static_cast<int>(fps / 2));
        es.audio = false;
        media::Encoder enc;
        QString err;
        if (!enc.open(tmp, es, &err))
            return fail(err);

        QImage matte(W, H, QImage::Format_RGBA8888);
        for (std::int64_t i = 0; i < total; ++i) {
            const QImage frame = dec.frameAt(static_cast<Micros>(std::llround(i * kSecond / fps)))
                                     .convertToFormat(QImage::Format_RGBA8888);
            if (frame.isNull() || frame.size() != size)
                break;
            // RGBA (HWC, 0..255) → RGB planar (CHW, 0..1).
            for (int y = 0; y < H; ++y) {
                const uchar *p = frame.constScanLine(y);
                for (int x = 0; x < W; ++x) {
                    const size_t o = static_cast<size_t>(y) * W + x;
                    src[o] = p[x * 4] / 255.f;
                    src[static_cast<size_t>(W) * H + o] = p[x * 4 + 1] / 255.f;
                    src[static_cast<size_t>(2) * W * H + o] = p[x * 4 + 2] / 255.f;
                }
            }
            std::vector<Ort::Value> inputs;
            inputs.push_back(Ort::Value::CreateTensor<float>(mem, src.data(), src.size(), srcShape.data(), srcShape.size()));
            for (int r = 0; r < 4; ++r) {
                if (rec.empty())
                    inputs.push_back(Ort::Value::CreateTensor<float>(mem, &zero, 1, zeroShape.data(), zeroShape.size()));
                else
                    inputs.push_back(std::move(rec[r]));
            }
            inputs.push_back(Ort::Value::CreateTensor<float>(mem, &ratio, 1, ratioShape.data(), ratioShape.size()));
            auto outputs = session.Run(Ort::RunOptions{nullptr}, inNames.data(), inputs.data(), inputs.size(),
                                       outNames.data(), outNames.size());
            const float *pha = outputs[0].GetTensorData<float>();
            for (int y = 0; y < H; ++y) {
                uchar *q = matte.scanLine(y);
                for (int x = 0; x < W; ++x) {
                    const auto v = static_cast<uchar>(std::lround(std::clamp(pha[static_cast<size_t>(y) * W + x], 0.f, 1.f) * 255));
                    q[x * 4] = q[x * 4 + 1] = q[x * 4 + 2] = v;
                    q[x * 4 + 3] = 255;
                }
            }
            rec.clear();
            for (int r = 1; r <= 4; ++r)
                rec.push_back(std::move(outputs[r]));
            if (!enc.writeVideo(matte))
                return fail(QCoreApplication::translate("Engine", "Failed to write the matte"));
            if (progress && !progress(static_cast<double>(i + 1) / total)) {
                enc.finish();
                QFile::remove(tmp);
                return fail(QCoreApplication::translate("Engine", "Background removal cancelled"));
            }
        }
        if (!enc.finish(&err)) {
            QFile::remove(tmp);
            return fail(err);
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

void registerMatte(const QString &mediaPath, const QString &mattePath)
{
    QMutexLocker lock(&g_mutex);
    g_mattes.insert(mediaPath, mattePath);
}

QString registeredMatte(const QString &mediaPath)
{
    QMutexLocker lock(&g_mutex);
    return g_mattes.value(mediaPath);
}

bool discoverMatte(const QString &mediaPath)
{
    const QString p = mattePathFor(mediaPath);
    if (QFileInfo(p).size() > 0) {
        registerMatte(mediaPath, p);
        return true;
    }
    return false;
}

} // namespace clv::engine
