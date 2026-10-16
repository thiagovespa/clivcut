// SPDX-License-Identifier: GPL-3.0-or-later
#include "Encoder.h"

#include "AudioDecoder.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

#include <QCoreApplication>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace clv::media {

namespace {

struct CodecChoice {
    const char *encoder;
    AVPixelFormat pixfmt;
};

std::vector<CodecChoice> softwareCandidates(VideoCodec c)
{
    switch (c) {
    case VideoCodec::H264: return {{"libx264", AV_PIX_FMT_YUV420P}, {"libopenh264", AV_PIX_FMT_YUV420P}};
    case VideoCodec::HEVC: return {{"libx265", AV_PIX_FMT_YUV420P}};
    case VideoCodec::AV1: return {{"libsvtav1", AV_PIX_FMT_YUV420P}, {"libaom-av1", AV_PIX_FMT_YUV420P}};
    case VideoCodec::ProRes: return {{"prores_ks", AV_PIX_FMT_YUV422P10LE}};
    case VideoCodec::ProRes4444: return {{"prores_ks", AV_PIX_FMT_YUVA444P10LE}};
    case VideoCodec::VP9Alpha: return {{"libvpx-vp9", AV_PIX_FMT_YUVA420P}};
    }
    return {};
}

// Encoders de GPU por ordem de preferência. VA-API recebe superfícies de GPU; os demais aceitam
// quadros em memória (NV12) e fazem o upload internamente.
std::vector<CodecChoice> hardwareCandidates(VideoCodec c)
{
    switch (c) {
    case VideoCodec::H264:
        return {{"h264_vaapi", AV_PIX_FMT_VAAPI}, {"h264_nvenc", AV_PIX_FMT_NV12}, {"h264_qsv", AV_PIX_FMT_NV12},
                {"h264_videotoolbox", AV_PIX_FMT_NV12}, {"h264_amf", AV_PIX_FMT_NV12}, {"h264_mf", AV_PIX_FMT_NV12}};
    case VideoCodec::HEVC:
        return {{"hevc_vaapi", AV_PIX_FMT_VAAPI}, {"hevc_nvenc", AV_PIX_FMT_NV12}, {"hevc_qsv", AV_PIX_FMT_NV12},
                {"hevc_videotoolbox", AV_PIX_FMT_NV12}, {"hevc_amf", AV_PIX_FMT_NV12}, {"hevc_mf", AV_PIX_FMT_NV12}};
    case VideoCodec::AV1:
        return {{"av1_vaapi", AV_PIX_FMT_VAAPI}, {"av1_nvenc", AV_PIX_FMT_NV12}, {"av1_qsv", AV_PIX_FMT_NV12},
                {"av1_amf", AV_PIX_FMT_NV12}};
    default: return {};
    }
}

bool endsWith(const QString &s, const char *suffix) { return s.endsWith(QLatin1String(suffix)); }

const std::pair<VideoCodec, const char *> kCodecNames[] = {
    {VideoCodec::H264, "h264"},           {VideoCodec::HEVC, "hevc"},        {VideoCodec::AV1, "av1"},
    {VideoCodec::ProRes, "prores"},       {VideoCodec::ProRes4444, "prores4444"}, {VideoCodec::VP9Alpha, "vp9alpha"}};

// Formatos de amostra que o encoder de áudio aceita.
std::vector<AVSampleFormat> sampleFormats(const AVCodec *codec)
{
    std::vector<AVSampleFormat> out;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void *cfg = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &cfg, &n) >= 0 && cfg)
        for (int i = 0; i < n; ++i)
            out.push_back(static_cast<const AVSampleFormat *>(cfg)[i]);
#else
    for (const AVSampleFormat *f = codec->sample_fmts; f && *f != AV_SAMPLE_FMT_NONE; ++f)
        out.push_back(*f);
#endif
    return out;
}

// Encoder de áudio para um arquivo só de áudio, pela extensão.
const AVCodec *audioOnlyCodec(const QString &path)
{
    const QString ext = path.section(QLatin1Char('.'), -1).toLower();
    const char *names[3] = {nullptr, nullptr, nullptr};
    if (ext == QLatin1String("wav"))
        names[0] = "pcm_s16le";
    else if (ext == QLatin1String("flac"))
        names[0] = "flac";
    else if (ext == QLatin1String("m4a") || ext == QLatin1String("aac"))
        names[0] = "aac";
    else if (ext == QLatin1String("mp3"))
        names[0] = "libmp3lame";
    else if (ext == QLatin1String("opus") || ext == QLatin1String("ogg"))
        names[0] = "libopus", names[1] = "opus";
    for (const char *n : names)
        if (n)
            if (const AVCodec *c = avcodec_find_encoder_by_name(n))
                return c;
    return nullptr;
}

} // namespace

QString videoCodecName(VideoCodec c)
{
    for (const auto &[codec, name] : kCodecNames)
        if (codec == c)
            return QString::fromLatin1(name);
    return QStringLiteral("h264");
}

VideoCodec videoCodecFromName(const QString &name)
{
    for (const auto &[codec, n] : kCodecNames)
        if (name == QLatin1String(n))
            return codec;
    return VideoCodec::H264;
}

bool codecHasAlpha(VideoCodec c) { return c == VideoCodec::ProRes4444 || c == VideoCodec::VP9Alpha; }

QString containerFor(VideoCodec c)
{
    switch (c) {
    case VideoCodec::ProRes:
    case VideoCodec::ProRes4444: return QStringLiteral("mov");
    case VideoCodec::VP9Alpha: return QStringLiteral("webm");
    default: return QStringLiteral("mp4");
    }
}

QStringList availableVideoCodecs()
{
    QStringList out;
    for (const auto &[c, name] : kCodecNames)
        for (const auto &cand : softwareCandidates(c))
            if (avcodec_find_encoder_by_name(cand.encoder)) {
                out << QString::fromLatin1(name);
                break;
            }
    return out;
}

QStringList availableHardwareCodecs()
{
    // Abrir um encoder de GPU é a única forma confiável de saber se o driver o suporta.
    static const QStringList codecs = [] {
        QStringList out;
        for (VideoCodec c : {VideoCodec::H264, VideoCodec::HEVC, VideoCodec::AV1}) {
            // 640x360: o VideoToolbox dos Macs Intel recusa sessões menores (-12903).
            EncodeSettings s;
            s.width = 640;
            s.height = 360;
            s.codec = c;
            s.hardware = true;
            s.audio = false;
            Encoder probe;
            for (const auto &cand : hardwareCandidates(c)) {
                const AVCodec *codec = avcodec_find_encoder_by_name(cand.encoder);
                if (codec && probe.tryVideoProbe(codec, cand.encoder, cand.pixfmt, s)) {
                    out << videoCodecName(c);
                    break;
                }
            }
        }
        return out;
    }();
    return codecs;
}

QStringList availableAudioFormats()
{
    QStringList out;
    for (const char *ext : {"wav", "flac", "m4a", "opus", "mp3"})
        if (audioOnlyCodec(QStringLiteral("x.") + QLatin1String(ext)))
            out << QString::fromLatin1(ext);
    return out;
}

Encoder::~Encoder()
{
    if (m_out && m_headerWritten && !m_finished)
        finish();
    if (m_out) {
        if (!(m_out->oformat->flags & AVFMT_NOFILE) && m_out->pb)
            avio_closep(&m_out->pb);
        avformat_free_context(m_out);
    }
    av_buffer_unref(&m_hwFrames);
    av_buffer_unref(&m_hwDevice);
}

bool Encoder::tryVideoProbe(const AVCodec *codec, const char *name, AVPixelFormat pixfmt, const EncodeSettings &s)
{
    const bool ok = tryVideo(codec, {name, pixfmt}, s);
    m_venc.reset();
    av_buffer_unref(&m_hwFrames);
    m_hwDirect = false;
    return ok;
}

bool Encoder::tryVideo(const AVCodec *vcodec, const Choice &choice, const EncodeSettings &s)
{
    m_venc.reset(avcodec_alloc_context3(vcodec));
    AVCodecContext *v = m_venc.get();
    const QString name = QString::fromLatin1(vcodec->name);
    v->width = s.width & ~1;
    v->height = s.height & ~1;
    const AVRational fr = av_d2q(s.fps, 100000);
    v->framerate = fr;
    v->time_base = av_inv_q(fr);
    v->pix_fmt = choice.pixfmt;
    v->gop_size = s.gopFrames > 0 ? s.gopFrames : static_cast<int>(s.fps * 2);
    v->color_primaries = AVCOL_PRI_BT709;
    v->color_trc = AVCOL_TRC_BT709;
    v->colorspace = AVCOL_SPC_BT709;
    v->color_range = AVCOL_RANGE_MPEG;
    if (!(vcodec->capabilities & AV_CODEC_CAP_HARDWARE))
        v->thread_count = std::min(QThread::idealThreadCount(), 16);
    if (m_out && (m_out->oformat->flags & AVFMT_GLOBALHEADER))
        v->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    const bool vaapi = choice.pixfmt == AV_PIX_FMT_VAAPI;
    if (vaapi) {
        // Superfícies NV12 na GPU; o quadro em memória é enviado com av_hwframe_transfer_data.
        if (!m_hwDevice && av_hwdevice_ctx_create(&m_hwDevice, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0)
            return false;
        av_buffer_unref(&m_hwFrames);
        m_hwFrames = av_hwframe_ctx_alloc(m_hwDevice);
        if (!m_hwFrames)
            return false;
        auto *fc = reinterpret_cast<AVHWFramesContext *>(m_hwFrames->data);
        fc->format = AV_PIX_FMT_VAAPI;
        fc->sw_format = AV_PIX_FMT_NV12;
        fc->width = v->width;
        fc->height = v->height;
        fc->initial_pool_size = 16;
        if (av_hwframe_ctx_init(m_hwFrames) < 0) {
            av_buffer_unref(&m_hwFrames);
            return false;
        }
        v->hw_frames_ctx = av_buffer_ref(m_hwFrames);
    }

    // Qualidade: CRF nos encoders de software; equivalentes aproximados nos de hardware.
    if (s.bitrateKbps > 0) {
        v->bit_rate = static_cast<int64_t>(s.bitrateKbps) * 1000;
    } else if (name == QLatin1String("libx264") || name == QLatin1String("libx265")) {
        av_opt_set_int(v->priv_data, "crf", s.crf, 0);
    } else if (name == QLatin1String("libsvtav1") || name == QLatin1String("libaom-av1")) {
        av_opt_set_int(v->priv_data, "crf", s.crf + 10, 0);
        v->bit_rate = 0;
    } else if (name == QLatin1String("libvpx-vp9")) {
        av_opt_set_int(v->priv_data, "crf", s.crf + 12, 0);
        av_opt_set_int(v->priv_data, "row-mt", 1, 0);
        v->bit_rate = 0;
    } else if (name == QLatin1String("prores_ks")) {
        av_opt_set_int(v->priv_data, "profile", codecHasAlpha(s.codec) ? 4 : 3, 0); // 4444 ou 422 HQ
        if (codecHasAlpha(s.codec))
            av_opt_set_int(v->priv_data, "alpha_bits", 16, 0);
    } else if (vaapi) {
        av_opt_set(v->priv_data, "rc_mode", "CQP", 0);
        av_opt_set_int(v, "qp", s.crf + 2, AV_OPT_SEARCH_CHILDREN);
    } else if (endsWith(name, "_nvenc")) {
        av_opt_set(v->priv_data, "rc", "vbr", 0);
        av_opt_set_int(v->priv_data, "cq", s.crf + 3, 0);
        v->bit_rate = 0;
    } else if (endsWith(name, "_qsv")) {
        v->global_quality = s.crf + 2; // ICQ
    } else {
        v->bit_rate = static_cast<int64_t>(v->width * static_cast<double>(v->height) * s.fps * 0.09 * std::pow(0.9, s.crf - 20));
    }
    if (name.startsWith(QLatin1String("libx26")))
        av_opt_set(v->priv_data, "preset", s.preset.toLatin1().constData(), 0);
    if (name == QLatin1String("libx265") || name.startsWith(QLatin1String("hevc_")))
        av_opt_set(v->priv_data, "tag", "hvc1", 0);

    if (avcodec_open2(v, vcodec, nullptr) < 0) {
        m_venc.reset();
        av_buffer_unref(&m_hwFrames);
        return false;
    }
    m_hwDirect = !vaapi && (vcodec->capabilities & AV_CODEC_CAP_HARDWARE);
    m_alpha = codecHasAlpha(s.codec);
    return true;
}

bool Encoder::openVideo(const EncodeSettings &s, QString *error)
{
    std::vector<CodecChoice> list;
    if (s.hardware)
        list = hardwareCandidates(s.codec);
    for (const auto &c : softwareCandidates(s.codec))
        list.push_back(c);
    const AVCodec *vcodec = nullptr;
    for (const auto &cand : list) {
        const AVCodec *c = avcodec_find_encoder_by_name(cand.encoder);
        if (c && tryVideo(c, {cand.encoder, cand.pixfmt}, s)) {
            vcodec = c;
            break;
        }
    }
    if (!vcodec) {
        if (error)
            *error = QCoreApplication::translate("Media", "No encoder available for the chosen codec in this FFmpeg");
        return false;
    }
    AVCodecContext *v = m_venc.get();
    m_codecName = QString::fromLatin1(vcodec->name);
    m_vst = avformat_new_stream(m_out, nullptr);
    avcodec_parameters_from_context(m_vst->codecpar, v);
    m_vst->time_base = v->time_base;
    m_vst->avg_frame_rate = v->framerate;
    if (m_codecName == QLatin1String("libx265") || m_codecName.startsWith(QLatin1String("hevc_")))
        m_vst->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');

    m_vframe.reset(av_frame_alloc());
    m_vframe->format = m_hwFrames ? AV_PIX_FMT_NV12 : v->pix_fmt;
    m_vframe->width = v->width;
    m_vframe->height = v->height;
    av_frame_get_buffer(m_vframe.get(), 0);
    if (m_hwFrames)
        m_hwFrame.reset(av_frame_alloc());
    return true;
}

bool Encoder::openAudio(const QString &path, const EncodeSettings &s, QString *error)
{
    const AVOutputFormat *ofmt = m_out->oformat;
    const AVCodec *acodec = nullptr;
    if (!s.video) {
        acodec = audioOnlyCodec(path);
    } else if (ofmt == av_guess_format("webm", nullptr, nullptr)) {
        acodec = avcodec_find_encoder_by_name("libopus");
        if (!acodec)
            acodec = avcodec_find_encoder_by_name("opus");
    } else {
        acodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (ofmt == av_guess_format("matroska", nullptr, nullptr) && avcodec_find_encoder_by_name("libopus"))
            acodec = avcodec_find_encoder_by_name("libopus");
    }
    if (!acodec) {
        if (!s.video && error)
            *error = QCoreApplication::translate("Media", "No audio encoder available for this format");
        return s.video; // vídeo segue sem áudio; só áudio sem encoder é erro
    }
    m_ast = avformat_new_stream(m_out, nullptr);
    m_aenc.reset(avcodec_alloc_context3(acodec));
    AVCodecContext *a = m_aenc.get();
    a->sample_rate = kSampleRate;
    // Preferência: float planar, float, 16 bits (formatos que o mixer sabe entregar).
    const auto fmts = sampleFormats(acodec);
    a->sample_fmt = fmts.empty() ? AV_SAMPLE_FMT_FLTP : fmts.front();
    for (AVSampleFormat want : {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_S32})
        if (std::find(fmts.begin(), fmts.end(), want) != fmts.end()) {
            a->sample_fmt = want;
            break;
        }
    av_channel_layout_default(&a->ch_layout, 2);
    a->bit_rate = static_cast<int64_t>(s.audioBitrateKbps) * 1000;
    a->time_base = {1, kSampleRate};
    a->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL; // encoder opus nativo
    if (ofmt->flags & AVFMT_GLOBALHEADER)
        a->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    const int err = avcodec_open2(a, acodec, nullptr);
    if (err < 0) {
        if (error)
            *error = QCoreApplication::translate("Media", "Failed to open the audio encoder: %1").arg(avError(err));
        return false;
    }
    avcodec_parameters_from_context(m_ast->codecpar, a);
    m_ast->time_base = a->time_base;
    m_aframe.reset(av_frame_alloc());
    m_aframe->format = a->sample_fmt;
    av_channel_layout_copy(&m_aframe->ch_layout, &a->ch_layout);
    m_aframe->sample_rate = a->sample_rate;
    m_aframe->nb_samples = a->frame_size > 0 ? a->frame_size : 1024;
    av_frame_get_buffer(m_aframe.get(), 0);
    if (!s.video)
        m_codecName = QString::fromLatin1(acodec->name);
    return true;
}

bool Encoder::open(const QString &path, const EncodeSettings &s, QString *error)
{
    auto fail = [&](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    AVFormatContext *oc = nullptr;
    int err = avformat_alloc_output_context2(&oc, nullptr, nullptr, path.toUtf8().constData());
    if (err < 0 || !oc)
        return fail(QCoreApplication::translate("Media", "Unsupported output format: %1").arg(avError(err)));
    m_out = oc;

    if (s.video && !openVideo(s, error))
        return false;
    if ((s.audio || !s.video) && !openAudio(path, s, error))
        return false;

    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        err = avio_open(&oc->pb, path.toUtf8().constData(), AVIO_FLAG_WRITE);
        if (err < 0)
            return fail(QCoreApplication::translate("Media", "Could not create %1: %2").arg(path, avError(err)));
    }
    AVDictionary *opts = nullptr;
    if (path.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive) || path.endsWith(QLatin1String(".mov"), Qt::CaseInsensitive)
        || path.endsWith(QLatin1String(".m4a"), Qt::CaseInsensitive))
        av_dict_set(&opts, "movflags", "+faststart", 0);
    err = avformat_write_header(oc, &opts);
    av_dict_free(&opts);
    if (err < 0)
        return fail(QCoreApplication::translate("Media", "Failed to write header: %1").arg(avError(err)));
    m_headerWritten = true;
    m_pkt.reset(av_packet_alloc());
    return true;
}

bool Encoder::encode(AVCodecContext *ctx, AVStream *st, AVFrame *f)
{
    int err = avcodec_send_frame(ctx, f);
    if (err < 0 && err != AVERROR_EOF)
        return false;
    for (;;) {
        err = avcodec_receive_packet(ctx, m_pkt.get());
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
            return true;
        if (err < 0)
            return false;
        av_packet_rescale_ts(m_pkt.get(), ctx->time_base, st->time_base);
        m_pkt->stream_index = st->index;
        if (av_interleaved_write_frame(m_out, m_pkt.get()) < 0)
            return false;
    }
}

bool Encoder::writeVideo(const QImage &img)
{
    AVCodecContext *v = m_venc.get();
    if (!v)
        return false;
    // Com alfa, o codec espera cor "reta": converter desfaz a pré-multiplicação.
    const QImage src = m_alpha ? img.convertToFormat(QImage::Format_RGBA8888)
                       : img.format() == QImage::Format_RGBA8888 || img.format() == QImage::Format_RGBA8888_Premultiplied
                           ? img
                           : img.convertToFormat(QImage::Format_RGBA8888);
    const auto dstFmt = static_cast<AVPixelFormat>(m_vframe->format);
    if (!m_sws) {
        m_sws.reset(sws_getContext(src.width(), src.height(), AV_PIX_FMT_RGBA, v->width, v->height, dstFmt,
                                   SWS_BICUBIC, nullptr, nullptr, nullptr));
        sws_setColorspaceDetails(m_sws.get(), sws_getCoefficients(SWS_CS_DEFAULT), 1, sws_getCoefficients(SWS_CS_ITU709),
                                 0, 0, 1 << 16, 1 << 16);
    }
    if (av_frame_make_writable(m_vframe.get()) < 0)
        return false;
    const uint8_t *srcData[4] = {src.constBits(), nullptr, nullptr, nullptr};
    const int srcStride[4] = {static_cast<int>(src.bytesPerLine()), 0, 0, 0};
    sws_scale(m_sws.get(), srcData, srcStride, 0, src.height(), m_vframe->data, m_vframe->linesize);
    m_vframe->pts = m_videoPts++;
    if (!m_hwFrames)
        return encode(v, m_vst, m_vframe.get());
    // VA-API: envia o quadro para uma superfície da GPU.
    av_frame_unref(m_hwFrame.get());
    if (av_hwframe_get_buffer(m_hwFrames, m_hwFrame.get(), 0) < 0
        || av_hwframe_transfer_data(m_hwFrame.get(), m_vframe.get(), 0) < 0)
        return false;
    m_hwFrame->pts = m_vframe->pts;
    return encode(v, m_vst, m_hwFrame.get());
}

bool Encoder::flushAudioFifo(bool final)
{
    if (!m_aenc)
        return true;
    AVCodecContext *a = m_aenc.get();
    const int n = m_aframe->nb_samples;
    while (m_audioFifo.size() >= static_cast<size_t>(n * kChannels) || (final && !m_audioFifo.empty())) {
        const int take = std::min<int>(n, static_cast<int>(m_audioFifo.size() / kChannels));
        if (av_frame_make_writable(m_aframe.get()) < 0)
            return false;
        // Último bloco menor quando o codec permite: a duração sai exata, sem silêncio extra.
        const bool shortLast = take < n && (a->codec->capabilities & (AV_CODEC_CAP_SMALL_LAST_FRAME | AV_CODEC_CAP_VARIABLE_FRAME_SIZE));
        m_aframe->nb_samples = shortLast ? take : n;
        auto sample = [&](int i, int c) { return i < take ? std::clamp(m_audioFifo[i * 2 + c], -1.f, 1.f) : 0.f; };
        switch (a->sample_fmt) {
        case AV_SAMPLE_FMT_FLTP:
            for (int c = 0; c < kChannels; ++c)
                for (int i = 0; i < n; ++i)
                    reinterpret_cast<float *>(m_aframe->data[c])[i] = sample(i, c);
            break;
        case AV_SAMPLE_FMT_FLT:
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < kChannels; ++c)
                    reinterpret_cast<float *>(m_aframe->data[0])[i * 2 + c] = sample(i, c);
            break;
        case AV_SAMPLE_FMT_S16:
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < kChannels; ++c)
                    reinterpret_cast<int16_t *>(m_aframe->data[0])[i * 2 + c] = static_cast<int16_t>(std::lrint(sample(i, c) * 32767.f));
            break;
        case AV_SAMPLE_FMT_S16P:
            for (int c = 0; c < kChannels; ++c)
                for (int i = 0; i < n; ++i)
                    reinterpret_cast<int16_t *>(m_aframe->data[c])[i] = static_cast<int16_t>(std::lrint(sample(i, c) * 32767.f));
            break;
        case AV_SAMPLE_FMT_S32:
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < kChannels; ++c)
                    reinterpret_cast<int32_t *>(m_aframe->data[0])[i * 2 + c] =
                        static_cast<int32_t>(std::lrint(sample(i, c) * 2147483520.0));
            break;
        default:
            return false;
        }
        m_audioFifo.erase(m_audioFifo.begin(), m_audioFifo.begin() + take * kChannels);
        m_aframe->pts = m_audioPts;
        m_audioPts += m_aframe->nb_samples;
        if (!encode(a, m_ast, m_aframe.get()))
            return false;
    }
    return true;
}

bool Encoder::writeAudio(const float *interleaved, int frames)
{
    if (!m_aenc)
        return true;
    m_audioFifo.insert(m_audioFifo.end(), interleaved, interleaved + static_cast<size_t>(frames) * kChannels);
    return flushAudioFifo(false);
}

bool Encoder::finish(QString *error)
{
    if (m_finished || !m_headerWritten)
        return m_finished;
    m_finished = true;
    bool ok = flushAudioFifo(true);
    if (m_venc)
        ok = encode(m_venc.get(), m_vst, nullptr) && ok;
    if (m_aenc)
        ok = encode(m_aenc.get(), m_ast, nullptr) && ok;
    const int err = av_write_trailer(m_out);
    if (err < 0 || !ok) {
        if (error)
            *error = QCoreApplication::translate("Media", "Failed to finalize the file: %1").arg(avError(err));
        return false;
    }
    return true;
}

} // namespace clv::media
