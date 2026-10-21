// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Model.h"

#include <QByteArray>
#include <QString>

#include <memory>
#include <mutex>
#include <map>
#include <vector>

/**
 * Estabilização de vídeo. A análise (engine/Stabilization.h) mede, uma vez por arquivo, o
 * caminho da câmera quadro a quadro; aqui fica esse caminho e a correção aplicada na hora de
 * mostrar: o caminho suavizado menos o real, com um zoom que esconde as bordas. Como a
 * correção é calculada ao vivo, a intensidade pode mudar sem reanalisar.
 */
namespace clv {

/// Caminho acumulado da imagem: deslocamento (fração da largura), rotação (rad) e escala (log).
struct MotionTrack {
    double fps = 30;
    double aspect = 16.0 / 9; ///< largura / altura do quadro analisado
    std::vector<double> x, y, angle, logScale;

    int frames() const { return static_cast<int>(x.size()); }
    /// Zoom que esconde as bordas com essa intensidade (calculado uma vez por nível).
    double zoomFor(double strength) const;

private:
    mutable std::mutex m_mutex;
    mutable std::map<int, double> m_zoom;
};

/// Correção de um quadro: a imagem é girada, escalada e deslocada (em fração da largura).
struct StabCorrection {
    double dx = 0, dy = 0, angle = 0, scale = 1, zoom = 1;
    bool active() const { return dx != 0 || dy != 0 || angle != 0 || scale != 1 || zoom != 1; }
};

/** Correção no tempo de origem `t`, para a intensidade 0..1 (0 = nenhuma). */
StabCorrection stabilizationAt(const MotionTrack &m, Micros t, double strength);

/** Mapeia um ponto (u, v) da área mostrada para o ponto da imagem de origem que vai nele. */
void stabilizedUv(const StabCorrection &c, double aspect, double &u, double &v);

QByteArray motionToJson(const MotionTrack &m);
std::shared_ptr<const MotionTrack> motionFromJson(const QByteArray &data);

/// Caminhos já analisados, por arquivo de mídia (usados pela avaliação do quadro).
void registerMotion(const QString &mediaPath, std::shared_ptr<const MotionTrack> track);
std::shared_ptr<const MotionTrack> registeredMotion(const QString &mediaPath);

} // namespace clv
