// SPDX-License-Identifier: GPL-3.0-or-later
#include "ObjectCutout.h"

#include "AiModels.h"
#include "Proxy.h"
#include "media/Encoder.h"
#include "media/MediaProbe.h"
#include "media/VideoDecoder.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <map>
#include <vector>

#ifdef CLIVCUT_HAVE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace clv::engine {

namespace {

// Dimensões fixas do SAM 2.1 Hiera-Tiny (imagem de 1024 px, recursos a 1/16).
constexpr int kImage = 1024;
constexpr int kFeat = 64;
constexpr int kLowRes = 256;
constexpr int kHidden = 256;
constexpr int kMemDim = 64;
constexpr int kNumMaskMem = 7;   ///< o quadro clicado + os 6 anteriores
constexpr int kMaxObjPtrs = 16;
constexpr double kMaxFps = 30;   ///< acima disso, um quadro a cada tantos

struct Entry {
    QString key;
    Micros start = 0, end = 0;
    double fps = 30;
    QString path;
};

QMutex g_mutex;
QHash<QString, QList<Entry>> g_entries; ///< por mídia

QString cacheDir()
{
    return QDir(QFileInfo(proxyCacheDir()).absolutePath()).filePath(QStringLiteral("cutouts"));
}

QString filePrefix(const QString &mediaPath)
{
    const QFileInfo fi(mediaPath);
    const QByteArray k = fi.absoluteFilePath().toUtf8() + '|' + QByteArray::number(fi.size()) + '|' +
                         QByteArray::number(fi.lastModified().toMSecsSinceEpoch()) + "|sam21t";
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(k, QCryptographicHash::Sha1).toHex().left(16));
    return QStringLiteral("%1_%2").arg(fi.completeBaseName().left(40), hash);
}

std::int64_t frameIndex(Micros t, double fps)
{
    return static_cast<std::int64_t>(std::floor(static_cast<double>(t) * fps / kSecond + 1e-6));
}

Micros frameTime(std::int64_t i, double fps)
{
    return static_cast<Micros>(std::llround(static_cast<double>(i) * kSecond / fps));
}

/** Logits 256×256 → matte do tamanho pedido, com borda suave de ~1 px (bilinear, como o SAM 2). */
QImage logitsToMatte(const std::vector<float> &logits, QSize size)
{
    QImage out(size, QImage::Format_Grayscale8);
    const int W = size.width(), H = size.height();
    for (int y = 0; y < H; ++y) {
        const double sy = std::clamp((y + 0.5) * kLowRes / H - 0.5, 0.0, kLowRes - 1.0);
        const int y0 = static_cast<int>(sy), y1 = std::min(y0 + 1, kLowRes - 1);
        const double fy = sy - y0;
        uchar *q = out.scanLine(y);
        for (int x = 0; x < W; ++x) {
            const double sx = std::clamp((x + 0.5) * kLowRes / W - 0.5, 0.0, kLowRes - 1.0);
            const int x0 = static_cast<int>(sx), x1 = std::min(x0 + 1, kLowRes - 1);
            const double fx = sx - x0;
            const double v = (logits[y0 * kLowRes + x0] * (1 - fx) + logits[y0 * kLowRes + x1] * fx) * (1 - fy) +
                             (logits[y1 * kLowRes + x0] * (1 - fx) + logits[y1 * kLowRes + x1] * fx) * fy;
            q[x] = static_cast<uchar>(std::lround(std::clamp((v + 2.0) / 4.0, 0.0, 1.0) * 255));
        }
    }
    return out;
}

QSize matteSizeFor(QSize video)
{
    QSize s = video;
    if (std::max(s.width(), s.height()) > kCutoutMatteMaxSide)
        s = s.scaled(kCutoutMatteMaxSide, kCutoutMatteMaxSide, Qt::KeepAspectRatio);
    return {std::max(2, s.width() & ~1), std::max(2, s.height() & ~1)};
}

} // namespace

bool objectCutoutAvailable()
{
#ifdef CLIVCUT_HAVE_ONNX
    return true;
#else
    return false;
#endif
}

bool objectCutoutModelsInstalled()
{
    return std::all_of(kCutoutModels.begin(), kCutoutModels.end(), [](const QString &id) { return aiModelInstalled(id); });
}

QString cutoutKey(const ObjectCutout &cutout)
{
    QByteArray k = QByteArray::number(cutout.time);
    for (const CutoutPoint &p : cutout.points)
        k += '|' + QByteArray::number(p.u, 'f', 5) + ',' + QByteArray::number(p.v, 'f', 5) + (p.positive ? "+" : "-");
    return QString::fromLatin1(QCryptographicHash::hash(k, QCryptographicHash::Sha1).toHex().left(12));
}

#ifdef CLIVCUT_HAVE_ONNX

namespace {

/// Saída do codificador de imagem para um quadro.
struct Encoded {
    std::vector<float> feat, nomem, pos, hr0, hr1;
};

/// O que o rastreamento guarda de cada quadro processado.
struct FrameMemory {
    std::vector<float> feat, pos; ///< memória (64×64×64), só dos quadros recentes
    std::vector<float> ptr;       ///< ponteiro do objeto (256)
};

} // namespace

struct ObjectSegmenter::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "clivcut-sam2"};
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::unique_ptr<Ort::Session> encoder, heads, memEncoder, memAttention;
    QString cachedPath;
    Micros cachedTime = -1;
    Encoded cached;

    Ort::Value tensor(const std::vector<float> &data, std::vector<int64_t> shape) const
    {
        return Ort::Value::CreateTensor<float>(mem, const_cast<float *>(data.data()), data.size(), shape.data(), shape.size());
    }

    static std::vector<float> copy(const Ort::Value &v)
    {
        const float *p = v.GetTensorData<float>();
        return {p, p + v.GetTensorTypeAndShapeInfo().GetElementCount()};
    }

    std::vector<Ort::Value> run(Ort::Session &s, const std::vector<const char *> &in, std::vector<Ort::Value> &values,
                                const std::vector<const char *> &out) const
    {
        return s.Run(Ort::RunOptions{nullptr}, in.data(), values.data(), values.size(), out.data(), out.size());
    }

    /** RGBA (qualquer tamanho) → 1024×1024, normalizado (média e desvio do ImageNet), e o codificador. */
    Encoded encode(const QImage &frame)
    {
        const QImage img = frame.scaled(kImage, kImage, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                               .convertToFormat(QImage::Format_RGBA8888);
        static constexpr std::array<float, 3> kMean{0.485f, 0.456f, 0.406f}, kStd{0.229f, 0.224f, 0.225f};
        std::vector<float> input(static_cast<size_t>(3) * kImage * kImage);
        for (int y = 0; y < kImage; ++y) {
            const uchar *p = img.constScanLine(y);
            for (int x = 0; x < kImage; ++x)
                for (int c = 0; c < 3; ++c)
                    input[(static_cast<size_t>(c) * kImage + y) * kImage + x] = (p[x * 4 + c] / 255.f - kMean[c]) / kStd[c];
        }
        std::vector<Ort::Value> in;
        in.push_back(tensor(input, {1, 3, kImage, kImage}));
        auto out = run(*encoder, {"image"}, in, {"vision_feat", "vision_feat_nomem", "vision_pos", "high_res_0", "high_res_1"});
        return {copy(out[0]), copy(out[1]), copy(out[2]), copy(out[3]), copy(out[4])};
    }

    /** Decodificador de máscara: logits 256×256, ponteiro do objeto e a nota de presença dele. */
    void decodeMask(const std::vector<float> &pix, const Encoded &e, const std::vector<CutoutPoint> &points, bool multimask,
                    std::vector<float> &mask, std::vector<float> &ptr, float &score)
    {
        std::vector<float> coords;
        std::vector<int32_t> labels;
        for (const CutoutPoint &p : points) {
            coords.push_back(static_cast<float>(p.u * kImage));
            coords.push_back(static_cast<float>(p.v * kImage));
            labels.push_back(p.positive ? 1 : 0);
        }
        if (labels.empty()) { // quadros seguintes: sem cliques
            coords = {0.f, 0.f};
            labels = {-1};
        }
        const std::array<int64_t, 3> cShape{1, static_cast<int64_t>(labels.size()), 2};
        const std::array<int64_t, 2> lShape{1, static_cast<int64_t>(labels.size())};
        const std::array<int64_t, 1> one{1};
        float mm = multimask ? 1.f : 0.f;
        std::vector<Ort::Value> in;
        in.push_back(tensor(pix, {1, kHidden, kFeat, kFeat}));
        in.push_back(tensor(e.hr0, {1, 32, kLowRes, kLowRes}));
        in.push_back(tensor(e.hr1, {1, 64, kLowRes / 2, kLowRes / 2}));
        in.push_back(Ort::Value::CreateTensor<float>(mem, coords.data(), coords.size(), cShape.data(), cShape.size()));
        in.push_back(Ort::Value::CreateTensor<int32_t>(mem, labels.data(), labels.size(), lShape.data(), lShape.size()));
        in.push_back(Ort::Value::CreateTensor<float>(mem, &mm, 1, one.data(), one.size()));
        auto out = run(*heads, {"pix_feat", "high_res_0", "high_res_1", "point_coords", "point_labels", "multimask"}, in,
                       {"mask_logits", "obj_ptr", "object_score"});
        mask = copy(out[0]);
        ptr = copy(out[1]);
        score = out[2].GetTensorData<float>()[0];
    }

    void encodeMemory(const Encoded &e, const std::vector<float> &mask, float score, bool fromPoints, FrameMemory &m)
    {
        const std::array<int64_t, 2> sShape{1, 1};
        const std::array<int64_t, 1> one{1};
        float flag = fromPoints ? 1.f : 0.f;
        std::vector<Ort::Value> in;
        in.push_back(tensor(e.feat, {1, kHidden, kFeat, kFeat}));
        in.push_back(tensor(mask, {1, 1, kLowRes, kLowRes}));
        in.push_back(Ort::Value::CreateTensor<float>(mem, &score, 1, sShape.data(), sShape.size()));
        in.push_back(Ort::Value::CreateTensor<float>(mem, &flag, 1, one.data(), one.size()));
        auto out = run(*memEncoder, {"pix_feat", "mask_logits", "object_score", "from_points"}, in,
                       {"maskmem_feat", "maskmem_pos"});
        m.feat = copy(out[0]);
        m.pos = copy(out[1]);
    }

    /** Recursos do quadro atual condicionados à memória (quadros anteriores e ponteiros). */
    std::vector<float> attend(const Encoded &e, const std::vector<const FrameMemory *> &memories,
                              const std::vector<int64_t> &memTpos, const std::vector<const std::vector<float> *> &ptrs,
                              const std::vector<float> &ptrTpos, float tposMax)
    {
        const size_t memSize = static_cast<size_t>(kMemDim) * kFeat * kFeat;
        std::vector<float> feats, poss, ptrData;
        feats.reserve(memSize * memories.size());
        poss.reserve(memSize * memories.size());
        for (const FrameMemory *m : memories) {
            feats.insert(feats.end(), m->feat.begin(), m->feat.end());
            poss.insert(poss.end(), m->pos.begin(), m->pos.end());
        }
        for (const auto *p : ptrs)
            ptrData.insert(ptrData.end(), p->begin(), p->end());
        const int64_t M = static_cast<int64_t>(memories.size()), P = static_cast<int64_t>(ptrs.size());
        std::vector<int64_t> tpos = memTpos;
        std::vector<float> ptpos = ptrTpos;
        const std::array<int64_t, 1> mShape{M}, pShape{P}, one{1};
        std::vector<Ort::Value> in;
        in.push_back(tensor(e.feat, {1, kHidden, kFeat, kFeat}));
        in.push_back(tensor(e.pos, {1, kHidden, kFeat, kFeat}));
        in.push_back(tensor(feats, {M, kMemDim, kFeat, kFeat}));
        in.push_back(tensor(poss, {M, kMemDim, kFeat, kFeat}));
        in.push_back(Ort::Value::CreateTensor<int64_t>(mem, tpos.data(), tpos.size(), mShape.data(), mShape.size()));
        in.push_back(tensor(ptrData, {P, kHidden}));
        in.push_back(Ort::Value::CreateTensor<float>(mem, ptpos.data(), ptpos.size(), pShape.data(), pShape.size()));
        in.push_back(Ort::Value::CreateTensor<float>(mem, &tposMax, 1, one.data(), one.size()));
        auto out = run(*memAttention,
                       {"vision_feat", "vision_pos", "memory_feats", "memory_pos", "memory_tpos", "obj_ptrs", "obj_ptr_tpos",
                        "obj_ptr_tpos_max"},
                       in, {"pix_feat"});
        return copy(out[0]);
    }
};

#else

struct ObjectSegmenter::Impl {};

#endif

ObjectSegmenter::ObjectSegmenter() : d(std::make_unique<Impl>()) {}

ObjectSegmenter::~ObjectSegmenter() = default;

bool ObjectSegmenter::load(QString *error)
{
#ifdef CLIVCUT_HAVE_ONNX
    if (d->encoder)
        return true;
    if (!objectCutoutModelsInstalled()) {
        if (error)
            *error = QCoreApplication::translate("Engine", "The object cutout model is not installed");
        return false;
    }
    try {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(std::max(1, QThread::idealThreadCount()));
        opts.AddConfigEntry("session.intra_op.allow_spinning", "0");
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        const auto open = [&](const QString &id) {
#ifdef _WIN32
            const std::wstring path = aiModelPath(id).toStdWString();
#else
            const std::string path = aiModelPath(id).toStdString();
#endif
            return std::make_unique<Ort::Session>(d->env, path.c_str(), opts);
        };
        d->encoder = open(kCutoutEncoder);
        d->heads = open(kCutoutHeads);
        d->memEncoder = open(kCutoutMemoryEncoder);
        d->memAttention = open(kCutoutMemoryAttention);
        return true;
    } catch (const Ort::Exception &e) {
        d->encoder.reset();
        if (error)
            *error = QCoreApplication::translate("Engine", "AI model error: %1").arg(QString::fromUtf8(e.what()));
        return false;
    }
#else
    if (error)
        *error = QCoreApplication::translate("Engine", "This build was compiled without ONNX Runtime");
    return false;
#endif
}

QImage ObjectSegmenter::segmentFrame(const QString &mediaPath, Micros t, const std::vector<CutoutPoint> &points,
                                     QString *error)
{
#ifdef CLIVCUT_HAVE_ONNX
    if (!load(error))
        return {};
    media::VideoDecoder dec;
    if (!dec.open(mediaPath, QSize(kImage, kImage), error, true))
        return {};
    const QImage frame = dec.frameAt(t);
    if (frame.isNull()) {
        if (error)
            *error = QCoreApplication::translate("Engine", "Could not read the video");
        return {};
    }
    try {
        // A codificação (a parte cara) é reaproveitada enquanto os cliques são no mesmo quadro.
        if (d->cachedPath != mediaPath || d->cachedTime != t) {
            d->cached = d->encode(frame);
            d->cachedPath = mediaPath;
            d->cachedTime = t;
        }
        std::vector<float> mask, ptr;
        float score = 0;
        d->decodeMask(d->cached.nomem, d->cached, points, points.size() <= 1, mask, ptr, score);
        return logitsToMatte(mask, matteSizeFor(frame.size()));
    } catch (const Ort::Exception &e) {
        if (error)
            *error = QCoreApplication::translate("Engine", "AI model error: %1").arg(QString::fromUtf8(e.what()));
        return {};
    }
#else
    (void)mediaPath;
    (void)t;
    (void)points;
    if (error)
        *error = QCoreApplication::translate("Engine", "This build was compiled without ONNX Runtime");
    return {};
#endif
}

bool ObjectSegmenter::track(const QString &mediaPath, const ObjectCutout &cutout, Micros in, Micros out,
                            const QString &outPath, Micros *fileStart, double *fileFps,
                            const std::function<bool(double)> &progress, QString *error)
{
    auto fail = [&](const QString &m) {
        if (error)
            *error = m;
        return false;
    };
#ifndef CLIVCUT_HAVE_ONNX
    (void)mediaPath;
    (void)cutout;
    (void)in;
    (void)out;
    (void)outPath;
    (void)fileStart;
    (void)fileFps;
    (void)progress;
    return fail(QCoreApplication::translate("Engine", "This build was compiled without ONNX Runtime"));
#else
    if (!cutout.hasPoints())
        return fail(QCoreApplication::translate("Engine", "Click on the object to cut out first"));
    if (!load(error))
        return false;
    MediaAsset info;
    if (!media::probe(mediaPath, info, error))
        return false;
    if (info.kind != MediaKind::Video)
        return fail(QCoreApplication::translate("Engine", "Object cutout only applies to videos"));
    media::VideoDecoder dec;
    if (!dec.open(mediaPath, QSize(kImage, kImage), error, true))
        return false;
    const QSize matteSize = matteSizeFor(dec.outputSize());

    // Quadros processados: os da mídia, um a cada `stride` acima de 30 por segundo.
    const double fps = info.fps > 1 ? info.fps : 30.0;
    const int stride = std::max(1, static_cast<int>(std::ceil(fps / kMaxFps - 0.01)));
    const double rate = fps / stride;
    const std::int64_t first = frameIndex(in, rate);
    const std::int64_t last = std::max(first, frameIndex(out, rate));
    const std::int64_t prompt = std::llround(static_cast<double>(cutout.time) * rate / kSecond);
    const std::int64_t lo = std::min(first, prompt), hi = std::max(last, prompt);
    const float tposMax = static_cast<float>(std::min<std::int64_t>(hi - lo + 1, kMaxObjPtrs) - 1);
    if (fileStart)
        *fileStart = frameTime(first, rate);
    if (fileFps)
        *fileFps = rate;

    const auto decode = [&](std::int64_t i) { return dec.frameAt(frameTime(i, rate)); };
    std::map<std::int64_t, QImage> mattes;
    const double total = static_cast<double>(hi - lo + 1);
    try {
        // Quadro clicado: sem memória, com os pontos.
        const QImage promptFrame = decode(prompt);
        if (promptFrame.isNull())
            return fail(QCoreApplication::translate("Engine", "Could not read the video"));
        const Encoded e0 = d->encode(promptFrame);
        std::vector<float> mask, ptr;
        float score = 0;
        d->decodeMask(e0.nomem, e0, cutout.points, cutout.points.size() <= 1, mask, ptr, score);
        FrameMemory cond;
        d->encodeMemory(e0, mask, score, true, cond);
        cond.ptr = ptr;
        mattes[prompt] = logitsToMatte(mask, matteSize);
        double done = 1;

        // Para a frente e para trás a partir dele, cada sentido com a sua memória.
        for (const int dir : {1, -1}) {
            std::map<std::int64_t, FrameMemory> history; ///< quadros já processados neste sentido
            for (std::int64_t f = prompt + dir; f >= lo && f <= hi; f += dir) {
                const QImage frame = decode(f);
                if (frame.isNull())
                    break;
                const Encoded e = d->encode(frame);
                // Memórias: o quadro clicado e os 6 anteriores (no sentido do acompanhamento), do mais antigo ao mais novo.
                std::vector<const FrameMemory *> memories{&cond};
                std::vector<int64_t> memTpos{kNumMaskMem - 1};
                for (int rel = kNumMaskMem - 1; rel >= 1; --rel)
                    if (const auto it = history.find(f - rel * dir); it != history.end()) {
                        memories.push_back(&it->second);
                        memTpos.push_back(rel - 1);
                    }
                // Ponteiros do objeto: o do quadro clicado e os dos últimos quadros, com a distância de cada um.
                std::vector<const std::vector<float> *> ptrs{&cond.ptr};
                std::vector<float> ptrTpos{static_cast<float>(std::llabs(f - prompt))};
                for (int diff = 1; diff <= static_cast<int>(tposMax); ++diff)
                    if (const auto it = history.find(f - diff * dir); it != history.end()) {
                        ptrs.push_back(&it->second.ptr);
                        ptrTpos.push_back(static_cast<float>(diff));
                    }
                const std::vector<float> pix = d->attend(e, memories, memTpos, ptrs, ptrTpos, tposMax);
                d->decodeMask(pix, e, {}, true, mask, ptr, score);
                FrameMemory m;
                d->encodeMemory(e, mask, score, false, m);
                m.ptr = ptr;
                history[f] = std::move(m);
                // Só os 6 últimos guardam a memória completa; os ponteiros vão até 15 quadros.
                if (const auto old = history.find(f - (kNumMaskMem - 1) * dir); old != history.end()) {
                    old->second.feat.clear();
                    old->second.feat.shrink_to_fit();
                    old->second.pos.clear();
                    old->second.pos.shrink_to_fit();
                }
                history.erase(f - kMaxObjPtrs * dir);
                mattes[f] = logitsToMatte(mask, matteSize);
                if (progress && !progress(++done / total))
                    return fail(QCoreApplication::translate("Engine", "Object cutout cancelled"));
            }
        }
    } catch (const Ort::Exception &e) {
        return fail(QCoreApplication::translate("Engine", "AI model error: %1").arg(QString::fromUtf8(e.what())));
    }

    QDir().mkpath(QFileInfo(outPath).absolutePath());
    const QString tmp = outPath + QStringLiteral(".part.mp4");
    media::EncodeSettings es;
    es.width = matteSize.width();
    es.height = matteSize.height();
    es.fps = rate;
    es.crf = 16;
    es.preset = QStringLiteral("veryfast");
    es.gopFrames = std::max(1, static_cast<int>(rate / 2));
    es.audio = false;
    media::Encoder enc;
    QString err;
    if (!enc.open(tmp, es, &err))
        return fail(err);
    QImage previous;
    for (std::int64_t f = first; f <= last; ++f) {
        const auto it = mattes.find(f);
        const QImage gray = it != mattes.end() ? it->second : previous;
        if (gray.isNull())
            continue;
        previous = gray;
        if (!enc.writeVideo(gray.convertToFormat(QImage::Format_RGBA8888)))
            return fail(QCoreApplication::translate("Engine", "Failed to write the matte"));
    }
    if (!enc.finish(&err)) {
        QFile::remove(tmp);
        return fail(err);
    }
    QFile::remove(outPath);
    if (!QFile::rename(tmp, outPath))
        return fail(QCoreApplication::translate("Engine", "Could not write %1").arg(outPath));
    return true;
#endif
}

QString objectMattePathFor(const QString &mediaPath, const ObjectCutout &cutout, Micros in, Micros out)
{
    return QDir(cacheDir()).filePath(
        QStringLiteral("%1_%2_%3_%4.mp4").arg(filePrefix(mediaPath), cutoutKey(cutout)).arg(in).arg(out));
}

void registerObjectMatte(const QString &mediaPath, const ObjectCutout &cutout, Micros in, Micros out, Micros fileStart,
                         double fileFps, const QString &path)
{
    Q_UNUSED(in);
    QMutexLocker lock(&g_mutex);
    auto &list = g_entries[mediaPath];
    for (const Entry &e : list)
        if (e.path == path)
            return;
    list.append({cutoutKey(cutout), fileStart, out, fileFps, path});
}

QString findObjectMatte(const QString &mediaPath, const ObjectCutout &cutout, Micros in, Micros out, Micros *fileStart,
                        double *fileFps)
{
    const QString key = cutoutKey(cutout);
    QMutexLocker lock(&g_mutex);
    const auto it = g_entries.constFind(mediaPath);
    if (it == g_entries.constEnd())
        return {};
    for (const Entry &e : *it)
        if (e.key == key && e.start <= in && e.end >= out) {
            if (fileStart)
                *fileStart = e.start;
            if (fileFps)
                *fileFps = e.fps;
            return e.path;
        }
    return {};
}

void discoverObjectMattes(const QString &mediaPath)
{
    // Nome: <prefixo>_<cliques>_<início pedido>_<fim>.mp4; o resto vem do próprio arquivo.
    static const QRegularExpression re(QStringLiteral("_([0-9a-f]{12})_(\\d+)_(\\d+)\\.mp4$"));
    const QString prefix = filePrefix(mediaPath);
    for (const QFileInfo &fi : QDir(cacheDir()).entryInfoList({prefix + QStringLiteral("_*.mp4")}, QDir::Files)) {
        const auto m = re.match(fi.fileName());
        if (!m.hasMatch() || fi.size() <= 0)
            continue;
        MediaAsset matte;
        if (!media::probe(fi.absoluteFilePath(), matte) || matte.fps <= 0)
            continue;
        const Micros in = m.captured(2).toLongLong();
        const Entry e{m.captured(1), frameTime(frameIndex(in, matte.fps), matte.fps), m.captured(3).toLongLong(), matte.fps,
                      fi.absoluteFilePath()};
        QMutexLocker lock(&g_mutex);
        auto &list = g_entries[mediaPath];
        if (std::none_of(list.begin(), list.end(), [&](const Entry &x) { return x.path == e.path; }))
            list.append(e);
    }
}

} // namespace clv::engine
