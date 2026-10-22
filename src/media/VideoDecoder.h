// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "FFmpegUtil.h"
#include "core/Model.h"
#include "core/VideoImage.h"

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
     * O mesmo quadro de `frameAt`, mas em YUV 4:2:0 de 8 bits (para a GPU converter) quando o
     * vídeo permite; senão (outros formatos, rotação fora de 90°, `allowYuv` falso) em RGBA.
     */
    VideoImage frameImage(Micros t, bool allowYuv = true);
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
    /** Posiciona m_last no quadro de `t`; false se não há quadro novo (fica o anterior). */
    bool position(Micros t);
    /** Quadro na memória (reduzido na GPU e baixado, se estiver na GPU), ou nulo. */
    const AVFrame *toCpu(const AVFrame *f);
    QImage toRgba(const AVFrame *f);
    VideoImage toVideoImage(const AVFrame *gpuOrCpu);

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

    // Último quadro convertido para RGBA e para VideoImage, e o pts do último devolvido.
    QImage m_current;
    Micros m_currentPts = -1;
    VideoImage m_image;
    Micros m_imagePts = -1;
    Micros m_shownPts = -1;
    SwsPtr m_yuvSws; ///< só redimensiona (YUV → YUV)
    FramePtr m_yuvFrame;
    int m_yuvSwsFormat = -1;
    QSize m_yuvSwsSrc, m_yuvSwsDst;
    Micros m_frameDur = 33'333;
    bool m_eof = false;
    int m_seeks = 0;
    int m_swsSrcFormat = -1;
    int m_swsSrcW = 0, m_swsSrcH = 0;
};

} // namespace clv::media
