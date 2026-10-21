// SPDX-License-Identifier: GPL-3.0-or-later
#include "Tracking.h"

#include "Edit.h"
#include "Evaluate.h"
#include "Keyframes.h"

#include <QtMath>

#include <algorithm>
#include <cmath>

namespace clv {

namespace {

/// Geometria da imagem de um clipe no canvas: centro, tamanho, rotação e espelhamento.
struct Placement {
    double cx, cy, w, h, rad;
    bool flipH, flipV;
};

bool placementOf(const Project &p, const Clip &c, Micros rel, Placement &out)
{
    const MediaAsset *a = c.hasMedia() ? p.asset(c.assetId) : nullptr;
    if (!a || a->width <= 0 || a->height <= 0)
        return false;
    const Micros k = std::clamp<Micros>(rel, 0, c.duration);
    // Mesmo encaixe do compositor: a mídia cabe no canvas e depois escala.
    const double fit = std::min(static_cast<double>(p.width) / a->width, static_cast<double>(p.height) / a->height);
    const double s = fit * propAt(c, Prop::Scale, k);
    out.w = a->width * s;
    out.h = a->height * s;
    out.cx = p.width / 2.0 + propAt(c, Prop::X, k) * p.width / 2.0;
    out.cy = p.height / 2.0 + propAt(c, Prop::Y, k) * p.height / 2.0;
    out.rad = qDegreesToRadians(propAt(c, Prop::Rotation, k));
    out.flipH = c.transform.flipH;
    out.flipV = c.transform.flipV;
    return true;
}

/** Douglas-Peucker sobre (t, x, y): marca os pontos que precisam ficar. */
void simplify(const std::vector<double> &t, const std::vector<double> &x, const std::vector<double> &y, size_t a,
              size_t b, double tol, std::vector<bool> &keep)
{
    if (b <= a + 1)
        return;
    double worst = 0;
    size_t idx = a;
    for (size_t i = a + 1; i < b; ++i) {
        const double f = (t[i] - t[a]) / std::max(1e-9, t[b] - t[a]);
        const double ex = x[a] + (x[b] - x[a]) * f - x[i], ey = y[a] + (y[b] - y[a]) * f - y[i];
        const double d = std::hypot(ex, ey);
        if (d > worst) {
            worst = d;
            idx = i;
        }
    }
    if (worst > tol) {
        keep[idx] = true;
        simplify(t, x, y, a, idx, tol, keep);
        simplify(t, x, y, idx, b, tol, keep);
    }
}

} // namespace

QPointF clipImageToCanvas(const Project &p, const Clip &video, Micros rel, double u, double v)
{
    Placement g{};
    if (!placementOf(p, video, rel, g))
        return {};
    if (g.flipH)
        u = 1 - u;
    if (g.flipV)
        v = 1 - v;
    const double lx = (u - 0.5) * g.w, ly = (v - 0.5) * g.h;
    const double cs = std::cos(g.rad), sn = std::sin(g.rad);
    return {g.cx + lx * cs - ly * sn, g.cy + lx * sn + ly * cs};
}

bool canvasToClipImage(const Project &p, const Clip &video, Micros rel, QPointF canvas, double &u, double &v)
{
    Placement g{};
    if (!placementOf(p, video, rel, g))
        return false;
    const double dx = canvas.x() - g.cx, dy = canvas.y() - g.cy;
    const double cs = std::cos(-g.rad), sn = std::sin(-g.rad);
    u = (dx * cs - dy * sn) / g.w + 0.5;
    v = (dx * sn + dy * cs) / g.h + 0.5;
    if (g.flipH)
        u = 1 - u;
    if (g.flipV)
        v = 1 - v;
    return true;
}

bool applyTrackedPath(Project &p, const QString &followerId, const QString &videoId, Micros anchor,
                      std::vector<TrackedPoint> path)
{
    Clip *follower = edit::clipById(p, followerId);
    const Clip *video = edit::clipById(p, videoId);
    if (!follower || !video || path.empty())
        return false;
    const Micros a = std::max(follower->start, video->start), b = std::min(follower->end(), video->end());
    if (b <= a)
        return false;
    std::sort(path.begin(), path.end(), [](const TrackedPoint &x, const TrackedPoint &y) { return x.t < y.t; });
    auto pointAt = [&](Micros s) {
        auto it = std::lower_bound(path.begin(), path.end(), s, [](const TrackedPoint &x, Micros t) { return x.t < t; });
        if (it == path.begin())
            return QPointF(it->u, it->v);
        if (it == path.end())
            return QPointF(path.back().u, path.back().v);
        const auto &hi = *it, &lo = *(it - 1);
        const double f = hi.t > lo.t ? static_cast<double>(s - lo.t) / (hi.t - lo.t) : 0.0;
        return QPointF(lo.u + (hi.u - lo.u) * f, lo.v + (hi.v - lo.v) * f);
    };
    auto canvasAt = [&](Micros T) {
        const Micros rel = T - video->start;
        const QPointF uv = pointAt(sourceTimeAt(p, *video, rel));
        return clipImageToCanvas(p, *video, rel, uv.x(), uv.y());
    };

    anchor = std::clamp(anchor, a, b - 1);
    const QPointF base = canvasAt(anchor);
    const double x0 = propAt(*follower, Prop::X, anchor - follower->start);
    const double y0 = propAt(*follower, Prop::Y, anchor - follower->start);
    std::vector<double> ts, xs, ys;
    const Micros step = p.frameDuration();
    for (Micros T = a; T < b; T += step) {
        const QPointF c = canvasAt(T);
        ts.push_back(static_cast<double>(T - follower->start));
        xs.push_back(x0 + (c.x() - base.x()) / (p.width / 2.0));
        ys.push_back(y0 + (c.y() - base.y()) / (p.height / 2.0));
    }
    if (ts.empty())
        return false;
    // Tolerância de ~1 px no canvas (em unidades normalizadas de meia largura).
    std::vector<bool> keep(ts.size(), false);
    keep.front() = keep.back() = true;
    simplify(ts, xs, ys, 0, ts.size() - 1, 2.0 / p.width, keep);
    auto &kx = follower->keyframes[static_cast<int>(Prop::X)];
    auto &ky = follower->keyframes[static_cast<int>(Prop::Y)];
    kx.clear();
    ky.clear();
    for (size_t i = 0; i < ts.size(); ++i)
        if (keep[i]) {
            const Micros t = static_cast<Micros>(ts[i]);
            kx.push_back({t, xs[i], Easing::Linear});
            ky.push_back({t, ys[i], Easing::Linear});
        }
    return true;
}

} // namespace clv
