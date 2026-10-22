// SPDX-License-Identifier: GPL-3.0-or-later
#include "PreviewItem.h"

#include "ProjectController.h"
#include "core/Evaluate.h"
#include "engine/DecoderFrameSource.h"
#include "engine/RenderCache.h"
#include "media/VideoDecoder.h"
#include "render/Compositor.h"

#include <QElapsedTimer>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>

namespace {

QRectF fitRect(QSizeF content, QSizeF box)
{
    if (content.isEmpty() || box.isEmpty())
        return {};
    const QSizeF s = content.scaled(box, Qt::KeepAspectRatio);
    return {(box.width() - s.width()) / 2, (box.height() - s.height()) / 2, s.width(), s.height()};
}

// Quadros decodificados à frente do cursor, tocando e parado (para avançar quadro a quadro).
constexpr int kPlayLookahead = 8;
constexpr int kPausedLookahead = 2;

// Fonte de um único quadro: o trecho já renderizado do cache.
class ImageSource : public clv::render::FrameSource {
public:
    QImage image;
    QImage frameFor(const clv::VisualLayer &) override { return image; }
};

class PreviewRenderer : public QQuickRhiItemRenderer {
public:
    void initialize(QRhiCommandBuffer *) override
    {
        if (!m_compositor || m_rhi != rhi()) {
            m_rhi = rhi();
            m_compositor = std::make_unique<clv::render::Compositor>(m_rhi);
        }
    }

    void synchronize(QQuickRhiItem *item) override
    {
        auto *ctrl = ProjectController::instance();
        if (!ctrl)
            return;
        m_project = ctrl->snapshot();
        m_time = static_cast<clv::Micros>(ctrl->playhead());
        m_maxRes = static_cast<PreviewItem *>(item)->maxResolution();
        m_playing = ctrl->playing();
        m_frames.setPreferProxy(ctrl->useProxies());
        m_cache = ctrl->renderCacheView();
        m_scopes = ctrl->scopesEnabled();
    }

    // Escopos: no máximo 10 leituras por segundo do quadro composto, reduzido para 320 px de largura.
    void maybeReadScopes(QRhiCommandBuffer *cb)
    {
        if (!m_scopes || m_scopePending || (m_scopeTimer.isValid() && m_scopeTimer.elapsed() < 100))
            return;
        m_scopePending = true;
        m_scopeTimer.start();
        m_scopeRb = std::make_unique<QRhiReadbackResult>();
        m_scopeRb->completed = [this] {
            QImage img = m_compositor->imageFromReadback(*m_scopeRb);
            if (img.width() > 320)
                img = img.scaledToWidth(320, Qt::FastTransformation);
            if (auto *ctrl = ProjectController::instance())
                ctrl->setScopeFrame(img.convertToFormat(QImage::Format_RGBA8888));
            m_scopePending = false;
        };
        m_compositor->readback(cb, m_scopeRb.get());
    }

    // Quadro do cache de renderização no tempo atual, ou nulo se o trecho não estiver renderizado.
    QImage cachedFrame()
    {
        const QString file = m_cache ? m_cache->fileAt(m_time) : QString();
        if (file.isEmpty())
            return {};
        auto &dec = m_cacheDecoders[file];
        if (!dec) {
            // Poucos trechos abertos de cada vez (o atual e os vizinhos).
            if (m_cacheDecoders.size() > 4)
                for (auto it = m_cacheDecoders.begin(); it != m_cacheDecoders.end();)
                    it = it->first != file ? m_cacheDecoders.erase(it) : std::next(it);
            dec = std::make_unique<clv::media::VideoDecoder>();
            if (!dec->open(file)) {
                m_cacheDecoders.erase(file);
                return {};
            }
        }
        const auto chunk = m_time / clv::engine::kRenderChunk;
        return dec->frameAt(m_time - chunk * clv::engine::kRenderChunk);
    }

    void render(QRhiCommandBuffer *cb) override
    {
        QRhiRenderTarget *rt = renderTarget();
        const QSize out = rt->pixelSize();
        if (!m_project) {
            return;
        }
        const QRectF vp = fitRect(QSizeF(m_project->width, m_project->height), QSizeF(out));
        // Resolução de composição: a da área visível, limitada por maxResolution.
        QSize renderSize = vp.size().toSize();
        const int longSide = std::max(renderSize.width(), renderSize.height());
        if (longSide > m_maxRes)
            renderSize = (QSizeF(renderSize) * (static_cast<double>(m_maxRes) / longSide)).toSize();
        renderSize = renderSize.expandedTo(QSize(16, 16));
        m_compositor->setRenderSize(renderSize);
        m_frames.setMaxSize(renderSize);

        // Trecho pré-renderizado: desenha o quadro pronto em vez de compor as camadas.
        const QImage cached = cachedFrame();
        if (!cached.isNull()) {
            m_cacheAsset.width = m_project->width;
            m_cacheAsset.height = m_project->height;
            m_cacheClip.id = QStringLiteral("#render-cache");
            clv::VisualLayer layer;
            layer.trackIndex = 1; // não alimenta o desfoque de fundo (o quadro já o contém)
            layer.clip = &m_cacheClip;
            layer.asset = &m_cacheAsset;
            clv::FrameDesc desc;
            desc.layers.push_back(layer);
            ImageSource src;
            src.image = cached;
            m_compositor->composite(cb, *m_project, desc, src);
            maybeReadScopes(cb);
            m_compositor->blitTo(cb, rt, vp, QColor(0x14, 0x14, 0x17));
            return;
        }

        // Decodificação em segundo plano: o quadro atual (todas as camadas em paralelo), os
        // próximos durante a reprodução e o primeiro quadro dos clipes que vão começar.
        const clv::Micros step = static_cast<clv::Micros>(clv::kSecond / std::max(1.0, m_project->fps));
        std::vector<clv::Micros> ahead{m_time};
        for (int k = 1; k <= (m_playing ? kPlayLookahead : kPausedLookahead); ++k)
            ahead.push_back(m_time + k * step);
        std::vector<clv::Micros> warm;
        if (m_playing)
            warm = {m_time + clv::kSecond / 2, m_time + clv::kSecond};
        m_frames.prefetch(*m_project, ahead, warm);

        const clv::FrameDesc frame = clv::evaluate(*m_project, m_time);
        if (++m_pruneCounter % 120 == 0) {
            QSet<QString> live;
            m_project->forEachClip([&](const clv::Clip &c) { live.insert(c.id); });
            m_frames.prune(live);
        }
        m_compositor->composite(cb, *m_project, frame, m_frames);
        maybeReadScopes(cb);
        m_compositor->blitTo(cb, rt, vp, QColor(0x14, 0x14, 0x17));
    }

private:
    QRhi *m_rhi = nullptr;
    std::unique_ptr<clv::render::Compositor> m_compositor;
    clv::engine::DecoderFrameSource m_frames;
    std::shared_ptr<const clv::Project> m_project;
    clv::Micros m_time = 0;
    int m_maxRes = 1280;
    bool m_playing = false;
    int m_pruneCounter = 0;
    std::shared_ptr<const clv::engine::RenderCacheView> m_cache;
    std::unordered_map<QString, std::unique_ptr<clv::media::VideoDecoder>> m_cacheDecoders;
    clv::Clip m_cacheClip;
    bool m_scopes = false;
    bool m_scopePending = false;
    QElapsedTimer m_scopeTimer;
    std::unique_ptr<QRhiReadbackResult> m_scopeRb;
    clv::MediaAsset m_cacheAsset;
};

} // namespace

PreviewItem::PreviewItem(QQuickItem *parent) : QQuickRhiItem(parent)
{
    if (auto *ctrl = ProjectController::instance()) {
        connect(ctrl, &ProjectController::revisionChanged, this, &QQuickItem::update);
        connect(ctrl, &ProjectController::playheadChanged, this, &QQuickItem::update);
        connect(ctrl, &ProjectController::renderCacheChanged, this, &QQuickItem::update);
        connect(ctrl, &ProjectController::scopesChanged, this, &QQuickItem::update);
        connect(ctrl, &ProjectController::projectChanged, this, &PreviewItem::canvasRectChanged);
    }
}

QQuickRhiItemRenderer *PreviewItem::createRenderer()
{
    return new PreviewRenderer;
}

QRectF PreviewItem::canvasRect() const
{
    auto *ctrl = ProjectController::instance();
    if (!ctrl)
        return {};
    return fitRect(QSizeF(ctrl->canvasWidth(), ctrl->canvasHeight()), size());
}

void PreviewItem::setMaxResolution(int r)
{
    if (r == m_maxResolution)
        return;
    m_maxResolution = r;
    emit maxResolutionChanged();
    update();
}

void PreviewItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickRhiItem::geometryChange(newGeometry, oldGeometry);
    emit canvasRectChanged();
}
