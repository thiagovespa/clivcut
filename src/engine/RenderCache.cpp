// SPDX-License-Identifier: GPL-3.0-or-later
#include "RenderCache.h"

#include "core/Stabilize.h"

#include "DecoderFrameSource.h"
#include "Matting.h"
#include "core/Evaluate.h"
#include "core/Serialize.h"
#include "media/Encoder.h"
#include "render/Compositor.h"
#include "render/OffscreenRhi.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace clv::engine {

namespace {

// Clipes até esta distância de um trecho entram no hash (transições e animações olham os vizinhos).
constexpr Micros kNeighborMargin = 2 * kSecond;

QByteArray mediaStamp(const MediaAsset &a)
{
    const QFileInfo fi(a.path);
    QByteArray s = fi.absoluteFilePath().toUtf8() + '|' + QByteArray::number(fi.size()) + '|'
                   + QByteArray::number(fi.lastModified().toMSecsSinceEpoch());
    // O recorte por IA chega depois: o trecho muda quando o matte fica pronto.
    if (!registeredMatte(a.path).isEmpty())
        s += "|matte";
    return s;
}

} // namespace

QString renderCacheDir()
{
    QString base = qEnvironmentVariable("CLIVCUT_CACHE_DIR");
    if (base.isEmpty())
        base = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)).filePath(QStringLiteral("clivcut"));
    return QDir(base).filePath(QStringLiteral("render"));
}

int renderChunkCount(const Project &p)
{
    return static_cast<int>((p.duration() + kRenderChunk - 1) / kRenderChunk);
}

std::vector<QString> renderChunkKeys(const Project &p)
{
    const int n = renderChunkCount(p);
    QCryptographicHash canvas(QCryptographicHash::Sha1);
    canvas.addData(QByteArray::number(p.width) + 'x' + QByteArray::number(p.height) + '@' + QByteArray::number(p.fps, 'g', 10)
                   + p.background.toUtf8() + QByteArray::number(p.backgroundBlur, 'g', 6));

    // Hash de cada clipe visual uma vez; o trecho combina os que o tocam.
    struct ClipHash {
        Micros start, end;
        QByteArray hash;
    };
    std::vector<ClipHash> clips;
    for (size_t ti = 0; ti < p.tracks.size(); ++ti) {
        const Track &t = p.tracks[ti];
        if (t.hidden || t.kind == TrackKind::Audio)
            continue;
        for (const Clip &c : t.clips) {
            if (c.hasMedia() && c.audioOnly)
                continue;
            QByteArray data = QByteArray::number(static_cast<int>(ti)) + '|'
                              + QJsonDocument(clipToJson(c)).toJson(QJsonDocument::Compact);
            if (const MediaAsset *a = p.asset(c.assetId)) {
                data += mediaStamp(*a);
                // A estabilização só vale depois da análise: o trecho muda quando ela termina.
                if (c.stabilize > 0 && registeredMotion(a->path))
                    data += "|motion";
            }
            clips.push_back({c.start, c.end(), QCryptographicHash::hash(data, QCryptographicHash::Sha1)});
        }
    }
    std::vector<QString> keys(n);
    for (int i = 0; i < n; ++i) {
        const Micros t0 = i * kRenderChunk - kNeighborMargin;
        const Micros t1 = (i + 1) * kRenderChunk + kNeighborMargin;
        QCryptographicHash h(QCryptographicHash::Sha1);
        h.addData(canvas.result());
        h.addData(QByteArray::number(i));
        // Trecho final pode ser mais curto: o comprimento entra na chave.
        h.addData(QByteArray::number(std::min(kRenderChunk, p.duration() - i * kRenderChunk)));
        for (const auto &c : clips)
            if (c.end > t0 && c.start < t1)
                h.addData(c.hash);
        keys[i] = QString::fromLatin1(h.result().toHex().left(24));
    }
    return keys;
}

QSize renderCacheSize(const Project &p, int longSide)
{
    const double k = std::min(1.0, static_cast<double>(longSide) / std::max(p.width, p.height));
    return QSize(static_cast<int>(std::lround(p.width * k)) & ~1, static_cast<int>(std::lround(p.height * k)) & ~1)
        .expandedTo(QSize(16, 16));
}

QString renderCacheFile(const QString &key, int longSide)
{
    return QDir(renderCacheDir()).filePath(QStringLiteral("%1_%2.mp4").arg(key).arg(longSide));
}

bool renderChunks(const Project &p, const std::vector<int> &chunks, int longSide, render::OffscreenRhi *rhiCtx,
                  const std::function<bool(double)> &progress, const std::function<void(int)> &chunkDone,
                  QString *error)
{
    auto fail = [&](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    QDir().mkpath(renderCacheDir());
    const std::vector<QString> keys = renderChunkKeys(p);
    const QSize size = renderCacheSize(p, longSide);
    QRhi *rhi = rhiCtx->rhi();
    render::Compositor compositor(rhi);
    compositor.setRenderSize(size);
    DecoderFrameSource frames(size * 2);

    // Total de quadros, para o progresso.
    const double fps = p.fps;
    std::int64_t total = 0, done = 0;
    for (int c : chunks)
        total += std::llround(static_cast<double>(std::min(kRenderChunk, p.duration() - c * kRenderChunk)) * fps / kSecond);
    total = std::max<std::int64_t>(1, total);

    for (int c : chunks) {
        if (c < 0 || c >= static_cast<int>(keys.size()))
            continue;
        const QString out = renderCacheFile(keys[c], longSide);
        const Micros start = c * kRenderChunk;
        const std::int64_t n = std::llround(static_cast<double>(std::min(kRenderChunk, p.duration() - start)) * fps / kSecond);
        if (QFileInfo::exists(out)) {
            done += n;
            continue;
        }
        media::EncodeSettings es;
        es.width = size.width();
        es.height = size.height();
        es.fps = fps;
        es.crf = 16;
        es.preset = QStringLiteral("ultrafast");
        es.gopFrames = 1; // só keyframes: busca instantânea dentro do trecho
        es.audio = false;
        const QString tmp = out + QStringLiteral(".part.mp4");
        media::Encoder enc;
        QString err;
        if (!enc.open(tmp, es, &err))
            return fail(err);
        for (std::int64_t i = 0; i < n; ++i) {
            const Micros t = start + static_cast<Micros>(std::llround(i * kSecond / fps));
            QRhiCommandBuffer *cb = nullptr;
            if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
                return fail(QCoreApplication::translate("Engine", "GPU failure while rendering frame %1").arg(i));
            compositor.composite(cb, p, evaluate(p, t), frames);
            QRhiReadbackResult rb;
            compositor.readback(cb, &rb);
            rhi->endOffscreenFrame();
            if (!enc.writeVideo(compositor.imageFromReadback(rb)))
                return fail(QCoreApplication::translate("Engine", "Failed to encode the video at frame %1").arg(i));
            ++done;
            if (progress && !progress(static_cast<double>(done) / total)) {
                enc.finish();
                QFile::remove(tmp);
                return fail(QCoreApplication::translate("Engine", "Render cancelled"));
            }
        }
        if (!enc.finish(&err)) {
            QFile::remove(tmp);
            return fail(err);
        }
        QFile::remove(out);
        if (!QFile::rename(tmp, out))
            return fail(QCoreApplication::translate("Engine", "Could not write %1").arg(out));
        if (chunkDone)
            chunkDone(c);
    }
    return true;
}

qint64 clearRenderCache()
{
    qint64 freed = 0;
    QDirIterator it(renderCacheDir(), {QStringLiteral("*.mp4")}, QDir::Files);
    while (it.hasNext()) {
        const QFileInfo fi(it.next());
        freed += fi.size();
        QFile::remove(fi.absoluteFilePath());
    }
    return freed;
}

} // namespace clv::engine
