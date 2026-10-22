// SPDX-License-Identifier: GPL-3.0-or-later
#include "VideoImage.h"

#include <QTransform>

#include <algorithm>
#include <atomic>

namespace clv {

VideoImage::VideoImage(QImage img) : format(img.isNull() ? Format::None : Format::Rgba), rgba(std::move(img))
{
    size = rgba.size();
}

QSize VideoImage::displaySize() const
{
    if (format == Format::Rgba)
        return rgba.size();
    return rotation == 90 || rotation == 270 ? size.transposed() : size;
}

qint64 VideoImage::newKey()
{
    // QImage::cacheKey usa números de série pequenos nos 32 bits altos; o bit 62 separa as duas faixas.
    static std::atomic<qint64> next{1};
    return (qint64(1) << 62) | next++;
}

QImage VideoImage::toImage(QSize target) const
{
    if (format == Format::Rgba || format == Format::None)
        return target.isEmpty() || rgba.isNull() ? rgba : rgba.scaled(target, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    const int sw = size.width(), sh = size.height();
    // Tamanho de saída antes da rotação.
    QSize out = target.isEmpty() ? size : (rotation == 90 || rotation == 270 ? target.transposed() : target);
    const int w = out.width(), h = out.height();
    const int cw = chromaSize().width();
    // Mesmos coeficientes do shader de conversão.
    const float kr = bt709 ? 1.5748f : 1.402f, kgb = bt709 ? 0.187324f : 0.344136f, kgr = bt709 ? 0.468124f : 0.714136f,
                kb = bt709 ? 1.8556f : 1.772f;
    const float ys = fullRange ? 1.f : 255.f / 219.f, yo = fullRange ? 0.f : 16.f;
    const float cs = fullRange ? 1.f : 255.f / 224.f;
    QImage img(out, QImage::Format_RGBA8888);
    const auto *Y = reinterpret_cast<const uchar *>(planes[0].constData());
    const auto *U = reinterpret_cast<const uchar *>(planes[1].constData());
    const auto *V = reinterpret_cast<const uchar *>(planes[2].constData());
    for (int y = 0; y < h; ++y) {
        uchar *dst = img.scanLine(y);
        const int sy = h == sh ? y : static_cast<int>((y + 0.5) * sh / h);
        for (int x = 0; x < w; ++x) {
            const int sx = w == sw ? x : static_cast<int>((x + 0.5) * sw / w);
            const int ci = (sy / 2) * cw + sx / 2;
            const float cb = ((format == Format::Nv12 ? U[ci * 2] : U[ci]) - 128.f) * cs;
            const float cr = ((format == Format::Nv12 ? U[ci * 2 + 1] : V[ci]) - 128.f) * cs;
            const float l = (Y[sy * sw + sx] - yo) * ys;
            dst[x * 4 + 0] = static_cast<uchar>(std::clamp(l + kr * cr + 0.5f, 0.f, 255.f));
            dst[x * 4 + 1] = static_cast<uchar>(std::clamp(l - kgb * cb - kgr * cr + 0.5f, 0.f, 255.f));
            dst[x * 4 + 2] = static_cast<uchar>(std::clamp(l + kb * cb + 0.5f, 0.f, 255.f));
            dst[x * 4 + 3] = 255;
        }
    }
    if (rotation)
        img = img.transformed(QTransform().rotate(rotation));
    return img;
}

bool VideoImage::operator==(const VideoImage &o) const
{
    if (format != o.format)
        return false;
    if (format == Format::Rgba)
        return rgba == o.rgba;
    return size == o.size && bt709 == o.bt709 && fullRange == o.fullRange && rotation == o.rotation
           && planes[0] == o.planes[0] && planes[1] == o.planes[1] && planes[2] == o.planes[2];
}

} // namespace clv
