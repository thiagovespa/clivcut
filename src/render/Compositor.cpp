// SPDX-License-Identifier: GPL-3.0-or-later
#include "Compositor.h"

#include "TextRaster.h"
#include "core/Lut.h"

#include <QFile>
#include <QFloat16>
#include <QMatrix4x4>
#include <QtMath>

#include <rhi/qshader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace clv::render {

namespace {

constexpr int kMaxLayers = 128;
constexpr int kFloatsPerVertex = 4; // x, y, u, v

QShader loadShader(const QString &name)
{
    QFile f(QStringLiteral(":/clivcut/shaders/") + name + QStringLiteral(".qsb"));
    if (!f.open(QIODevice::ReadOnly))
        qFatal("Shader ausente: %s", qPrintable(name));
    return QShader::fromSerialized(f.readAll());
}

int blendIndex(BlendMode m)
{
    return static_cast<int>(m);
}

} // namespace

struct Compositor::LayerUbo {
    float mvp[16];
    float canvasSize[4];
    float adjust[4];
    float chromaColor[4];
    float chromaParams[4];
    float mask[4];
    float misc[4];
    float adjust2[4];
    float adjust3[4];
    float clipMask[4];
    float clipMaskGeom[4];
    float clipMaskExtra[4];
    float grade[4];
    float effect[4];
};

struct Compositor::Slot {
    std::unique_ptr<QRhiTexture> tex; ///< nulo em camadas de ajuste (usam o backdrop) e em compostos
    QRhiTexture *external = nullptr;  ///< composto: a saída da composição interna
    std::unique_ptr<QRhiShaderResourceBindings> srb;
    QRhiTexture *filterLut = nullptr;
    QRhiTexture *userLut = nullptr;
    QRhiTexture *gradeLut = nullptr;
    bool usesBackdrop = false;
    QSize size;
    qint64 imageKey = -1;
    quint64 lastFrame = 0;

    // Quadros YUV: os planos e o passe que os converte em `tex`.
    std::unique_ptr<QRhiTexture> planes[3];
    VideoImage::Format planeFormat = VideoImage::Format::None;
    QSize planeSize;
    std::unique_ptr<QRhiTextureRenderTarget> rt;
    std::unique_ptr<QRhiRenderPassDescriptor> rp;
    std::unique_ptr<QRhiBuffer> yuvUbo;
    std::unique_ptr<QRhiShaderResourceBindings> yuvSrb;
};

Compositor::Compositor(QRhi *rhi) : m_rhi(rhi) {}

Compositor::~Compositor() = default;

void Compositor::setRenderSize(QSize size)
{
    size = size.expandedTo(QSize(2, 2));
    if (size == m_size)
        return;
    m_size = size;
    // Recria tudo que depende do tamanho (as SRBs referenciam o backdrop).
    m_slots.clear();
    m_accum.reset();
}

QRhiTexture *Compositor::lutTexture(const QString &key, QRhiResourceUpdateBatch *u)
{
    if (key.isEmpty() || !m_rhi->isFeatureSupported(QRhi::ThreeDimensionalTextures))
        return m_identityLut.get();
    const bool grade = key.startsWith(QLatin1String("grade:"));
    if (grade)
        m_lutUsed[key] = m_frameCounter;
    if (auto it = m_luts.find(key); it != m_luts.end())
        return it->second.get();
    // LUTs de correção mudam a cada ajuste dos controles: libera os que saíram de uso
    // (nem neste quadro nem no anterior) e invalida os slots que ainda os referenciam.
    if (grade) {
        for (auto it = m_luts.begin(); it != m_luts.end();) {
            const auto used = m_lutUsed.find(it->first);
            if (used == m_lutUsed.end() || used->second + 1 >= m_frameCounter) {
                ++it;
                continue;
            }
            for (auto &slot : m_slots)
                if (slot->gradeLut == it->second.get())
                    slot->srb.reset();
            m_lutUsed.erase(used);
            it = m_luts.erase(it);
        }
    }
    const auto lut = lutForKey(key);
    if (!lut) {
        m_luts[key] = nullptr;
        return m_identityLut.get();
    }
    m_luts[key] = uploadLut(*lut, u);
    return m_luts[key].get();
}

std::unique_ptr<QRhiTexture> Compositor::uploadLut(const Lut3D &lut, QRhiResourceUpdateBatch *u)
{
    const int n = lut.size;
    std::unique_ptr<QRhiTexture> tex(m_rhi->newTexture(QRhiTexture::RGBA16F, n, n, n, 1, QRhiTexture::ThreeDimensional));
    tex->create();
    QList<QRhiTextureUploadEntry> entries;
    for (int z = 0; z < n; ++z) {
        QByteArray slice(n * n * 4 * static_cast<int>(sizeof(qfloat16)), Qt::Uninitialized);
        auto *dst = reinterpret_cast<qfloat16 *>(slice.data());
        const float *src = lut.rgb.data() + static_cast<size_t>(z) * n * n * 3;
        for (int i = 0; i < n * n; ++i) {
            dst[i * 4 + 0] = qfloat16(src[i * 3 + 0]);
            dst[i * 4 + 1] = qfloat16(src[i * 3 + 1]);
            dst[i * 4 + 2] = qfloat16(src[i * 3 + 2]);
            dst[i * 4 + 3] = qfloat16(1.0f);
        }
        entries.append(QRhiTextureUploadEntry(z, 0, QRhiTextureSubresourceUploadDescription(slice)));
    }
    QRhiTextureUploadDescription desc;
    desc.setEntries(entries.begin(), entries.end());
    u->uploadTexture(tex.get(), desc);
    return tex;
}

void Compositor::ensureResources(QRhiResourceUpdateBatch *u)
{
    if (!m_identityLut) {
        Lut3D identity;
        identity.size = 2;
        identity.rgb = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1};
        if (m_rhi->isFeatureSupported(QRhi::ThreeDimensionalTextures))
            m_identityLut = uploadLut(identity, u);
        else {
            m_identityLut.reset(m_rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
            m_identityLut->create();
        }
    }
    if (m_accum)
        return;
    m_accum.reset(m_rhi->newTexture(QRhiTexture::RGBA8, m_size, 1,
                                    QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    m_accum->create();
    m_backdrop.reset(m_rhi->newTexture(QRhiTexture::RGBA8, m_size, 1, QRhiTexture::UsedAsTransferSource));
    m_backdrop->create();

    m_rtClear.reset(m_rhi->newTextureRenderTarget({m_accum.get()}));
    m_rpClear.reset(m_rtClear->newCompatibleRenderPassDescriptor());
    m_rtClear->setRenderPassDescriptor(m_rpClear.get());
    m_rtClear->create();
    m_rtLoad.reset(m_rhi->newTextureRenderTarget({m_accum.get()}, QRhiTextureRenderTarget::PreserveColorContents));
    m_rpLoad.reset(m_rtLoad->newCompatibleRenderPassDescriptor());
    m_rtLoad->setRenderPassDescriptor(m_rpLoad.get());
    m_rtLoad->create();

    if (!m_sampler) {
        m_sampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                          QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        m_sampler->create();
        m_uboStride = m_rhi->ubufAligned(sizeof(LayerUbo));
        m_ubo.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, m_uboStride * kMaxLayers));
        m_ubo->create();
        m_vbo.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer,
                                     kMaxLayers * 4 * kFloatsPerVertex * sizeof(float)));
        m_vbo->create();
    }

    // SRB de layout (compatível com as SRBs de cada slot).
    m_layoutSrb.reset(m_rhi->newShaderResourceBindings());
    m_layoutSrb->setBindings({
        QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_ubo.get(),
            sizeof(LayerUbo)),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_backdrop.get(),
                                                  m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, m_backdrop.get(),
                                                  m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage, m_identityLut.get(),
                                                  m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(4, QRhiShaderResourceBinding::FragmentStage, m_identityLut.get(),
                                                  m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(5, QRhiShaderResourceBinding::FragmentStage, m_identityLut.get(),
                                                  m_sampler.get()),
    });
    m_layoutSrb->create();

    auto makePipe = [&](bool premulBlend) {
        std::unique_ptr<QRhiGraphicsPipeline> pipe(m_rhi->newGraphicsPipeline());
        pipe->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        pipe->setShaderStages({{QRhiShaderStage::Vertex, loadShader(QStringLiteral("layer.vert"))},
                               {QRhiShaderStage::Fragment, loadShader(QStringLiteral("layer.frag"))}});
        QRhiVertexInputLayout layout;
        layout.setBindings({{kFloatsPerVertex * sizeof(float)}});
        layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0},
                              {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)}});
        pipe->setVertexInputLayout(layout);
        if (premulBlend) {
            QRhiGraphicsPipeline::TargetBlend b;
            b.enable = true;
            b.srcColor = QRhiGraphicsPipeline::One;
            b.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            b.srcAlpha = QRhiGraphicsPipeline::One;
            b.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            pipe->setTargetBlends({b});
        }
        pipe->setShaderResourceBindings(m_layoutSrb.get());
        pipe->setRenderPassDescriptor(m_rpClear.get());
        pipe->create();
        return pipe;
    };
    m_pipeNormal = makePipe(true);
    m_pipeReplace = makePipe(false);
}

Compositor::Slot &Compositor::slotFor(const QString &key, QSize texSize, QRhiTexture *filterLut, QRhiTexture *userLut,
                                      QRhiTexture *gradeLut, bool usesBackdrop, QRhiTexture *external)
{
    auto &ptr = m_slots[key];
    if (!ptr)
        ptr = std::make_shared<Slot>();
    Slot &s = *ptr;
    bool rebuild = !s.srb || s.filterLut != filterLut || s.userLut != userLut || s.gradeLut != gradeLut
                   || s.usesBackdrop != usesBackdrop || s.external != external;
    if (external) {
        s.tex.reset();
        s.size = texSize;
    } else if (!usesBackdrop && (!s.tex || s.size != texSize)) {
        // Alvo de render também: quadros YUV são convertidos direto nela.
        s.rt.reset();
        s.yuvSrb.reset();
        s.tex.reset(m_rhi->newTexture(QRhiTexture::RGBA8, texSize, 1, QRhiTexture::RenderTarget));
        s.tex->create();
        s.size = texSize;
        s.imageKey = -1;
        rebuild = true;
    }
    if (usesBackdrop)
        s.tex.reset();
    if (rebuild) {
        s.filterLut = filterLut;
        s.userLut = userLut;
        s.gradeLut = gradeLut;
        s.usesBackdrop = usesBackdrop;
        s.external = external;
        QRhiTexture *layerTex = external ? external : usesBackdrop ? m_backdrop.get() : s.tex.get();
        s.srb.reset(m_rhi->newShaderResourceBindings());
        s.srb->setBindings({
            QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_ubo.get(),
                sizeof(LayerUbo)),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, layerTex,
                                                      m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, m_backdrop.get(),
                                                      m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage, filterLut,
                                                      m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(4, QRhiShaderResourceBinding::FragmentStage, userLut,
                                                      m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(5, QRhiShaderResourceBinding::FragmentStage, gradeLut,
                                                      m_sampler.get()),
        });
        s.srb->create();
    }
    s.lastFrame = m_frameCounter;
    return s;
}

void Compositor::fillUbo(LayerUbo &u, const VisualLayer &l, const Project &p, double frameAspect) const
{
    std::memset(&u, 0, sizeof u);
    QMatrix4x4 m = m_rhi->clipSpaceCorrMatrix();
    m.ortho(0, m_size.width(), m_size.height(), 0, -1, 1);
    std::memcpy(u.mvp, m.constData(), sizeof u.mvp);
    u.canvasSize[0] = m_size.width();
    u.canvasSize[1] = m_size.height();
    u.adjust[0] = static_cast<float>(l.brightness);
    u.adjust[1] = static_cast<float>(l.contrast);
    u.adjust[2] = static_cast<float>(l.saturation);
    u.adjust[3] = static_cast<float>(l.temperature);
    if (l.clip && l.clip->chroma.enabled && !l.clip->isText()) {
        const QColor c(l.clip->chroma.color);
        u.chromaColor[0] = c.redF();
        u.chromaColor[1] = c.greenF();
        u.chromaColor[2] = c.blueF();
        u.chromaColor[3] = 1;
        u.chromaParams[0] = static_cast<float>(l.clip->chroma.similarity);
        u.chromaParams[1] = static_cast<float>(l.clip->chroma.smoothness);
    }
    u.mask[0] = static_cast<float>(l.mask);
    u.mask[1] = static_cast<float>(l.maskProgress);
    u.mask[2] = static_cast<float>(p.width) / p.height;
    u.effect[0] = static_cast<float>(l.effect);
    u.effect[1] = static_cast<float>(std::clamp(l.effectIntensity, 0.0, 1.0));
    u.effect[2] = static_cast<float>(l.effectTime);
    u.effect[3] = l.nested ? 1.f : 0.f;
    u.misc[0] = static_cast<float>(std::clamp(l.opacity, 0.0, 1.0));
    u.misc[1] = static_cast<float>(blendIndex(l.blend));
    u.misc[2] = m_rhi->isYUpInFramebuffer() ? 1.f : 0.f;
    if (l.clip && !l.clip->isText()) {
        const ColorAdjust &c = l.clip->color;
        u.adjust2[0] = static_cast<float>(c.exposure);
        u.adjust2[1] = static_cast<float>(c.tint);
        u.adjust2[2] = static_cast<float>(c.highlights);
        u.adjust2[3] = static_cast<float>(c.shadows);
        u.adjust3[0] = static_cast<float>(c.vignette);
        u.adjust3[1] = static_cast<float>(c.sharpen);
        const bool lut3d = m_rhi->isFeatureSupported(QRhi::ThreeDimensionalTextures);
        u.adjust3[2] = lut3d && c.hasFilter() ? static_cast<float>(c.filterIntensity) : 0.f;
        u.adjust3[3] = lut3d && c.hasLut() ? static_cast<float>(c.lutIntensity) : 0.f;
        u.grade[0] = lut3d && c.hasGrade() ? 1.f : 0.f;
        u.misc[3] = l.clip->isAdjust() ? 1.f : 0.f;
    }
    if (l.clip && l.clip->mask.active() && !l.clip->isText()) {
        const Mask &m = l.clip->mask;
        u.clipMask[0] = static_cast<float>(m.shape);
        u.clipMask[1] = m.invert ? 1.f : 0.f;
        u.clipMask[2] = static_cast<float>(m.feather);
        u.clipMask[3] = static_cast<float>(m.roundness);
        u.clipMaskGeom[0] = static_cast<float>(m.x);
        u.clipMaskGeom[1] = static_cast<float>(m.y);
        u.clipMaskGeom[2] = static_cast<float>(m.width);
        u.clipMaskGeom[3] = static_cast<float>(m.height);
        u.clipMaskExtra[0] = static_cast<float>(qDegreesToRadians(m.rotation));
        u.clipMaskExtra[1] = static_cast<float>(frameAspect);
    }
}

const QImage &Compositor::blurredBackground(const VideoImage &frame, double amount)
{
    if (frame.cacheKey() == m_bgSourceKey && amount == m_bgAmount)
        return m_bgImage;
    // Desfoque barato: reduz muito, borra a imagem pequena e deixa o filtro linear da GPU ampliar.
    const int side = static_cast<int>(std::lround(96 - 72 * std::clamp(amount, 0.0, 1.0))); // 96 → 24 px
    const QSize small = frame.displaySize().scaled(side, side, Qt::KeepAspectRatio).expandedTo({2, 2});
    QImage img = frame.toImage(small * 4)
                     .scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                     .convertToFormat(QImage::Format_RGBA8888);
    // Duas passadas de caixa 3×3 suavizam os blocos que a ampliação deixaria visíveis.
    for (int pass = 0; pass < 2; ++pass) {
        const QImage in = img.copy();
        const int w = img.width(), h = img.height();
        for (int y = 0; y < h; ++y) {
            uchar *dst = img.scanLine(y);
            for (int x = 0; x < w; ++x) {
                int sum[3] = {0, 0, 0};
                for (int dy = -1; dy <= 1; ++dy) {
                    const uchar *row = in.constScanLine(std::clamp(y + dy, 0, h - 1));
                    for (int dx = -1; dx <= 1; ++dx) {
                        const uchar *px = row + std::clamp(x + dx, 0, w - 1) * 4;
                        sum[0] += px[0];
                        sum[1] += px[1];
                        sum[2] += px[2];
                    }
                }
                dst[x * 4] = static_cast<uchar>(sum[0] / 9);
                dst[x * 4 + 1] = static_cast<uchar>(sum[1] / 9);
                dst[x * 4 + 2] = static_cast<uchar>(sum[2] / 9);
                dst[x * 4 + 3] = 255;
            }
        }
    }
    m_bgImage = img;
    m_bgSourceKey = frame.cacheKey();
    m_bgAmount = amount;
    return m_bgImage;
}

void Compositor::composite(QRhiCommandBuffer *cb, const Project &p, const FrameDesc &frame, FrameSource &source)
{
    ++m_frameCounter;
    // Clipes compostos primeiro: cada um vira uma textura do tamanho do canvas, com fundo transparente.
    for (const VisualLayer &l : frame.layers) {
        if (!l.nested || !l.clip || l.opacity <= 0.001)
            continue;
        auto &[child, used] = m_children[l.clip->id];
        if (!child) {
            child = std::make_unique<Compositor>(m_rhi);
            child->setTransparentBackground(true);
        }
        child->setRenderSize(m_size);
        child->composite(cb, p, *l.nested, source);
        used = m_frameCounter;
    }
    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    ensureResources(u);

    const double k = static_cast<double>(m_size.width()) / p.width; // px de render por px do projeto
    struct Draw {
        Slot *slot;
        bool normal;
        int index;
    };
    std::vector<Draw> draws;
    std::vector<float> verts;
    verts.reserve(kMaxLayers * 4 * kFloatsPerVertex);

    // Fundo desfocado: o clipe da faixa principal cobrindo o canvas, bem borrado, atrás de tudo.
    if (p.backgroundBlur > 0 && !m_transparent) {
        const auto main = std::find_if(frame.layers.begin(), frame.layers.end(), [](const VisualLayer &l) {
            return l.trackIndex == 0 && l.clip && l.clip->hasMedia() && l.asset;
        });
        const VideoImage src = main != frame.layers.end() ? source.frameFor(*main) : VideoImage();
        if (!src.isNull()) {
            const QImage &bg = blurredBackground(src, p.backgroundBlur);
            Slot &slot = slotFor(QStringLiteral("#background"), bg.size(), m_identityLut.get(), m_identityLut.get(),
                                 m_identityLut.get(), false);
            if (slot.imageKey != bg.cacheKey()) {
                u->uploadTexture(slot.tex.get(), bg);
                slot.imageKey = bg.cacheKey();
            }
            // "Cobrir": escala até não sobrar borda, centralizado.
            const double W = m_size.width(), H = m_size.height();
            const double s = std::max(W / bg.width(), H / bg.height());
            const double w = bg.width() * s, h = bg.height() * s;
            const float x0 = static_cast<float>((W - w) / 2), y0 = static_cast<float>((H - h) / 2);
            const float x1 = static_cast<float>(x0 + w), y1 = static_cast<float>(y0 + h);
            verts.insert(verts.end(), {x0, y0, 0, 0, x1, y0, 1, 0, x0, y1, 0, 1, x1, y1, 1, 1});
            VisualLayer plain;
            LayerUbo ubo;
            fillUbo(ubo, plain, p, w / h);
            u->updateDynamicBuffer(m_ubo.get(), 0, sizeof ubo, &ubo);
            draws.push_back({&slot, true, 0});
        }
    }

    for (const VisualLayer &l : frame.layers) {
        if (static_cast<int>(draws.size()) >= kMaxLayers)
            break;
        if (l.opacity <= 0.001 || !l.clip)
            continue;

        const ColorAdjust &color = l.clip->color;
        QRhiTexture *filterLut = l.clip->isText() || !color.hasFilter() ? m_identityLut.get() : lutTexture(color.filter, u);
        QRhiTexture *userLut = l.clip->isText() || !color.hasLut() ? m_identityLut.get() : lutTexture(color.lutPath, u);
        QRhiTexture *gradeLut = l.clip->isText() || !color.hasGrade() ? m_identityLut.get() : lutTexture(color.gradeKey(), u);

        if (l.clip->isAdjust()) {
            // Camada de ajuste: quadro inteiro, amostrando o fundo já composto.
            Slot &slot = slotFor(l.clip->id, {}, filterLut, userLut, gradeLut, true);
            const bool flipV = m_rhi->isYUpInFramebuffer();
            const float W = m_size.width(), H = m_size.height();
            const float t0 = flipV ? 1.f : 0.f, t1 = flipV ? 0.f : 1.f;
            verts.insert(verts.end(), {0, 0, 0, t0, W, 0, 1, t0, 0, H, 0, t1, W, H, 1, t1});
            LayerUbo ubo;
            fillUbo(ubo, l, p, static_cast<double>(p.width) / p.height);
            const int index = static_cast<int>(draws.size());
            u->updateDynamicBuffer(m_ubo.get(), index * m_uboStride, sizeof ubo, &ubo);
            draws.push_back({&slot, false, index});
            continue;
        }

        VideoImage img;
        QString key;
        QSizeF natural; // tamanho natural em px do projeto antes de escala
        double fit = 1;
        qint64 imageKey;
        QRhiTexture *external = nullptr;
        if (l.nested) {
            const auto child = m_children.find(l.clip->id);
            if (child == m_children.end() || !child->second.first->output())
                continue;
            external = child->second.first->output();
            key = l.clip->id;
            natural = QSizeF(p.width, p.height);
            imageKey = -1;
        } else if (l.clip->isText()) {
            const double pxScale = p.height / kTextReferenceHeight * k;
            const quint64 tkey = textCacheKey(*l.clip, pxScale);
            key = l.clip->id;
            auto it = m_slots.find(key);
            if (it != m_slots.end() && (*it)->imageKey == static_cast<qint64>(tkey)) {
                natural = QSizeF((*it)->size) / k;
                imageKey = static_cast<qint64>(tkey);
            } else {
                img = rasterizeText(*l.clip, pxScale);
                natural = QSizeF(img.displaySize()) / k;
                imageKey = static_cast<qint64>(tkey);
            }
        } else if (l.clip->isEmptySlot()) {
            // Espaço de template vazio: um cartão do tamanho do canvas.
            key = l.clip->id;
            const quint64 skey = qHash(l.clip->slot) ^ (static_cast<quint64>(m_size.width()) << 32) ^ m_size.height();
            auto it = m_slots.find(key);
            if (it == m_slots.end() || (*it)->imageKey != static_cast<qint64>(skey))
                img = rasterizeSlot(l.clip->slot, m_size);
            imageKey = static_cast<qint64>(skey);
            natural = QSizeF(p.width, p.height);
        } else {
            img = source.frameFor(l);
            if (img.isNull())
                continue;
            key = l.clip->id;
            imageKey = img.cacheKey();
            natural = (l.asset && l.asset->width > 0) ? QSizeF(l.asset->width, l.asset->height) : QSizeF(img.displaySize());
            fit = std::min(p.width / natural.width(), p.height / natural.height());
        }

        const QSize texSize = external ? m_size : img.isNull() ? m_slots[key]->size : img.displaySize();
        Slot &slot = slotFor(key, texSize, filterLut, userLut, gradeLut, false, external);
        if (!img.isNull() && slot.imageKey != imageKey) {
            if (img.isYuv() && yuvSupported())
                convertYuv(cb, slot, img);
            else
                u->uploadTexture(slot.tex.get(), img.isYuv() ? img.toImage() : img.rgba);
            slot.imageKey = imageKey;
        }

        // Geometria: retângulo (com crop) centrado, girado e posicionado.
        const double s = fit * l.scale;
        const double w = natural.width() * s;
        const double h = natural.height() * s;
        Crop crop = l.clip->isText() ? Crop{} : l.clip->crop;
        const double u0 = crop.left, v0 = crop.top, u1 = 1 - crop.right, v1 = 1 - crop.bottom;
        const double cx = p.width / 2.0 + l.x * p.width / 2.0;
        const double cy = p.height / 2.0 + l.y * p.height / 2.0;
        const double rad = qDegreesToRadians(l.rotation);
        const double cs = std::cos(rad), sn = std::sin(rad);
        const std::array<std::array<double, 2>, 4> corners{{{u0, v0}, {u1, v0}, {u0, v1}, {u1, v1}}};
        for (const auto &c : corners) {
            const double lx = (c[0] - 0.5) * w;
            const double ly = (c[1] - 0.5) * h;
            const double px = (cx + lx * cs - ly * sn) * k;
            const double py = (cy + lx * sn + ly * cs) * k;
            double su = c[0], sv = c[1];
            if (l.stab.active())
                stabilizedUv(l.stab, l.stabAspect, su, sv);
            const double tu = l.flipH ? 1 - su : su;
            double tv = l.flipV ? 1 - sv : sv;
            // Textura de render (composto) fica de cabeça para baixo onde o framebuffer tem Y para cima.
            if (external && m_rhi->isYUpInFramebuffer())
                tv = 1 - tv;
            verts.insert(verts.end(), {static_cast<float>(px), static_cast<float>(py), static_cast<float>(tu),
                                       static_cast<float>(tv)});
        }

        LayerUbo ubo;
        fillUbo(ubo, l, p, natural.width() / std::max(1.0, natural.height()));
        const int index = static_cast<int>(draws.size());
        u->updateDynamicBuffer(m_ubo.get(), index * m_uboStride, sizeof ubo, &ubo);
        draws.push_back({&slot, l.blend == BlendMode::Normal, index});
    }
    if (!verts.empty())
        u->updateDynamicBuffer(m_vbo.get(), 0, static_cast<quint32>(verts.size() * sizeof(float)), verts.data());

    const QColor bg = m_transparent ? QColor(0, 0, 0, 0) : QColor(p.background);
    cb->beginPass(m_rtClear.get(), bg, {1.0f, 0}, u);
    const QRhiViewport vp(0, 0, m_size.width(), m_size.height());
    for (const Draw &d : draws) {
        if (!d.normal) {
            // Modo avançado: congela o fundo atual numa cópia e desenha substituindo.
            cb->endPass();
            QRhiResourceUpdateBatch *copy = m_rhi->nextResourceUpdateBatch();
            copy->copyTexture(m_backdrop.get(), m_accum.get());
            cb->beginPass(m_rtLoad.get(), bg, {1.0f, 0}, copy);
        }
        cb->setGraphicsPipeline(d.normal ? m_pipeNormal.get() : m_pipeReplace.get());
        cb->setViewport(vp);
        const QRhiCommandBuffer::DynamicOffset off(0, d.index * m_uboStride);
        cb->setShaderResources(d.slot->srb.get(), 1, &off);
        const QRhiCommandBuffer::VertexInput vin(m_vbo.get(), d.index * 4 * kFloatsPerVertex * sizeof(float));
        cb->setVertexInput(0, 1, &vin);
        cb->draw(4);
    }
    cb->endPass();

    // Libera texturas de clipes que não aparecem há um tempo.
    if (m_frameCounter % 120 == 0) {
        for (auto it = m_slots.begin(); it != m_slots.end();) {
            if (m_frameCounter - (*it)->lastFrame > 240)
                it = m_slots.erase(it);
            else
                ++it;
        }
        std::erase_if(m_children, [&](const auto &kv) { return m_frameCounter - kv.second.second > 240; });
    }
}

bool Compositor::yuvSupported() const
{
    return m_rhi->isTextureFormatSupported(QRhiTexture::R8) && m_rhi->isTextureFormatSupported(QRhiTexture::RG8);
}

void Compositor::convertYuv(QRhiCommandBuffer *cb, Slot &slot, const VideoImage &img)
{
    constexpr int kYuvUboSize = 32; // flip + params
    const bool nv12 = img.format == VideoImage::Format::Nv12;
    const QSize chroma = img.chromaSize();

    // Pipeline única; o layout vem de um alvo e de texturas de referência próprios.
    if (!m_yuvPipe) {
        m_yuvLayoutTarget.reset(m_rhi->newTexture(QRhiTexture::RGBA8, QSize(2, 2), 1, QRhiTexture::RenderTarget));
        m_yuvLayoutTarget->create();
        m_yuvLayoutRt.reset(m_rhi->newTextureRenderTarget({m_yuvLayoutTarget.get()}));
        m_yuvRp.reset(m_yuvLayoutRt->newCompatibleRenderPassDescriptor());
        m_yuvLayoutRt->setRenderPassDescriptor(m_yuvRp.get());
        m_yuvLayoutRt->create();
        m_yuvLayoutPlane.reset(m_rhi->newTexture(QRhiTexture::R8, QSize(2, 2)));
        m_yuvLayoutPlane->create();
        m_yuvLayoutUbo.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kYuvUboSize));
        m_yuvLayoutUbo->create();
        m_yuvLayoutSrb.reset(m_rhi->newShaderResourceBindings());
        const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
        m_yuvLayoutSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, stages, m_yuvLayoutUbo.get()),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_yuvLayoutPlane.get(), m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, m_yuvLayoutPlane.get(), m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage, m_yuvLayoutPlane.get(), m_sampler.get()),
        });
        m_yuvLayoutSrb->create();
        m_yuvPipe.reset(m_rhi->newGraphicsPipeline());
        m_yuvPipe->setShaderStages({{QRhiShaderStage::Vertex, loadShader(QStringLiteral("yuv.vert"))},
                                    {QRhiShaderStage::Fragment, loadShader(QStringLiteral("yuv.frag"))}});
        m_yuvPipe->setVertexInputLayout({});
        m_yuvPipe->setShaderResourceBindings(m_yuvLayoutSrb.get());
        m_yuvPipe->setRenderPassDescriptor(m_yuvRp.get());
        m_yuvPipe->create();
    }

    // Planos: Y em R8; cor em RG8 (NV12) ou dois R8 (I420), na metade do tamanho.
    if (slot.planeFormat != img.format || slot.planeSize != img.size) {
        slot.planes[0].reset(m_rhi->newTexture(QRhiTexture::R8, img.size));
        slot.planes[0]->create();
        slot.planes[1].reset(m_rhi->newTexture(nv12 ? QRhiTexture::RG8 : QRhiTexture::R8, chroma));
        slot.planes[1]->create();
        slot.planes[2].reset();
        if (!nv12) {
            slot.planes[2].reset(m_rhi->newTexture(QRhiTexture::R8, chroma));
            slot.planes[2]->create();
        }
        slot.planeFormat = img.format;
        slot.planeSize = img.size;
        slot.yuvSrb.reset();
    }
    if (!slot.rt) {
        slot.rt.reset(m_rhi->newTextureRenderTarget({slot.tex.get()}));
        slot.rp.reset(slot.rt->newCompatibleRenderPassDescriptor());
        slot.rt->setRenderPassDescriptor(slot.rp.get());
        slot.rt->create();
    }
    if (!slot.yuvUbo) {
        slot.yuvUbo.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kYuvUboSize));
        slot.yuvUbo->create();
    }
    if (!slot.yuvSrb) {
        slot.yuvSrb.reset(m_rhi->newShaderResourceBindings());
        const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
        QRhiTexture *v = nv12 ? slot.planes[1].get() : slot.planes[2].get();
        slot.yuvSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, stages, slot.yuvUbo.get()),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, slot.planes[0].get(), m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, slot.planes[1].get(), m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage, v, m_sampler.get()),
        });
        slot.yuvSrb->create();
    }

    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    const auto upload = [&](int i, int bytesPerPixel, QSize size) {
        QRhiTextureSubresourceUploadDescription d(img.planes[i].constData(), static_cast<quint32>(img.planes[i].size()));
        d.setDataStride(static_cast<quint32>(size.width() * bytesPerPixel));
        u->uploadTexture(slot.planes[i].get(), QRhiTextureUploadDescription({0, 0, d}));
    };
    upload(0, 1, img.size);
    upload(1, nv12 ? 2 : 1, chroma);
    if (!nv12)
        upload(2, 1, chroma);
    // Linha 0 da textura = topo da imagem, como num upload de QImage.
    const float params[8] = {m_rhi->isYUpInNDC() != m_rhi->isYUpInFramebuffer() ? 1.f : 0.f, 0, 0, 0,
                             nv12 ? 1.f : 0.f, static_cast<float>(img.rotation / 90), img.fullRange ? 1.f : 0.f,
                             img.bt709 ? 1.f : 0.f};
    u->updateDynamicBuffer(slot.yuvUbo.get(), 0, sizeof params, params);

    cb->beginPass(slot.rt.get(), Qt::black, {1.0f, 0}, u);
    cb->setGraphicsPipeline(m_yuvPipe.get());
    cb->setViewport(QRhiViewport(0, 0, slot.size.width(), slot.size.height()));
    cb->setShaderResources(slot.yuvSrb.get());
    cb->draw(3);
    cb->endPass();
}

void Compositor::blitTo(QRhiCommandBuffer *cb, QRhiRenderTarget *rt, const QRectF &viewport, const QColor &clear)
{
    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    ensureResources(u);
    QRhiRenderPassDescriptor *rp = rt->renderPassDescriptor();
    const QVector<quint32> fmt = rp->serializedFormat();
    if (!m_blitUbo) {
        m_blitUbo.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 16));
        m_blitUbo->create();
    }
    // A SRB referencia o acumulador, que muda com o tamanho; a pipeline depende do alvo.
    const bool srbStale = !m_blitSrb || m_blitSrbTex != m_accum.get();
    if (srbStale) {
        m_blitSrb.reset(m_rhi->newShaderResourceBindings());
        m_blitSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, m_blitUbo.get()),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_accum.get(),
                                                      m_sampler.get()),
        });
        m_blitSrb->create();
        m_blitSrbTex = m_accum.get();
    }
    if (srbStale || !m_blitPipe || fmt != m_blitRpFormat) {
        m_blitPipe.reset(m_rhi->newGraphicsPipeline());
        m_blitPipe->setShaderStages({{QRhiShaderStage::Vertex, loadShader(QStringLiteral("blit.vert"))},
                                     {QRhiShaderStage::Fragment, loadShader(QStringLiteral("blit.frag"))}});
        m_blitPipe->setVertexInputLayout({});
        m_blitPipe->setShaderResourceBindings(m_blitSrb.get());
        m_blitPipe->setRenderPassDescriptor(rp);
        m_blitPipe->create();
        m_blitRpFormat = fmt;
    }

    const float flip[4] = {m_rhi->isYUpInNDC() != m_rhi->isYUpInFramebuffer() ? 1.f : 0.f, 0, 0, 0};
    u->updateDynamicBuffer(m_blitUbo.get(), 0, sizeof flip, flip);
    cb->beginPass(rt, clear, {1.0f, 0}, u);
    cb->setGraphicsPipeline(m_blitPipe.get());
    // Viewport do RHI tem origem embaixo à esquerda.
    const QSize outSize = rt->pixelSize();
    cb->setViewport(QRhiViewport(viewport.x(), outSize.height() - viewport.y() - viewport.height(), viewport.width(),
                                 viewport.height()));
    cb->setShaderResources(m_blitSrb.get());
    cb->draw(3);
    cb->endPass();
}

void Compositor::readback(QRhiCommandBuffer *cb, QRhiReadbackResult *result)
{
    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    u->readBackTexture({m_accum.get()}, result);
    cb->resourceUpdate(u);
}

QImage Compositor::imageFromReadback(const QRhiReadbackResult &r) const
{
    QImage img(reinterpret_cast<const uchar *>(r.data.constData()), r.pixelSize.width(), r.pixelSize.height(),
               QImage::Format_RGBA8888_Premultiplied);
    img = img.copy();
    if (m_rhi->isYUpInFramebuffer()) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        img.flip(Qt::Vertical);
#else
        img = img.mirrored(false, true);
#endif
    }
    return img;
}

} // namespace clv::render
