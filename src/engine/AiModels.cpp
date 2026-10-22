// SPDX-License-Identifier: GPL-3.0-or-later
#include "AiModels.h"

#include "WhisperModels.h"

#include <QCoreApplication>

#include <QDir>
#include <QFileInfo>

namespace clv::engine {

const std::vector<AiModel> &aiModels()
{
    static const std::vector<AiModel> models = [] {
        std::vector<AiModel> list;
        for (const auto &w : whisperModels())
            list.push_back({w.id, w.label, w.sizeBytes, w.description, QStringLiteral("whisper"),
                            QStringLiteral("ggml-%1.bin").arg(w.id), whisperModelUrl(w.id), QStringLiteral("MIT")});
        list.push_back({kMattingModel, QString::fromUtf8(QT_TRANSLATE_NOOP("Models", "People cutout (RVM)")), 14'975'696,
                        QString::fromUtf8(QT_TRANSLATE_NOOP("Models", "Removes the background from videos of people, stable across frames")),
                        QStringLiteral("matting"), QStringLiteral("rvm_mobilenetv3_fp32.onnx"),
                        QUrl(QStringLiteral("https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/"
                                            "rvm_mobilenetv3_fp32.onnx")),
                        QStringLiteral("GPL-3.0")});
        list.push_back({kVoiceModel, QString::fromUtf8(QT_TRANSLATE_NOOP("Models", "Voice isolation (DeepFilterNet3)")), 16'104'687,
                        QString::fromUtf8(QT_TRANSLATE_NOOP("Models", "Separates speech from noise, music and room sound")),
                        QStringLiteral("voice"), QStringLiteral("deepfilternet3_streaming.onnx"),
                        QUrl(QStringLiteral("https://huggingface.co/kimtos-labs/denoiser-dfn3/resolve/main/denoiser_model.onnx")),
                        QStringLiteral("MIT OR Apache-2.0")});
        // Exportação ONNX do HT-Demucs v4 (pesos em fp16), presa a um commit e conferida pelo hash.
        list.push_back({kStemsModel, QString::fromUtf8(QT_TRANSLATE_NOOP("Models", "Music separation (HT-Demucs)")), 165'612'636,
                        QString::fromUtf8(QT_TRANSLATE_NOOP("Models", "Splits music into vocals, drums, bass and other instruments")),
                        QStringLiteral("stems"), QStringLiteral("htdemucs_fp16weights.onnx"),
                        QUrl(QStringLiteral("https://huggingface.co/StemSplitio/htdemucs-onnx/resolve/"
                                            "d54ed9eb60e258ea82131c6ee14578628816456a/htdemucs_fp16weights.onnx")),
                        QStringLiteral("MIT"),
                        QStringLiteral("d05c269d0178d2a72ad484b10b11dd370193fc923201c3b27a99f848745db70a")});
        // Kokoro v1.0 (pesos Apache-2.0) exportado em ONNX pelo projeto kokoro-onnx (MIT). A versão fp16
        // soa como a original e é rápida em todas as versões do ONNX Runtime usadas (a int8 fica
        // ~4x mais lenta antes da 1.31).
        list.push_back({kTtsModel, QString::fromUtf8(QT_TRANSLATE_NOOP("Models", "AI voice (Kokoro)")), 177'464'787,
                        QString::fromUtf8(QT_TRANSLATE_NOOP("Models", "Natural-sounding narration from text, generated on your computer")),
                        QStringLiteral("tts"), QStringLiteral("kokoro-v1.0.fp16.onnx"),
                        QUrl(QStringLiteral("https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/"
                                            "kokoro-v1.0.fp16.onnx")),
                        QStringLiteral("Apache-2.0"),
                        QStringLiteral("c1610a859f3bdea01107e73e50100685af38fff88f5cd8e5c56df109ec880204")});
        list.push_back({kTtsVoices, QString::fromUtf8(QT_TRANSLATE_NOOP("Models", "AI voices (Kokoro)")), 28'214'398,
                        QString::fromUtf8(QT_TRANSLATE_NOOP("Models", "The voices for the AI narration")),
                        QStringLiteral("tts"), QStringLiteral("voices-v1.0.bin"),
                        QUrl(QStringLiteral("https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/"
                                            "voices-v1.0.bin")),
                        QStringLiteral("Apache-2.0"),
                        QStringLiteral("bca610b8308e8d99f32e6fe4197e7ec01679264efed0cac9140fe9c29f1fbf7d")});
        // RIFE v4.26 (Practical-RIFE, MIT) em ONNX, gerado por tools/rife/export_onnx.py a partir dos
        // pesos oficiais e publicado numa release só de modelos.
        list.push_back({kInterpolationModel, QString::fromUtf8(QT_TRANSLATE_NOOP("Models", "Frame interpolation (RIFE)")), 22'856'917,
                        QString::fromUtf8(QT_TRANSLATE_NOOP("Models", "Creates in-between frames for smooth slow motion")),
                        QStringLiteral("interpolation"), QStringLiteral("rife-v4.26.onnx"),
                        QUrl(QStringLiteral("https://github.com/thiagovespa/clivcut/releases/download/models-v1/rife-v4.26.onnx")),
                        QStringLiteral("MIT"),
                        QStringLiteral("e4d8207ca790f803efc8324e84e3af69b1e65359abdfa7e92c976c224df60b8f")});
        // SAM 2.1 Hiera-Tiny (Meta, Apache-2.0) em quatro grafos ONNX gerados por tools/sam2/export_onnx.py.
        const auto sam2 = [&](const QString &id, const char *label, qint64 size, const char *file, const char *sha) {
            list.push_back({id, QString::fromUtf8(label), size,
                            QString::fromUtf8(QT_TRANSLATE_NOOP("Models", "Cuts out any object you click on, following it through the clip")),
                            QStringLiteral("cutout"), QString::fromLatin1(file),
                            QUrl(QStringLiteral("https://github.com/thiagovespa/clivcut/releases/download/models-v1/") +
                                 QString::fromLatin1(file)),
                            QStringLiteral("Apache-2.0"), QString::fromLatin1(sha)});
        };
        sam2(kCutoutEncoder, QT_TRANSLATE_NOOP("Models", "Object cutout (SAM 2): image encoder"), 134'437'025,
             "sam2_encoder.onnx", "7c7249d45308131f2537b6d367c4ad395fdac0178a69c51d1aff1b688c9f595b");
        sam2(kCutoutHeads, QT_TRANSLATE_NOOP("Models", "Object cutout (SAM 2): mask decoder"), 26'060'486,
             "sam2_heads.onnx", "55c8c974629c806173e4f119809b5e06a1d4a2536a2b59fe353443469ae00a4a");
        sam2(kCutoutMemoryEncoder, QT_TRANSLATE_NOOP("Models", "Object cutout (SAM 2): memory encoder"), 6'629'392,
             "sam2_memory_encoder.onnx", "83e7dc02bbf3994d0931d71bc850a961ddc008d8b4acb52c53c1493d8ca8dd68");
        sam2(kCutoutMemoryAttention, QT_TRANSLATE_NOOP("Models", "Object cutout (SAM 2): memory attention"), 28'163'613,
             "sam2_memory_attention.onnx", "d7490e629164319ce0d924fafea76ee3148580771ea6bbb49a915153319a3920");
        return list;
    }();
    return models;
}

const AiModel *findAiModel(const QString &id)
{
    for (const auto &m : aiModels())
        if (m.id == id)
            return &m;
    return nullptr;
}

QString aiModelPath(const QString &id)
{
    const AiModel *m = findAiModel(id);
    return m ? QDir(whisperModelsDir()).filePath(m->fileName) : QString();
}

bool aiModelInstalled(const QString &id)
{
    const QFileInfo fi(aiModelPath(id));
    return fi.exists() && fi.size() > 1'000'000;
}

} // namespace clv::engine
