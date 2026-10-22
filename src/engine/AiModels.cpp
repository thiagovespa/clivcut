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
