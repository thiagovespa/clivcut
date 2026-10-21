// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QImage>

/**
 * Correlação de fase entre janelas de imagens em tons de cinza (Grayscale8): quanto o
 * conteúdo andou de uma imagem para outra, com precisão de subpixel. Usada pela
 * estabilização e pelo rastreamento de movimento.
 */
namespace clv::engine {

struct Shift {
    double dx = 0, dy = 0;
    double peak = 0; ///< altura do pico (0..1): confiança da medida
};

/**
 * Janela w×h (potências de 2) de `a` centrada em (ax, ay) contra a de `b` centrada em (bx, by).
 * Fora das bordas repete o pixel mais próximo.
 */
Shift phaseCorrelate(const QImage &a, int ax, int ay, const QImage &b, int bx, int by, int w, int h);

/** Pixel (x, y) de uma imagem Grayscale8, repetindo a borda fora dela. */
float grayAt(const QImage &img, int x, int y);

/** Maior potência de 2 que não passa de v (mínimo 1). */
int floorPow2(int v);

} // namespace clv::engine
