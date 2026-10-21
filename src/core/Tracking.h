// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Model.h"

#include <QPointF>

#include <vector>

/**
 * Rastreamento aplicado: converte o caminho de um ponto na imagem de um vídeo (tempo de
 * origem → posição) em keyframes de posição de outro clipe, que passa a acompanhá-lo.
 */
namespace clv {

/// Um ponto seguido na mídia: tempo de origem e posição normalizada (0..1) na imagem.
struct TrackedPoint {
    Micros t = 0;
    double u = 0, v = 0;
};

/** Ponto (u, v) da imagem de um clipe de vídeo → ponto do canvas (px do projeto), no tempo `rel`. */
QPointF clipImageToCanvas(const Project &p, const Clip &video, Micros rel, double u, double v);

/** Ponto do canvas (px do projeto) → ponto (u, v) da imagem do clipe; false se o clipe não tem imagem. */
bool canvasToClipImage(const Project &p, const Clip &video, Micros rel, QPointF canvas, double &u, double &v);

/**
 * Faz o clipe `followerId` acompanhar o caminho `path` (pontos em qualquer ordem) do vídeo
 * `videoId`: no trecho em que os dois se sobrepõem, os keyframes de X e Y do seguidor recebem
 * o deslocamento do ponto no canvas, a partir da posição que ele tem no instante `anchor`
 * (tempo da timeline). Keyframes quase colineares são removidos. Retorna false se não houver
 * trecho em comum.
 */
bool applyTrackedPath(Project &p, const QString &followerId, const QString &videoId, Micros anchor,
                      std::vector<TrackedPoint> path);

} // namespace clv
