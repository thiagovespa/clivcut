// SPDX-License-Identifier: GPL-3.0-or-later
#include "Stabilization.h"

#include "Proxy.h"
#include "media/MediaProbe.h"
#include "media/VideoDecoder.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace clv::engine {

namespace {

constexpr int kAnalysisSide = 320; // lado maior do quadro analisado
constexpr int kBlock = 32;

using cf = std::complex<float>;

void fft1(cf *a, int n, int stride, bool inverse)
{
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i * stride], a[j * stride]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = (inverse ? 2.f : -2.f) * static_cast<float>(M_PI) / static_cast<float>(len);
        const cf wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            cf w(1.f, 0.f);
            for (int k = 0; k < len / 2; ++k) {
                const cf u = a[(i + k) * stride], v = a[(i + k + len / 2) * stride] * w;
                a[(i + k) * stride] = u + v;
                a[(i + k + len / 2) * stride] = u - v;
                w *= wl;
            }
        }
    }
}

/** FFT 2D in-place de uma imagem w×h (potências de 2), linha a linha e coluna a coluna. */
void fft2(std::vector<cf> &a, int w, int h, bool inverse)
{
    for (int y = 0; y < h; ++y)
        fft1(a.data() + static_cast<size_t>(y) * w, w, 1, inverse);
    for (int x = 0; x < w; ++x)
        fft1(a.data() + x, h, w, inverse);
}

float pixel(const QImage &img, int x, int y)
{
    x = std::clamp(x, 0, img.width() - 1);
    y = std::clamp(y, 0, img.height() - 1);
    return img.constScanLine(y)[x];
}

struct Shift {
    double dx = 0, dy = 0, peak = 0;
};

/**
 * Correlação de fase entre a janela w×h de `a` centrada em (ax, ay) e a de `b` centrada em
 * (bx, by): quanto o conteúdo andou de a para b, com precisão de subpixel.
 */
Shift phaseCorrelate(const QImage &a, int ax, int ay, const QImage &b, int bx, int by, int w, int h)
{
    std::vector<cf> A(static_cast<size_t>(w) * h), B(A.size());
    double ma = 0, mb = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            ma += pixel(a, ax - w / 2 + x, ay - h / 2 + y);
            mb += pixel(b, bx - w / 2 + x, by - h / 2 + y);
        }
    ma /= A.size();
    mb /= A.size();
    for (int y = 0; y < h; ++y) {
        const float wy = 0.5f - 0.5f * std::cos(2.f * static_cast<float>(M_PI) * y / h);
        for (int x = 0; x < w; ++x) {
            const float win = wy * (0.5f - 0.5f * std::cos(2.f * static_cast<float>(M_PI) * x / w));
            const size_t o = static_cast<size_t>(y) * w + x;
            A[o] = cf(static_cast<float>(pixel(a, ax - w / 2 + x, ay - h / 2 + y) - ma) * win, 0.f);
            B[o] = cf(static_cast<float>(pixel(b, bx - w / 2 + x, by - h / 2 + y) - mb) * win, 0.f);
        }
    }
    fft2(A, w, h, false);
    fft2(B, w, h, false);
    for (size_t i = 0; i < A.size(); ++i) {
        const cf r = std::conj(A[i]) * B[i];
        const float m = std::abs(r);
        A[i] = m > 1e-9f ? r / m : cf(0.f, 0.f);
    }
    fft2(A, w, h, true);
    size_t best = 0;
    for (size_t i = 1; i < A.size(); ++i)
        if (A[i].real() > A[best].real())
            best = i;
    const int px = static_cast<int>(best % w), py = static_cast<int>(best / w);
    auto at = [&](int x, int y) { return A[static_cast<size_t>((y + h) % h) * w + (x + w) % w].real(); };
    // Refinamento parabólico em cada eixo.
    auto refine = [](double l, double c, double r) {
        const double den = l - 2 * c + r;
        return std::abs(den) > 1e-12 ? std::clamp(0.5 * (l - r) / den, -0.5, 0.5) : 0.0;
    };
    Shift s;
    s.dx = (px > w / 2 ? px - w : px) + refine(at(px - 1, py), at(px, py), at(px + 1, py));
    s.dy = (py > h / 2 ? py - h : py) + refine(at(px, py - 1), at(px, py), at(px, py + 1));
    s.peak = at(px, py) / static_cast<double>(A.size());
    return s;
}

int floorPow2(int v)
{
    int p = 1;
    while (p * 2 <= v)
        p *= 2;
    return p;
}

double textureOf(const QImage &img, int cx, int cy)
{
    // Variação local (gradiente médio): blocos lisos não dizem nada sobre o movimento.
    double acc = 0;
    for (int y = cy - kBlock / 2; y < cy + kBlock / 2; y += 2)
        for (int x = cx - kBlock / 2; x < cx + kBlock / 2; x += 2)
            acc += std::abs(pixel(img, x + 1, y) - pixel(img, x, y)) + std::abs(pixel(img, x, y + 1) - pixel(img, x, y));
    return acc / (kBlock * kBlock / 4);
}

} // namespace

FrameMotion estimateMotion(const QImage &prevIn, const QImage &nextIn)
{
    const QImage prev = prevIn.convertToFormat(QImage::Format_Grayscale8);
    const QImage next = nextIn.convertToFormat(QImage::Format_Grayscale8);
    FrameMotion m;
    const int W = prev.width(), H = prev.height();
    if (W < kBlock * 2 || H < kBlock * 2 || next.size() != prev.size())
        return m;

    // 1) Deslocamento grosso do quadro todo (pega panorâmicas rápidas).
    const Shift global = phaseCorrelate(prev, W / 2, H / 2, next, W / 2, H / 2, floorPow2(W * 9 / 10), floorPow2(H * 9 / 10));
    m.dx = global.dx;
    m.dy = global.dy;

    // 2) Blocos numa grade, já compensados pelo deslocamento grosso.
    struct Pt {
        double px, py, qx, qy;
    };
    std::vector<Pt> pts;
    const int cols = 6, rows = 4;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const int cx = kBlock / 2 + 4 + (W - kBlock - 8) * c / (cols - 1);
            const int cy = kBlock / 2 + 4 + (H - kBlock - 8) * r / (rows - 1);
            if (textureOf(prev, cx, cy) < 2.0)
                continue;
            const int gx = static_cast<int>(std::lround(global.dx)), gy = static_cast<int>(std::lround(global.dy));
            const Shift s = phaseCorrelate(prev, cx, cy, next, cx + gx, cy + gy, kBlock, kBlock);
            if (s.peak < 0.08)
                continue;
            pts.push_back({cx - W / 2.0, cy - H / 2.0, cx - W / 2.0 + gx + s.dx, cy - H / 2.0 + gy + s.dy});
        }

    // 3) Similaridade (escala, rotação, deslocamento) por mínimos quadrados, sem os pontos discrepantes.
    std::vector<bool> keep(pts.size(), true);
    double a = 1, b = 0, tx = global.dx, ty = global.dy;
    int inliers = 0;
    for (int iter = 0; iter < 4; ++iter) {
        double mpx = 0, mpy = 0, mqx = 0, mqy = 0;
        int n = 0;
        for (size_t i = 0; i < pts.size(); ++i)
            if (keep[i]) {
                mpx += pts[i].px;
                mpy += pts[i].py;
                mqx += pts[i].qx;
                mqy += pts[i].qy;
                ++n;
            }
        if (n < 4)
            break;
        mpx /= n;
        mpy /= n;
        mqx /= n;
        mqy /= n;
        double sxx = 0, sab = 0, sba = 0;
        for (size_t i = 0; i < pts.size(); ++i)
            if (keep[i]) {
                const double px = pts[i].px - mpx, py = pts[i].py - mpy;
                const double qx = pts[i].qx - mqx, qy = pts[i].qy - mqy;
                sxx += px * px + py * py;
                sab += px * qx + py * qy;
                sba += px * qy - py * qx;
            }
        if (sxx < 1e-6)
            break;
        a = sab / sxx;
        b = sba / sxx;
        tx = mqx - (a * mpx - b * mpy);
        ty = mqy - (b * mpx + a * mpy);
        inliers = n;
        // Resíduos e novo corte (2,5× a mediana, no mínimo meio pixel).
        std::vector<double> res(pts.size());
        std::vector<double> kept;
        for (size_t i = 0; i < pts.size(); ++i) {
            const double ex = a * pts[i].px - b * pts[i].py + tx - pts[i].qx;
            const double ey = b * pts[i].px + a * pts[i].py + ty - pts[i].qy;
            res[i] = std::hypot(ex, ey);
            if (keep[i])
                kept.push_back(res[i]);
        }
        std::nth_element(kept.begin(), kept.begin() + kept.size() / 2, kept.end());
        const double cut = std::max(0.5, 2.5 * kept[kept.size() / 2]);
        bool changed = false;
        for (size_t i = 0; i < pts.size(); ++i) {
            const bool k = res[i] <= cut;
            changed = changed || k != keep[i];
            keep[i] = k;
        }
        if (!changed)
            break;
    }
    if (inliers >= 4) {
        const double s = std::hypot(a, b), ang = std::atan2(b, a);
        // Valores absurdos para um quadro (corte, flash) ficam só com o deslocamento.
        if (std::abs(std::log(s)) < 0.05 && std::abs(ang) < 0.1) {
            m.scale = s;
            m.angle = ang;
            m.dx = tx;
            m.dy = ty;
            m.inliers = inliers;
        }
    }
    return m;
}

QString motionPathFor(const QString &mediaPath)
{
    const QFileInfo fi(mediaPath);
    const QByteArray k = fi.absoluteFilePath().toUtf8() + '|' + QByteArray::number(fi.size()) + '|' +
                         QByteArray::number(fi.lastModified().toMSecsSinceEpoch()) + "|motion1";
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(k, QCryptographicHash::Sha1).toHex().left(16));
    const QString dir = QDir(QFileInfo(proxyCacheDir()).absolutePath()).filePath(QStringLiteral("motion"));
    return QDir(dir).filePath(QStringLiteral("%1_%2.json").arg(fi.completeBaseName().left(40), hash));
}

bool analyzeMotion(const QString &mediaPath, const QString &outPath, const std::function<bool(double)> &progress,
                   QString *error)
{
    auto fail = [&](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    MediaAsset info;
    if (!media::probe(mediaPath, info, error))
        return false;
    if (info.kind != MediaKind::Video)
        return fail(QCoreApplication::translate("Engine", "Stabilization only applies to videos"));
    media::VideoDecoder dec;
    if (!dec.open(mediaPath, QSize(kAnalysisSide, kAnalysisSide), error, true))
        return false;
    const QSize size = dec.outputSize();
    const double fps = info.fps > 1 ? std::min(info.fps, 60.0) : 30.0;
    const Micros duration = std::max<Micros>(dec.duration(), info.duration);
    const std::int64_t total = std::max<std::int64_t>(2, std::llround(static_cast<double>(duration) * fps / kSecond));

    auto track = std::make_shared<MotionTrack>();
    track->fps = fps;
    track->aspect = static_cast<double>(size.width()) / std::max(1, size.height());
    double x = 0, y = 0, angle = 0, logScale = 0;
    QImage prev;
    for (std::int64_t i = 0; i < total; ++i) {
        const QImage frame =
            dec.frameAt(static_cast<Micros>(std::llround(i * kSecond / fps))).convertToFormat(QImage::Format_Grayscale8);
        if (frame.isNull() || frame.size() != size)
            break;
        if (!prev.isNull()) {
            const FrameMotion m = estimateMotion(prev, frame);
            x += m.dx / size.width();
            y += m.dy / size.width();
            angle += m.angle;
            logScale += std::log(m.scale);
        }
        track->x.push_back(x);
        track->y.push_back(y);
        track->angle.push_back(angle);
        track->logScale.push_back(logScale);
        prev = frame;
        if (progress && !progress(static_cast<double>(i + 1) / total))
            return fail(QCoreApplication::translate("Engine", "Canceled"));
    }
    if (track->frames() < 2)
        return fail(QCoreApplication::translate("Engine", "Could not read the video frames"));

    QDir().mkpath(QFileInfo(outPath).absolutePath());
    QSaveFile f(outPath);
    if (!f.open(QIODevice::WriteOnly) || f.write(motionToJson(*track)) < 0 || !f.commit())
        return fail(QCoreApplication::translate("Engine", "Could not write %1").arg(outPath));
    registerMotion(mediaPath, std::move(track));
    return true;
}

bool discoverMotion(const QString &mediaPath)
{
    if (registeredMotion(mediaPath))
        return true;
    QFile f(motionPathFor(mediaPath));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    auto track = motionFromJson(f.readAll());
    if (!track)
        return false;
    registerMotion(mediaPath, std::move(track));
    return true;
}

} // namespace clv::engine
