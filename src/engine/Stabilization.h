// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/Stabilize.h"

#include <QImage>
#include <QString>

#include <functional>

/**
 * Análise de movimento para a estabilização (o caminho da câmera, quadro a quadro). Roda uma
 * vez por arquivo, em segundo plano, e o resultado fica em cache; a correção é aplicada na
 * avaliação do quadro (core/Stabilize.h), então preview e export usam o mesmo caminho.
 */
namespace clv::engine {

/// Movimento de um quadro para o seguinte: a imagem nova = escala·rotação·anterior + deslocamento.
struct FrameMotion {
    double dx = 0, dy = 0; ///< em pixels do quadro analisado
    double angle = 0;      ///< radianos
    double scale = 1;
    int inliers = 0;       ///< blocos que concordaram com o movimento (0 = só o deslocamento global)
};

/** Movimento entre dois quadros em tons de cinza do mesmo tamanho. */
FrameMotion estimateMotion(const QImage &prev, const QImage &next);

QString motionPathFor(const QString &mediaPath);

/** Analisa o vídeo inteiro, grava o caminho em `outPath` e o registra para a mídia. */
bool analyzeMotion(const QString &mediaPath, const QString &outPath, const std::function<bool(double)> &progress,
                   QString *error);

/** Carrega uma análise já feita (cache) para a mídia, se existir. */
bool discoverMotion(const QString &mediaPath);

} // namespace clv::engine
