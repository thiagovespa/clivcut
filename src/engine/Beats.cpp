// SPDX-License-Identifier: GPL-3.0-or-later
#include "Beats.h"

#include "Transcriber.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>

namespace clv::engine {

namespace {

constexpr int kRate = 16000;
constexpr int kFrame = 1024;
constexpr int kHop = 160; // 10 ms: 100 quadros de ataque por segundo
constexpr double kFps = static_cast<double>(kRate) / kHop;
constexpr int kBands = 32;

void fft(std::vector<std::complex<float>> &a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.f * static_cast<float>(M_PI) / static_cast<float>(len);
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.f, 0.f);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

/** Curva de ataques: aumento da energia (log) em bandas logarítmicas, quadro a quadro. */
std::vector<double> onsetEnvelope(const std::vector<float> &pcm)
{
    // Quadros centrados em t·hop (meio quadro de silêncio antes do início).
    const auto frames = static_cast<int>(pcm.size() / kHop);
    std::vector<int> bandOf(kFrame / 2, -1);
    const double lo = std::log(30.0), hi = std::log(7000.0);
    for (int k = 1; k < kFrame / 2; ++k) {
        const double f = static_cast<double>(k) * kRate / kFrame;
        if (f >= 30 && f <= 7000)
            bandOf[k] = std::min(kBands - 1, static_cast<int>((std::log(f) - lo) / (hi - lo) * kBands));
    }
    std::vector<float> window(kFrame);
    for (int i = 0; i < kFrame; ++i)
        window[i] = 0.5f - 0.5f * std::cos(2.f * static_cast<float>(M_PI) * i / kFrame);

    std::vector<double> env(frames, 0.0);
    std::vector<double> prev(kBands, 0.0), cur(kBands);
    std::vector<std::complex<float>> buf(kFrame);
    for (int t = 0; t < frames; ++t) {
        const std::int64_t first = static_cast<std::int64_t>(t) * kHop - kFrame / 2;
        for (int i = 0; i < kFrame; ++i) {
            const std::int64_t s = first + i;
            const float x = s >= 0 && s < static_cast<std::int64_t>(pcm.size()) ? pcm[static_cast<size_t>(s)] : 0.f;
            buf[i] = {std::isfinite(x) ? x * window[i] : 0.f, 0.f};
        }
        fft(buf);
        std::fill(cur.begin(), cur.end(), 0.0);
        for (int k = 1; k < kFrame / 2; ++k)
            if (bandOf[k] >= 0)
                cur[bandOf[k]] += std::norm(buf[k]);
        double flux = 0;
        for (int b = 0; b < kBands; ++b) {
            cur[b] = std::log1p(100.0 * std::sqrt(cur[b]));
            if (t > 0)
                flux += std::max(0.0, cur[b] - prev[b]);
        }
        env[t] = flux;
        std::swap(prev, cur);
    }

    // Tira a tendência lenta (média móvel de 1 s), fica com a parte positiva e normaliza.
    const int half = static_cast<int>(kFps / 2);
    std::vector<double> prefix(frames + 1, 0.0);
    for (int t = 0; t < frames; ++t)
        prefix[t + 1] = prefix[t] + env[t];
    std::vector<double> out(frames);
    for (int t = 0; t < frames; ++t) {
        const int a = std::max(0, t - half), b = std::min(frames, t + half + 1);
        out[t] = std::max(0.0, env[t] - (prefix[b] - prefix[a]) / (b - a));
    }
    double sq = 0;
    for (double v : out)
        sq += v * v;
    const double sd = frames > 0 ? std::sqrt(sq / frames) : 0;
    if (sd < 1e-9)
        return {}; // silêncio ou som constante: não há ataques
    for (double &v : out)
        v /= sd;
    return out;
}

/** Período (em quadros, fracionário) pela autocorrelação, ponderada perto de 120 BPM. */
double estimatePeriod(const std::vector<double> &env)
{
    const int minLag = static_cast<int>(kFps * 60 / 240), maxLag = static_cast<int>(kFps * 60 / 40);
    const int n = static_cast<int>(env.size());
    if (n < maxLag * 2)
        return 0;
    std::vector<double> score(maxLag + 2, 0.0);
    for (int lag = minLag - 1; lag <= maxLag + 1; ++lag) {
        double acc = 0;
        for (int t = lag; t < n; ++t)
            acc += env[t] * env[t - lag];
        const double bpm = 60 * kFps / lag;
        const double oct = std::log2(bpm / 120.0);
        score[lag] = acc / (n - lag) * std::exp(-0.5 * oct * oct);
    }
    int best = minLag;
    for (int lag = minLag; lag <= maxLag; ++lag)
        if (score[lag] > score[best])
            best = lag;
    // Refinamento parabólico entre os vizinhos.
    const double a = score[best - 1], b = score[best], c = score[best + 1];
    const double den = a - 2 * b + c;
    const double delta = std::abs(den) > 1e-12 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
    return best + delta;
}

} // namespace

BeatResult detectBeatsPcm16k(const std::vector<float> &pcm, Micros origin)
{
    BeatResult r;
    const std::vector<double> env = onsetEnvelope(pcm);
    const double period = estimatePeriod(env);
    if (period <= 0)
        return r;
    const int n = static_cast<int>(env.size());

    // Pontuação local: ataques suavizados por uma gaussiana estreita (period/32).
    const double sigma = std::max(1.0, period / 32);
    const int radius = static_cast<int>(std::ceil(3 * sigma));
    std::vector<double> local(n, 0.0);
    for (int t = 0; t < n; ++t)
        for (int d = -radius; d <= radius; ++d)
            if (t + d >= 0 && t + d < n)
                local[t] += env[t + d] * std::exp(-0.5 * d * d / (sigma * sigma));

    // Programação dinâmica: cada batida vem de outra entre P/2 e 2P antes, com custo
    // pelo quanto o intervalo foge do período.
    constexpr double kTightness = 100;
    std::vector<double> cum(n);
    std::vector<int> back(n, -1);
    const int lo = static_cast<int>(std::round(period / 2)), hi = static_cast<int>(std::round(2 * period));
    for (int t = 0; t < n; ++t) {
        double best = -1e300;
        for (int tau = t - hi; tau <= t - lo; ++tau) {
            if (tau < 0)
                continue;
            const double x = std::log((t - tau) / period);
            const double s = cum[tau] - kTightness * x * x;
            if (s > best) {
                best = s;
                back[t] = tau;
            }
        }
        cum[t] = local[t] + (back[t] >= 0 ? best : 0.0);
    }

    // Última batida: a de maior pontuação acumulada dentro do último período.
    int last = -1;
    for (int t = std::max(0, n - static_cast<int>(std::ceil(period))); t < n; ++t)
        if (last < 0 || cum[t] > cum[last])
            last = t;
    std::vector<int> frames;
    for (int t = last; t >= 0; t = back[t])
        frames.push_back(t);
    std::reverse(frames.begin(), frames.end());

    // Descarta batidas fracas nas pontas (introdução e final sem ritmo).
    double sq = 0;
    for (int t : frames)
        sq += local[t] * local[t];
    const double threshold = 0.5 * std::sqrt(sq / frames.size());
    while (!frames.empty() && local[frames.front()] < threshold)
        frames.erase(frames.begin());
    while (!frames.empty() && local[frames.back()] < threshold)
        frames.pop_back();
    if (frames.size() < 2)
        return r;

    for (int t : frames) {
        r.beats.push_back(origin + static_cast<Micros>(std::llround(t * kHop * static_cast<double>(kSecond) / kRate)));
        r.strength.push_back(local[t]);
    }
    // Andamento final pela reta que melhor passa pelas batidas (mais preciso que o atraso inteiro).
    const double m = static_cast<double>(frames.size());
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
        sx += i;
        sy += frames[i];
        sxx += static_cast<double>(i) * i;
        sxy += static_cast<double>(i) * frames[i];
    }
    const double slope = (m * sxy - sx * sy) / (m * sxx - sx * sx);
    r.bpm = slope > 0 ? 60 * kFps / slope : 0;
    return r;
}

BeatResult detectBeats(const Project &p, int trackIndex, Micros start, Micros end,
                       const std::function<bool(double)> &progress)
{
    if (end <= start)
        return {};
    return detectBeatsPcm16k(renderProjectAudio16k(p, trackIndex, start, end, progress), start);
}

std::vector<Micros> everyNthBeat(const BeatResult &r, int every)
{
    if (every <= 1)
        return r.beats;
    int bestPhase = 0;
    double bestSum = -1;
    for (int phase = 0; phase < every; ++phase) {
        double sum = 0;
        int count = 0;
        for (size_t i = phase; i < r.strength.size(); i += every, ++count)
            sum += r.strength[i];
        if (count > 0 && sum / count > bestSum) {
            bestSum = sum / count;
            bestPhase = phase;
        }
    }
    std::vector<Micros> out;
    for (size_t i = bestPhase; i < r.beats.size(); i += every)
        out.push_back(r.beats[i]);
    return out;
}

} // namespace clv::engine
