// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/Model.h"

#include <QString>

#include <functional>

/**
 * Câmera lenta fluida: quadros intermediários gerados por IA (RIFE v4.26 via ONNX Runtime).
 * O trecho da mídia que o clipe usa é processado uma vez, em segundo plano, e vira um vídeo em
 * cache com `factor` vezes mais quadros; preview e export decodificam esse vídeo no lugar da
 * mídia (DecoderFrameSource), então a câmera lenta deixa de repetir quadros.
 */
namespace clv::engine {

/// Lado maior dos quadros processados (a rede custa ~1 s por quadro em 720p numa CPU comum).
constexpr int kInterpolationMaxSide = 1280;
constexpr int kInterpolationMaxFactor = 8;

/** Trecho da mídia (tempos da fonte) e quantos quadros por quadro original. */
struct InterpolationSpan {
    Micros in = 0, out = 0;
    int factor = 1; ///< 1 = o clipe não precisa (não está mais lento que a mídia)
};

bool interpolationAvailable();

/** O que deixa o clipe fluido no projeto: o trecho usado e o fator (velocidade mínima, fps). */
InterpolationSpan interpolationSpanFor(const Project &p, const Clip &c);

QString interpolationPathFor(const QString &mediaPath, const InterpolationSpan &span);

/** Gera o vídeo interpolado do trecho em `outPath`; o arquivo começa em `*fileStart` (tempo da mídia). */
bool generateInterpolation(const QString &mediaPath, const InterpolationSpan &span, const QString &modelPath,
                           const QString &outPath, Micros *fileStart, const std::function<bool(double)> &progress,
                           QString *error);

void registerInterpolation(const QString &mediaPath, const InterpolationSpan &span, Micros fileStart,
                           const QString &path);
/**
 * Vídeo interpolado já pronto que cobre o trecho com pelo menos o fator pedido, e o tempo da
 * mídia no início dele. Só consulta o registro (rápido; serve a cada quadro).
 */
QString findInterpolation(const QString &mediaPath, const InterpolationSpan &span, Micros *fileStart = nullptr,
                          int *factor = nullptr);
/** Registra os vídeos interpolados desta mídia que já estão no cache em disco. */
void discoverInterpolations(const QString &mediaPath);

} // namespace clv::engine
