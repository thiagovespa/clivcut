// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "render/Compositor.h"

#include <QSet>
#include <QSize>

#include <memory>
#include <unordered_map>
#include <vector>

namespace clv::engine {

/** Quadros decodificados à frente no export e na pré-renderização. */
constexpr int kExportLookahead = 3;

/**
 * FrameSource que mantém um decodificador por clipe (dois clipes da mesma
 * mídia, p.ex. numa transição, não disputam o mesmo decodificador).
 * `maxSize` limita a resolução decodificada (preview usa algo menor).
 *
 * Cada clipe decodifica numa thread própria. `prefetch` diz quais quadros vêm a
 * seguir; eles são decodificados em segundo plano e `frameFor` só espera quando o
 * quadro pedido ainda não está pronto. O resultado é o mesmo da decodificação direta.
 */
class DecoderFrameSource : public render::FrameSource {
public:
    explicit DecoderFrameSource(QSize maxSize = {}, bool preferProxy = false);
    ~DecoderFrameSource() override;

    QImage frameFor(const VisualLayer &layer) override;

    /**
     * Decodifica em segundo plano os quadros de `p` nos instantes `times` da timeline,
     * em ordem de prioridade (o primeiro é o atual). Substitui o pedido anterior.
     * Clipes que só começam depois de `times[0]` e aparecem nos instantes `warmTimes`
     * já abrem e decodificam o primeiro quadro, para o corte não travar.
     */
    void prefetch(const Project &p, const std::vector<Micros> &times, const std::vector<Micros> &warmTimes = {});
    /** Quadros guardados por clipe (o padrão serve para o preview). */
    void setCacheFrames(int frames) { m_cacheFrames = frames; }

    void setMaxSize(QSize s);
    /// Usa proxies registrados (engine/Proxy.h) quando houver.
    void setPreferProxy(bool on) { m_preferProxy = on; }
    /** Fecha decodificadores de clipes que não estão mais no projeto. */
    void prune(const QSet<QString> &liveClipIds);
    void clear();

    /** Pedidos atendidos com um quadro já decodificado / que precisaram esperar (testes). */
    int prefetchHits() const { return m_hits; }
    int prefetchMisses() const { return m_misses; }
    /** Buscas feitas pelos decodificadores (o prefetch não deve fazê-los voltar no arquivo). */
    int decoderSeeks() const;

private:
    class Worker;
    struct Config;
    Config configFor(const VisualLayer &layer) const;
    Worker &workerFor(const QString &clipId, const Config &config);

    std::unordered_map<QString, std::unique_ptr<Worker>> m_workers;
    QSize m_maxSize;
    bool m_preferProxy;
    int m_cacheFrames = 10;
    quint64 m_tick = 0;
    int m_hits = 0, m_misses = 0;
};

} // namespace clv::engine
