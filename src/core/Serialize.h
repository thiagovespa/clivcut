// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Model.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace clv {

QJsonObject toJson(const Project &p);
QJsonObject clipToJson(const Clip &c);
/// Curvas, HSL e rodas de cor (vazio se tudo neutro).
QJsonObject gradeToJson(const ColorAdjust &c);
void gradeFromJson(const QJsonObject &o, ColorAdjust &c);
Clip clipFromJson(const QJsonObject &o);
QJsonObject assetToJson(const MediaAsset &a);
/// Sequência de clipe composto (id, nome e faixas).
QJsonObject sequenceToJson(const Sequence &s);
Sequence sequenceFromJson(const QJsonObject &o);
MediaAsset assetFromJson(const QJsonObject &o);
QByteArray serializeProject(const Project &p);

/** Lê um projeto .clivcut; em erro, retorna false e preenche `error`. */
bool parseProject(const QByteArray &data, Project &out, QString *error = nullptr);

bool saveProjectFile(const Project &p, const QString &path, QString *error = nullptr);
bool loadProjectFile(const QString &path, Project &out, QString *error = nullptr);

QString easingName(Easing e);
Easing easingFromName(const QString &s);
QString blendModeName(BlendMode m);
BlendMode blendModeFromName(const QString &s);
QString transitionName(TransitionKind k);
TransitionKind transitionFromName(const QString &s);
QString animationName(AnimationKind k);
AnimationKind animationFromName(const QString &s);
QString maskShapeName(MaskShape s);
MaskShape maskShapeFromName(const QString &s);

} // namespace clv
