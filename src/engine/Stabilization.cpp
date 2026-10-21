// SPDX-License-Identifier: GPL-3.0-or-later
#include "Stabilization.h"

#include "PhaseCorrelation.h"
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

double textureOf(const QImage &img, int cx, int cy)
{
    // Variação local (gradiente médio): blocos lisos não dizem nada sobre o movimento.
    double acc = 0;
    for (int y = cy - kBlock / 2; y < cy + kBlock / 2; y += 2)
        for (int x = cx - kBlock / 2; x < cx + kBlock / 2; x += 2)
            acc += std::abs(grayAt(img, x + 1, y) - grayAt(img, x, y)) + std::abs(grayAt(img, x, y + 1) - grayAt(img, x, y));
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
