// SPDX-License-Identifier: GPL-3.0-or-later
#include "ImageProviders.h"

#include "ProjectController.h"
#include "core/Lut.h"
#include "media/MediaProbe.h"

#include <QCache>
#include <QColor>
#include <QMutex>
#include <QLinearGradient>
#include <QPainter>
#include <QFile>
#include <QRunnable>
#include <QSvgRenderer>
#include <QThreadPool>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace {

constexpr int kWaveBucketsPerSecond = 100;

QThreadPool *pool()
{
    static QThreadPool *p = [] {
        auto *tp = new QThreadPool;
        tp->setMaxThreadCount(std::max(2, QThread::idealThreadCount() / 2));
        return tp;
    }();
    return p;
}

} // namespace

void waitForImageJobs()
{
    pool()->clear();
    pool()->waitForDone();
}

namespace {

QString pathFor(const QString &assetId)
{
    auto *ctrl = ProjectController::instance();
    if (!ctrl)
        return {};
    auto snap = ctrl->snapshot();
    const clv::MediaAsset *a = snap->asset(assetId);
    return a ? a->path : QString();
}

QMutex g_thumbMutex;
QCache<QString, QImage> g_thumbs(400);

QMutex g_waveMutex;
QCache<QString, std::vector<float>> g_waves(64);

class Response : public QQuickImageResponse {
public:
    QQuickTextureFactory *textureFactory() const override { return QQuickTextureFactory::textureFactoryForImage(m_image); }
    void finish(QImage img)
    {
        m_image = std::move(img);
        emit finished();
    }

private:
    QImage m_image;
};

std::shared_ptr<std::vector<float>> peaksFor(const QString &path)
{
    {
        QMutexLocker lock(&g_waveMutex);
        if (auto *p = g_waves.object(path))
            return std::make_shared<std::vector<float>>(*p);
    }
    clv::MediaAsset info;
    if (!clv::media::probe(path, info) || info.duration <= 0)
        return std::make_shared<std::vector<float>>();
    const int buckets = std::clamp(static_cast<int>(info.duration * kWaveBucketsPerSecond / clv::kSecond), 1, 200000);
    auto peaks = std::make_shared<std::vector<float>>(clv::media::waveform(path, buckets));
    QMutexLocker lock(&g_waveMutex);
    g_waves.insert(path, new std::vector<float>(*peaks));
    return peaks;
}

} // namespace

QQuickImageResponse *ThumbnailProvider::requestImageResponse(const QString &id, const QSize &requestedSize)
{
    auto *resp = new Response;
    const QStringList parts = id.split(QLatin1Char('/'));
    const QString assetId = parts.value(0);
    const clv::Micros t = parts.value(1).toLongLong();
    const int width = requestedSize.width() > 0 ? requestedSize.width() : 160;
    const QString path = pathFor(assetId);
    // Arredonda o tempo para reaproveitar miniaturas entre zooms.
    const clv::Micros bucket = (t / 250'000) * 250'000;
    const QString key = QStringLiteral("%1|%2|%3").arg(path).arg(bucket).arg(width);
    {
        QMutexLocker lock(&g_thumbMutex);
        if (auto *img = g_thumbs.object(key)) {
            QImage copy = *img;
            QMetaObject::invokeMethod(resp, [resp, copy] { resp->finish(copy); }, Qt::QueuedConnection);
            return resp;
        }
    }
    pool()->start([resp, path, bucket, width, key] {
        QImage img = path.isEmpty() ? QImage() : clv::media::thumbnail(path, bucket, width);
        if (img.isNull()) {
            // Falha não vai para o cache: a mídia pode ser reconectada depois.
            img = QImage(width, width * 9 / 16, QImage::Format_RGBA8888);
            img.fill(QColor(0x2a, 0x2a, 0x30));
        } else {
            QMutexLocker lock(&g_thumbMutex);
            g_thumbs.insert(key, new QImage(img));
        }
        resp->finish(img);
    });
    return resp;
}

QQuickImageResponse *WaveformProvider::requestImageResponse(const QString &id, const QSize &requestedSize)
{
    auto *resp = new Response;
    const QStringList parts = id.split(QLatin1Char('/'));
    const QString path = pathFor(parts.value(0));
    const clv::Micros in = parts.value(1).toLongLong();
    const clv::Micros dur = std::max<clv::Micros>(1, parts.value(2).toLongLong());
    const QColor color(QStringLiteral("#") + parts.value(3, QStringLiteral("5fd4a8")));
    const QSize size(std::clamp(requestedSize.width() > 0 ? requestedSize.width() : 400, 1, 8192),
                     std::clamp(requestedSize.height() > 0 ? requestedSize.height() : 40, 1, 512));
    pool()->start([resp, path, in, dur, color, size] {
        QImage img(size, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        auto peaks = path.isEmpty() ? std::make_shared<std::vector<float>>() : peaksFor(path);
        if (!peaks->empty()) {
            QPainter p(&img);
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            const double mid = size.height() / 2.0;
            for (int x = 0; x < size.width(); ++x) {
                const double t0 = in + static_cast<double>(dur) * x / size.width();
                const double t1 = in + static_cast<double>(dur) * (x + 1) / size.width();
                const int b0 = static_cast<int>(t0 * kWaveBucketsPerSecond / clv::kSecond);
                const int b1 = std::max(b0 + 1, static_cast<int>(t1 * kWaveBucketsPerSecond / clv::kSecond));
                float peak = 0;
                for (int b = b0; b < b1 && b < static_cast<int>(peaks->size()); ++b)
                    if (b >= 0)
                        peak = std::max(peak, (*peaks)[b]);
                const double h = std::max(1.0, static_cast<double>(std::sqrt(peak)) * (size.height() - 2));
                p.drawRect(QRectF(x, mid - h / 2, 1, h));
            }
        }
        resp->finish(img);
    });
    return resp;
}

namespace {

/** Cena de amostra (céu, pele, vegetação, neutros) para prévias sem mídia. */
QImage sampleScene(QSize size)
{
    QImage img(size, QImage::Format_RGBA8888);
    QPainter p(&img);
    QLinearGradient sky(0, 0, 0, size.height() * 0.6);
    sky.setColorAt(0, QColor(70, 130, 200));
    sky.setColorAt(1, QColor(250, 190, 140));
    p.fillRect(QRectF(0, 0, size.width(), size.height() * 0.6), sky);
    p.fillRect(QRectF(0, size.height() * 0.6, size.width(), size.height() * 0.4), QColor(60, 120, 60));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(224, 172, 138));
    p.drawEllipse(QPointF(size.width() * 0.68, size.height() * 0.48), size.height() * 0.2, size.height() * 0.24);
    p.setBrush(QColor(250, 230, 120));
    p.drawEllipse(QPointF(size.width() * 0.2, size.height() * 0.25), size.height() * 0.1, size.height() * 0.1);
    for (int i = 0; i < 6; ++i)
        p.fillRect(QRectF(size.width() * i / 6.0, size.height() * 0.9, size.width() / 6.0 + 1, size.height() * 0.1),
                   QColor::fromHsv(0, 0, 40 * i + 30));
    return img;
}

} // namespace

QQuickImageResponse *FilterPreviewProvider::requestImageResponse(const QString &id, const QSize &requestedSize)
{
    auto *resp = new Response;
    const QStringList parts = id.split(QLatin1Char('/'));
    const QString filter = parts.value(0);
    const QString assetId = parts.value(1);
    const clv::Micros t = parts.value(2).toLongLong();
    const int width = requestedSize.width() > 0 ? requestedSize.width() : 120;
    const QString path = assetId == QLatin1String("sample") ? QString() : pathFor(assetId);
    pool()->start([resp, filter, path, t, width] {
        QImage frame;
        if (!path.isEmpty()) {
            const QString key = QStringLiteral("%1|%2|%3").arg(path).arg((t / 250'000) * 250'000).arg(width);
            {
                QMutexLocker lock(&g_thumbMutex);
                if (auto *img = g_thumbs.object(key))
                    frame = *img;
            }
            if (frame.isNull()) {
                frame = clv::media::thumbnail(path, (t / 250'000) * 250'000, width);
                if (!frame.isNull()) {
                    QMutexLocker lock(&g_thumbMutex);
                    g_thumbs.insert(key, new QImage(frame));
                }
            }
        }
        if (frame.isNull())
            frame = sampleScene(QSize(width, width * 9 / 16));
        const auto lut = clv::lutForKey(filter);
        resp->finish(lut ? clv::applyLut(frame, *lut, 1.0f) : frame.convertToFormat(QImage::Format_RGBA8888));
    });
    return resp;
}

QImage CutoutPreviewProvider::requestImage(const QString &, QSize *size, const QSize &requestedSize)
{
    auto *ctrl = ProjectController::instance();
    QImage img = ctrl ? ctrl->cutoutPreviewImage() : QImage();
    if (!img.isNull() && requestedSize.isValid())
        img = img.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (size)
        *size = img.size();
    return img;
}

QImage IconProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    const QString name = id.section(QLatin1Char('/'), 0, 0);
    const QString color = QStringLiteral("#") + id.section(QLatin1Char('/'), 1, 1);
    const QSize out = requestedSize.isValid() && !requestedSize.isEmpty() ? requestedSize : QSize(24, 24);
    if (size)
        *size = out;
    static QMutex mutex;
    static QHash<QString, QByteArray> sources;
    QByteArray svg;
    {
        QMutexLocker lock(&mutex);
        auto it = sources.find(name);
        if (it == sources.end()) {
            QFile f(QStringLiteral(":/qt/qml/Clivcut/icons/ui/%1.svg").arg(name));
            it = sources.insert(name, f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray());
        }
        svg = *it;
    }
    QImage img(out, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    if (svg.isEmpty())
        return img;
    svg.replace("currentColor", color.toLatin1());
    QSvgRenderer renderer(svg);
    QPainter p(&img);
    renderer.render(&p);
    return img;
}
