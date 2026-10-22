// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/Model.h"

#include <QImage>

namespace clv::render {

/// Altura de referência: style.fontSize é expresso em px de um canvas 1080p.
constexpr double kTextReferenceHeight = 1080.0;

/**
 * Rasteriza um clipe de texto (com contorno, sombra e caixa de fundo) numa
 * imagem RGBA8888 de alfa reto. `pxScale` = pixels de saída por px de
 * referência (ex.: canvas 720p no preview → 720/1080).
 */
QImage rasterizeText(const Clip &clip, double pxScale);

/** Chave de cache que muda sempre que o resultado de rasterizeText mudaria. */
quint64 textCacheKey(const Clip &clip, double pxScale);

/** Cartão de um espaço de template vazio ("+ Video 1"), no tamanho dado. */
QImage rasterizeSlot(const QString &label, QSize size);

} // namespace clv::render
