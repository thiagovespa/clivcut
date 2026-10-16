// SPDX-License-Identifier: GPL-3.0-or-later
#include "Effects.h"

#include <QCoreApplication>

namespace clv {

const std::vector<VideoEffect> &videoEffects()
{
    static const std::vector<VideoEffect> effects{
        {QStringLiteral("shake"), QString::fromUtf8(QT_TRANSLATE_NOOP("Effects", "Shake")), 1},
        {QStringLiteral("blur"), QString::fromUtf8(QT_TRANSLATE_NOOP("Effects", "Blur")), 2},
        {QStringLiteral("glitch"), QString::fromUtf8(QT_TRANSLATE_NOOP("Effects", "Glitch")), 3},
        {QStringLiteral("rgbSplit"), QString::fromUtf8(QT_TRANSLATE_NOOP("Effects", "RGB split")), 4},
        {QStringLiteral("vhs"), QString::fromUtf8(QT_TRANSLATE_NOOP("Effects", "VHS")), 5},
        {QStringLiteral("glow"), QString::fromUtf8(QT_TRANSLATE_NOOP("Effects", "Glow")), 6},
        {QStringLiteral("grain"), QString::fromUtf8(QT_TRANSLATE_NOOP("Effects", "Film grain")), 7},
        {QStringLiteral("pixelate"), QString::fromUtf8(QT_TRANSLATE_NOOP("Effects", "Pixelate")), 8},
        {QStringLiteral("mirror"), QString::fromUtf8(QT_TRANSLATE_NOOP("Effects", "Mirror")), 9},
        {QStringLiteral("snow"), QString::fromUtf8(QT_TRANSLATE_NOOP("Effects", "Snow")), 10},
        {QStringLiteral("zoomPulse"), QString::fromUtf8(QT_TRANSLATE_NOOP("Effects", "Zoom pulse")), 11},
        {QStringLiteral("strobe"), QString::fromUtf8(QT_TRANSLATE_NOOP("Effects", "Strobe")), 12},
    };
    return effects;
}

int effectShaderIndex(const QString &id)
{
    if (id.isEmpty())
        return 0;
    for (const auto &e : videoEffects())
        if (e.id == id)
            return e.shaderIndex;
    return 0;
}

} // namespace clv
