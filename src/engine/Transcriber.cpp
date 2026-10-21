// SPDX-License-Identifier: GPL-3.0-or-later
#include "Transcriber.h"

#include "AudioMixer.h"
#include "media/AudioDecoder.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include <QCoreApplication>
#include <QFileInfo>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cmath>

#ifdef CLIVCUT_HAVE_WHISPER
#include <whisper.h>
#endif

namespace clv::engine {

namespace {
constexpr int kWhisperRate = 16000;
constexpr int kVadWindow = kWhisperRate / 50; // 20 ms

/** Energia RMS em janelas de 20 ms. */
std::vector<float> windowRms(const std::vector<float> &pcm)
{
    std::vector<float> rms(pcm.size() / kVadWindow);
    for (size_t w = 0; w < rms.size(); ++w) {
        double acc = 0;
        for (int i = 0; i < kVadWindow; ++i) {
            const float v = pcm[w * kVadWindow + i];
            acc += v * v;
        }
        rms[w] = static_cast<float>(std::sqrt(acc / kVadWindow));
    }
    return rms;
}

/** Limiar de fala: acima do ruído de fundo, relativo ao pico. */
float speechThreshold(const std::vector<float> &rms)
{
    float peak = 0;
    for (float r : rms)
        peak = std::max(peak, r);
    return std::max(0.004f, peak * 0.08f);
}

Micros windowToMicros(size_t w)
{
    return static_cast<Micros>(w) * kVadWindow * kSecond / kWhisperRate;
}
} // namespace

bool transcriptionAvailable()
{
#ifdef CLIVCUT_HAVE_WHISPER
    return true;
#else
    return false;
#endif
}

std::vector<float> renderProjectAudio16k(const Project &p, int trackIndex, Micros start, Micros end,
                                         const std::function<bool(double)> &progress)
{
    std::vector<float> out;
    if (end <= start)
        return out;

    // Só a faixa pedida: as demais ficam mudas numa cópia do projeto.
    Project src = p;
    if (trackIndex >= 0 && trackIndex < static_cast<int>(src.tracks.size()))
        for (int i = 0; i < static_cast<int>(src.tracks.size()); ++i)
            if (i != trackIndex)
                src.tracks[i].muted = true;

    SwrContext *swr = nullptr;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    if (swr_alloc_set_opts2(&swr, &mono, AV_SAMPLE_FMT_FLT, kWhisperRate, &stereo, AV_SAMPLE_FMT_FLT, media::kSampleRate,
                            0, nullptr) < 0 ||
        swr_init(swr) < 0) {
        swr_free(&swr);
        return out;
    }

    AudioMixer mixer;
    constexpr int kChunk = media::kSampleRate; // 1 s por bloco
    std::vector<float> buf(static_cast<size_t>(kChunk) * media::kChannels);
    std::vector<float> res(kChunk);
    const std::int64_t totalFrames = av_rescale(end - start, media::kSampleRate, kSecond);
    out.reserve(static_cast<size_t>(av_rescale(end - start, kWhisperRate, kSecond)) + kWhisperRate);
    for (std::int64_t done = 0; done < totalFrames;) {
        const int n = static_cast<int>(std::min<std::int64_t>(kChunk, totalFrames - done));
        mixer.mix(src, start + av_rescale(done, kSecond, media::kSampleRate), buf.data(), n);
        const uint8_t *in = reinterpret_cast<const uint8_t *>(buf.data());
        uint8_t *o = reinterpret_cast<uint8_t *>(res.data());
        const int got = swr_convert(swr, &o, static_cast<int>(res.size()), &in, n);
        if (got > 0)
            out.insert(out.end(), res.begin(), res.begin() + got);
        done += n;
        if (progress && !progress(static_cast<double>(done) / totalFrames)) {
            out.clear();
            break;
        }
    }
    swr_free(&swr);
    return out;
}

bool transcribePcm16k(const std::vector<float> &pcm, const TranscribeOptions &opts,
                      const std::function<bool(double)> &progress, std::vector<CaptionSegment> &out, QString *error)
{
#ifndef CLIVCUT_HAVE_WHISPER
    (void)pcm;
    (void)opts;
    (void)progress;
    (void)out;
    if (error)
        *error = QCoreApplication::translate("Engine", "This clivcut build was compiled without whisper.cpp");
    return false;
#else
    auto fail = [&](const QString &m) {
        if (error)
            *error = m;
        return false;
    };
    if (!QFileInfo::exists(opts.modelPath))
        return fail(QCoreApplication::translate("Engine", "Transcription model not found: %1").arg(opts.modelPath));
    if (pcm.size() < static_cast<size_t>(kWhisperRate / 2))
        return fail(QCoreApplication::translate("Engine", "Audio too short to transcribe"));

    whisper_log_set([](ggml_log_level, const char *, void *) {}, nullptr);
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = true; // só tem efeito se o whisper.cpp foi compilado com GPU
    whisper_context *ctx = whisper_init_from_file_with_params(opts.modelPath.toUtf8().constData(), cparams);
    if (!ctx)
        return fail(QCoreApplication::translate("Engine", "Failed to load model %1").arg(QFileInfo(opts.modelPath).fileName()));

    struct CallbackState {
        const std::function<bool(double)> *progress;
        std::atomic<bool> cancelled{false};
    } state{&progress};

    const QByteArray lang = opts.language.isEmpty() ? QByteArray("auto") : opts.language.toLatin1();
    whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    // Metade dos núcleos lógicos: as threads do ggml esperam girando, e com todos ocupados
    // (hyper-threading, um build ou uma exportação ao lado) a transcrição fica dezenas de vezes mais lenta.
    wp.n_threads = opts.threads > 0 ? opts.threads : std::clamp(QThread::idealThreadCount() / 2, 1, 8);
    wp.language = lang.constData();
    wp.detect_language = false;
    wp.translate = opts.translateToEnglish;
    wp.print_progress = false;
    wp.print_realtime = false;
    wp.print_timestamps = false;
    wp.print_special = false;
    wp.suppress_blank = true;
    wp.no_context = false;
    if (opts.maxChars > 0) {
        wp.token_timestamps = true;
        wp.max_len = opts.maxChars;
        wp.split_on_word = true;
    }
    wp.progress_callback = [](whisper_context *, whisper_state *, int pct, void *user) {
        auto *s = static_cast<CallbackState *>(user);
        if (*s->progress && !(*s->progress)(pct / 100.0))
            s->cancelled = true;
    };
    wp.progress_callback_user_data = &state;
    wp.abort_callback = [](void *user) { return static_cast<CallbackState *>(user)->cancelled.load(); };
    wp.abort_callback_user_data = &state;

    const int rc = whisper_full(ctx, wp, pcm.data(), static_cast<int>(pcm.size()));
    if (rc != 0 || state.cancelled) {
        whisper_free(ctx);
        return fail(state.cancelled ? QCoreApplication::translate("Engine", "Transcription cancelled")
                                    : QCoreApplication::translate("Engine", "Transcription failed (code %1)").arg(rc));
    }

    const int n = whisper_full_n_segments(ctx);
    for (int i = 0; i < n; ++i) {
        QString text = QString::fromUtf8(whisper_full_get_segment_text(ctx, i)).trimmed();
        // Marcações como [Música] ou (aplausos) não viram legenda.
        if (text.isEmpty() || ((text.startsWith(QLatin1Char('[')) && text.endsWith(QLatin1Char(']'))) ||
                               (text.startsWith(QLatin1Char('(')) && text.endsWith(QLatin1Char(')')))))
            continue;
        // Tempos do whisper em centésimos de segundo.
        const Micros t0 = whisper_full_get_segment_t0(ctx, i) * 10'000;
        const Micros t1 = whisper_full_get_segment_t1(ctx, i) * 10'000;
        if (t1 <= t0)
            continue;
        // Com max_len, pontuação solta pode virar segmento próprio: junta ao anterior.
        if (!out.empty() && text.size() <= 2 && !text.at(0).isLetterOrNumber()) {
            out.back().text += text;
            out.back().end = std::max(out.back().end, t1);
            continue;
        }
        out.push_back({t0, t1, text});
    }
    whisper_free(ctx);
    return true;
#endif
}

bool transcribeProject(const Project &p, const TranscribeOptions &opts, const std::function<bool(double)> &progress,
                       std::vector<CaptionSegment> &out, QString *error)
{
    const Micros end = opts.rangeEnd < 0 ? p.duration() : std::min(opts.rangeEnd, p.duration());
    const Micros start = std::clamp<Micros>(opts.rangeStart, 0, end);
    if (end - start < kSecond / 2) {
        if (error)
            *error = QCoreApplication::translate("Engine", "The timeline does not have enough audio to transcribe");
        return false;
    }
    // 15% do progresso para extrair o áudio, o resto para o whisper.
    auto stage = [&](double base, double weight) {
        return [&, base, weight](double f) { return !progress || progress(base + f * weight); };
    };
    std::vector<float> pcm = renderProjectAudio16k(p, opts.trackIndex, start, end, stage(0.0, 0.15));
    if (pcm.empty()) {
        if (error)
            *error = QCoreApplication::translate("Engine", "Could not extract the timeline audio");
        return false;
    }
    float peak = 0;
    for (float s : pcm)
        peak = std::max(peak, std::abs(s));
    if (peak < 1e-4f) {
        if (error)
            *error = QCoreApplication::translate("Engine", "The timeline is silent: there is no speech to transcribe");
        return false;
    }
    // O whisper tende a ancorar o 1º segmento no início do áudio: corta o
    // silêncio inicial (mantendo 200 ms de folga) e compensa depois.
    const std::vector<float> rms = windowRms(pcm);
    const float threshold = speechThreshold(rms);
    size_t firstVoice = 0;
    while (firstVoice < rms.size() && rms[firstVoice] < threshold)
        ++firstVoice;
    const size_t lead = firstVoice > 10 ? firstVoice - 10 : 0;
    const Micros leadUs = windowToMicros(lead);
    std::vector<float> trimmed(pcm.begin() + static_cast<std::ptrdiff_t>(lead * kVadWindow), pcm.end());

    std::vector<CaptionSegment> segs;
    if (!transcribePcm16k(trimmed, opts, stage(0.15, 0.85), segs, error))
        return false;

    // Refina o início de cada legenda para onde a fala realmente começa.
    for (auto &s : segs) {
        s.start += leadUs;
        s.end += leadUs;
        size_t w = static_cast<size_t>(s.start * kWhisperRate / kSecond / kVadWindow);
        const size_t wEnd = std::min(rms.size(), static_cast<size_t>(s.end * kWhisperRate / kSecond / kVadWindow));
        while (w < wEnd && rms[w] < threshold)
            ++w;
        if (w < wEnd)
            s.start = std::max(s.start, windowToMicros(w > 2 ? w - 2 : 0)); // 40 ms antes do ataque
        s.start += start;
        s.end += start;
    }
    out = std::move(segs);
    return true;
}

} // namespace clv::engine
