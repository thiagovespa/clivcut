// SPDX-License-Identifier: GPL-3.0-or-later
#include "VideoDecoder.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
}

#include <QCoreApplication>
#include <QFileInfo>
#include <QImageReader>
#include <QTransform>

#include <cmath>
#include <cstring>
#include <limits>

namespace clv::media {

namespace {

constexpr Micros kForwardDecodeLimit = 2 * kSecond;

QSize fitWithin(QSize src, QSize max)
{
    if (max.isEmpty() || (src.width() <= max.width() && src.height() <= max.height()))
        return src;
    QSize s = src.scaled(max, Qt::KeepAspectRatio);
    // Dimensões pares evitam artefatos em conversões YUV posteriores.
    return {std::max(2, s.width() & ~1), std::max(2, s.height() & ~1)};
}

double streamRotation(const AVStream *st)
{
    const AVPacketSideData *sd = av_packet_side_data_get(st->codecpar->coded_side_data,
                                                         st->codecpar->nb_coded_side_data,
                                                         AV_PKT_DATA_DISPLAYMATRIX);
    if (!sd || sd->size < 9 * 4)
        return 0;
    const double r = -av_display_rotation_get(reinterpret_cast<const int32_t *>(sd->data));
    return std::isnan(r) ? 0 : r;
}

struct DecodeState {
    AVFormatContext *fmt;
    AVCodecContext *dec;
    AVPacket *pkt;
    int stream;
    bool *eof;
};

/** Próximo quadro decodificado do stream; false no fim. */
bool nextFrame(DecodeState s, AVFrame *out)
{
    for (;;) {
        int err = avcodec_receive_frame(s.dec, out);
        if (err == 0)
            return true;
        if (err == AVERROR_EOF)
            return false;
        if (err != AVERROR(EAGAIN))
            return false;
        if (*s.eof) {
            return false;
        }
        err = av_read_frame(s.fmt, s.pkt);
        if (err < 0) {
            *s.eof = true;
            avcodec_send_packet(s.dec, nullptr); // drena o decodificador
            continue;
        }
        if (s.pkt->stream_index == s.stream)
            avcodec_send_packet(s.dec, s.pkt);
        av_packet_unref(s.pkt);
    }
}

} // namespace

/**
 * Redimensiona quadros de GPU na própria GPU antes de baixá-los para a CPU
 * (VA-API: scale_vaapi + hwdownload). Copiar um 4K inteiro e reduzir na CPU
 * custa mais que decodificar em software.
 */
struct VideoDecoder::GpuScaler {
    AVFilterGraph *graph = nullptr;
    AVFilterContext *src = nullptr;
    AVFilterContext *sink = nullptr;
    QSize size;
    bool failed = false;

    ~GpuScaler() { avfilter_graph_free(&graph); }

    bool init(const AVFrame *f, QSize out, AVRational tb)
    {
        size = out; // mesmo em falha: não tenta recriar a cada quadro
        graph = avfilter_graph_alloc();
        // O formato de hardware exige hw_frames_ctx antes de inicializar o filtro.
        src = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"), "in");
        if (!src)
            return false;
        AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
        par->format = f->format;
        par->width = f->width;
        par->height = f->height;
        par->time_base = tb.den > 0 ? tb : AVRational{1, 1};
        par->sample_aspect_ratio = AVRational{1, 1};
        par->hw_frames_ctx = f->hw_frames_ctx;
#if LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(10, 4, 100) // FFmpeg 7.1+
        par->color_space = f->colorspace;
        par->color_range = f->color_range;
#endif
        const int rc = av_buffersrc_parameters_set(src, par);
        av_free(par);
        if (rc < 0 || avfilter_init_str(src, nullptr) < 0)
            return false;
        if (avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph) < 0)
            return false;
        const QByteArray chain =
            QStringLiteral("scale_vaapi=w=%1:h=%2:format=nv12,hwdownload,format=nv12").arg(out.width()).arg(out.height()).toLatin1();
        AVFilterInOut *outputs = avfilter_inout_alloc();
        AVFilterInOut *inputs = avfilter_inout_alloc();
        outputs->name = av_strdup("in");
        outputs->filter_ctx = src;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink;
        const int prc = avfilter_graph_parse_ptr(graph, chain.constData(), &inputs, &outputs, nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        return prc >= 0 && avfilter_graph_config(graph, nullptr) >= 0;
    }

    /** Quadro reduzido em NV12 na CPU, ou false se falhar. */
    bool scale(const AVFrame *in, AVFrame *out)
    {
        AVFrame *ref = av_frame_clone(in);
        const int rc = av_buffersrc_add_frame(src, ref);
        av_frame_free(&ref);
        if (rc < 0)
            return false;
        av_frame_unref(out);
        return av_buffersink_get_frame(sink, out) >= 0;
    }
};

VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() = default;

bool VideoDecoder::open(const QString &path, QSize maxSize, QString *error, bool allowHardware)
{
    m_path = path;
    m_maxSize = maxSize;

    // Imagens estáticas: QImage cuida de EXIF/orientação.
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QByteArray fmt = reader.format();
    if (!fmt.isEmpty() && fmt != "gif" && reader.imageCount() <= 1) {
        QImage img = reader.read();
        if (!img.isNull()) {
            m_outSize = fitWithin(img.size(), maxSize);
            m_still = img.scaled(m_outSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                          .convertToFormat(QImage::Format_RGBA8888);
            return true;
        }
    }

    m_fmt = openInput(path, error);
    if (!m_fmt)
        return false;
    m_stream = av_find_best_stream(m_fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_stream < 0) {
        if (error)
            *error = QCoreApplication::translate("Media", "No video stream");
        m_fmt.reset();
        return false;
    }
    AVStream *st = m_fmt->streams[m_stream];
    // Imagens (MJPEG/PNG em contêiner) não ganham nada com a GPU.
    m_dec = openVideoDecoder(st, allowHardware && !isStillImageCodec(st->codecpar->codec_id), &m_hwFormat, error);
    if (!m_dec) {
        m_fmt.reset();
        return false;
    }
    m_tb = st->time_base;
    m_startTime = st->start_time != AV_NOPTS_VALUE ? streamToMicros(st->start_time, m_tb) : 0;
    m_duration = st->duration != AV_NOPTS_VALUE ? streamToMicros(st->duration, m_tb)
                                                 : (m_fmt->duration > 0 ? m_fmt->duration : 0);
    const AVRational fr = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
    if (fr.num > 0 && fr.den > 0)
        m_frameDur = static_cast<Micros>(kSecond * static_cast<double>(fr.den) / fr.num);

    QSize src(m_dec->width, m_dec->height);
    const double rot = streamRotation(st);
    const bool swap = std::lround(std::abs(rot)) % 180 == 90;
    if (swap)
        src.transpose();
    m_outSize = fitWithin(src, maxSize);
    m_pkt.reset(av_packet_alloc());
    m_rotation = rot;
    return true;
}

bool VideoDecoder::seekTo(Micros t)
{
    ++m_seeks;
    const std::int64_t ts = microsToStream(t, m_tb);
    av_seek_frame(m_fmt.get(), m_stream, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_dec.get());
    m_eof = false;
    m_last.reset();
    m_peek.reset();
    m_lastPts = -1;
    m_peekPts = -1;
    return true;
}

bool VideoDecoder::decodeUntil(Micros target)
{
    DecodeState s{m_fmt.get(), m_dec.get(), m_pkt.get(), m_stream, &m_eof};
    if (m_peek && m_peekPts <= target) {
        m_last = std::move(m_peek);
        m_lastPts = m_peekPts;
        m_peek.reset();
        m_peekPts = -1;
    }
    while (!m_peek) {
        FramePtr f(av_frame_alloc());
        if (!nextFrame(s, f.get()))
            break;
        const std::int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        const Micros pts = ts == AV_NOPTS_VALUE ? (m_lastPts < 0 ? 0 : m_lastPts + m_frameDur) : streamToMicros(ts, m_tb);
        if (pts <= target || !m_last) {
            // Sem quadro anterior (seek impreciso), o primeiro quadro serve.
            if (pts > target && !m_last) {
                m_last = std::move(f);
                m_lastPts = pts;
                break;
            }
            m_last = std::move(f);
            m_lastPts = pts;
        } else {
            m_peek = std::move(f);
            m_peekPts = pts;
        }
    }
    return bool(m_last);
}

bool VideoDecoder::position(Micros t)
{
    const Micros target = std::max<Micros>(0, t) + m_startTime;
    const bool haveFrame = m_last && m_lastPts >= 0;
    const bool withinCurrent = haveFrame && target >= m_lastPts && (!m_peek || target < m_peekPts);
    if (withinCurrent)
        return true;
    const bool forward = haveFrame && target > m_lastPts && target - m_lastPts < kForwardDecodeLimit;
    if (!forward)
        seekTo(target);
    return decodeUntil(target);
}

QImage VideoDecoder::frameAt(Micros t)
{
    if (!m_still.isNull())
        return m_still;
    if (!m_fmt)
        return {};
    if (!position(t))
        return m_current;
    if (m_lastPts != m_currentPts || m_current.isNull()) {
        m_current = toRgba(toCpu(m_last.get()));
        m_currentPts = m_lastPts;
    }
    m_shownPts = m_currentPts;
    return m_current;
}

VideoImage VideoDecoder::frameImage(Micros t, bool allowYuv)
{
    if (!allowYuv)
        return frameAt(t);
    if (!m_still.isNull())
        return m_still;
    if (!m_fmt)
        return {};
    if (!position(t))
        return m_image;
    if (m_lastPts != m_imagePts || m_image.isNull()) {
        m_image = toVideoImage(m_last.get());
        m_imagePts = m_lastPts;
    }
    m_shownPts = m_imagePts;
    return m_image;
}

Micros VideoDecoder::frameStart() const
{
    if (!m_still.isNull() || m_shownPts < 0)
        return std::numeric_limits<Micros>::min();
    return m_shownPts - m_startTime;
}

Micros VideoDecoder::frameEnd() const
{
    if (!m_still.isNull() || m_shownPts < 0)
        return std::numeric_limits<Micros>::max();
    if (m_shownPts == m_lastPts && m_peek)
        return m_peekPts - m_startTime;
    if (m_eof)
        return std::numeric_limits<Micros>::max();
    return m_shownPts + m_frameDur - m_startTime;
}

const AVFrame *VideoDecoder::toCpu(const AVFrame *f)
{
    if (!f)
        return nullptr;
    // Quadro na GPU: reduz na GPU (VA-API) e então copia só o necessário.
    QSize gpuTarget = m_outSize;
    if (std::lround(std::abs(m_rotation)) % 180 == 90)
        gpuTarget.transpose();
    if (f->hw_frames_ctx && m_hwFormat == AV_PIX_FMT_VAAPI && gpuTarget != QSize(f->width, f->height)) {
        if (!m_gpuScaler || m_gpuScaler->size != gpuTarget) {
            m_gpuScaler = std::make_unique<GpuScaler>();
            if (!m_gpuScaler->init(f, gpuTarget, m_tb)) {
                m_gpuScaler->failed = true;
                qWarning("clivcut: GPU scaling unavailable; falling back to a full copy");
            }
        }
        if (!m_gpuScaler->failed) {
            if (!m_swFrame)
                m_swFrame.reset(av_frame_alloc());
            if (m_gpuScaler->scale(f, m_swFrame.get())) {
                m_swFrame->color_range = f->color_range;
                m_swFrame->colorspace = f->colorspace;
                f = m_swFrame.get();
            } else {
                m_gpuScaler->failed = true;
            }
        }
    }
    // Quadro ainda na GPU: copia para a memória (NV12/P010) antes de converter.
    if (f->hw_frames_ctx) {
        if (!m_swFrame)
            m_swFrame.reset(av_frame_alloc());
        av_frame_unref(m_swFrame.get());
        if (av_hwframe_transfer_data(m_swFrame.get(), f, 0) < 0)
            return nullptr;
        m_swFrame->color_range = f->color_range;
        m_swFrame->colorspace = f->colorspace;
        f = m_swFrame.get();
    }
    return f;
}

QImage VideoDecoder::toRgba(const AVFrame *f)
{
    if (!f)
        return {};
    QSize decoded = m_outSize;
    const bool swap = std::lround(std::abs(m_rotation)) % 180 == 90;
    if (swap)
        decoded.transpose();
    if (!m_sws || m_swsSrcFormat != f->format || m_swsSrcW != f->width || m_swsSrcH != f->height) {
        m_sws.reset(sws_getContext(f->width, f->height, static_cast<AVPixelFormat>(f->format), decoded.width(),
                                   decoded.height(), AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!m_sws)
            return {};
        const int colorspace = f->colorspace == AVCOL_SPC_BT709 || (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720)
                                   ? SWS_CS_ITU709
                                   : SWS_CS_ITU601;
        const int srcRange = f->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
        sws_setColorspaceDetails(m_sws.get(), sws_getCoefficients(colorspace), srcRange, sws_getCoefficients(SWS_CS_DEFAULT),
                                 1, 0, 1 << 16, 1 << 16);
        m_swsSrcFormat = f->format;
        m_swsSrcW = f->width;
        m_swsSrcH = f->height;
    }
    QImage img(decoded, QImage::Format_RGBA8888);
    uint8_t *dst[4] = {img.bits(), nullptr, nullptr, nullptr};
    int dstStride[4] = {static_cast<int>(img.bytesPerLine()), 0, 0, 0};
    sws_scale(m_sws.get(), f->data, f->linesize, 0, f->height, dst, dstStride);
    if (std::abs(m_rotation) > 0.5)
        img = img.transformed(QTransform().rotate(m_rotation));
    return img;
}

VideoImage VideoDecoder::toVideoImage(const AVFrame *original)
{
    const AVFrame *f = toCpu(original);
    if (!f)
        return {};
    const int rot = static_cast<int>(((std::lround(m_rotation) % 360) + 360) % 360);
    const auto fmt = static_cast<AVPixelFormat>(f->format);
    const bool nv12 = fmt == AV_PIX_FMT_NV12;
    const bool i420 = fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P;
    if (rot % 90 != 0 || !(nv12 || i420))
        return toRgba(f);

    QSize decoded = m_outSize;
    if (rot == 90 || rot == 270)
        decoded.transpose();
    const QSize srcSize(f->width, f->height);
    if (srcSize != decoded) {
        // Só redimensiona, no mesmo formato: a cor é convertida na GPU. YUVJ é o mesmo layout
        // de YUV420P; tratado assim, o sws não converte a faixa de cor.
        const AVPixelFormat swsFmt = nv12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
        if (!m_yuvSws || m_yuvSwsFormat != swsFmt || m_yuvSwsSrc != srcSize || m_yuvSwsDst != decoded) {
            m_yuvSws.reset(sws_getContext(srcSize.width(), srcSize.height(), swsFmt, decoded.width(), decoded.height(), swsFmt,
                                          SWS_BILINEAR, nullptr, nullptr, nullptr));
            m_yuvFrame.reset(av_frame_alloc());
            m_yuvFrame->format = swsFmt;
            m_yuvFrame->width = decoded.width();
            m_yuvFrame->height = decoded.height();
            if (!m_yuvSws || av_frame_get_buffer(m_yuvFrame.get(), 0) < 0) {
                m_yuvSws.reset();
                return toRgba(f);
            }
            m_yuvSwsFormat = swsFmt;
            m_yuvSwsSrc = srcSize;
            m_yuvSwsDst = decoded;
        }
        sws_scale(m_yuvSws.get(), f->data, f->linesize, 0, f->height, m_yuvFrame->data, m_yuvFrame->linesize);
    }
    const AVFrame *src = srcSize != decoded ? m_yuvFrame.get() : f;

    VideoImage img;
    img.format = nv12 ? VideoImage::Format::Nv12 : VideoImage::Format::I420;
    img.size = decoded;
    img.rotation = rot;
    // Mesma escolha de matriz da conversão em RGBA, pela altura original do vídeo.
    img.bt709 = f->colorspace == AVCOL_SPC_BT709 || (f->colorspace == AVCOL_SPC_UNSPECIFIED && original->height >= 720);
    img.fullRange = f->color_range == AVCOL_RANGE_JPEG || fmt == AV_PIX_FMT_YUVJ420P;
    img.key = VideoImage::newKey();
    const QSize c = img.chromaSize();
    const auto copyPlane = [&](int i, int rowBytes, int rows) {
        QByteArray &out = img.planes[i];
        out.resize(static_cast<qsizetype>(rowBytes) * rows);
        for (int y = 0; y < rows; ++y)
            std::memcpy(out.data() + static_cast<qsizetype>(y) * rowBytes, src->data[i] + static_cast<qsizetype>(y) * src->linesize[i],
                        static_cast<size_t>(rowBytes));
    };
    copyPlane(0, decoded.width(), decoded.height());
    if (nv12) {
        copyPlane(1, c.width() * 2, c.height());
    } else {
        copyPlane(1, c.width(), c.height());
        copyPlane(2, c.width(), c.height());
    }
    return img;
}

} // namespace clv::media
