// SPDX-License-Identifier: GPL-3.0-or-later
#include "Stabilize.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

namespace clv {

namespace {

std::mutex g_mutex;
QHash<QString, std::shared_ptr<const MotionTrack>> g_tracks;

constexpr int kZoomLevels = 20;
constexpr double kMaxZoom = 1.5;

/** Média gaussiana do caminho em volta do quadro i (bordas repetem o primeiro/último quadro). */
double smoothAt(const std::vector<double> &v, int i, double sigma)
{
    const int n = static_cast<int>(v.size());
    const int radius = static_cast<int>(std::ceil(3 * sigma));
    double acc = 0, wsum = 0;
    for (int k = -radius; k <= radius; ++k) {
        const double w = std::exp(-0.5 * k * k / (sigma * sigma));
        acc += w * v[static_cast<size_t>(std::clamp(i + k, 0, n - 1))];
        wsum += w;
    }
    return acc / wsum;
}

/// Suavização: até ~1 s de janela (sigma) na intensidade máxima.
double sigmaFor(const MotionTrack &m, double strength) { return std::max(1.0, strength * m.fps); }

StabCorrection correctionAtFrame(const MotionTrack &m, int i, double strength)
{
    StabCorrection c;
    const double sigma = sigmaFor(m, strength);
    c.dx = smoothAt(m.x, i, sigma) - m.x[static_cast<size_t>(i)];
    c.dy = smoothAt(m.y, i, sigma) - m.y[static_cast<size_t>(i)];
    c.angle = smoothAt(m.angle, i, sigma) - m.angle[static_cast<size_t>(i)];
    c.scale = std::exp(smoothAt(m.logScale, i, sigma) - m.logScale[static_cast<size_t>(i)]);
    return c;
}

/** Menor zoom para que a área mostrada, depois da correção, caia toda dentro da imagem. */
double neededZoom(const StabCorrection &c, double aspect)
{
    const double hw = 0.5, hh = 0.5 / aspect;
    const double cs = std::cos(-c.angle), sn = std::sin(-c.angle);
    double z = 1;
    for (const double qx : {-hw, hw})
        for (const double qy : {-hh, hh}) {
            const double px = qx - c.dx, py = qy - c.dy;
            const double wx = px * cs - py * sn, wy = px * sn + py * cs;
            z = std::max(z, std::max(std::abs(wx) / hw, std::abs(wy) / hh) / c.scale);
        }
    return z;
}

} // namespace

double MotionTrack::zoomFor(double strength) const
{
    const int level = std::clamp(static_cast<int>(std::lround(strength * kZoomLevels)), 0, kZoomLevels);
    std::lock_guard lock(m_mutex);
    if (auto it = m_zoom.find(level); it != m_zoom.end())
        return it->second;
    double z = 1;
    // Ignora os 2% piores quadros (um tranco isolado não deve ampliar o clipe inteiro).
    std::vector<double> need;
    need.reserve(x.size());
    for (int i = 0; i < frames(); ++i)
        need.push_back(neededZoom(correctionAtFrame(*this, i, static_cast<double>(level) / kZoomLevels), aspect));
    if (!need.empty()) {
        const size_t k = std::min(need.size() - 1, static_cast<size_t>(need.size() * 0.98));
        std::nth_element(need.begin(), need.begin() + static_cast<std::ptrdiff_t>(k), need.end());
        z = std::min(kMaxZoom, need[k]);
    }
    m_zoom[level] = z;
    return z;
}

StabCorrection stabilizationAt(const MotionTrack &m, Micros t, double strength)
{
    if (strength <= 0 || m.frames() < 2)
        return {};
    const int i = std::clamp(static_cast<int>(std::llround(static_cast<double>(t) * m.fps / kSecond)), 0, m.frames() - 1);
    StabCorrection c = correctionAtFrame(m, i, strength);
    c.zoom = m.zoomFor(strength);
    return c;
}

void stabilizedUv(const StabCorrection &c, double aspect, double &u, double &v)
{
    // Ponto mostrado (centro = 0, unidades de largura) → ponto da origem: desfaz deslocamento,
    // rotação e escala da correção.
    const double qx = u - 0.5, qy = (v - 0.5) / aspect;
    const double px = qx - c.dx, py = qy - c.dy;
    const double cs = std::cos(-c.angle), sn = std::sin(-c.angle);
    const double k = 1.0 / (c.zoom * c.scale);
    u = (px * cs - py * sn) * k + 0.5;
    v = (px * sn + py * cs) * k * aspect + 0.5;
}

QByteArray motionToJson(const MotionTrack &m)
{
    auto arr = [](const std::vector<double> &v) {
        QJsonArray a;
        for (double x : v)
            a.append(std::round(x * 1e6) / 1e6);
        return a;
    };
    return QJsonDocument(QJsonObject{{"format", "clivcut-motion"},
                                     {"version", 1},
                                     {"fps", m.fps},
                                     {"aspect", m.aspect},
                                     {"x", arr(m.x)},
                                     {"y", arr(m.y)},
                                     {"angle", arr(m.angle)},
                                     {"logScale", arr(m.logScale)}})
        .toJson(QJsonDocument::Compact);
}

std::shared_ptr<const MotionTrack> motionFromJson(const QByteArray &data)
{
    const QJsonObject o = QJsonDocument::fromJson(data).object();
    if (o.value("format").toString() != QLatin1String("clivcut-motion"))
        return nullptr;
    auto m = std::make_shared<MotionTrack>();
    m->fps = o.value("fps").toDouble(30);
    m->aspect = o.value("aspect").toDouble(16.0 / 9);
    auto vec = [&](const char *key) {
        std::vector<double> v;
        for (const auto &x : o.value(QLatin1String(key)).toArray())
            v.push_back(x.toDouble());
        return v;
    };
    m->x = vec("x");
    m->y = vec("y");
    m->angle = vec("angle");
    m->logScale = vec("logScale");
    const size_t n = m->x.size();
    if (n < 2 || m->y.size() != n || m->angle.size() != n || m->logScale.size() != n || m->fps <= 0 || m->aspect <= 0)
        return nullptr;
    return m;
}

void registerMotion(const QString &mediaPath, std::shared_ptr<const MotionTrack> track)
{
    std::lock_guard lock(g_mutex);
    g_tracks.insert(mediaPath, std::move(track));
}

std::shared_ptr<const MotionTrack> registeredMotion(const QString &mediaPath)
{
    std::lock_guard lock(g_mutex);
    return g_tracks.value(mediaPath);
}

} // namespace clv
