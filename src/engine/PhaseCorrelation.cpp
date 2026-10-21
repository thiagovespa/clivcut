// SPDX-License-Identifier: GPL-3.0-or-later
#include "PhaseCorrelation.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace clv::engine {

namespace {

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


} // namespace

float grayAt(const QImage &img, int x, int y)
{
    x = std::clamp(x, 0, img.width() - 1);
    y = std::clamp(y, 0, img.height() - 1);
    return img.constScanLine(y)[x];
}

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
            ma += grayAt(a, ax - w / 2 + x, ay - h / 2 + y);
            mb += grayAt(b, bx - w / 2 + x, by - h / 2 + y);
        }
    ma /= A.size();
    mb /= A.size();
    for (int y = 0; y < h; ++y) {
        const float wy = 0.5f - 0.5f * std::cos(2.f * static_cast<float>(M_PI) * y / h);
        for (int x = 0; x < w; ++x) {
            const float win = wy * (0.5f - 0.5f * std::cos(2.f * static_cast<float>(M_PI) * x / w));
            const size_t o = static_cast<size_t>(y) * w + x;
            A[o] = cf(static_cast<float>(grayAt(a, ax - w / 2 + x, ay - h / 2 + y) - ma) * win, 0.f);
            B[o] = cf(static_cast<float>(grayAt(b, bx - w / 2 + x, by - h / 2 + y) - mb) * win, 0.f);
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

} // namespace clv::engine
