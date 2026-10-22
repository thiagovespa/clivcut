// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

/**
 * Narração por texto com voz neural local (Kokoro v1.0 via ONNX Runtime). O texto vira
 * fonemas IPA com o espeak-ng (como na implementação de referência), os fonemas viram
 * tokens do modelo e o áudio sai a 24 kHz, mono.
 */
namespace clv::engine {

struct TtsVoice {
    QString id;       ///< "pf_dora"
    QString name;     ///< "Dora"
    QString language; ///< código do espeak-ng: "pt-br", "es", "en-us"...
    bool female = false;
};

constexpr int kTtsSampleRate = 24000;

/** Compilado com voz neural (ONNX Runtime + espeak-ng). */
bool ttsAvailable();

/** Vozes oferecidas (as línguas cuja conversão em fonemas é feita pelo espeak-ng). */
const std::vector<TtsVoice> &ttsVoices();
const TtsVoice *findTtsVoice(const QString &id);
/** Voz padrão para uma língua da interface ("pt_BR", "es", "en"...). */
QString defaultTtsVoice(const QString &uiLanguage);

/** Texto → fonemas IPA no formato que o Kokoro espera (pontuação preservada). */
QString ttsPhonemize(const QString &text, const QString &language, QString *error = nullptr);
/** Fonemas → ids do vocabulário do modelo (símbolos desconhecidos são descartados). */
std::vector<std::int64_t> ttsTokenize(const QString &phonemes);

/**
 * Gera a fala de `text` com a voz `voiceId` (velocidade 0,5–2) e grava um WAV mono de 24 kHz
 * em `outPath`. `progress(fração)` retornando false cancela.
 */
bool synthesizeSpeech(const QString &text, const QString &voiceId, double speed, const QString &modelPath,
                      const QString &voicesPath, const QString &outPath, const std::function<bool(double)> &progress,
                      QString *error);

} // namespace clv::engine
