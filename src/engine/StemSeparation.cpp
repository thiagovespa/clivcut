// SPDX-License-Identifier: GPL-3.0-or-later
#include "StemSeparation.h"

#include "Proxy.h"
#include "media/AudioDecoder.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#ifdef CLIVCUT_HAVE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace clv::engine {

namespace {

constexpr int kModelRate = 44100;
constexpr int kSegment = 343980;          // 7,8 s: tamanho fixo de entrada do modelo
constexpr int kOverlap = kSegment / 4;
constexpr int kStride = kSegment - kOverlap;
constexpr int kModelStems = 4;            // saída do modelo: bateria, baixo, outros, voz

const char *stemFile(Stem s)
{
    switch (s) {
    case Stem::Drums: return "drums";
    case Stem::Bass: return "bass";
    case Stem::Other: return "other";
    case Stem::Vocals: return "vocals";
    case Stem::Instrumental: return "instrumental";
    }
    return "other";
}

/** Cabeçalho WAV float 32 bits estéreo para `frames` quadros a `rate` Hz. */
QByteArray wavHeader(std::int64_t frames, int rate)
{
    const quint32 dataBytes = static_cast<quint32>(std::min<std::int64_t>(frames * 2 * 4, 0xFFFFFFF0LL));
    QByteArray h;
    auto u32 = [&](quint32 v) { h.append(reinterpret_cast<const char *>(&v), 4); };
    auto u16 = [&](quint16 v) { h.append(reinterpret_cast<const char *>(&v), 2); };
    h.append("RIFF");
    u32(36 + dataBytes);
    h.append("WAVEfmt ");
    u32(16);
    u16(3); // IEEE float
    u16(2);
    u32(static_cast<quint32>(rate));
    u32(static_cast<quint32>(rate) * 2 * 4);
    u16(2 * 4);
    u16(32);
    h.append("data");
    u32(dataBytes);
    return h;
}

} // namespace

bool stemSeparationAvailable()
{
#ifdef CLIVCUT_HAVE_ONNX
    return true;
#else
    return false;
#endif
}

QString stemName(Stem s)
{
    switch (s) {
    case Stem::Drums: return QCoreApplication::translate("Engine", "Drums");
    case Stem::Bass: return QCoreApplication::translate("Engine", "Bass");
    case Stem::Other: return QCoreApplication::translate("Engine", "Other instruments");
    case Stem::Vocals: return QCoreApplication::translate("Engine", "Vocals");
    case Stem::Instrumental: return QCoreApplication::translate("Engine", "Instrumental");
    }
    return {};
}

QString stemPathFor(const QString &mediaPath, Stem s)
{
    const QFileInfo fi(mediaPath);
    const QByteArray k = fi.absoluteFilePath().toUtf8() + '|' + QByteArray::number(fi.size()) + '|'
                         + QByteArray::number(fi.lastModified().toMSecsSinceEpoch()) + "|htdemucs1";
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(k, QCryptographicHash::Sha1).toHex().left(16));
    const QString dir = QDir(QFileInfo(proxyCacheDir()).absolutePath()).filePath(QStringLiteral("stems"));
    return QDir(dir).filePath(QStringLiteral("%1_%2_%3.wav").arg(fi.completeBaseName().left(40), hash,
                                                                 QString::fromLatin1(stemFile(s))));
}

bool stemsReady(const QString &mediaPath)
{
    for (int i = 0; i < kStemCount; ++i)
        if (!QFileInfo::exists(stemPathFor(mediaPath, static_cast<Stem>(i))))
            return false;
    return true;
}

bool separateStems(const QString &mediaPath, const QString &modelPath, const std::function<bool(double)> &progress,
                   QString *error)
{
    auto fail = [&](const QString &m) {
        if (error)
            *error = m;
        return false;
    };
#ifndef CLIVCUT_HAVE_ONNX
    (void)mediaPath;
    (void)modelPath;
    (void)progress;
    return fail(QCoreApplication::translate("Engine", "This build was compiled without ONNX Runtime"));
#else
    if (!QFileInfo::exists(modelPath))
        return fail(QCoreApplication::translate("Engine", "Stem separation model not found: %1").arg(modelPath));
    media::AudioDecoder dec;
    if (!dec.open(mediaPath, error))
        return false;
    const std::int64_t total48 = static_cast<std::int64_t>(std::llround(static_cast<double>(dec.duration()) * media::kSampleRate / kSecond));
    const std::int64_t total = static_cast<std::int64_t>(std::llround(static_cast<double>(dec.duration()) * kModelRate / kSecond));
    if (total <= 0)
        return fail(QCoreApplication::translate("Engine", "Audio has no duration"));

    // 48 kHz (decodificador) → 44,1 kHz (modelo), com contexto contínuo.
    SwrContext *swr = nullptr;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, kModelRate, &stereo, AV_SAMPLE_FMT_FLT, media::kSampleRate, 0,
                            nullptr) < 0
        || swr_init(swr) < 0) {
        swr_free(&swr);
        return fail(QCoreApplication::translate("Engine", "Could not set up the audio resampler"));
    }
    std::unique_ptr<SwrContext, void (*)(SwrContext *)> swrGuard(swr, [](SwrContext *s) { swr_free(&s); });

    // Saídas: grava em .part e só renomeia no fim (faixas pela metade nunca valem como cache).
    std::array<std::unique_ptr<QFile>, kStemCount> files;
    for (int i = 0; i < kStemCount; ++i) {
        const QString path = stemPathFor(mediaPath, static_cast<Stem>(i));
        QDir().mkpath(QFileInfo(path).absolutePath());
        files[i] = std::make_unique<QFile>(path + QStringLiteral(".part"));
        if (!files[i]->open(QIODevice::WriteOnly | QIODevice::Truncate))
            return fail(files[i]->errorString());
        files[i]->write(wavHeader(total, kModelRate));
    }
    auto discard = [&] {
        for (auto &f : files) {
            f->close();
            f->remove();
        }
    };

    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "clivcut");
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(std::max(1, QThread::idealThreadCount()));
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef _WIN32
        const std::wstring model = modelPath.toStdWString();
#else
        const std::string model = modelPath.toStdString();
#endif
        Ort::Session session(env, model.c_str(), opts);
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const char *inNames[] = {"mix"};
        const char *outNames[] = {"stems"};
        const std::array<int64_t, 3> inShape{1, 2, kSegment};

        // Entrada a 44,1 kHz (intercalada), a partir da amostra absoluta `inBase`.
        std::vector<float> in;
        std::int64_t inBase = 0, read48 = 0;
        bool flushed = false;
        std::vector<float> block(static_cast<size_t>(media::kSampleRate) * 2), res;
        auto fill = [&](std::int64_t until) {
            while (inBase + static_cast<std::int64_t>(in.size() / 2) < until && !flushed) {
                const int n = static_cast<int>(std::min<std::int64_t>(media::kSampleRate, total48 - read48));
                int got;
                res.resize(static_cast<size_t>(swr_get_out_samples(swr, std::max(n, 0))) * 2 + 64);
                uint8_t *o = reinterpret_cast<uint8_t *>(res.data());
                if (n > 0) {
                    std::fill(block.begin(), block.end(), 0.f);
                    dec.read(static_cast<Micros>(std::llround(static_cast<double>(read48) * kSecond / media::kSampleRate)),
                             block.data(), n);
                    const uint8_t *ip = reinterpret_cast<const uint8_t *>(block.data());
                    got = swr_convert(swr, &o, static_cast<int>(res.size() / 2), &ip, n);
                    read48 += n;
                } else {
                    got = swr_convert(swr, &o, static_cast<int>(res.size() / 2), nullptr, 0);
                    flushed = true;
                }
                if (got > 0)
                    in.insert(in.end(), res.begin(), res.begin() + static_cast<std::ptrdiff_t>(got) * 2);
            }
        };

        std::vector<float> window(kSegment, 1.f);
        for (int i = 0; i < kOverlap; ++i) {
            const float f = static_cast<float>(i) / (kOverlap - 1);
            window[i] = f;
            window[kSegment - 1 - i] = f;
        }
        std::vector<float> x(static_cast<size_t>(2) * kSegment);
        // Cauda do trecho anterior (somada à cabeça do próximo): 4 faixas × 2 canais + peso.
        std::vector<float> pend(static_cast<size_t>(kModelStems) * 2 * kOverlap, 0.f), pendW(kOverlap, 0.f);
        std::vector<float> outBuf;
        const std::int64_t chunks = std::max<std::int64_t>(1, (total + kStride - 1) / kStride);
        for (std::int64_t c = 0; c < chunks; ++c) {
            const std::int64_t start = c * kStride;
            const int clen = static_cast<int>(std::min<std::int64_t>(kSegment, total - start));
            fill(start + kSegment);
            // Descarta a entrada que já ficou para trás.
            if (start > inBase) {
                const std::int64_t drop = std::min<std::int64_t>(start - inBase, static_cast<std::int64_t>(in.size() / 2));
                in.erase(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(drop) * 2);
                inBase += drop;
            }
            const std::int64_t have = static_cast<std::int64_t>(in.size() / 2);
            for (int k = 0; k < kSegment; ++k) {
                const bool ok = k < have;
                x[static_cast<size_t>(k)] = ok ? in[static_cast<size_t>(k) * 2] : 0.f;
                x[static_cast<size_t>(kSegment) + k] = ok ? in[static_cast<size_t>(k) * 2 + 1] : 0.f;
            }
            Ort::Value input = Ort::Value::CreateTensor<float>(mem, x.data(), x.size(), inShape.data(), inShape.size());
            auto outs = session.Run(Ort::RunOptions{nullptr}, inNames, &input, 1, outNames, 1);
            const float *y = outs[0].GetTensorData<float>(); // (1, 4, 2, kSegment)

            // Mistura com a cauda anterior e grava o que já está final: tudo até o próximo
            // trecho começar (ou o resto, no último).
            const bool last = c == chunks - 1;
            const int emitCount = last ? clen : std::min(clen, kStride);
            outBuf.resize(static_cast<size_t>(emitCount) * 2);
            auto value = [&](int s, int ch, int k) {
                double v = static_cast<double>(y[(static_cast<size_t>(s) * 2 + ch) * kSegment + k]) * window[k];
                double w = window[k];
                if (c > 0 && k < kOverlap) {
                    v += pend[(static_cast<size_t>(s) * 2 + ch) * kOverlap + k];
                    w += pendW[k];
                }
                return static_cast<float>(v / std::max(w, 1e-8));
            };
            for (int s = 0; s < kStemCount; ++s) {
                for (int k = 0; k < emitCount; ++k)
                    for (int ch = 0; ch < 2; ++ch) {
                        float v;
                        if (s < kModelStems)
                            v = value(s, ch, k);
                        else // instrumental: bateria + baixo + outros
                            v = value(0, ch, k) + value(1, ch, k) + value(2, ch, k);
                        outBuf[static_cast<size_t>(k) * 2 + ch] = v;
                    }
                files[s]->write(reinterpret_cast<const char *>(outBuf.data()), static_cast<qint64>(outBuf.size()) * 4);
            }
            if (!last) {
                for (int s = 0; s < kModelStems; ++s)
                    for (int ch = 0; ch < 2; ++ch)
                        for (int k = 0; k < kOverlap; ++k) {
                            const int j = kStride + k;
                            pend[(static_cast<size_t>(s) * 2 + ch) * kOverlap + k] =
                                j < clen ? y[(static_cast<size_t>(s) * 2 + ch) * kSegment + j] * window[j] : 0.f;
                        }
                for (int k = 0; k < kOverlap; ++k)
                    pendW[k] = kStride + k < clen ? window[kStride + k] : 0.f;
            }
            if (progress && !progress(static_cast<double>(c + 1) / chunks)) {
                discard();
                return fail(QCoreApplication::translate("Engine", "Stem separation cancelled"));
            }
        }
    } catch (const Ort::Exception &e) {
        discard();
        return fail(QCoreApplication::translate("Engine", "AI model error: %1").arg(QString::fromUtf8(e.what())));
    }
    for (int i = 0; i < kStemCount; ++i) {
        files[i]->close();
        const QString path = stemPathFor(mediaPath, static_cast<Stem>(i));
        QFile::remove(path);
        if (!files[i]->rename(path)) {
            discard();
            return fail(QCoreApplication::translate("Engine", "Could not write %1").arg(path));
        }
    }
    return true;
#endif
}

} // namespace clv::engine
