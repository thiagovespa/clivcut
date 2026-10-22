// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "FFmpegUtil.h"
#include "core/Model.h"

#include <QImage>
#include <QString>

#include <memory>

namespace clv::media {

/**
 * Decodificador de vídeo com acesso aleatório por tempo.
 *
 * `frameAt(t)` devolve o quadro exibido no instante t (o último com pts <= t).
 * Leitura sequencial é barata (só decodifica para frente); saltos para trás ou
 * muito à frente fazem seek no keyframe anterior e decodificam até t.
 * Imagens estáticas são carregadas uma vez via QImage.
 *
 * Não é thread-safe: use uma instância por thread.
 */
class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder &) = delete;
    VideoDecoder &operator=(const VideoDecoder &) = delete;

    /**
     * `maxSize` limita a resolução de saída (0 = original). Com `allowHardware`,
     * usa decodificação por GPU quando disponível (cai para software se não).
     */
    bool open(const QString &path, QSize maxSize = {}, QString *error = nullptr, bool allowHardware = true);
    /** Dispositivo de hardware em uso ("vaapi", "cuda"...) ou vazio se software. */
    QString hardwareDecoder() const { return hardwareDecoderName(m_hwFormat); }
    bool isOpen() const { return m_still.isNull() ? bool(m_fmt) : true; }
    QString path() const { return m_path; }
    QSize outputSize() const { return m_outSize; }
    Micros duration() const { return m_duration; }

    /** Quadro em RGBA8888 premultiplicado. Retorna imagem nula em erro/fim. */
    QImage frameAt(Micros t);
    /**
     * Intervalo [início, fim) de tempos que devolvem o mesmo quadro do último `frameAt`
     * (imagens estáticas: o tempo todo; último quadro do vídeo: até o infinito).
     */
    Micros frameStart() const;
    Micros frameEnd() const;
    /** Buscas feitas (saltos no arquivo), para medir o custo de um padrão de acesso. */
    int seeks() const { return m_seeks; }

private:
    bool decodeUntil(Micros t);
    bool seekTo(Micros t);
    QImage convert(const AVFrame *f);

    QString m_path;
    QImage m_still;
    FormatCtxPtr m_fmt;
    CodecCtxPtr m_dec;
    SwsPtr m_sws;
    PacketPtr m_pkt;
    int m_stream = -1;
    AVRational m_tb{1, 1};
    Micros m_duration = 0;
    Micros m_startTime = 0;
    QSize m_outSize;
    QSize m_maxSize;

    double m_rotation = 0; ///< metadado de rotação (vídeos de celular)
    AVPixelFormat m_hwFormat = AV_PIX_FMT_NONE;
    FramePtr m_swFrame; ///< destino da cópia GPU → CPU
    struct GpuScaler;
    std::unique_ptr<GpuScaler> m_gpuScaler;

    // Último quadro com pts <= alvo e o quadro seguinte já decodificado.
    FramePtr m_last;
    Micros m_lastPts = -1;
    FramePtr m_peek;
    Micros m_peekPts = -1;

    // Último quadro convertido para RGBA.
    QImage m_current;
    Micros m_currentPts = -1;
    Micros m_frameDur = 33'333;
    bool m_eof = false;
    int m_seeks = 0;
    int m_swsSrcFormat = -1;
    int m_swsSrcW = 0, m_swsSrcH = 0;
};

} // namespace clv::media
