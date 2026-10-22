// SPDX-License-Identifier: GPL-3.0-or-later
#include "DecoderFrameSource.h"

#include "FrameInterpolation.h"
#include "Matting.h"
#include "Proxy.h"
#include "core/Evaluate.h"
#include "media/VideoDecoder.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace clv::engine {

struct DecoderFrameSource::Config {
    QString path;      ///< arquivo decodificado (mídia ou proxy)
    QString mattePath; ///< matte de remoção de fundo, se o clipe pede e ele já existe
    Micros offset = 0; ///< tempo da mídia no início de `path` (vídeo interpolado de um trecho)
    QSize maxSize;
    bool operator==(const Config &) const = default;
};

/**
 * Decodificador de um clipe na sua própria thread. A thread é a única que toca nos
 * decodificadores; a de render só lê os quadros prontos ou espera por um.
 */
class DecoderFrameSource::Worker {
public:
    Worker(Config config, int capacity) : config(std::move(config)), m_capacity(std::max(2, capacity))
    {
        m_thread = std::thread([this] { run(); });
    }

    ~Worker()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_wake.notify_all();
        m_thread.join();
    }

    const Config config;
    quint64 lastUsed = 0; ///< só a thread de render mexe
    std::atomic<int> seeks = 0;

    /** Quadro no tempo `t` da mídia; espera a decodificação se ele ainda não estiver pronto. */
    VideoImage get(Micros t, bool *hit)
    {
        std::unique_lock lock(m_mutex);
        m_lastRequested = t;
        if (const Frame *f = covering(t)) {
            *hit = true;
            return f->img;
        }
        *hit = false;
        const quint64 id = ++m_urgentId;
        m_urgent = t;
        m_wake.notify_one();
        m_done.wait(lock, [&] { return m_servedId >= id; });
        return m_served;
    }

    /** Próximos tempos (da mídia) a decodificar, em ordem; substitui o pedido anterior. */
    void want(std::vector<Micros> times)
    {
        {
            std::lock_guard lock(m_mutex);
            m_wanted = std::move(times);
            m_attempted.clear();
        }
        m_wake.notify_one();
    }

private:
    struct Frame {
        VideoImage img;
        Micros from = 0, until = 0; ///< tempos da mídia que mostram este quadro
    };

    const Frame *covering(Micros t) const
    {
        for (const Frame &f : m_cache)
            if (t >= f.from && t < f.until)
                return &f;
        return nullptr;
    }

    std::optional<size_t> nextWanted() const
    {
        for (size_t i = 0; i < m_wanted.size(); ++i)
            if (!covering(m_wanted[i]) && !m_attempted.count(m_wanted[i]))
                return i;
        return std::nullopt;
    }

    void run()
    {
        std::unique_lock lock(m_mutex);
        for (;;) {
            m_wake.wait(lock, [&] { return m_stop || m_urgent || nextWanted(); });
            if (m_stop)
                return;
            Micros t;
            quint64 id = 0;
            if (m_urgent) {
                t = *m_urgent;
                id = m_urgentId;
                m_urgent.reset();
            } else {
                // Continua na lista (protege o quadro do descarte até ser usado); só não é tentado de novo.
                t = m_wanted[*nextWanted()];
                m_attempted.insert(t);
            }
            lock.unlock();
            Frame f = read(t);
            lock.lock();
            if (!f.img.isNull() && !covering(f.from)) {
                m_cache.push_back(f);
                evict();
            }
            if (id) {
                m_served = f.img;
                m_servedId = id;
                m_done.notify_all();
            }
        }
    }

    /** Descarta os quadros que nenhum pedido usa, os mais distantes do último pedido primeiro. */
    void evict()
    {
        const auto needed = [&](const Frame &f) {
            if (m_lastRequested >= f.from && m_lastRequested < f.until)
                return true;
            for (Micros w : m_wanted)
                if (w >= f.from && w < f.until)
                    return true;
            return false;
        };
        const auto distance = [&](const Frame &f) {
            return f.until <= m_lastRequested ? m_lastRequested - f.until : f.from - m_lastRequested;
        };
        while (static_cast<int>(m_cache.size()) > m_capacity) {
            auto victim = m_cache.end();
            for (int pass = 0; pass < 2 && victim == m_cache.end(); ++pass)
                for (auto it = m_cache.begin(); it != m_cache.end(); ++it)
                    if ((pass == 1 || !needed(*it)) && (victim == m_cache.end() || distance(*it) > distance(*victim)))
                        victim = it;
            m_cache.erase(victim);
        }
    }

    // Só na thread do worker.
    Frame read(Micros t)
    {
        if (m_failed)
            return {};
        if (!m_dec) {
            m_dec = std::make_unique<media::VideoDecoder>();
            if (!m_dec->open(config.path, config.maxSize)) {
                m_failed = true;
                m_dec.reset();
                return {};
            }
        }
        // Com matte, o alfa é aplicado em RGBA na CPU; sem, a GPU converte o YUV.
        const bool matte = !config.mattePath.isEmpty();
        Frame f;
        f.img = m_dec->frameImage(t - config.offset, !matte);
        // De volta aos tempos da mídia (os extremos "sempre" ficam como estão).
        const auto toMedia = [&](Micros v) {
            return v == std::numeric_limits<Micros>::min() || v == std::numeric_limits<Micros>::max() ? v : v + config.offset;
        };
        f.from = toMedia(m_dec->frameStart());
        f.until = toMedia(m_dec->frameEnd());
        seeks = m_dec->seeks();
        if (matte && !f.img.isNull())
            applyMatte(t, f.img.rgba);
        return f;
    }

    void applyMatte(Micros t, QImage &img)
    {
        // Sem matte pronto (ainda gerando), o quadro segue opaco.
        if (m_matteFailed)
            return;
        if (!m_matte) {
            m_matte = std::make_unique<media::VideoDecoder>();
            if (!m_matte->open(config.mattePath)) {
                m_matteFailed = true;
                m_matte.reset();
                return;
            }
        }
        QImage m = m_matte->frameAt(t);
        if (m.isNull())
            return;
        if (m.size() != img.size())
            m = m.scaled(img.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        m = m.convertToFormat(QImage::Format_RGBA8888);
        img = img.convertToFormat(QImage::Format_RGBA8888); // cópia própria: o decodificador guarda a dele
        // O matte é cinza: o canal R vira o alfa (reto) do quadro.
        for (int y = 0; y < img.height(); ++y) {
            uchar *dst = img.scanLine(y);
            const uchar *src = m.constScanLine(y);
            for (int x = 0; x < img.width(); ++x)
                dst[x * 4 + 3] = src[x * 4];
        }
    }

    const int m_capacity;
    std::unique_ptr<media::VideoDecoder> m_dec, m_matte;
    bool m_failed = false, m_matteFailed = false;

    std::mutex m_mutex;
    std::condition_variable m_wake, m_done;
    std::deque<Frame> m_cache;
    std::vector<Micros> m_wanted;
    std::set<Micros> m_attempted; ///< pedidos de m_wanted já decodificados (ou que falharam)
    std::optional<Micros> m_urgent;
    quint64 m_urgentId = 0, m_servedId = 0;
    VideoImage m_served;
    Micros m_lastRequested = 0;
    bool m_stop = false;
    std::thread m_thread; // por último: começa com o resto já construído
};

DecoderFrameSource::DecoderFrameSource(QSize maxSize, bool preferProxy) : m_maxSize(maxSize), m_preferProxy(preferProxy) {}

DecoderFrameSource::~DecoderFrameSource() = default;

DecoderFrameSource::Config DecoderFrameSource::configFor(const VisualLayer &layer) const
{
    const MediaAsset &a = *layer.asset;
    Config c;
    c.path = a.path;
    // Câmera lenta fluida: o vídeo com os quadros intermediários, quando já foi gerado.
    QString smooth;
    Micros start = 0;
    int factor = 1;
    if (layer.clip->smoothSlowMotion && a.kind == MediaKind::Video)
        smooth = findInterpolation(a.path, {layer.clip->sourceIn, layer.clip->sourceOut(), 2}, &start, &factor);
    if (!smooth.isEmpty()) {
        c.path = smooth;
        // Cada quadro interpolado representa um instante: vale o mais próximo, não o último que
        // começou (senão um arredondamento de 1 µs repete o quadro anterior).
        const double fileFps = (a.fps > 1 ? a.fps : 30.0) * factor;
        c.offset = start - static_cast<Micros>(kSecond / (2 * fileFps));
    } else if (m_preferProxy) {
        const QString proxy = registeredProxy(a.path);
        if (!proxy.isEmpty())
            c.path = proxy;
    }
    if (layer.clip->removeBackground)
        c.mattePath = registeredMatte(a.path);
    c.maxSize = m_maxSize;
    return c;
}

DecoderFrameSource::Worker &DecoderFrameSource::workerFor(const QString &clipId, const Config &config)
{
    auto &w = m_workers[clipId];
    if (!w || !(w->config == config))
        w = std::make_unique<Worker>(config, m_cacheFrames);
    w->lastUsed = m_tick;
    return *w;
}

VideoImage DecoderFrameSource::frameFor(const VisualLayer &layer)
{
    if (!layer.clip || !layer.asset)
        return {};
    bool hit = false;
    const VideoImage img = workerFor(layer.clip->id, configFor(layer)).get(layer.sourceTime, &hit);
    ++(hit ? m_hits : m_misses);
    return img;
}

void DecoderFrameSource::prefetch(const Project &p, const std::vector<Micros> &times, const std::vector<Micros> &warmTimes)
{
    ++m_tick;
    struct Wanted {
        Config config;
        std::vector<Micros> times;
    };
    std::unordered_map<QString, Wanted> wanted;
    const std::function<void(const std::vector<VisualLayer> &)> collect = [&](const std::vector<VisualLayer> &layers) {
        for (const VisualLayer &l : layers) {
            if (l.nested) {
                collect(l.nested->layers);
                continue;
            }
            if (!l.clip || !l.asset || l.opacity <= 0.001)
                continue;
            auto [it, inserted] = wanted.try_emplace(l.clip->id);
            if (inserted)
                it->second.config = configFor(l);
            it->second.times.push_back(l.sourceTime);
        }
    };
    for (Micros t : times)
        collect(evaluate(p, t).layers);
    // Clipes que vão começar: só o primeiro quadro (pedir um quadro adiantado de um clipe já em
    // andamento faria o decodificador ir e voltar).
    const Micros now = times.empty() ? 0 : times.front();
    for (Micros w : warmTimes)
        for (const VisualLayer &l : evaluate(p, w).layers) {
            if (!l.clip || !l.asset || l.nested || wanted.count(l.clip->id) || w - l.rel <= now)
                continue;
            Wanted &first = wanted[l.clip->id];
            first.config = configFor(l);
            first.times.push_back(sourceTimeAt(p, *l.clip, 0));
        }

    for (auto &[id, w] : m_workers)
        if (!wanted.count(id))
            w->want({});
    for (auto &[id, w] : wanted)
        workerFor(id, w.config).want(std::move(w.times));

    // Clipes que ficaram para trás liberam a thread e o decodificador depois de um tempo.
    if (m_tick % 60 == 0)
        for (auto it = m_workers.begin(); it != m_workers.end();)
            it = it->second->lastUsed + 600 < m_tick ? m_workers.erase(it) : std::next(it);
}

void DecoderFrameSource::setMaxSize(QSize s)
{
    if (s == m_maxSize)
        return;
    m_maxSize = s;
    m_workers.clear();
}

void DecoderFrameSource::prune(const QSet<QString> &liveClipIds)
{
    for (auto it = m_workers.begin(); it != m_workers.end();) {
        if (!liveClipIds.contains(it->first))
            it = m_workers.erase(it);
        else
            ++it;
    }
}

int DecoderFrameSource::decoderSeeks() const
{
    int n = 0;
    for (const auto &[id, w] : m_workers)
        n += w->seeks;
    return n;
}

void DecoderFrameSource::clear()
{
    m_workers.clear();
}

} // namespace clv::engine
