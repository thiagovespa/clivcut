// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <functional>

/**
 * Separação de música em faixas por IA local (HT-Demucs v4 via ONNX Runtime): bateria,
 * baixo, outros e voz, mais o instrumental (tudo menos a voz). O arquivo inteiro é
 * processado em segundo plano, em trechos de 7,8 s com sobreposição, e as faixas ficam em
 * cache como WAV alinhados à mídia original.
 */
namespace clv::engine {

enum class Stem { Drums, Bass, Other, Vocals, Instrumental };
constexpr int kStemCount = 5;

/** Disponível neste build (compilado com ONNX Runtime). */
bool stemSeparationAvailable();

/** Nome traduzido da faixa ("Vocals", "Drums"...). */
QString stemName(Stem s);

QString stemPathFor(const QString &mediaPath, Stem s);
/** Todas as faixas desta mídia já estão no cache. */
bool stemsReady(const QString &mediaPath);

/** `progress(fração)` retornando false cancela. */
bool separateStems(const QString &mediaPath, const QString &modelPath, const std::function<bool(double)> &progress,
                   QString *error);

} // namespace clv::engine
