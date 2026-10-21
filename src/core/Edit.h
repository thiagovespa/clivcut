// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Model.h"

#include <utility>
#include <vector>

#include <QStringList>

#include <optional>

/**
 * Operações de edição sobre o Project. São funções puras sobre a estrutura
 * (sem UI, sem I/O), retornando false quando a operação é inválida e
 * deixando o projeto intacto nesse caso.
 */
namespace clv::edit {

struct ClipRef {
    int track = -1;
    int index = -1;
};

std::optional<ClipRef> findClip(const Project &p, const QString &clipId);
Clip *clipById(Project &p, const QString &clipId);
const Clip *clipById(const Project &p, const QString &clipId);

/** Tipo de faixa adequado para um clipe (texto, áudio ou vídeo/imagem). */
TrackKind trackKindFor(const Project &p, const Clip &c);

bool fitsInTrack(const Track &t, Micros start, Micros duration, const QString &ignoreId = {});
void sortTrack(Track &t);

/** Na faixa principal magnética, remove buracos mantendo a ordem. */
void packMainTrack(Project &p);

/** Remove faixas vazias (a principal nunca é removida). */
void removeEmptyTracks(Project &p);

/**
 * Faixa onde um clipe cabe: tenta `preferred`, depois as demais do mesmo tipo
 * e, por fim, cria uma nova faixa. Retorna o índice.
 */
int ensureTrackFor(Project &p, TrackKind kind, Micros start, Micros duration, int preferred = -1);

/** Maior duração possível na timeline dado o material de origem (ou -1 = ilimitada). */
Micros maxDuration(const Project &p, const Clip &c);

/**
 * Insere um clipe. Na faixa principal magnética o clipe entra entre os
 * clipes existentes (antes/depois conforme o ponto médio) e a faixa é
 * recompactada. Retorna o id do clipe inserido ou vazio se falhar.
 */
QString insertClip(Project &p, Clip c, int trackIndex = -1);

/** Divide o clipe em `t` (tempo da timeline). Retorna o id da metade direita. */
std::optional<QString> splitClip(Project &p, const QString &clipId, Micros t);

/** Divide todos os clipes sob `t` (ou só `onlyIds`, se dado). Retorna quantos. */
int splitAt(Project &p, Micros t, const QStringList &onlyIds = {});

/**
 * Remove trechos de tempo da timeline inteira (todas as faixas desbloqueadas), com ripple:
 * o que vem depois recua. Marcadores e entrada/saída acompanham. Usado pela remoção de
 * silêncios e pela edição por transcrição. Retorna o tempo total removido.
 */
Micros removeRanges(Project &p, std::vector<std::pair<Micros, Micros>> ranges);

/** Remove clipes; com ripple, os clipes seguintes na mesma faixa recuam. */
bool deleteClips(Project &p, const QStringList &ids, bool ripple);

enum class Edge { Start, End };

/** Apara um clipe movendo uma das bordas para `newTime` (tempo da timeline). */
bool trimClip(Project &p, const QString &clipId, Edge edge, Micros newTime);

/** "Excluir à esquerda/direita" do cursor (atalhos Q/W): apara o clipe sob `t`. */
bool trimToPlayhead(Project &p, const QString &clipId, Micros t, Edge removeSide, bool ripple);

/** Move um clipe para outra posição/faixa. */
bool moveClip(Project &p, const QString &clipId, int targetTrack, Micros newStart);

/** Muda a velocidade mantendo o mesmo trecho de origem. */
bool setSpeed(Project &p, const QString &clipId, double speed);

/**
 * Separa o áudio de um clipe de vídeo num clipe de áudio (mesmo tempo, velocidade,
 * volume, fades); o vídeo fica mudo. Retorna o id do clipe de áudio.
 */
std::optional<QString> detachAudio(Project &p, const QString &clipId);

/** Aplica uma curva de velocidade mantendo o trecho de origem (vazia = constante). */
bool setSpeedCurve(Project &p, const QString &clipId, std::vector<SpeedPoint> curve);

std::optional<QString> duplicateClip(Project &p, const QString &clipId);

/** Inclui na lista todos os clipes dos grupos já presentes nela. */
QStringList expandToGroups(const Project &p, const QStringList &ids);

/** Agrupa os clipes (2 ou mais). Retorna o id do grupo ou vazio. */
QString groupClips(Project &p, const QStringList &ids);
bool ungroupClips(Project &p, const QStringList &ids);

/**
 * Move vários clipes juntos no tempo (cada um na sua faixa). Clipes que não
 * cabem mais na faixa vão para outra do mesmo tipo (ou uma nova).
 */
bool moveClips(Project &p, const QStringList &ids, Micros delta);

/**
 * Snapping: devolve o ponto de encaixe mais próximo de `t` dentro de
 * `threshold` (bordas de clipes, cursor, marcadores, zero) ou `t` se nenhum.
 */
Micros snap(const Project &p, Micros t, Micros threshold, const QStringList &ignoreClips, Micros playhead);

/** Motivo de falha de createCompound (para a mensagem ao usuário). */
enum class CompoundError { None, NoClips, Locked, MainTrackGap };

/**
 * Junta os clipes num clipe composto: eles vão para uma sequência própria (mesmas faixas
 * relativas e tempos a partir de 0) e um clipe só os substitui, na faixa mais baixa entre eles.
 * Na faixa principal magnética, os clipes escolhidos nela precisam ser vizinhos e cobrir todo o
 * trecho dos demais (o resto da timeline não se mexe). Retorna o id do novo clipe ou vazio.
 */
QString createCompound(Project &p, const QStringList &ids, const QString &name, CompoundError *error = nullptr);

/**
 * Desfaz o clipe composto: o trecho visível dos clipes internos volta para a timeline, na
 * mesma posição, com a primeira faixa interna no lugar do composto. Retorna os ids novos.
 */
QStringList breakCompound(Project &p, const QString &clipId);

/** Remove as sequências que nenhum clipe composto usa mais. */
void pruneCompounds(Project &p);

/**
 * Projeto de trabalho para editar o conteúdo de um clipe composto: o mesmo projeto, com as
 * faixas da sequência no lugar das faixas da timeline (a primeira é sempre de vídeo), sem
 * marcadores nem entrada/saída e sem faixa magnética.
 */
std::optional<Project> compoundWorkspace(const Project &outer, const QString &seqId);

/**
 * Devolve ao projeto de fora o que foi feito no projeto de trabalho: as faixas da sequência,
 * mídias importadas e outras sequências criadas ou mudadas lá dentro. Clipes compostos que
 * usam a sequência são encurtados se o conteúdo ficou menor (e somem se ficou vazio).
 */
void mergeCompoundWorkspace(Project &outer, const QString &seqId, const Project &inner);

} // namespace clv::edit
