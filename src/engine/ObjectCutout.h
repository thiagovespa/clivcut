// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/Model.h"

#include <QImage>
#include <QString>

#include <functional>
#include <memory>

/**
 * Recorte de um objeto escolhido com cliques (SAM 2.1 Hiera-Tiny via ONNX Runtime). Os cliques
 * num quadro definem o objeto; o modelo o acompanha pelo trecho da mídia que o clipe usa, para
 * a frente e para trás, guardando a memória dos quadros anteriores como o SAM 2 faz em vídeo.
 * O resultado é um vídeo de matte em cache (cinza = opacidade), usado como alfa do clipe no
 * preview e no export, como o recorte automático de pessoas (engine/Matting.h).
 */
namespace clv::engine {

/// Lado maior do matte gerado.
constexpr int kCutoutMatteMaxSide = 960;

bool objectCutoutAvailable();
/** Os quatro arquivos do modelo estão instalados. */
bool objectCutoutModelsInstalled();

/** Identifica os cliques (instante e pontos): muda quando o usuário refaz a seleção. */
QString cutoutKey(const ObjectCutout &cutout);

/**
 * O modelo carregado, para uma thread só. Guarda a imagem codificada do último quadro, então
 * clicar de novo no mesmo quadro só roda o decodificador de máscara (rápido).
 */
class ObjectSegmenter {
public:
    ObjectSegmenter();
    ~ObjectSegmenter();
    ObjectSegmenter(const ObjectSegmenter &) = delete;
    ObjectSegmenter &operator=(const ObjectSegmenter &) = delete;

    bool load(QString *error);
    /** Máscara (Grayscale8, imagem do vídeo reduzida a `kCutoutMatteMaxSide`) dos pontos no quadro `t`. */
    QImage segmentFrame(const QString &mediaPath, Micros t, const std::vector<CutoutPoint> &points, QString *error);

    /**
     * Acompanha o objeto dos cliques pelo trecho [in, out] da mídia e grava o matte em `outPath`
     * (um quadro de matte por quadro da mídia, até 30 por segundo). `fileStart` e `fileFps`
     * recebem o tempo da mídia no início do arquivo e a cadência dele.
     */
    bool track(const QString &mediaPath, const ObjectCutout &cutout, Micros in, Micros out, const QString &outPath,
               Micros *fileStart, double *fileFps, const std::function<bool(double)> &progress, QString *error);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

QString objectMattePathFor(const QString &mediaPath, const ObjectCutout &cutout, Micros in, Micros out);
void registerObjectMatte(const QString &mediaPath, const ObjectCutout &cutout, Micros in, Micros out, Micros fileStart,
                         double fileFps, const QString &path);
/**
 * Matte já gerado para estes cliques que cobre o trecho, com o tempo da mídia no início do
 * arquivo e a cadência dele. Só consulta o registro (serve a cada quadro).
 */
QString findObjectMatte(const QString &mediaPath, const ObjectCutout &cutout, Micros in, Micros out,
                        Micros *fileStart = nullptr, double *fileFps = nullptr);
/** Registra os mattes de objeto desta mídia que já estão no cache em disco. */
void discoverObjectMattes(const QString &mediaPath);

} // namespace clv::engine
