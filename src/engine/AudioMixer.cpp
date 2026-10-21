// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioMixer.h"

#include "core/Evaluate.h"
#include "core/SpeedCurve.h"
#include "AudioCleanup.h"
#include "VoiceEffects.h"
#include "VoiceIsolation.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace clv::engine {

using media::kChannels;
using media::kSampleRate;

media::AudioDecoder *AudioMixer::decoderFor(const Clip &c, const MediaAsset &a)
{
    // Com redução de ruído, usa a versão limpa quando já estiver pronta (alinhada à original).
    QString path = a.path;
    // Voz isolada (IA) tem prioridade: já remove o ruído também.
    const QString voice = c.voiceIsolation > 0 ? registeredVoice(a.path, c.voiceIsolation) : QString();
    if (!voice.isEmpty())
        path = voice;
    else if (c.denoise > 0) {
        const QString clean = registeredDenoised(a.path, c.denoise);
        if (!clean.isEmpty())
            path = clean;
    }
    // Efeito de voz sobre a melhor versão disponível (isolada, limpa ou original).
    if (!c.voiceEffect.isEmpty()) {
        const QString fx = registeredEffect(path, c.voiceEffect);
        if (!fx.isEmpty())
            path = fx;
    }
    Entry &e = m_decoders[c.id];
    if (e.path != path) {
        e = Entry{};
        e.path = path;
    }
    if (e.failed)
        return nullptr;
    if (!e.dec) {
        e.dec = std::make_unique<media::AudioDecoder>();
        if (!e.dec->open(path)) {
            e.failed = true;
            e.dec.reset();
            return nullptr;
        }
    }
    return e.dec.get();
}

namespace {
std::int64_t toFrames(Micros t) { return static_cast<std::int64_t>(std::llround(static_cast<double>(t) * kSampleRate / kSecond)); }
Micros toMicros(std::int64_t f) { return static_cast<Micros>(std::llround(static_cast<double>(f) * kSecond / kSampleRate)); }
} // namespace

void AudioMixer::readStream(Entry &e, std::int64_t k, float *dst, int frames)
{
    if (e.dir > 0) {
        e.dec->read(e.origin + toMicros(k), dst, frames);
        return;
    }
    // Reverso: a amostra k do fluxo é a amostra (origem - 1 - k) da mídia. Lê blocos grandes
    // para trás (uma busca a cada meio segundo) e copia invertido.
    const std::int64_t originFrame = toFrames(e.origin);
    for (int i = 0; i < frames;) {
        const std::int64_t src = originFrame - 1 - (k + i);
        if (src < 0) {
            std::memset(dst + i * kChannels, 0, sizeof(float) * (frames - i) * kChannels);
            return;
        }
        const std::int64_t have = static_cast<std::int64_t>(e.revBuf.size() / kChannels);
        if (src < e.revStart || src >= e.revStart + have) {
            const int chunk = std::max(frames, kSampleRate / 2);
            const std::int64_t start = std::max<std::int64_t>(0, src + 1 - chunk);
            const int len = static_cast<int>(src + 1 - start);
            e.revBuf.resize(static_cast<size_t>(len) * kChannels);
            e.dec->read(toMicros(start), e.revBuf.data(), len);
            e.revStart = start;
        }
        // Copia até o começo do bloco em cache, invertendo.
        const int n = static_cast<int>(std::min<std::int64_t>(frames - i, src - e.revStart + 1));
        for (int j = 0; j < n; ++j) {
            const float *s = e.revBuf.data() + (src - j - e.revStart) * kChannels;
            dst[(i + j) * kChannels] = s[0];
            dst[(i + j) * kChannels + 1] = s[1];
        }
        i += n;
    }
}

void AudioMixer::mixCompound(const Project &p, const Clip &c, Micros t, int frames, float *dst, int depth)
{
    const Sequence *seq = depth < 8 ? p.compound(c.compoundId) : nullptr;
    if (!seq)
        return;
    const Micros span = static_cast<Micros>(static_cast<double>(frames) * kSecond / kSampleRate);
    const Micros segStart = std::max(t, c.start);
    const Micros segEnd = std::min(t + span, c.end());
    const int off = static_cast<int>(std::llround(static_cast<double>(segStart - t) * kSampleRate / kSecond));
    const int n = std::min(frames - off,
                           static_cast<int>(std::llround(static_cast<double>(segEnd - segStart) * kSampleRate / kSecond)));
    if (n <= 0)
        return;
    const Micros rel = segStart - c.start;
    const Micros inner = c.sourceIn + rel; // compostos tocam a 1x
    std::vector<float> buf(static_cast<size_t>(n) * kChannels, 0.f);
    for (const Track &track : seq->tracks) {
        if (track.muted || track.kind == TrackKind::Text)
            continue;
        for (const Clip &ic : track.clips)
            mixClip(p, ic, inner, n, buf.data(), depth + 1);
    }
    const double g0 = audioGainAt(c, rel);
    const double g1 = audioGainAt(c, rel + (segEnd - segStart));
    for (int i = 0; i < n; ++i) {
        const double g = g0 + (g1 - g0) * (static_cast<double>(i) / n);
        dst[(off + i) * 2] += static_cast<float>(buf[static_cast<size_t>(i) * 2] * g);
        dst[(off + i) * 2 + 1] += static_cast<float>(buf[static_cast<size_t>(i) * 2 + 1] * g);
    }
}

void AudioMixer::mixClip(const Project &p, const Clip &c, Micros t, int frames, float *dst, int depth)
{
    const Micros span = static_cast<Micros>(static_cast<double>(frames) * kSecond / kSampleRate);
    const Micros tEnd = t + span;
    if (c.isCompound()) {
        if (!c.audioMuted && c.end() > t && c.start < tEnd)
            mixCompound(p, c, t, frames, dst, depth);
        return;
    }
    if (!c.hasMedia() || c.audioMuted || c.end() <= t || c.start >= tEnd || c.speed <= 0)
        return;
    const MediaAsset *a = p.asset(c.assetId);
    if (!a || !a->hasAudio)
        return;
    media::AudioDecoder *dec = decoderFor(c, *a);
    if (!dec)
        return;

    // Trecho do bloco coberto por este clipe.
    const Micros segStart = std::max(t, c.start);
    const Micros segEnd = std::min(tEnd, c.end());
    const int off = static_cast<int>(std::llround(static_cast<double>(segStart - t) * kSampleRate / kSecond));
    const int n = std::min(frames - off,
                           static_cast<int>(std::llround(static_cast<double>(segEnd - segStart) * kSampleRate / kSecond)));
    if (n <= 0)
        return;

    const Micros rel = segStart - c.start;
    const Micros src = sourceTimeAt(p, c, rel);
    const double speed = speedAt(c, rel); // instantânea (curvas mudam ao longo do clipe)
    const bool varying = c.speed != 1.0 || !c.speedCurve.empty();
    const bool stretched = c.reverse || (c.keepPitch && varying);
    int srcFrames = n;
    if (stretched) {
        // Fluxo contínuo: recomeça em busca, edição ou deriva de mais de 50 ms.
        Entry &e = entryFor(c);
        const std::int64_t outIdx = toFrames(t) + off;
        const int dir = c.reverse ? -1 : 1;
        const Micros expected = e.origin + dir * toMicros(static_cast<std::int64_t>(e.stretch ? e.stretch->inputPosition() : 0));
        if (!e.stretch || e.nextOut < 0 || std::llabs(outIdx - e.nextOut) > 2 || e.dir != dir
            || std::llabs(expected - src) > 60'000 + static_cast<Micros>(speed * 40'000)) {
            if (!e.stretch)
                e.stretch = std::make_unique<TimeStretch>();
            e.stretch->reset();
            e.origin = src;
            e.dir = dir;
            e.revBuf.clear();
        }
        m_tmp.resize(static_cast<size_t>(n) * kChannels);
        e.stretch->process(m_tmp.data(), n, speed,
                           [&](std::int64_t k, float *dst, int count) { readStream(e, k, dst, count); });
        e.nextOut = outIdx + n;
    } else {
        srcFrames = std::max(1, static_cast<int>(std::ceil(n * speed)) + 1);
        m_tmp.resize(static_cast<size_t>(srcFrames) * kChannels);
        dec->read(src, m_tmp.data(), srcFrames);
    }

    const double g0 = audioGainAt(c, rel);
    const double g1 = audioGainAt(c, rel + (segEnd - segStart));
    for (int i = 0; i < n; ++i) {
        const double g = g0 + (g1 - g0) * (static_cast<double>(i) / n);
        float l, r;
        if (stretched || speed == 1.0) {
            l = m_tmp[i * 2];
            r = m_tmp[i * 2 + 1];
        } else {
            const double pos = i * speed;
            const int i0 = std::min(static_cast<int>(pos), srcFrames - 2);
            const float fr = static_cast<float>(pos - i0);
            l = m_tmp[i0 * 2] * (1 - fr) + m_tmp[(i0 + 1) * 2] * fr;
            r = m_tmp[i0 * 2 + 1] * (1 - fr) + m_tmp[(i0 + 1) * 2 + 1] * fr;
        }
        dst[(off + i) * 2] += static_cast<float>(l * g);
        dst[(off + i) * 2 + 1] += static_cast<float>(r * g);
    }
}

void AudioMixer::mix(const Project &p, Micros t, float *out, int frames)
{
    std::memset(out, 0, sizeof(float) * frames * kChannels);
    const Micros span = static_cast<Micros>(static_cast<double>(frames) * kSecond / kSampleRate);
    const Micros tEnd = t + span;

    // 1ª passada: tudo o que não abaixa (voz, efeitos...). 2ª: clipes com ducking, que abaixam
    // conforme a envoltória da 1ª.
    std::vector<const Clip *> ducked;
    for (const Track &track : p.tracks) {
        if (track.muted || track.kind == TrackKind::Text)
            continue;
        for (const Clip &c : track.clips) {
            if (!(c.hasMedia() || c.isCompound()) || c.audioMuted || c.end() <= t || c.start >= tEnd || c.speed <= 0)
                continue;
            if (c.ducking > 0)
                ducked.push_back(&c);
            else
                mixClip(p, c, t, frames, out);
        }
    }

    // Envoltória da voz: segue rápido quando a voz entra e solta devagar (sem "bombear").
    // Contínua entre blocos; um salto no tempo (busca) recomeça do zero.
    const std::int64_t first = toFrames(t);
    if (first != m_duckNext)
        m_duckEnv = 0;
    m_duckNext = first + frames;
    if (!ducked.empty()) {
        constexpr double attack = 1.0 - 0.0002083; // ~100 ms para subir
        constexpr double release = 1.0 - 0.0000417; // ~500 ms para soltar
        m_voice.resize(static_cast<size_t>(frames));
        for (int i = 0; i < frames; ++i) {
            const double x = std::abs(0.5 * (out[i * 2] + out[i * 2 + 1]));
            // Atividade de voz: 0 abaixo de -45 dBFS, 1 acima de -30 dBFS.
            const double db = 20 * std::log10(std::max(x, 1e-6));
            const double active = std::clamp((db + 45) / 15, 0.0, 1.0);
            const double k = active > m_duckEnv ? attack : release;
            m_duckEnv = active + (m_duckEnv - active) * k;
            m_voice[static_cast<size_t>(i)] = static_cast<float>(m_duckEnv);
        }
        m_duckBuf.resize(static_cast<size_t>(frames) * kChannels);
        for (const Clip *c : ducked) {
            std::fill(m_duckBuf.begin(), m_duckBuf.end(), 0.f);
            mixClip(p, *c, t, frames, m_duckBuf.data());
            const float depth = static_cast<float>(std::clamp(c->ducking, 0.0, 1.0)) * 0.85f; // até -16 dB
            for (int i = 0; i < frames; ++i) {
                const float g = 1.f - depth * m_voice[static_cast<size_t>(i)];
                out[i * 2] += m_duckBuf[static_cast<size_t>(i) * 2] * g;
                out[i * 2 + 1] += m_duckBuf[static_cast<size_t>(i) * 2 + 1] * g;
            }
        }
    }

    // Limitador suave para evitar clipping duro na soma.
    for (int i = 0; i < frames * kChannels; ++i) {
        const float x = out[i];
        if (x > 1.f || x < -1.f)
            out[i] = std::tanh(x);
    }
}

void AudioMixer::prune(const QSet<QString> &liveClipIds)
{
    for (auto it = m_decoders.begin(); it != m_decoders.end();) {
        if (!liveClipIds.contains(it->first))
            it = m_decoders.erase(it);
        else
            ++it;
    }
}

} // namespace clv::engine
