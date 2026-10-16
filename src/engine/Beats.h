// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/Model.h"

#include <functional>
#include <vector>

namespace clv::engine {

struct BeatResult {
    double bpm = 0;
    std::vector<Micros> beats;    ///< instantes das batidas, em ordem
    std::vector<double> strength; ///< força do ataque em cada batida (mesmo tamanho de beats)
};

/**
 * Batidas de amostras mono 16 kHz que começam em `origin`: fluxo espectral como curva de
 * ataques, andamento por autocorrelação (com preferência perto de 120 BPM) e as batidas
 * escolhidas por programação dinâmica, que equilibra ataques fortes e intervalo regular.
 */
BeatResult detectBeatsPcm16k(const std::vector<float> &pcm, Micros origin);

/** Batidas do que se ouve na faixa `trackIndex` entre `start` e `end` (a mixagem da faixa). */
BeatResult detectBeats(const Project &p, int trackIndex, Micros start, Micros end,
                       const std::function<bool(double)> &progress = {});

/**
 * Uma a cada `every` batidas, começando na fase com ataques mais fortes (para every = 4,
 * isso costuma cair no primeiro tempo do compasso).
 */
std::vector<Micros> everyNthBeat(const BeatResult &r, int every);

} // namespace clv::engine
