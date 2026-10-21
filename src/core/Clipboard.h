// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Model.h"

#include <QByteArray>
#include <QStringList>

#include <vector>

/** Copiar/colar de clipes e de atributos (inclusive entre projetos). */
namespace clv {

/// Tipo MIME usado na área de transferência do sistema.
inline constexpr const char *kClipboardMime = "application/x-clivcut-clips";

struct ClipboardItem {
    int track = 0; ///< índice da faixa de origem (para manter a disposição)
    TrackKind kind = TrackKind::Video;
    Clip clip;
};

struct Clipboard {
    std::vector<ClipboardItem> items;
    std::vector<MediaAsset> assets; ///< mídias usadas (para colar em outro projeto)
    std::vector<Sequence> compounds; ///< sequências dos clipes compostos copiados (inclusive aninhadas)
    bool empty() const { return items.empty(); }
};

Clipboard copyClips(const Project &p, const QStringList &ids);
QByteArray clipboardToJson(const Clipboard &c);
bool clipboardFromJson(const QByteArray &data, Clipboard &out);

/**
 * Cola no tempo `at`, preservando os deslocamentos entre clipes e, quando
 * possível, as faixas de origem. Mídias ausentes no projeto são adicionadas.
 * Grupos recebem ids novos. Retorna os ids criados.
 */
QStringList pasteClips(Project &p, const Clipboard &c, Micros at);

/**
 * Copia atributos visuais/de áudio compatíveis de `src` para `dst`
 * (transformação, cor/filtro/LUT, máscara, mesclagem, chroma, animações,
 * estilo de texto, volume/fades), sem mudar mídia nem tempo.
 */
void pasteAttributes(Clip &dst, const Clip &src);

} // namespace clv
