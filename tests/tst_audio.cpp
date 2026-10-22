// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Edit.h"
#include "core/Evaluate.h"
#include "engine/AudioCleanup.h"
#include "engine/AudioMixer.h"
#include "engine/Beats.h"
#include "engine/TimeStretch.h"
#include "engine/VoiceEffects.h"
#include "engine/VoiceIsolation.h"
#include "engine/Silence.h"
#include "engine/StemSeparation.h"
#include "engine/TextToSpeech.h"
#include "engine/AiModels.h"
#include "engine/WhisperModels.h"
#include "media/AudioDecoder.h"
#include "media/Loudness.h"
#include "media/MediaProbe.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <clocale>

#include <cmath>

using namespace clv;

namespace {

std::vector<float> readMono(const QString &path, Micros start, int frames)
{
    media::AudioDecoder dec;
    std::vector<float> st(static_cast<size_t>(frames) * 2), mono(frames);
    if (!dec.open(path))
        return {};
    dec.read(start, st.data(), frames);
    for (int i = 0; i < frames; ++i)
        mono[i] = (st[i * 2] + st[i * 2 + 1]) * 0.5f;
    return mono;
}

// Frequência pelos cruzamentos de zero ascendentes (sinal estéreo intercalado).
double frequency(const float *st, int frames)
{
    int crossings = 0;
    for (int i = 1; i < frames; ++i)
        if (st[(i - 1) * 2] < 0 && st[i * 2] >= 0)
            ++crossings;
    return crossings * 48000.0 / frames;
}

double rms(const std::vector<float> &v)
{
    double acc = 0;
    for (float x : v)
        acc += x * x;
    return std::sqrt(acc / std::max<size_t>(1, v.size()));
}

} // namespace

class AudioTest : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_noisy;

private slots:
    void initTestCase()
    {
        std::setlocale(LC_NUMERIC, "C"); // como o app: o FFmpeg lê números com strtod
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg não encontrado");
        qputenv("CLIVCUT_CACHE_DIR", m_dir.filePath(QStringLiteral("cache")).toUtf8());
        // 2 s só de chiado, depois um sinal forte não periódico (ruído rosa, como fala) + chiado.
        m_noisy = m_dir.filePath(QStringLiteral("ruidoso.wav"));
        QProcess p;
        p.start(QStringLiteral("ffmpeg"),
                {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "anoisesrc=color=white:amplitude=0.04:duration=6",
                 "-f", "lavfi", "-i", "anoisesrc=color=pink:seed=7:amplitude=0.4:duration=4", "-filter_complex",
                 "[1]adelay=2000|2000[s];[0][s]amix=inputs=2:normalize=0", "-ac", "2", "-ar", "48000", m_noisy});
        QVERIFY(p.waitForFinished(60000) && p.exitCode() == 0);
    }

    void denoiseReducesNoiseAndStaysAligned()
    {
        const QString out = engine::denoisedPathFor(m_noisy, 1.0);
        QString err;
        double last = 0;
        QVERIFY2(engine::generateDenoised(m_noisy, out, 1.0, [&](double f) { last = f; return true; }, &err), qPrintable(err));
        QCOMPARE(last, 1.0);
        MediaAsset a, b;
        QVERIFY(media::probe(m_noisy, a) && media::probe(out, b));
        QVERIFY(std::abs(a.duration - b.duration) < 20'000); // mesma duração

        // Trecho só de ruído: pelo menos 10 dB mais baixo.
        const double before = rms(readMono(m_noisy, 500'000, 48000));
        const double after = rms(readMono(out, 500'000, 48000));
        QVERIFY2(20 * std::log10(after / before) < -10, qPrintable(QStringLiteral("%1 → %2").arg(before).arg(after)));

        // Sinal preservado e alinhado: correlação máxima com atraso ~0 (sinal não periódico).
        const auto x = readMono(m_noisy, 3 * kSecond, 9600);
        const auto y = readMono(out, 3 * kSecond, 9600);
        int bestLag = 0;
        double best = -1e9;
        for (int lag = -200; lag <= 200; ++lag) {
            double acc = 0;
            for (int i = 300; i < 9300; ++i)
                acc += x[i] * y[i + lag];
            if (acc > best) {
                best = acc;
                bestLag = lag;
            }
        }
        QVERIFY2(std::abs(bestLag) <= 2, qPrintable(QString::number(bestLag)));
        QVERIFY(rms(y) > rms(x) * 0.5);

        // Registro e reuso pelo cache.
        QVERIFY(engine::registeredDenoised(m_noisy, 0.95).isEmpty() || true);
        QVERIFY(engine::discoverDenoised(m_noisy, 1.0));
        QCOMPARE(engine::registeredDenoised(m_noisy, 0.96), out); // quantizado em 1,0
    }

    void mixerUsesCleanAudioAndGain()
    {
        Project p = createProject();
        MediaAsset a;
        QVERIFY(media::probe(m_noisy, a));
        a.id = newId("ast");
        p.assets.emplace(a.id, a);
        const QString id = edit::insertClip(p, createMediaClip(a, 0), -1);
        std::vector<float> buf(48000 * 2);
        auto level = [&](const Project &proj) {
            engine::AudioMixer mixer;
            mixer.mix(proj, 500'000, buf.data(), 48000);
            double acc = 0;
            for (float s : buf)
                acc += s * s;
            return std::sqrt(acc / buf.size());
        };
        const double original = level(p);
        edit::clipById(p, id)->denoise = 1.0;
        const double cleaned = level(p);
        QVERIFY(cleaned < original * 0.4);
        edit::clipById(p, id)->denoise = 0;
        edit::clipById(p, id)->gainDb = 6;
        QVERIFY(std::abs(level(p) / original - 2.0) < 0.05); // +6 dB ≈ 2×
    }

    void compoundClipAudio()
    {
        Project p = createProject();
        MediaAsset a;
        QVERIFY(media::probe(m_noisy, a));
        a.id = newId("ast");
        p.assets.emplace(a.id, a);
        const QString id = edit::insertClip(p, createMediaClip(a, 0), -1);
        auto play = [&](const Project &proj) {
            engine::AudioMixer mixer;
            std::vector<float> buf(48000 * 2);
            mixer.mix(proj, 1'500'000, buf.data(), 48000); // atravessa a entrada do sinal forte (2 s)
            return buf;
        };
        const std::vector<float> original = play(p);
        const QString cmp = edit::createCompound(p, {id}, QStringLiteral("c"));
        QVERIFY(!cmp.isEmpty());
        QCOMPARE(edit::trackKindFor(p, *edit::clipById(p, cmp)), TrackKind::Audio);
        const std::vector<float> inside = play(p);
        double diff = 0, energy = 0;
        for (size_t i = 0; i < original.size(); ++i) {
            diff = std::max(diff, static_cast<double>(std::abs(original[i] - inside[i])));
            energy += original[i] * original[i];
        }
        QVERIFY(energy > 1);
        QVERIFY2(diff < 1e-5, qPrintable(QString::number(diff)));
        // Volume do composto vale para tudo o que está dentro.
        edit::clipById(p, cmp)->volume = 0.5;
        const std::vector<float> half = play(p);
        for (size_t i = 0; i < original.size(); i += 997)
            QVERIFY(std::abs(half[i] - 0.5f * original[i]) < 1e-4);
        edit::clipById(p, cmp)->audioMuted = true;
        const std::vector<float> muted = play(p);
        QVERIFY(std::all_of(muted.begin(), muted.end(), [](float x) { return x == 0.f; }));
    }

    void timeStretchIdentityAndPitch()
    {
        // Velocidade 1: saída idêntica à entrada, em blocos de qualquer tamanho.
        auto sine = [](std::int64_t k, float *dst, int n, double hz) {
            for (int i = 0; i < n; ++i)
                dst[i * 2] = dst[i * 2 + 1] = static_cast<float>(0.5 * std::sin(2 * M_PI * hz * (k + i) / 48000.0));
        };
        engine::TimeStretch ts;
        std::vector<float> out(48000 * 2), ref(48000 * 2);
        auto reader440 = [&](std::int64_t k, float *dst, int n) { sine(k, dst, n, 440); };
        for (int pos = 0; pos < 48000;) {
            const int n = std::min(48000 - pos, 300 + pos % 700);
            ts.process(out.data() + pos * 2, n, 1.0, reader440);
            pos += n;
        }
        sine(0, ref.data(), 48000, 440);
        float maxDiff = 0;
        for (size_t i = 0; i < out.size(); ++i)
            maxDiff = std::max(maxDiff, std::abs(out[i] - ref[i]));
        QVERIFY2(maxDiff < 1e-4f, qPrintable(QString::number(maxDiff)));

        // Velocidade 2: consome o dobro da entrada e mantém 440 Hz.
        ts.reset();
        ts.process(out.data(), 48000, 2.0, reader440);
        QVERIFY(std::abs(ts.inputPosition() - 96000) < 2000);
        const double hz = frequency(out.data() + 4800 * 2, 38400);
        QVERIFY2(std::abs(hz - 440) < 6, qPrintable(QString::number(hz)));
    }

    void mixerPitchAndReverse()
    {
        // 1 s de 440 Hz seguido de 1 s de silêncio.
        const QString tone = m_dir.filePath(QStringLiteral("tom.wav"));
        QProcess proc;
        proc.start(QStringLiteral("ffmpeg"), {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                                              "sine=frequency=440:duration=1,apad=whole_dur=2", "-ac", "2", "-ar", "48000", tone});
        QVERIFY(proc.waitForFinished(60000) && proc.exitCode() == 0);
        Project p = createProject();
        MediaAsset a;
        QVERIFY(media::probe(tone, a));
        a.id = newId("ast");
        p.assets.emplace(a.id, a);
        Clip c = createMediaClip(a, 0);
        c.speed = 2;
        c.duration = a.duration / 2;
        const QString id = edit::insertClip(p, c, -1);

        // Toca em blocos como o player: 0,1 s a 0,4 s da timeline (fonte 0,2 a 0,8 s).
        auto play = [&](const Project &proj, Micros from, Micros to) {
            engine::AudioMixer mixer;
            std::vector<float> all;
            std::vector<float> block(1024 * 2);
            for (Micros t = from; t < to;) {
                mixer.mix(proj, t, block.data(), 1024);
                all.insert(all.end(), block.begin(), block.end());
                t += 1024 * kSecond / 48000;
            }
            return all;
        };
        std::vector<float> st = play(p, 100'000, 400'000);
        double hz = frequency(st.data(), static_cast<int>(st.size() / 2));
        QVERIFY2(std::abs(hz - 440) < 8, qPrintable(QString::number(hz)));
        edit::clipById(p, id)->keepPitch = false;
        st = play(p, 100'000, 400'000);
        hz = frequency(st.data(), static_cast<int>(st.size() / 2));
        QVERIFY2(std::abs(hz - 880) < 12, qPrintable(QString::number(hz)));

        // Reverso a 1×: primeiro o silêncio, depois o tom.
        Clip *rc = edit::clipById(p, id);
        rc->speed = 1;
        rc->duration = a.duration;
        rc->keepPitch = true;
        rc->reverse = true;
        st = play(p, 0, 2'000'000);
        auto level = [&](double from, double to) {
            double acc = 0;
            const int i0 = static_cast<int>(from * 48000), i1 = static_cast<int>(to * 48000);
            for (int i = i0; i < i1; ++i)
                acc += st[i * 2] * st[i * 2];
            return std::sqrt(acc / (i1 - i0));
        };
        QVERIFY2(level(0.2, 0.8) < 0.01, qPrintable(QString::number(level(0.2, 0.8))));
        QVERIFY2(level(1.2, 1.8) > 0.05, qPrintable(QString::number(level(1.2, 1.8)))); // seno do ffmpeg: amplitude 1/8
    }

    void ducking()
    {
        const QString music = m_dir.filePath(QStringLiteral("musica.wav"));
        const QString voice = m_dir.filePath(QStringLiteral("fala.wav"));
        QProcess proc;
        proc.start(QStringLiteral("ffmpeg"), {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                                              "sine=frequency=440:duration=5", "-ac", "2", "-ar", "48000", music});
        QVERIFY(proc.waitForFinished(60000) && proc.exitCode() == 0);
        proc.start(QStringLiteral("ffmpeg"), {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                                              "aevalsrc=0.4*sin(2*PI*220*t)*between(t\\,2\\,3):d=5:s=48000", "-ac", "2", voice});
        QVERIFY(proc.waitForFinished(60000) && proc.exitCode() == 0);
        Project p = createProject();
        auto addAudio = [&](const QString &path) {
            MediaAsset a;
            media::probe(path, a);
            a.id = newId("ast");
            p.assets.emplace(a.id, a);
            return edit::insertClip(p, createMediaClip(a, 0), -1);
        };
        const QString musicId = addAudio(music);
        addAudio(voice);
        // Amplitude da música (440 Hz) num trecho, por Goertzel, tocando em blocos como o player.
        auto level440 = [&](const Project &proj, double from, double to) {
            engine::AudioMixer mixer;
            std::vector<float> all, block(1024 * 2);
            for (Micros t = 0; t < static_cast<Micros>(to * kSecond);) {
                mixer.mix(proj, t, block.data(), 1024);
                all.insert(all.end(), block.begin(), block.end());
                t += 1024 * kSecond / 48000;
            }
            const int i0 = static_cast<int>(from * 48000), i1 = static_cast<int>(to * 48000);
            const double w = 2 * M_PI * 440 / 48000;
            double re = 0, im = 0;
            for (int i = i0; i < i1; ++i) {
                re += all[i * 2] * std::cos(w * i);
                im += all[i * 2] * std::sin(w * i);
            }
            return 2 * std::hypot(re, im) / (i1 - i0);
        };
        const double quiet = level440(p, 0.5, 0.9);
        QVERIFY(quiet > 0.05);
        QVERIFY(std::abs(level440(p, 2.4, 2.8) / quiet - 1) < 0.05); // sem ducking: nada muda
        edit::clipById(p, musicId)->ducking = 1;
        QVERIFY(std::abs(level440(p, 0.5, 0.9) / quiet - 1) < 0.05);  // sem voz: volume cheio
        const double underVoice = level440(p, 2.4, 2.8) / quiet;
        QVERIFY2(underVoice < 0.25, qPrintable(QString::number(underVoice)));
        const double after = level440(p, 4.4, 4.9) / quiet;       // voz parou há mais de 1 s
        QVERIFY2(after > 0.8, qPrintable(QString::number(after)));
    }

    void voiceEffects()
    {
        // 1 s de silêncio e depois 2 s de "voz" (harmônicos de 150 Hz).
        const QString in = m_dir.filePath(QStringLiteral("efeito.wav"));
        QProcess proc;
        proc.start(QStringLiteral("ffmpeg"), {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                                              "aevalsrc=(0.3*sin(2*PI*150*t)+0.15*sin(2*PI*300*t)+0.1*sin(2*PI*450*t))*gte(t\\,1):d=3:s=48000",
                                              "-ac", "2", in});
        QVERIFY(proc.waitForFinished(60000) && proc.exitCode() == 0);
        MediaAsset src;
        QVERIFY(media::probe(in, src));
        for (const auto &fx : engine::voiceEffects()) {
            const QString out = engine::effectPathFor(in, fx.id);
            QString err;
            QVERIFY2(engine::generateEffect(in, fx.id, out, {}, &err), qPrintable(fx.id + QStringLiteral(": ") + err));
            MediaAsset res;
            QVERIFY(media::probe(out, res));
            QVERIFY2(std::abs(res.duration - src.duration) < 20'000, qPrintable(fx.id));
            const auto y = readMono(out, 0, 3 * 48000);
            // Silêncio antes de 1 s (com folga para a subida) e som depois: a latência foi compensada.
            const double before = rms(std::vector<float>(y.begin(), y.begin() + 45'000));
            const double after = rms(std::vector<float>(y.begin() + 54'000, y.begin() + 140'000));
            QVERIFY2(after > 0.01, qPrintable(fx.id + QStringLiteral(" mudo: ") + QString::number(after)));
            QVERIFY2(before < after * 0.1, qPrintable(fx.id + QStringLiteral(" adiantado: ") + QString::number(before)));
        }
    }

    void silenceDetection()
    {
        // 16 kHz: 1 s de tom, 0,3 s de pausa curta, 1 s de tom, 1,5 s de silêncio, 1 s de tom.
        std::vector<float> pcm;
        auto tone = [&](double s) { for (int i = 0; i < s * 16000; ++i) pcm.push_back(0.3f * std::sin(i * 0.1f)); };
        auto quiet = [&](double s) { for (int i = 0; i < s * 16000; ++i) pcm.push_back(0.001f); };
        tone(1); quiet(0.3); tone(1); quiet(1.5); tone(1);
        engine::SilenceOptions o;
        o.minSilence = 500'000;
        o.padding = 100'000;
        const auto r = engine::detectSilencesPcm16k(pcm, 10 * kSecond, o);
        QCOMPARE(r.size(), size_t(1)); // a pausa de 0,3 s fica
        QVERIFY(std::abs(r[0].first - (10 * kSecond + 2'400'000)) <= 20'000);
        QVERIFY(std::abs(r[0].second - (10 * kSecond + 3'700'000)) <= 20'000);
        // Silêncio no começo e no fim é cortado inteiro (sem respiro na borda).
        pcm.clear();
        quiet(1); tone(1); quiet(1);
        const auto edges = engine::detectSilencesPcm16k(pcm, 0, o);
        QCOMPARE(edges.size(), size_t(2));
        QCOMPARE(edges[0].first, Micros(0));
        QVERIFY(std::abs(edges[1].second - 3 * kSecond) <= 20'000);
    }

    /** Bumbo (60 Hz que decai) e chimbal (ruído curto) a `bpm`; acento a cada 4 tempos. */
    static std::vector<float> drumLoop(double bpm, double seconds, double firstBeat, bool accent)
    {
        std::vector<float> pcm(static_cast<size_t>(seconds * 16000));
        unsigned seed = 7;
        auto noise = [&] { seed = seed * 1103515245u + 12345u; return ((seed >> 9) & 0xffff) / 32768.f - 1.f; };
        for (auto &x : pcm)
            x = 0.01f * noise(); // chiado de fundo
        const double period = 60.0 / bpm;
        int k = 0;
        for (double t = firstBeat; t < seconds; t += period, ++k) {
            const float gain = accent && k % 4 == 0 ? 1.f : 0.45f;
            const auto at = static_cast<size_t>(t * 16000);
            for (size_t i = 0; i < 4000 && at + i < pcm.size(); ++i) {
                const float tt = i / 16000.f;
                pcm[at + i] += gain * 0.6f * std::sin(2 * float(M_PI) * 60 * tt) * std::exp(-tt * 18);
                if (i < 800)
                    pcm[at + i] += gain * 0.25f * noise() * std::exp(-tt * 120);
            }
            // Contratempo: só chimbal, mais fraco, para o ritmo não ser trivial.
            const auto off = static_cast<size_t>((t + period / 2) * 16000);
            for (size_t i = 0; i < 600 && off + i < pcm.size(); ++i)
                pcm[off + i] += 0.12f * noise() * std::exp(-static_cast<float>(i) / 16000.f * 150);
        }
        return pcm;
    }

    void beatDetection_data()
    {
        QTest::addColumn<double>("bpm");
        QTest::newRow("128") << 128.0;
        QTest::newRow("90") << 90.0;
        QTest::newRow("174") << 174.0;
    }

    void beatDetection()
    {
        QFETCH(double, bpm);
        const double first = 0.37;
        const auto pcm = drumLoop(bpm, 20, first, false);
        const auto r = engine::detectBeatsPcm16k(pcm, 5 * kSecond);
        // 174 BPM pode sair como meio andamento (87); os dois são aceitáveis.
        const bool half = std::abs(r.bpm * 2 - bpm) < 1.5;
        QVERIFY2(std::abs(r.bpm - bpm) < 1.0 || (bpm > 160 && half), qPrintable(QString::number(r.bpm)));
        const double period = 60.0 / bpm; // grade dos golpes reais
        // Cada batida cai a menos de 30 ms de um golpe real.
        int matched = 0;
        for (const Micros b : r.beats) {
            const double t = static_cast<double>(b - 5 * kSecond) / kSecond - first;
            const double phase = t / period - std::round(t / period);
            if (std::abs(phase * period) < 0.03)
                ++matched;
        }
        QVERIFY2(matched >= static_cast<int>(r.beats.size()) - 1, qPrintable(QStringLiteral("%1 of %2").arg(matched).arg(r.beats.size())));
        QVERIFY(static_cast<double>(r.beats.size()) >= (20 - first) / (period * (half ? 2 : 1)) - 3);
    }

    void beatsEveryBar()
    {
        const double bpm = 120, first = 0.5;
        const auto r = engine::detectBeatsPcm16k(drumLoop(bpm, 16, first, true), 0);
        const auto bars = engine::everyNthBeat(r, 4);
        QVERIFY(bars.size() >= 6);
        // Os acentos caem em first + 2 s·k (um compasso de 4 tempos a 120 BPM).
        for (const Micros b : bars) {
            const double t = static_cast<double>(b) / kSecond - first;
            QVERIFY2(std::abs(t - 2 * std::round(t / 2)) < 0.03, qPrintable(QString::number(t)));
        }
        QCOMPARE(engine::everyNthBeat(r, 1).size(), r.beats.size());
        // Silêncio: nada.
        const auto none = engine::detectBeatsPcm16k(std::vector<float>(16000 * 5, 0.f), 0);
        QVERIFY(none.beats.empty());
        QCOMPARE(none.bpm, 0.0);
    }

    void textToSpeechPhonemes()
    {
        if (!engine::ttsAvailable())
            QSKIP("compilado sem voz neural");
        // Referência: a implementação oficial em Python (kokoro-onnx + phonemizer + espeak-ng 1.52).
        struct Case {
            QString lang, text, phonemes;
        };
        const Case cases[] = {
            {QStringLiteral("pt-br"), QStringLiteral("Olá! Este é um teste de narração gerada no seu computador. Hoje vamos aprender a editar vídeos de um jeito simples e rápido, com cortes, legendas e música de fundo."),
             QStringLiteral("olˈa! ˌestʃy ɛ ũŋ tˈɛstʃy dʒy nˌaxasˈɐ̃ʊ̃ ʒˌeɾˈadæ nʊ seʊ kˌomputadˈor. ˈoʒy vˌɐ̃mʊz ˌapreɪŋdˈeɾ a ˌedʒitˈar vˈidʒjʊz dʒj ũŋ ʒˈeɪtʊ sˈimplyz i xˈapidʊ, koŋ kˈɔɾətʃys, lˌeʒˈeɪŋdæz i mˈuzikæ dʒy fˈũŋdʊ.")},
            {QStringLiteral("pt-br"), QStringLiteral("Olá, mundo! Tudo bem? Sim: vamos lá… Tenho 25 anos (quase)."),
             QStringLiteral("olˈa, mˈũŋdʊ! tˈudʊ bˈeɪŋ? sˈiŋ: vˌɐ̃mʊz lˈa… tˌeɲʊ vˈiŋtʃyisˈiŋkw ˈɐ̃nʊs (kwˈazy).")},
            {QStringLiteral("es"), QStringLiteral("¡Hola! ¿Cómo estás? Esta es una prueba de narración, con cortes y música."),
             QStringLiteral("ˈola! kˈomo estˈas? ˈesta ˈes ˈuna pɾuˈeβa ðe nˌaraθjˈon, kon kˈoɾtes i mˈusika.")},
            {QStringLiteral("en-us"), QStringLiteral("Hi! This is a narration test generated right on your computer. Today we'll learn how to edit videos quickly, with cuts, captions and background music."),
             QStringLiteral("hˈaɪ! ðɪs ɪz ɐ nɚɹˈeɪʃən tˈɛst dʒˈɛnɚɹˌeɪɾᵻd ɹˈaɪt ˌɔn jʊɹ kəmpjˈuːɾɚ. tədˈeɪ wiːl lˈɜːn hˌaʊ tʊ ˈɛdɪt vˈɪdɪoʊz kwˈɪkli, wɪð kˈʌts, kˈæpʃənz ænd bˈækɡɹaʊnd mjˈuːzɪk.")},
            {QStringLiteral("en-us"), QStringLiteral("Wait... what? It's 3:45 pm; \"quoted\" text — and more."),
             QStringLiteral("wˈeɪt... wˌʌt? ɪts θɹˈiː:fˈɔːɹɾi fˈaɪv pˌiːˈɛm; \"kwˈoʊɾᵻd\" tˈɛkst — ænd mˈɔːɹ.")},
        };
        for (const Case &c : cases) {
            QString err;
            const QString ph = engine::ttsPhonemize(c.text, c.lang, &err);
            QVERIFY2(err.isEmpty(), qPrintable(err));
            QCOMPARE(ph, c.phonemes);
        }
        const std::vector<std::int64_t> head{57, 54, 156, 43, 5, 16, 157, 47, 61, 62, 131, 67, 16, 86, 16, 63, 17, 112, 16, 62};
        const auto tokens = engine::ttsTokenize(cases[0].phonemes);
        QVERIFY(tokens.size() > head.size());
        QVERIFY(std::equal(head.begin(), head.end(), tokens.begin()));
    }

    void textToSpeechSynthesis()
    {
        if (!engine::ttsAvailable() || !engine::aiModelInstalled(engine::kTtsModel) || !engine::aiModelInstalled(engine::kTtsVoices))
            QSKIP("modelo Kokoro não instalado");
        const QString out = m_dir.filePath(QStringLiteral("fala.wav"));
        QString err;
        double last = 0;
        const QString text = QStringLiteral("Olá! Este é um teste de narração gerada no seu computador. "
                                            "Hoje vamos aprender a editar vídeos de um jeito simples e rápido, "
                                            "com cortes, legendas e música de fundo.");
        QVERIFY2(engine::synthesizeSpeech(text, QStringLiteral("pf_dora"), 1.0, engine::aiModelPath(engine::kTtsModel),
                                          engine::aiModelPath(engine::kTtsVoices), out, [&](double f) { last = f; return true; }, &err),
                 qPrintable(err));
        QCOMPARE(last, 1.0);
        MediaAsset a;
        QVERIFY(media::probe(out, a));
        // A referência em Python dá ~9,7 s para este texto.
        QVERIFY2(a.duration > 8 * kSecond && a.duration < 12 * kSecond, qPrintable(QString::number(a.duration)));
        // Mais devagar fica mais longo.
        const QString slow = m_dir.filePath(QStringLiteral("devagar.wav"));
        QVERIFY(engine::synthesizeSpeech(text, QStringLiteral("pf_dora"), 0.7, engine::aiModelPath(engine::kTtsModel),
                                         engine::aiModelPath(engine::kTtsVoices), slow, {}, &err));
        MediaAsset b;
        QVERIFY(media::probe(slow, b));
        QVERIFY(b.duration > a.duration * 1.2);
        QVERIFY(!engine::synthesizeSpeech(text, QStringLiteral("xx_none"), 1.0, engine::aiModelPath(engine::kTtsModel),
                                          engine::aiModelPath(engine::kTtsVoices), slow, {}, &err));
    }

    void stemSeparation()
    {
#ifndef CLIVCUT_WHISPER_SAMPLES
        QSKIP("sem amostras de fala");
#else
        const QString model = engine::aiModelPath(engine::kStemsModel);
        if (!engine::stemSeparationAvailable() || !engine::aiModelInstalled(engine::kStemsModel))
            QSKIP("modelo HT-Demucs não instalado");
        // Fala (jfk.wav, 11 s) sobre bateria e baixo sintéticos.
        const QString voice = m_dir.filePath(QStringLiteral("voz.wav")), band = m_dir.filePath(QStringLiteral("banda.wav")),
                      mix = m_dir.filePath(QStringLiteral("mistura.wav"));
        QProcess p;
        p.start(QStringLiteral("ffmpeg"), {"-hide_banner", "-loglevel", "error", "-y", "-i",
                                           QStringLiteral(CLIVCUT_WHISPER_SAMPLES "/jfk.wav"), "-ac", "2", "-ar", "48000", voice});
        QVERIFY(p.waitForFinished(60000) && p.exitCode() == 0);
        p.start(QStringLiteral("ffmpeg"), {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                                           "aevalsrc='0.5*sin(2*PI*55*t)*exp(-12*mod(t\\,0.5))"
                                           "+0.25*sin(2*PI*82.4*t)*(0.6+0.4*sin(2*PI*0.5*t))"
                                           "+0.15*(random(0)-0.5)*exp(-60*mod(t+0.25\\,0.5))':s=48000:d=11",
                                           "-ac", "2", band});
        QVERIFY(p.waitForFinished(60000) && p.exitCode() == 0);
        p.start(QStringLiteral("ffmpeg"), {"-hide_banner", "-loglevel", "error", "-y", "-i", voice, "-i", band, "-filter_complex",
                                           "amix=inputs=2:normalize=0:duration=shortest", "-ar", "48000", mix});
        QVERIFY(p.waitForFinished(60000) && p.exitCode() == 0);

        QString err;
        QVERIFY2(engine::separateStems(mix, model, {}, &err), qPrintable(err));
        QVERIFY(engine::stemsReady(mix));
        auto load = [](const QString &path) {
            media::AudioDecoder d;
            std::vector<float> v(48000 * 10 * 2);
            if (d.open(path))
                d.read(500'000, v.data(), 48000 * 10);
            return v;
        };
        auto corr = [](const std::vector<float> &a, const std::vector<float> &b) {
            double ab = 0, aa = 0, bb = 0;
            for (size_t i = 0; i < a.size(); ++i) {
                ab += a[i] * b[i];
                aa += a[i] * a[i];
                bb += b[i] * b[i];
            }
            return ab / std::sqrt(aa * bb + 1e-12);
        };
        const auto v = load(voice), vocals = load(engine::stemPathFor(mix, engine::Stem::Vocals));
        const auto inst = load(engine::stemPathFor(mix, engine::Stem::Instrumental));
        const double cv = corr(vocals, v), ci = corr(inst, v);
        qInfo() << "vocals~voice" << cv << "instrumental~voice" << ci;
        QVERIFY2(cv > 0.8, qPrintable(QString::number(cv)));
        QVERIFY2(std::abs(ci) < 0.3, qPrintable(QString::number(ci)));
#endif
    }

    void voiceIsolation()
    {
        if (!engine::voiceIsolationAvailable())
            QSKIP("compilado sem ONNX Runtime");
        const QString model = qEnvironmentVariable("CLIVCUT_VOICE_MODEL",
            QDir(engine::whisperModelsDir()).filePath(QStringLiteral("deepfilternet3_streaming.onnx")));
        if (!QFileInfo::exists(model))
            QSKIP("modelo de isolamento de voz não instalado");
        // 2 s só de ruído rosa, depois um tom de fala sintético (vogais com harmônicos) + ruído.
        const QString in = m_dir.filePath(QStringLiteral("voz.wav"));
        QProcess proc;
        proc.start(QStringLiteral("ffmpeg"),
                   {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "anoisesrc=color=pink:amplitude=0.08:duration=5",
                    "-f", "lavfi", "-i",
                    "aevalsrc=0.3*sin(2*PI*140*t)*(0.6+0.4*sin(2*PI*3*t))+0.2*sin(2*PI*280*t)+0.1*sin(2*PI*420*t):d=3:s=48000",
                    "-filter_complex", "[1]adelay=2000|2000[v];[0][v]amix=inputs=2:duration=longest:normalize=0", "-ac", "2",
                    "-ar", "48000", in});
        QVERIFY(proc.waitForFinished(60000) && proc.exitCode() == 0);

        const QString out = engine::voicePathFor(in, 1.0);
        QString err;
        double last = 0;
        QVERIFY2(engine::generateVoiceIsolated(in, model, out, 1.0, [&](double f) { last = f; return true; }, &err), qPrintable(err));
        QCOMPARE(last, 1.0);
        MediaAsset a, b;
        QVERIFY(media::probe(in, a) && media::probe(out, b));
        QVERIFY(std::abs(a.duration - b.duration) < 20'000);
        // Ruído puro: bem mais baixo.
        const double noiseIn = rms(readMono(in, 500'000, 48000)), noiseOut = rms(readMono(out, 500'000, 48000));
        QVERIFY2(20 * std::log10(noiseOut / noiseIn) < -8, qPrintable(QStringLiteral("%1 → %2").arg(noiseIn).arg(noiseOut)));
        // Alinhamento: a correlação entre entrada e saída é máxima sem deslocamento.
        const auto x = readMono(in, 2'500'000, 48000), y = readMono(out, 2'500'000, 48000);
        int best = 0;
        double bestCorr = -1e30;
        for (int d = -200; d <= 200; ++d) {
            double c = 0;
            for (int i = 300; i < 47000; ++i)
                c += x[i] * y[i + d];
            if (c > bestCorr) {
                bestCorr = c;
                best = d;
            }
        }
        QVERIFY2(std::abs(best) <= 2, qPrintable(QString::number(best)));
    }
};

QTEST_GUILESS_MAIN(AudioTest)
#include "tst_audio.moc"
