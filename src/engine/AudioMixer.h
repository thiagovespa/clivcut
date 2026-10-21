// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/Model.h"
#include "media/AudioDecoder.h"
#include "TimeStretch.h"

#include <unordered_map>
#include <QSet>

#include <memory>
#include <vector>

namespace clv::engine {

/**
 * Mixa todas as fontes de áudio do projeto num intervalo da timeline.
 * Saída: float estéreo intercalado 48 kHz. Aplica volume (com keyframes),
 * fades e velocidade: com "manter tom" (padrão) usa TimeStretch (WSOLA);
 * sem ele, reamostragem simples (o tom acompanha a velocidade). Clipes em
 * reverso tocam o áudio de trás para frente.
 *
 * Não é thread-safe: use uma instância por thread.
 */
class AudioMixer {
public:
    /** Mixa `frames` quadros começando em `t` (µs da timeline) em `out`. */
    void mix(const Project &p, Micros t, float *out, int frames);
    void prune(const QSet<QString> &liveClipIds);
    void clear() { m_decoders.clear(); }

private:
    struct Entry {
        std::unique_ptr<media::AudioDecoder> dec;
        QString path;
        bool failed = false;
        // Fluxo contínuo para velocidade com tom preservado e reverso.
        std::unique_ptr<TimeStretch> stretch;
        std::int64_t nextOut = -1;   ///< próxima amostra da timeline esperada
        Micros origin = 0;           ///< tempo de origem no último reset
        int dir = 1;                 ///< -1 = reverso
        std::vector<float> revBuf;   ///< bloco lido para trás (amostras da origem)
        std::int64_t revStart = 0;   ///< primeira amostra da origem em revBuf
    };
    media::AudioDecoder *decoderFor(const Clip &c, const MediaAsset &a);
    /** Mixa um clipe no bloco [t, t + frames) somando em `dst`. */
    void mixClip(const Project &p, const Clip &c, Micros t, int frames, float *dst, int depth = 0);
    /// Clipe composto: mistura as faixas internas no tempo da sequência, com o volume do composto.
    void mixCompound(const Project &p, const Clip &c, Micros t, int frames, float *dst, int depth);
    Entry &entryFor(const Clip &c) { return m_decoders[c.id]; }
    void readStream(Entry &e, std::int64_t k, float *dst, int frames);

    std::unordered_map<QString, Entry> m_decoders;
    std::vector<float> m_tmp;
    // Ducking: envoltória da voz contínua entre blocos.
    double m_duckEnv = 0;
    std::int64_t m_duckNext = -1;
    std::vector<float> m_voice;
    std::vector<float> m_duckBuf;
};

} // namespace clv::engine
