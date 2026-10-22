// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QUrl>

#include <vector>

namespace clv::engine {

/** Modelos de IA locais baixados sob demanda (transcrição, recorte, voz). */
struct AiModel {
    QString id;
    QString label;
    qint64 sizeBytes;
    QString description;
    QString kind;     ///< "whisper", "matting", "voice"
    QString fileName; ///< nome do arquivo na pasta de modelos
    QUrl url;
    QString license;
    QString sha256 = {}; ///< conferido depois do download (vazio = sem verificação)
};

const std::vector<AiModel> &aiModels();
const AiModel *findAiModel(const QString &id);
QString aiModelPath(const QString &id);
bool aiModelInstalled(const QString &id);

/// Modelo de recorte de pessoas (RobustVideoMatting, GPL-3.0).
inline const QString kMattingModel = QStringLiteral("rvm");
/// Isolamento de voz (DeepFilterNet3 em streaming, MIT ou Apache-2.0).
inline const QString kVoiceModel = QStringLiteral("dfn3");
/// Separação de música em faixas (HT-Demucs v4, MIT).
inline const QString kStemsModel = QStringLiteral("htdemucs");
/// Voz neural (Kokoro v1.0, Apache-2.0): o modelo e o arquivo com as vozes.
inline const QString kTtsModel = QStringLiteral("kokoro");
inline const QString kTtsVoices = QStringLiteral("kokoro-voices");
/// Interpolação de quadros (RIFE v4.26, MIT), exportada por tools/rife/export_onnx.py.
inline const QString kInterpolationModel = QStringLiteral("rife");

} // namespace clv::engine
