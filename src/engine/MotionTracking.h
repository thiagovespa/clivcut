// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/Model.h"

#include <QString>

#include <functional>
#include <vector>

/**
 * Rastreamento de movimento: segue uma região de um vídeo quadro a quadro (correlação de
 * fase com o quadro anterior, reancorada no aspecto inicial para não escorregar). Serve para
 * prender textos, stickers e imagens a algo que se mexe no vídeo.
 */
namespace clv::engine {

struct TrackPoint {
    Micros t = 0;         ///< tempo na mídia de origem
    double u = 0, v = 0;  ///< centro da região, normalizado (0..1) na imagem de origem
    double confidence = 0;
};

/**
 * Segue a região de centro (u, v) e tamanho (w, h) — normalizados na imagem de origem, no
 * tempo `from` — até o tempo `to` (para frente ou para trás). Devolve um ponto por quadro, em
 * ordem de processamento (o primeiro é o próprio `from`).
 */
std::vector<TrackPoint> trackRegion(const QString &mediaPath, Micros from, Micros to, double u, double v, double w,
                                    double h, const std::function<bool(double)> &progress, QString *error);

} // namespace clv::engine
