// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/Evaluate.h"
#include "core/Model.h"

#include <QColor>
#include <QHash>
#include <QImage>
#include <QRectF>
#include <QSize>

#include <rhi/qrhi.h>

#include <memory>
#include <unordered_map>

namespace clv {
struct Lut3D;
}

namespace clv::render {

/** Fornece a imagem (quadro de vídeo/imagem) de uma camada de mídia. */
class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual QImage frameFor(const VisualLayer &layer) = 0;
};

/**
 * Compositor GPU (Qt RHI: Vulkan/OpenGL/Metal/D3D). Desenha as camadas de um
 * FrameDesc numa textura "acumuladora" do tamanho do canvas e depois copia
 * para um alvo (item do preview) ou permite leitura (export).
 *
 * Todos os métodos devem ser chamados na thread dona do QRhi, fora de passes.
 */
class Compositor {
public:
    explicit Compositor(QRhi *rhi);
    ~Compositor();

    /** Tamanho em pixels da composição (pode ser menor que o projeto, no preview). */
    void setRenderSize(QSize size);
    /** Fundo transparente (exportação com alfa): ignora a cor e o desfoque de fundo do projeto. */
    void setTransparentBackground(bool on) { m_transparent = on; }
    QSize renderSize() const { return m_size; }

    /** Compõe o quadro na textura acumuladora. */
    void composite(QRhiCommandBuffer *cb, const Project &p, const FrameDesc &frame, FrameSource &source);

    /** Desenha a composição em `rt`, na área `viewport` (px do alvo), limpando com `clear`. */
    void blitTo(QRhiCommandBuffer *cb, QRhiRenderTarget *rt, const QRectF &viewport, const QColor &clear);

    /** Agenda a leitura da composição; o resultado fica válido após o fim do quadro. */
    void readback(QRhiCommandBuffer *cb, QRhiReadbackResult *result);

    /** Converte um resultado de readback em QImage na orientação correta (topo primeiro). */
    QImage imageFromReadback(const QRhiReadbackResult &result) const;

    QRhiTexture *output() const { return m_accum.get(); }

private:
    struct Slot;
    struct LayerUbo;

    void ensureResources(QRhiResourceUpdateBatch *u);
    Slot &slotFor(const QString &key, QSize texSize, QRhiTexture *filterLut, QRhiTexture *userLut, QRhiTexture *gradeLut,
                  bool usesBackdrop, QRhiTexture *external = nullptr);
    /** Textura 3D de um LUT (filtro embutido ou .cube); identidade se não houver. */
    QRhiTexture *lutTexture(const QString &key, QRhiResourceUpdateBatch *u);
    std::unique_ptr<QRhiTexture> uploadLut(const Lut3D &lut, QRhiResourceUpdateBatch *u);
    void fillUbo(LayerUbo &u, const VisualLayer &l, const Project &p, double frameAspect) const;
    /** Versão pequena e borrada do quadro, reaproveitada enquanto o quadro não muda. */
    const QImage &blurredBackground(const QImage &frame, double amount);

    QRhi *m_rhi;
    QSize m_size;

    std::unique_ptr<QRhiTexture> m_accum;
    std::unique_ptr<QRhiTexture> m_backdrop;
    std::unique_ptr<QRhiTextureRenderTarget> m_rtClear;
    std::unique_ptr<QRhiTextureRenderTarget> m_rtLoad;
    std::unique_ptr<QRhiRenderPassDescriptor> m_rpClear;
    std::unique_ptr<QRhiRenderPassDescriptor> m_rpLoad;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiBuffer> m_ubo;
    std::unique_ptr<QRhiBuffer> m_vbo;
    std::unique_ptr<QRhiShaderResourceBindings> m_layoutSrb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeNormal;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeReplace;
    quint32 m_uboStride = 0;

    // Blit
    std::unique_ptr<QRhiBuffer> m_blitUbo;
    std::unique_ptr<QRhiShaderResourceBindings> m_blitSrb;
    std::unique_ptr<QRhiGraphicsPipeline> m_blitPipe;
    QVector<quint32> m_blitRpFormat;
    QRhiTexture *m_blitSrbTex = nullptr;

    QHash<QString, std::shared_ptr<Slot>> m_slots;
    /// Composições internas dos clipes compostos (por id do clipe) e o último quadro em que apareceram.
    std::unordered_map<QString, std::pair<std::unique_ptr<Compositor>, quint64>> m_children;
    std::unique_ptr<QRhiTexture> m_identityLut;
    std::unordered_map<QString, std::unique_ptr<QRhiTexture>> m_luts;
    std::unordered_map<QString, quint64> m_lutUsed; ///< último quadro em que cada LUT de correção foi usado
    quint64 m_frameCounter = 0;
    bool m_transparent = false;
    qint64 m_bgSourceKey = 0;
    double m_bgAmount = -1;
    QImage m_bgImage;
};

} // namespace clv::render
