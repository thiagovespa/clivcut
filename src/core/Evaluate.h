// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Model.h"
#include "Stabilize.h"

#include <memory>
#include <vector>

/**
 * Avaliação do projeto num instante: quais camadas visuais e fontes de áudio
 * estão ativas e com que parâmetros já resolvidos (keyframes, animações de
 * entrada/saída, transições). É a única fonte de verdade usada tanto pelo
 * preview quanto pelo export, garantindo que o resultado seja idêntico.
 */
namespace clv {

enum class MaskKind { None, WipeLeft, WipeRight, Circle, WipeUp, WipeDown };

struct FrameDesc;

struct VisualLayer {
    int trackIndex = 0;
    const Clip *clip = nullptr;
    const MediaAsset *asset = nullptr; ///< nulo para texto
    Micros rel = 0;        ///< tempo relativo ao início do clipe
    Micros sourceTime = 0; ///< tempo na mídia de origem

    double x = 0, y = 0, scale = 1, rotation = 0, opacity = 1;
    bool flipH = false, flipV = false;
    double brightness = 0, contrast = 0, saturation = 0, temperature = 0;
    BlendMode blend = BlendMode::Normal;

    /// Máscara de transição (wipe/círculo) aplicada a esta camada.
    MaskKind mask = MaskKind::None;
    double maskProgress = 1;

    /// Efeito de vídeo (índice do shader, 0 = nenhum), intensidade e tempo no clipe (s).
    /// Transições podem substituir o efeito do clipe durante a passagem.
    int effect = 0;
    double effectIntensity = 0;
    double effectTime = 0;
    /// Estabilização deste quadro (correção sobre a imagem de origem) e o formato analisado.
    StabCorrection stab;
    double stabAspect = 16.0 / 9;
    /// Clipe composto: o quadro interno, composto à parte e usado como a imagem desta camada.
    std::shared_ptr<const FrameDesc> nested;
};

struct AudioSource {
    int trackIndex = 0;
    const Clip *clip = nullptr;
    const MediaAsset *asset = nullptr;
    Micros rel = 0;
    Micros sourceTime = 0;
};

struct FrameDesc {
    std::vector<VisualLayer> layers; ///< de baixo para cima
    std::vector<AudioSource> audio;
};

/** Tempo na mídia de origem para um tempo relativo ao clipe (aceita rel fora do clipe). */
Micros sourceTimeAt(const Project &p, const Clip &c, Micros rel);

/** Ganho linear de áudio num tempo relativo (volume + keyframes + fades). */
double audioGainAt(const Clip &c, Micros rel);

FrameDesc evaluate(const Project &p, Micros t);

} // namespace clv
