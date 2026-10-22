// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QQuickAsyncImageProvider>
#include <QQuickImageProvider>

/**
 * image://thumb/<assetId>/<tempoµs>  → miniatura de vídeo/imagem
 * image://wave/<assetId>/<inµs>/<durµs>/<cor> → forma de onda do trecho
 *
 * Geração em thread pool com cache em memória.
 */
/// Descarta pedidos pendentes e espera os em andamento (antes de encerrar o app).
void waitForImageJobs();

class ThumbnailProvider : public QQuickAsyncImageProvider {
public:
    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;
};

/** image://filter/<filtro>/<assetId>/<tempoµs> ou image://filter/<filtro>/sample */
class FilterPreviewProvider : public QQuickAsyncImageProvider {
public:
    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;
};

/**
 * image://icon/<nome>/<rrggbb> → ícone SVG (Lucide, ISC) colorido com a cor do
 * tema, renderizado no tamanho pedido (nítido em qualquer escala de tela).
 */
class IconProvider : public QQuickImageProvider {
public:
    IconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};

/** image://cutout/<n> → prévia do recorte de objeto (quadro clicado com a máscara), trocada pelo controller. */
class CutoutPreviewProvider : public QQuickImageProvider {
public:
    CutoutPreviewProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};

class WaveformProvider : public QQuickAsyncImageProvider {
public:
    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;
};
