// SPDX-License-Identifier: GPL-3.0-or-later
#include "Exporter.h"

#include "AudioMixer.h"
#include "AiModels.h"
#include "DecoderFrameSource.h"
#include "FrameInterpolation.h"
#include "Matting.h"
#include "Stabilization.h"
#include "core/Evaluate.h"
#include "render/Compositor.h"
#include "render/OffscreenRhi.h"

#include <QCoreApplication>
#include <QFile>

#include <cmath>
#include <memory>
#include <vector>

namespace clv::engine {

bool Exporter::run(const Project &p, ExportOptions opts, const Progress &progress, QString *error,
                   render::OffscreenRhi *externalRhi)
{
    auto fail = [&](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    // Análises já feitas (recorte de fundo, movimento) ficam em cache: quem exporta fora do
    // app (linha de comando) também as usa.
    p.forEachClip([&](const Clip &c) {
        const MediaAsset *a = c.hasMedia() ? p.asset(c.assetId) : nullptr;
        if (a && c.removeBackground)
            discoverMatte(a->path);
        // Sem análise ainda (ex.: linha de comando): analisa agora, para o vídeo sair estabilizado.
        if (a && c.stabilize > 0 && a->kind == MediaKind::Video && !discoverMotion(a->path))
            analyzeMotion(a->path, motionPathFor(a->path), {}, nullptr);
        // Câmera lenta fluida ainda sem os quadros intermediários: gera agora (precisa do modelo).
        if (a && c.smoothSlowMotion && a->kind == MediaKind::Video) {
            discoverInterpolations(a->path);
            const InterpolationSpan span = interpolationSpanFor(p, c);
            if (span.factor > 1 && findInterpolation(a->path, span).isEmpty() && aiModelInstalled(kInterpolationModel)) {
                Micros fileStart = 0;
                const QString out = interpolationPathFor(a->path, span);
                if (generateInterpolation(a->path, span, aiModelPath(kInterpolationModel), out, &fileStart, {}, nullptr))
                    registerInterpolation(a->path, span, fileStart, out);
            }
        }
    });
    media::EncodeSettings &enc = opts.encode;
    if (enc.width <= 0 || enc.height <= 0) {
        enc.width = p.width;
        enc.height = p.height;
    }
    enc.width &= ~1;
    enc.height &= ~1;
    if (enc.fps <= 0)
        enc.fps = p.fps;
    const Micros end = opts.rangeEnd < 0 ? p.duration() : std::min(opts.rangeEnd, p.duration());
    const Micros start = std::clamp<Micros>(opts.rangeStart, 0, end);
    if (end <= start)
        return fail(QCoreApplication::translate("Engine", "Nothing to export: the timeline is empty"));

    bool hasAudio = false;
    for (const auto &[id, a] : p.assets)
        hasAudio = hasAudio || a.hasAudio;
    enc.audio = enc.audio && hasAudio;

    // Só áudio: mixa em blocos de 0,1 s, sem GPU.
    if (!enc.video) {
        media::Encoder encoder;
        QString err;
        if (!encoder.open(opts.outputPath, enc, &err))
            return fail(err);
        m_encoderName = encoder.codecName();
        AudioMixer mixer;
        const std::int64_t s0 = std::llround(static_cast<double>(start) * media::kSampleRate / kSecond);
        const std::int64_t s1 = std::llround(static_cast<double>(end) * media::kSampleRate / kSecond);
        constexpr int kBlock = media::kSampleRate / 10;
        std::vector<float> buf(static_cast<size_t>(kBlock) * media::kChannels);
        for (std::int64_t s = s0; s < s1; s += kBlock) {
            const int n = static_cast<int>(std::min<std::int64_t>(kBlock, s1 - s));
            mixer.mix(p, static_cast<Micros>(std::llround(s * static_cast<double>(kSecond) / media::kSampleRate)), buf.data(), n);
            if (!encoder.writeAudio(buf.data(), n))
                return fail(QCoreApplication::translate("Engine", "Failed to encode the audio"));
            if (progress && !progress(static_cast<double>(s + n - s0) / (s1 - s0))) {
                encoder.finish();
                QFile::remove(opts.outputPath);
                return fail(QCoreApplication::translate("Engine", "Export cancelled"));
            }
        }
        if (!encoder.finish(&err))
            return fail(err);
        return true;
    }

    std::unique_ptr<render::OffscreenRhi> ownRhi;
    render::OffscreenRhi *rhiCtx = externalRhi;
    if (!rhiCtx) {
        ownRhi = std::make_unique<render::OffscreenRhi>();
        QString err;
        if (!ownRhi->create(&err))
            return fail(err);
        rhiCtx = ownRhi.get();
    }
    QRhi *rhi = rhiCtx->rhi();

    media::Encoder encoder;
    QString err;
    if (!encoder.open(opts.outputPath, enc, &err))
        return fail(err);
    m_encoderName = encoder.codecName();

    // A composição acontece na resolução de saída: o projeto é escalado inteiro.
    Project scaled = p;
    render::Compositor compositor(rhi);
    compositor.setRenderSize(QSize(enc.width, enc.height));
    compositor.setTransparentBackground(media::codecHasAlpha(enc.codec));
    DecoderFrameSource frames(QSize(enc.width * 2, enc.height * 2));
    frames.setCacheFrames(kExportLookahead + 2);
    AudioMixer mixer;
    std::vector<float> audioBuf;

    const std::int64_t totalFrames =
        std::max<std::int64_t>(1, std::llround(static_cast<double>(end - start) * enc.fps / kSecond));
    for (std::int64_t i = 0; i < totalFrames; ++i) {
        const auto frameTime = [&](std::int64_t k) { return start + static_cast<Micros>(std::llround(k * kSecond / enc.fps)); };
        const Micros t = frameTime(i);
        // Os próximos quadros decodificam enquanto este é composto e codificado.
        std::vector<Micros> ahead;
        for (std::int64_t k = i; k <= std::min(i + kExportLookahead, totalFrames - 1); ++k)
            ahead.push_back(frameTime(k));
        frames.prefetch(scaled, ahead);
        const FrameDesc frame = evaluate(scaled, t);

        QRhiCommandBuffer *cb = nullptr;
        if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
            return fail(QCoreApplication::translate("Engine", "GPU failure while rendering frame %1").arg(i));
        compositor.composite(cb, scaled, frame, frames);
        QRhiReadbackResult rb;
        compositor.readback(cb, &rb);
        rhi->endOffscreenFrame();
        if (!encoder.writeVideo(compositor.imageFromReadback(rb)))
            return fail(QCoreApplication::translate("Engine", "Failed to encode the video at frame %1").arg(i));

        if (enc.audio) {
            const std::int64_t s0 = std::llround(i * media::kSampleRate / enc.fps);
            const std::int64_t s1 = std::llround((i + 1) * media::kSampleRate / enc.fps);
            const int n = static_cast<int>(s1 - s0);
            audioBuf.resize(static_cast<size_t>(n) * media::kChannels);
            mixer.mix(p, start + static_cast<Micros>(std::llround(s0 * static_cast<double>(kSecond) / media::kSampleRate)),
                      audioBuf.data(), n);
            if (!encoder.writeAudio(audioBuf.data(), n))
                return fail(QCoreApplication::translate("Engine", "Failed to encode the audio"));
        }

        if (progress && !progress(static_cast<double>(i + 1) / totalFrames)) {
            encoder.finish();
            QFile::remove(opts.outputPath);
            return fail(QCoreApplication::translate("Engine", "Export cancelled"));
        }
    }
    if (!encoder.finish(&err))
        return fail(err);
    return true;
}

QImage Exporter::renderStill(const Project &p, Micros t, QSize size, render::OffscreenRhi *rhiCtx, QString *error)
{
    if (size.isEmpty())
        size = QSize(p.width, p.height);
    QRhi *rhi = rhiCtx->rhi();
    render::Compositor compositor(rhi);
    compositor.setRenderSize(size);
    DecoderFrameSource frames(size * 2);
    QRhiCommandBuffer *cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) {
        if (error)
            *error = QCoreApplication::translate("Engine", "GPU failure while rendering frame %1").arg(0);
        return {};
    }
    compositor.composite(cb, p, evaluate(p, t), frames);
    QRhiReadbackResult rb;
    compositor.readback(cb, &rb);
    rhi->endOffscreenFrame();
    return compositor.imageFromReadback(rb);
}

} // namespace clv::engine
