// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <vector>

/**
 * Efeitos de vídeo feitos no shader do compositor (layer.frag). O índice é o
 * valor passado ao shader; mantenha a ordem em sincronia com effectSample/effectPost.
 */
namespace clv {

struct VideoEffect {
    QString id;
    QString name; ///< em inglês; traduzir com QCoreApplication::translate("Effects", ...)
    int shaderIndex;
};

const std::vector<VideoEffect> &videoEffects();
/** Índice do shader (0 = nenhum). */
int effectShaderIndex(const QString &id);

} // namespace clv
