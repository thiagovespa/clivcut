// SPDX-License-Identifier: GPL-3.0-or-later
#include "TextToSpeech.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#ifdef CLIVCUT_HAVE_TTS
#include <espeak-ng/speak_lib.h>
#include <onnxruntime_cxx_api.h>
#endif

namespace clv::engine {

namespace {

#include "KokoroVocab.inc"

constexpr int kMaxPhonemes = 510; // contexto do modelo, sem os dois tokens de borda
constexpr int kStyleDim = 256;

/// Pontuação que o phonemizer preserva (e devolve no lugar, com o espaçamento original).
const QString kMarks = QStringLiteral(";:,.!?¡¿—…\"«»“”(){}[]");

QString tr(const char *s) { return QCoreApplication::translate("Engine", s); }

#ifdef CLIVCUT_HAVE_TTS
std::mutex g_espeakMutex;
bool g_espeakReady = false;

/** Pasta que contém espeak-ng-data: ao lado do app, no pacote ou a do build. */
QByteArray espeakDataParent()
{
#ifdef CLIVCUT_ESPEAK_SYSTEM_DATA
    return {}; // a do sistema, no caminho padrão dela
#else
    QStringList candidates;
    const QString env = qEnvironmentVariable("CLIVCUT_ESPEAK_DATA");
    if (!env.isEmpty())
        candidates << env;
    const QString app = QCoreApplication::applicationDirPath();
    candidates << app << app + QStringLiteral("/../share/clivcut") << app + QStringLiteral("/../Resources");
#ifdef CLIVCUT_ESPEAK_DATA_BUILD
    candidates << QStringLiteral(CLIVCUT_ESPEAK_DATA_BUILD);
#endif
    for (const QString &c : candidates)
        if (QFileInfo::exists(c + QStringLiteral("/espeak-ng-data/phontab")))
            return QDir::cleanPath(c).toUtf8();
    return {};
#endif
}

bool ensureEspeak(QString *error)
{
    if (g_espeakReady)
        return true;
    const QByteArray path = espeakDataParent();
#ifndef CLIVCUT_ESPEAK_SYSTEM_DATA
    if (path.isEmpty()) {
        if (error)
            *error = tr("Phoneme data (espeak-ng-data) not found");
        return false;
    }
#endif
    if (espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0, path.isEmpty() ? nullptr : path.constData(),
                          espeakINITIALIZE_DONT_EXIT) <= 0) {
        if (error)
            *error = tr("Could not start espeak-ng");
        return false;
    }
    g_espeakReady = true;
    return true;
}

/** Fonemas de um trecho sem pontuação, como o phonemizer: palavras separadas por espaço. */
QString phonemizeChunk(const QString &chunk)
{
    const QByteArray utf8 = chunk.toUtf8();
    const void *ptr = utf8.constData();
    QStringList clauses;
    // Fonemas IPA separados por '_' (removido depois); o espeak devolve uma oração por vez.
    const int mode = ('_' << 8) | 0x02;
    while (ptr) {
        const char *ph = espeak_TextToPhonemes(&ptr, espeakCHARS_UTF8, mode);
        if (ph && *ph)
            clauses << QString::fromUtf8(ph);
    }
    QString line = clauses.join(QLatin1Char(' ')).trimmed();
    line.replace(QLatin1Char('\n'), QLatin1Char(' ')).replace(QStringLiteral("  "), QStringLiteral(" "));
    static const QRegularExpression multi(QStringLiteral("_+"));
    line.replace(multi, QStringLiteral("_"));
    line.replace(QStringLiteral("_ "), QStringLiteral(" "));
    line.remove(QLatin1Char('_'));
    return line.simplified();
}
#endif

const QHash<char32_t, int> &vocab()
{
    static const QHash<char32_t, int> v = [] {
        QHash<char32_t, int> h;
        for (const auto &[c, id] : kVocab)
            h.insert(c, id);
        return h;
    }();
    return v;
}

/** Divide os fonemas em lotes que cabem no modelo, de preferência na pontuação. */
QStringList splitPhonemes(const QString &phonemes)
{
    static const QRegularExpression re(QStringLiteral("([.,!?;])"));
    QStringList batches;
    QString current;
    qsizetype pos = 0;
    QStringList parts;
    auto it = re.globalMatch(phonemes);
    while (it.hasNext()) {
        const auto m = it.next();
        parts << phonemes.mid(pos, m.capturedStart() - pos) << m.captured(1);
        pos = m.capturedEnd();
    }
    parts << phonemes.mid(pos);
    for (QString part : parts) {
        part = part.trimmed();
        if (part.isEmpty())
            continue;
        if (current.size() + part.size() + 1 >= kMaxPhonemes) {
            batches << current.trimmed();
            current = part;
        } else if (part.size() == 1 && QStringLiteral(".,!?;").contains(part)) {
            current += part;
        } else {
            if (!current.isEmpty())
                current += QLatin1Char(' ');
            current += part;
        }
    }
    if (!current.trimmed().isEmpty())
        batches << current.trimmed();
    return batches;
}

/** Corta silêncio no começo e no fim (abaixo de -60 dB do pico, em janelas de 2048). */
void trimSilence(std::vector<float> &a)
{
    constexpr int frame = 2048, hop = 512;
    if (a.size() < static_cast<size_t>(frame))
        return;
    std::vector<double> rms;
    for (size_t s = 0; s + frame <= a.size(); s += hop) {
        double acc = 0;
        for (int i = 0; i < frame; ++i)
            acc += static_cast<double>(a[s + i]) * a[s + i];
        rms.push_back(std::sqrt(acc / frame));
    }
    const double peak = *std::max_element(rms.begin(), rms.end());
    if (peak <= 0)
        return;
    const double threshold = peak * std::pow(10.0, -60.0 / 20);
    size_t first = 0, last = rms.size() - 1;
    while (first < rms.size() && rms[first] < threshold)
        ++first;
    while (last > first && rms[last] < threshold)
        --last;
    const size_t start = first * hop, end = std::min(a.size(), last * hop + frame);
    a = std::vector<float>(a.begin() + static_cast<std::ptrdiff_t>(start), a.begin() + static_cast<std::ptrdiff_t>(end));
}

QByteArray wavHeader(std::int64_t frames)
{
    const quint32 dataBytes = static_cast<quint32>(frames * 4);
    QByteArray h;
    auto u32 = [&](quint32 v) { h.append(reinterpret_cast<const char *>(&v), 4); };
    auto u16 = [&](quint16 v) { h.append(reinterpret_cast<const char *>(&v), 2); };
    h.append("RIFF");
    u32(36 + dataBytes);
    h.append("WAVEfmt ");
    u32(16);
    u16(3); // IEEE float
    u16(1); // mono
    u32(kTtsSampleRate);
    u32(kTtsSampleRate * 4);
    u16(4);
    u16(32);
    h.append("data");
    u32(dataBytes);
    return h;
}

/** Lê o arquivo de vozes (zip sem compressão de arrays NumPy 510×1×256 float32). */
bool loadVoice(const QString &path, const QString &voiceId, std::vector<float> &out, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = tr("Voice file not found: %1").arg(path);
        return false;
    }
    const QByteArray z = f.readAll();
    const QByteArray want = (voiceId + QStringLiteral(".npy")).toUtf8();
    auto u16 = [&](qsizetype o) { return static_cast<quint16>(static_cast<uchar>(z[o]) | static_cast<uchar>(z[o + 1]) << 8); };
    auto u32 = [&](qsizetype o) { return static_cast<quint32>(u16(o) | static_cast<quint32>(u16(o + 2)) << 16); };
    auto u64 = [&](qsizetype o) { return static_cast<quint64>(u32(o)) | static_cast<quint64>(u32(o + 4)) << 32; };
    // Diretório central (os tamanhos confiáveis ficam nele, inclusive em zip64).
    qsizetype eocd = -1;
    for (qsizetype p = z.size() - 22; p >= std::max<qsizetype>(0, z.size() - 65557); --p)
        if (u32(p) == 0x06054b50) {
            eocd = p;
            break;
        }
    if (eocd >= 0) {
        const quint16 entries = u16(eocd + 10);
        qsizetype p = u32(eocd + 16);
        for (quint16 e = 0; e < entries && p + 46 <= z.size() && u32(p) == 0x02014b50; ++e) {
            const quint16 method = u16(p + 10);
            quint64 size = u32(p + 20);
            quint64 local = u32(p + 42);
            const quint16 nameLen = u16(p + 28), extraLen = u16(p + 30), commentLen = u16(p + 32);
            const QByteArray name = z.mid(p + 46, nameLen);
            // zip64: tamanho e posição em 64 bits no campo extra 0x0001.
            for (qsizetype x = p + 46 + nameLen; x + 4 <= p + 46 + nameLen + extraLen;) {
                const quint16 tag = u16(x), len = u16(x + 2);
                if (tag == 0x0001) {
                    qsizetype f = x + 4;
                    if (u32(p + 24) == 0xFFFFFFFF) // tamanho original
                        f += 8;
                    if (u32(p + 20) == 0xFFFFFFFF) {
                        size = u64(f);
                        f += 8;
                    }
                    if (u32(p + 42) == 0xFFFFFFFF)
                        local = u64(f);
                }
                x += 4 + len;
            }
            if (name == want && method == 0 && local + 30 <= static_cast<quint64>(z.size())) {
                const qsizetype data = static_cast<qsizetype>(local) + 30 + u16(static_cast<qsizetype>(local) + 26)
                                       + u16(static_cast<qsizetype>(local) + 28);
                const QByteArray npy = z.mid(data, static_cast<qsizetype>(size));
                // Cabeçalho .npy: "\x93NUMPY", versão (2 bytes), tamanho do dicionário, dicionário.
                if (!npy.startsWith("\x93NUMPY"))
                    break;
                const int hlen = static_cast<uchar>(npy[8]) | static_cast<uchar>(npy[9]) << 8;
                const qsizetype off = 10 + hlen;
                const qsizetype count = (npy.size() - off) / 4;
                if (count < kStyleDim)
                    break;
                out.resize(static_cast<size_t>(count));
                std::memcpy(out.data(), npy.constData() + off, static_cast<size_t>(count) * 4);
                return true;
            }
            p += 46 + nameLen + extraLen + commentLen;
        }
    }
    if (error)
        *error = tr("Voice %1 not found in %2").arg(voiceId, QFileInfo(path).fileName());
    return false;
}

} // namespace

bool ttsAvailable()
{
#ifdef CLIVCUT_HAVE_TTS
    return true;
#else
    return false;
#endif
}

const std::vector<TtsVoice> &ttsVoices()
{
    static const std::vector<TtsVoice> voices = [] {
        // Vozes do Kokoro v1.0 nas línguas que o espeak-ng converte (japonês e mandarim usam outro G2P).
        const char *ids[] = {"pf_dora", "pm_alex", "pm_santa",
                             "ef_dora", "em_alex", "em_santa",
                             "af_heart", "af_bella", "af_nicole", "af_sarah", "af_sky", "am_michael", "am_adam",
                             "am_fenrir", "am_puck", "bf_emma", "bf_isabella", "bm_george", "bm_fable",
                             "ff_siwis", "if_sara", "im_nicola", "hf_alpha", "hm_omega"};
        const std::map<QChar, QString> lang{{QLatin1Char('p'), QStringLiteral("pt-br")}, {QLatin1Char('e'), QStringLiteral("es")},
                                            {QLatin1Char('a'), QStringLiteral("en-us")}, {QLatin1Char('b'), QStringLiteral("en-gb")},
                                            {QLatin1Char('f'), QStringLiteral("fr-fr")}, {QLatin1Char('i'), QStringLiteral("it")},
                                            {QLatin1Char('h'), QStringLiteral("hi")}};
        std::vector<TtsVoice> out;
        for (const char *id : ids) {
            const QString s = QString::fromLatin1(id);
            QString name = s.mid(3);
            name[0] = name[0].toUpper();
            out.push_back({s, name, lang.at(s[0]), s[1] == QLatin1Char('f')});
        }
        return out;
    }();
    return voices;
}

const TtsVoice *findTtsVoice(const QString &id)
{
    for (const auto &v : ttsVoices())
        if (v.id == id)
            return &v;
    return nullptr;
}

QString defaultTtsVoice(const QString &uiLanguage)
{
    if (uiLanguage.startsWith(QLatin1String("pt")))
        return QStringLiteral("pf_dora");
    if (uiLanguage.startsWith(QLatin1String("es")))
        return QStringLiteral("ef_dora");
    return QStringLiteral("af_heart");
}

QString ttsPhonemize(const QString &text, const QString &language, QString *error)
{
#ifndef CLIVCUT_HAVE_TTS
    (void)text;
    (void)language;
    if (error)
        *error = tr("This build was compiled without text-to-speech");
    return {};
#else
    std::lock_guard lock(g_espeakMutex);
    if (!ensureEspeak(error))
        return {};
    if (espeak_SetVoiceByName(language.toUtf8().constData()) != EE_OK) {
        if (error)
            *error = tr("Language not available for speech: %1").arg(language);
        return {};
    }
    // Trechos entre pontuações viram fonemas; a pontuação volta no lugar, com o espaçamento original.
    const QString s = text.trimmed();
    QString out;
    qsizetype i = 0;
    auto lastWasSpace = [&](qsizetype at) { return at > 0 && s[at - 1].isSpace(); };
    while (i < s.size()) {
        if (kMarks.contains(s[i])) {
            if (lastWasSpace(i) && !out.isEmpty() && !out.endsWith(QLatin1Char(' ')))
                out += QLatin1Char(' ');
            out += s[i];
            ++i;
            continue;
        }
        qsizetype j = i;
        while (j < s.size() && !kMarks.contains(s[j]))
            ++j;
        const QString chunk = s.mid(i, j - i);
        const QString ph = phonemizeChunk(chunk);
        if (!ph.isEmpty()) {
            if (!out.isEmpty() && !out.endsWith(QLatin1Char(' ')) && chunk.front().isSpace())
                out += QLatin1Char(' ');
            out += ph;
            if (chunk.back().isSpace() && j < s.size())
                out += QLatin1Char(' ');
        }
        i = j;
    }
    // Só os símbolos que o modelo conhece.
    QString filtered;
    const auto &v = vocab();
    for (const char32_t c : out.toUcs4())
        if (v.contains(c))
            filtered += QString::fromUcs4(&c, 1);
    return filtered.simplified();
#endif
}

std::vector<std::int64_t> ttsTokenize(const QString &phonemes)
{
    std::vector<std::int64_t> ids;
    const auto &v = vocab();
    for (const char32_t c : phonemes.toUcs4()) {
        const auto it = v.constFind(c);
        if (it != v.constEnd())
            ids.push_back(*it);
    }
    return ids;
}

bool synthesizeSpeech(const QString &text, const QString &voiceId, double speed, const QString &modelPath,
                      const QString &voicesPath, const QString &outPath, const std::function<bool(double)> &progress,
                      QString *error)
{
    auto fail = [&](const QString &m) {
        if (error)
            *error = m;
        return false;
    };
#ifndef CLIVCUT_HAVE_TTS
    (void)text;
    (void)voiceId;
    (void)speed;
    (void)modelPath;
    (void)voicesPath;
    (void)outPath;
    (void)progress;
    return fail(tr("This build was compiled without text-to-speech"));
#else
    const TtsVoice *voice = findTtsVoice(voiceId);
    if (!voice)
        return fail(tr("Unknown voice: %1").arg(voiceId));
    if (!QFileInfo::exists(modelPath))
        return fail(tr("Speech model not found: %1").arg(modelPath));
    std::vector<float> style;
    if (!loadVoice(voicesPath, voiceId, style, error))
        return false;
    const QString phonemes = ttsPhonemize(text, voice->language, error);
    if (phonemes.isEmpty())
        return error && !error->isEmpty() ? false : fail(tr("Nothing to read"));
    const QStringList batches = splitPhonemes(phonemes);

    std::vector<float> audio;
    try {
        // Sessão reaproveitada entre chamadas (carregar o modelo leva alguns segundos).
        static std::mutex sessionMutex;
        static std::unique_ptr<Ort::Env> env;
        static std::unique_ptr<Ort::Session> session;
        static QString sessionPath;
        std::lock_guard lock(sessionMutex);
        if (!session || sessionPath != modelPath) {
            if (!env)
                env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "clivcut");
            Ort::SessionOptions opts;
            opts.SetIntraOpNumThreads(std::max(1, QThread::idealThreadCount()));
            opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef _WIN32
            const std::wstring model = modelPath.toStdWString();
#else
            const std::string model = modelPath.toStdString();
#endif
            session = std::make_unique<Ort::Session>(*env, model.c_str(), opts);
            sessionPath = modelPath;
        }
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const char *inNames[] = {"tokens", "style", "speed"};
        const char *outNames[] = {"audio"};
        float spd = static_cast<float>(std::clamp(speed, 0.5, 2.0));
        const qsizetype rows = static_cast<qsizetype>(style.size() / kStyleDim);
        for (qsizetype b = 0; b < batches.size(); ++b) {
            std::vector<std::int64_t> tokens = ttsTokenize(batches[b].left(kMaxPhonemes));
            if (tokens.empty())
                continue;
            // O estilo depende do tamanho da frase: uma linha por quantidade de tokens.
            const qsizetype row = std::min<qsizetype>(static_cast<qsizetype>(tokens.size()), rows - 1);
            std::vector<float> st(style.begin() + row * kStyleDim, style.begin() + (row + 1) * kStyleDim);
            tokens.insert(tokens.begin(), 0);
            tokens.push_back(0);
            const std::array<int64_t, 2> tokShape{1, static_cast<int64_t>(tokens.size())};
            const std::array<int64_t, 2> styleShape{1, kStyleDim};
            const std::array<int64_t, 1> speedShape{1};
            std::array<Ort::Value, 3> in{
                Ort::Value::CreateTensor<std::int64_t>(mem, tokens.data(), tokens.size(), tokShape.data(), tokShape.size()),
                Ort::Value::CreateTensor<float>(mem, st.data(), st.size(), styleShape.data(), styleShape.size()),
                Ort::Value::CreateTensor<float>(mem, &spd, 1, speedShape.data(), speedShape.size())};
            auto res = session->Run(Ort::RunOptions{nullptr}, inNames, in.data(), in.size(), outNames, 1);
            const float *y = res[0].GetTensorData<float>();
            const size_t n = res[0].GetTensorTypeAndShapeInfo().GetElementCount();
            std::vector<float> part(y, y + n);
            trimSilence(part);
            audio.insert(audio.end(), part.begin(), part.end());
            if (progress && !progress(static_cast<double>(b + 1) / batches.size()))
                return fail(tr("Canceled"));
        }
    } catch (const Ort::Exception &e) {
        return fail(tr("AI model error: %1").arg(QString::fromUtf8(e.what())));
    }
    if (audio.empty())
        return fail(tr("Nothing to read"));
    QDir().mkpath(QFileInfo(outPath).absolutePath());
    QSaveFile f(outPath);
    if (!f.open(QIODevice::WriteOnly))
        return fail(f.errorString());
    f.write(wavHeader(static_cast<std::int64_t>(audio.size())));
    f.write(reinterpret_cast<const char *>(audio.data()), static_cast<qint64>(audio.size() * 4));
    if (!f.commit())
        return fail(tr("Could not write %1").arg(outPath));
    return true;
#endif
}

} // namespace clv::engine
