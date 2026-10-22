// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QImage>
#include <QSize>

namespace clv {

/**
 * Imagem de uma camada de vídeo: RGBA pronta ou os planos YUV 4:2:0 de 8 bits como saem
 * do decodificador, para a GPU converter (metade dos bytes no upload e nenhuma conversão
 * de cor na CPU).
 */
struct VideoImage {
    enum class Format { None, Rgba, I420, Nv12 };

    Format format = Format::None;
    QImage rgba;           ///< Format::Rgba
    QSize size;            ///< tamanho do plano Y, antes da rotação
    QByteArray planes[3];  ///< I420: Y, U, V; NV12: Y, UV intercalado (linhas sem folga)
    bool bt709 = false;    ///< matriz BT.709 (senão BT.601)
    bool fullRange = false;
    int rotation = 0;      ///< 0, 90, 180 ou 270 graus no sentido horário, aplicada na conversão
    qint64 key = 0;        ///< identidade do conteúdo (YUV), como QImage::cacheKey

    VideoImage() = default;
    VideoImage(QImage img); // NOLINT: QImage serve onde se espera uma VideoImage

    bool isNull() const { return format == Format::None || (format == Format::Rgba && rgba.isNull()); }
    bool isYuv() const { return format == Format::I420 || format == Format::Nv12; }
    /** Tamanho exibido (com a rotação aplicada). */
    QSize displaySize() const;
    /** Muda quando o conteúdo muda (para reaproveitar texturas). */
    qint64 cacheKey() const { return format == Format::Rgba ? rgba.cacheKey() : key; }
    /** Largura e altura dos planos de cor. */
    QSize chromaSize() const { return {(size.width() + 1) / 2, (size.height() + 1) / 2}; }

    /**
     * Conversão na CPU (RGBA8888), para quem precisa dos pixels (desfoque de fundo, testes).
     * Com `target` (tamanho exibido), reduz por amostragem simples ao mesmo tempo.
     */
    QImage toImage(QSize target = {}) const;

    /** Chave nova para um conteúdo YUV (não colide com as de QImage). */
    static qint64 newKey();

    bool operator==(const VideoImage &o) const;
};

} // namespace clv
