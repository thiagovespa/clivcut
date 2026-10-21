// SPDX-License-Identifier: GPL-3.0-or-later
#include "MotionTracking.h"

#include "PhaseCorrelation.h"
#include "media/MediaProbe.h"
#include "media/VideoDecoder.h"

#include <QCoreApplication>
#include <QImage>

#include <algorithm>
#include <cmath>

namespace clv::engine {

namespace {
constexpr int kTrackSide = 640;   // lado maior do quadro analisado
constexpr int kWindowFrames = 30; // para trás: decodifica blocos de ~1 s para a frente e percorre ao contrário
} // namespace

std::vector<TrackPoint> trackRegion(const QString &mediaPath, Micros from, Micros to, double u, double v, double w,
                                    double h, const std::function<bool(double)> &progress, QString *error)
{
    std::vector<TrackPoint> out;
    MediaAsset info;
    if (!media::probe(mediaPath, info, error))
        return out;
    if (info.kind != MediaKind::Video) {
        if (error)
            *error = QCoreApplication::translate("Engine", "Tracking only applies to videos");
        return out;
    }
    media::VideoDecoder dec;
    if (!dec.open(mediaPath, QSize(kTrackSide, kTrackSide), error, true))
        return out;
    const QSize size = dec.outputSize();
    const int W = size.width(), H = size.height();
    const double fps = info.fps > 1 ? std::min(info.fps, 60.0) : 30.0;
    const Micros step = static_cast<Micros>(std::llround(kSecond / fps));
    const Micros duration = std::max<Micros>(dec.duration(), info.duration);
    from = std::clamp<Micros>(from, 0, std::max<Micros>(0, duration - step));
    to = std::clamp<Micros>(to, 0, std::max<Micros>(0, duration - step));
    const int dir = to >= from ? 1 : -1;
    const int count = static_cast<int>(std::llabs(to - from) / step) + 1;
    // Tempos pela grade de quadros (n / fps), sem acumular arredondamento; a consulta é feita
    // 1 ms dentro do quadro para o decodificador não devolver o anterior.
    const std::int64_t n0 = std::llround(static_cast<double>(from) * fps / kSecond);
    auto frameTime = [&](int i) { return static_cast<Micros>(std::llround((n0 + dir * i) * kSecond / fps)); };

    // Janela: um pouco maior que a região (pega o contorno), em potência de 2.
    const int side = std::max(w * W, h * H) > 0 ? static_cast<int>(std::max(w * W, h * H) * 1.5) : 64;
    const int P = std::clamp(floorPow2(std::max(32, side)), 32, std::min(256, floorPow2(std::min(W, H))));

    auto gray = [&](Micros t) { return dec.frameAt(t).convertToFormat(QImage::Format_Grayscale8); };
    std::vector<QImage> window; // para trás: quadros do bloco atual, já na ordem de processamento
    int windowStart = 0;
    auto frameAtIndex = [&](int i) -> QImage {
        if (dir > 0)
            return gray(frameTime(i) + 1000);
        if (i < windowStart || i >= windowStart + static_cast<int>(window.size())) {
            // Decodifica o bloco [i, i + kWindowFrames) para a frente (tempos decrescentes ao contrário).
            window.clear();
            windowStart = i;
            const int n = std::min(kWindowFrames, count - i);
            std::vector<QImage> asc;
            for (int k = n - 1; k >= 0; --k)
                asc.push_back(gray(frameTime(i + k) + 1000));
            window.assign(asc.rbegin(), asc.rend());
        }
        return window[static_cast<size_t>(i - windowStart)];
    };

    QImage prev = frameAtIndex(0);
    if (prev.isNull() || prev.size() != size) {
        if (error)
            *error = QCoreApplication::translate("Engine", "Could not read the video frames");
        return out;
    }
    double cx = u * W, cy = v * H;
    const QImage tmpl = prev;
    const int tx = static_cast<int>(std::lround(cx)), ty = static_cast<int>(std::lround(cy));
    const double tfx = cx - tx, tfy = cy - ty;
    out.push_back({frameTime(0), u, v, 1.0});
    for (int i = 1; i < count; ++i) {
        const QImage cur = frameAtIndex(i);
        if (cur.isNull() || cur.size() != size)
            break;
        // 1) Quanto andou desde o quadro anterior.
        const int icx = static_cast<int>(std::lround(cx)), icy = static_cast<int>(std::lround(cy));
        const double px = cx, py = cy;
        const Shift s = phaseCorrelate(prev, icx, icy, cur, icx, icy, P, P);
        double conf = s.peak;
        if (s.peak > 0.05) {
            cx += s.dx;
            cy += s.dy;
            // Movimentos grandes saem subestimados (o objeto deixa o centro da janela):
            // um segundo passo, com a janela já no lugar novo, mede o que faltou.
            const int rx = static_cast<int>(std::lround(cx)), ry = static_cast<int>(std::lround(cy));
            const Shift r = phaseCorrelate(prev, icx, icy, cur, rx, ry, P, P);
            if (r.peak > 0.05 && std::abs(r.dx) < P / 4.0 && std::abs(r.dy) < P / 4.0) {
                cx = px + (rx - icx) + r.dx;
                cy = py + (ry - icy) + r.dy;
                conf = std::max(conf, r.peak);
            }
        }
        // 2) Reancora no aspecto inicial: segura a deriva acumulada quando o objeto ainda se parece com ele.
        const int jcx = static_cast<int>(std::lround(cx)), jcy = static_cast<int>(std::lround(cy));
        const Shift a = phaseCorrelate(tmpl, tx, ty, cur, jcx, jcy, P, P);
        if (a.peak > 0.12 && std::abs(a.dx) < P / 4.0 && std::abs(a.dy) < P / 4.0) {
            // Quanto mais parecido com o início, mais confia na âncora.
            const double k = std::clamp((a.peak - 0.12) / 0.25, 0.3, 1.0);
            cx = (1 - k) * cx + k * (jcx + a.dx + tfx);
            cy = (1 - k) * cy + k * (jcy + a.dy + tfy);
            conf = std::max(conf, a.peak);
        }
        cx = std::clamp(cx, 0.0, W - 1.0);
        cy = std::clamp(cy, 0.0, H - 1.0);
        out.push_back({frameTime(i), cx / W, cy / H, conf});
        prev = cur;
        if (progress && !progress(static_cast<double>(i) / (count - 1))) {
            if (error)
                *error = QCoreApplication::translate("Engine", "Canceled");
            out.clear();
            return out;
        }
    }
    return out;
}

} // namespace clv::engine
