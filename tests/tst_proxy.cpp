// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Edit.h"
#include "core/Evaluate.h"
#include "engine/DecoderFrameSource.h"
#include "engine/Matting.h"
#include "engine/RenderCache.h"
#include "engine/Stabilization.h"
#include "engine/MotionTracking.h"
#include "engine/Proxy.h"
#include "media/MediaProbe.h"
#include "media/VideoDecoder.h"

#include <QFileInfo>
#include <QPainter>
#include <QtMath>
#include <QTransform>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace clv;

class ProxyTest : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_video;

private slots:
    void initTestCase()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg não encontrado");
        qputenv("CLIVCUT_CACHE_DIR", m_dir.filePath(QStringLiteral("cache")).toUtf8());
        m_video = m_dir.filePath(QStringLiteral("grande.mp4"));
        QProcess p;
        p.start(QStringLiteral("ffmpeg"), {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                                           "testsrc2=size=2560x1440:rate=30:duration=3", "-c:v", "libx264", "-preset",
                                           "ultrafast", "-pix_fmt", "yuv420p", m_video});
        QVERIFY(p.waitForFinished(120000) && p.exitCode() == 0);
    }

    void recommendation()
    {
        MediaAsset a;
        QVERIFY(media::probe(m_video, a));
        QVERIFY(engine::proxyRecommended(a));
        a.width = 1920;
        a.height = 1080;
        QVERIFY(!engine::proxyRecommended(a));
        a.fps = 120;
        QVERIFY(engine::proxyRecommended(a));
    }

    void generateAndUse()
    {
        const QString out = engine::proxyPathFor(m_video);
        QVERIFY(out.startsWith(m_dir.path()));
        QVERIFY(!engine::discoverProxy(m_video));
        double last = 0;
        QString err;
        QVERIFY2(engine::generateProxy(m_video, out, [&](double f) { last = f; return true; }, &err), qPrintable(err));
        QCOMPARE(last, 1.0);
        MediaAsset p;
        QVERIFY(media::probe(out, p));
        QCOMPARE(p.width, 960);
        QCOMPARE(p.height, 540);
        QVERIFY(std::abs(p.duration - 3 * kSecond) < 100'000);
        QVERIFY(!QFileInfo::exists(out + QStringLiteral(".part.mp4")));

        // Registro + preferência: o preview decodifica o proxy (menor que o original).
        QVERIFY(engine::discoverProxy(m_video));
        QCOMPARE(engine::registeredProxy(m_video), out);
        Project proj = createProject(QStringLiteral("t"), 2560, 1440, 30);
        MediaAsset a;
        QVERIFY(media::probe(m_video, a));
        a.id = newId("ast");
        proj.assets.emplace(a.id, a);
        edit::insertClip(proj, createMediaClip(a, 0), 0);
        const VisualLayer layer = evaluate(proj, kSecond).layers.front();
        engine::DecoderFrameSource original(QSize(2560, 1440), false), proxied(QSize(2560, 1440), true);
        QCOMPARE(original.frameFor(layer).size(), QSize(2560, 1440));
        QCOMPARE(proxied.frameFor(layer).size(), QSize(960, 540));

        // Arquivo original alterado → outro caminho de proxy.
        engine::unregisterProxy(m_video);
        QVERIFY(engine::registeredProxy(m_video).isEmpty());
    }

    void matteAlpha()
    {
        // Matte sintético: metade esquerda branca (pessoa), direita preta (fundo).
        const QString matte = m_dir.filePath(QStringLiteral("matte.mp4"));
        QProcess p;
        p.start(QStringLiteral("ffmpeg"),
                {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                 "color=black:size=640x360:rate=30:duration=3,drawbox=x=0:y=0:w=320:h=360:color=white:t=fill",
                 "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", matte});
        QVERIFY(p.waitForFinished(120000) && p.exitCode() == 0);

        Project proj = createProject(QStringLiteral("t"), 2560, 1440, 30);
        MediaAsset a;
        QVERIFY(media::probe(m_video, a));
        a.id = newId("ast");
        proj.assets.emplace(a.id, a);
        edit::insertClip(proj, createMediaClip(a, 0), 0);
        proj.tracks[0].clips[0].removeBackground = true;
        const VisualLayer layer = evaluate(proj, kSecond).layers.front();
        engine::DecoderFrameSource source(QSize(1280, 720), false);

        // Sem matte pronto: quadro opaco.
        QCOMPARE(qAlpha(source.frameFor(layer).pixel(100, 360)), 255);

        engine::registerMatte(m_video, matte);
        const QImage img = source.frameFor(layer);
        QCOMPARE(img.size(), QSize(1280, 720));
        QVERIFY(qAlpha(img.pixel(200, 360)) > 240);
        QVERIFY(qAlpha(img.pixel(1080, 360)) < 15);
        // Mesmo quadro de novo: reaproveitado (mesma textura no compositor).
        QCOMPARE(source.frameFor(layer).cacheKey(), img.cacheKey());

        proj.tracks[0].clips[0].removeBackground = false;
        const VisualLayer plain = evaluate(proj, kSecond).layers.front();
        QCOMPARE(qAlpha(source.frameFor(plain).pixel(1080, 360)), 255);
    }

    void prefetchMatchesDirectDecoding()
    {
        // Clipe normal na faixa principal e o mesmo vídeo invertido a 1,5× por cima.
        Project proj = createProject(QStringLiteral("t"), 1280, 720, 30);
        MediaAsset a;
        QVERIFY(media::probe(m_video, a));
        a.id = newId("ast");
        proj.assets.emplace(a.id, a);
        edit::insertClip(proj, createMediaClip(a, 0), 0);
        proj.tracks.push_back(createTrack(TrackKind::Video));
        Clip rev = createMediaClip(a, 0);
        rev.speed = 1.5;
        rev.reverse = true;
        rev.duration = static_cast<Micros>(a.duration / rev.speed);
        proj.tracks.back().clips.push_back(rev);

        const QSize maxSize(640, 360);
        engine::DecoderFrameSource source(maxSize, false);
        std::unordered_map<QString, std::unique_ptr<media::VideoDecoder>> direct;
        const Micros step = kSecond / 30;
        // Reprodução, um salto para trás e mais reprodução.
        std::vector<Micros> times;
        for (int i = 0; i < 30; ++i)
            times.push_back(i * step);
        for (int i = 10; i < 40; ++i)
            times.push_back(i * step);
        int compared = 0;
        for (size_t i = 0; i < times.size(); ++i) {
            std::vector<Micros> ahead;
            for (int k = 0; k < 4; ++k)
                ahead.push_back(times[i] + k * step);
            source.prefetch(proj, ahead, {times[i] + kSecond / 2});
            if (i % 3 == 0)
                QTest::qWait(20); // dá tempo de decodificar à frente
            for (const VisualLayer &l : evaluate(proj, times[i]).layers) {
                auto &dec = direct[l.clip->id];
                if (!dec) {
                    dec = std::make_unique<media::VideoDecoder>();
                    QVERIFY(dec->open(m_video, maxSize));
                }
                const QImage expected = dec->frameAt(l.sourceTime);
                QVERIFY(!expected.isNull());
                QVERIFY2(source.frameFor(l) == expected, qPrintable(QStringLiteral("t=%1 clip=%2").arg(times[i]).arg(l.clip->id)));
                ++compared;
            }
        }
        QCOMPARE(compared, 120);
        QVERIFY(source.prefetchHits() > 0);
        QCOMPARE(source.prefetchHits() + source.prefetchMisses(), compared);

        // Reprodução para a frente como no export: um único seek (o inicial), por mais que a
        // decodificação à frente e os pedidos se intercalem.
        proj.tracks.pop_back();
        engine::DecoderFrameSource forward(maxSize, false);
        forward.setCacheFrames(engine::kExportLookahead + 2);
        for (int i = 0; i < 60; ++i) {
            std::vector<Micros> ahead;
            for (int k = i; k <= i + engine::kExportLookahead; ++k)
                ahead.push_back(k * step);
            forward.prefetch(proj, ahead);
            if (i % 7 == 0)
                QTest::qWait(15);
            QVERIFY(!forward.frameFor(evaluate(proj, i * step).layers.front()).isNull());
        }
        QCOMPARE(forward.decoderSeeks(), 1);
    }

    void renderCacheKeys()
    {
        // Clipe A em 0–2 s na faixa principal, B em 6–8 s numa faixa de cima: trechos 0..3.
        Project proj = createProject(QStringLiteral("t"), 1280, 720, 30);
        MediaAsset a;
        QVERIFY(media::probe(m_video, a));
        a.id = newId("ast");
        proj.assets.emplace(a.id, a);
        Clip ca = createMediaClip(a, 0);
        ca.duration = 2 * kSecond;
        edit::insertClip(proj, ca, 0);
        proj.tracks.push_back(createTrack(TrackKind::Video));
        Clip cb = createMediaClip(a, 6 * kSecond);
        cb.duration = 2 * kSecond;
        const QString idB = edit::insertClip(proj, cb, 1);
        const auto before = engine::renderChunkKeys(proj);
        QCOMPARE(static_cast<int>(before.size()), 4);
        QCOMPARE(engine::renderChunkKeys(proj), before); // determinística

        edit::clipById(proj, idB)->transform.scale = 0.5;
        const auto after = engine::renderChunkKeys(proj);
        QCOMPARE(after[0], before[0]); // longe de B: continua válido
        QVERIFY(after[3] != before[3]);
        proj.background = QStringLiteral("#ffffff");
        QVERIFY(engine::renderChunkKeys(proj)[0] != after[0]); // canvas afeta tudo
        QVERIFY(engine::renderCacheFile(after[0], 960) != engine::renderCacheFile(after[0], 1280));
        QCOMPARE(engine::renderCacheSize(proj, 640), QSize(640, 360));
    }

    /** Textura com detalhes em todas as escalas (ruído suavizado + formas), em cinza. */
    static QImage texture(QSize size)
    {
        QImage img(size, QImage::Format_Grayscale8);
        unsigned seed = 3;
        for (int y = 0; y < size.height(); ++y)
            for (int x = 0; x < size.width(); ++x) {
                seed = seed * 1103515245u + 12345u;
                img.scanLine(y)[x] = static_cast<uchar>(128 + ((seed >> 16) & 63) - 32);
            }
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        for (int i = 0; i < 60; ++i) {
            seed = seed * 1103515245u + 12345u;
            const int x = (seed >> 8) % size.width(), y = (seed >> 3) % size.height();
            p.setBrush(QColor::fromHsv(0, 0, (seed >> 12) % 255));
            p.setPen(Qt::NoPen);
            p.drawEllipse(QPoint(x, y), 4 + (seed % 14), 3 + ((seed >> 5) % 12));
        }
        p.end();
        return img.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    void motionEstimation()
    {
        // Quadro grande e recortes deslocados/girados dele: o movimento medido bate com o aplicado.
        const QImage big = texture({640, 400});
        auto view = [&](double dx, double dy, double deg) {
            // O conteúdo anda (dx, dy) e gira deg graus em torno do centro da janela 320×180
            // (amostragem bilinear, para deslocamentos de subpixel de verdade).
            QImage out(320, 180, QImage::Format_Grayscale8);
            const double r = qDegreesToRadians(deg), cs = std::cos(r), sn = std::sin(r);
            for (int y = 0; y < 180; ++y)
                for (int x = 0; x < 320; ++x) {
                    const double qx = x - 160 - dx, qy = y - 90 - dy;
                    const double sx = cs * qx + sn * qy + 320, sy = -sn * qx + cs * qy + 200;
                    const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
                    const double fx = sx - x0, fy = sy - y0;
                    auto g = [&](int xx, int yy) { return static_cast<double>(big.constScanLine(yy)[xx]); };
                    const double v = (g(x0, y0) * (1 - fx) + g(x0 + 1, y0) * fx) * (1 - fy)
                                     + (g(x0, y0 + 1) * (1 - fx) + g(x0 + 1, y0 + 1) * fx) * fy;
                    out.scanLine(y)[x] = static_cast<uchar>(std::lround(v));
                }
            return out;
        };
        const QImage a = view(0, 0, 0);
        engine::FrameMotion m = engine::estimateMotion(a, view(3.4, -2.2, 0));
        QVERIFY2(std::abs(m.dx - 3.4) < 0.25 && std::abs(m.dy + 2.2) < 0.25, qPrintable(QStringLiteral("%1 %2").arg(m.dx).arg(m.dy)));
        QVERIFY(m.inliers >= 8);
        m = engine::estimateMotion(a, view(-1.5, 1.0, 1.2));
        QVERIFY2(std::abs(m.angle - qDegreesToRadians(1.2)) < 0.004, qPrintable(QString::number(qRadiansToDegrees(m.angle))));
        QVERIFY2(std::abs(m.dx + 1.5) < 0.4 && std::abs(m.dy - 1.0) < 0.4, qPrintable(QStringLiteral("%1 %2").arg(m.dx).arg(m.dy)));
        // Panorâmica rápida (maior que um bloco): o deslocamento global resolve.
        m = engine::estimateMotion(a, view(24, 0, 0));
        QVERIFY2(std::abs(m.dx - 24) < 0.5, qPrintable(QString::number(m.dx)));
    }

    void stabilizationAnalysis()
    {
        // Vídeo "tremido": uma janela que oscila rápido sobre uma imagem parada.
        const QString shaky = m_dir.filePath(QStringLiteral("tremido.mp4"));
        QProcess p;
        p.start(QStringLiteral("ffmpeg"), {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                                           "mandelbrot=size=800x450:rate=30:end_pts=0.01,loop=loop=-1:size=1,trim=duration=3",
                                           "-vf", "crop=640:360:'80+12*sin(2*PI*3*t)':'45+8*cos(2*PI*4*t)'",
                                           "-c:v", "libx264", "-preset", "ultrafast", "-crf", "12", "-pix_fmt", "yuv420p", shaky});
        QVERIFY(p.waitForFinished(120000) && p.exitCode() == 0);
        const QString out = engine::motionPathFor(shaky);
        QString err;
        QVERIFY2(engine::analyzeMotion(shaky, out, {}, &err), qPrintable(err));
        QVERIFY(QFileInfo::exists(out));
        const auto track = registeredMotion(shaky);
        QVERIFY(track && track->frames() >= 80);
        // A janela anda +x → o conteúdo anda -x. O caminho medido segue o aplicado (320 px de largura analisada).
        double worst = 0;
        for (int i = 0; i < track->frames(); ++i) {
            const double t = i / 30.0;
            const double expect = -(12 * std::sin(2 * M_PI * 3 * t) - 0) / 640.0;
            worst = std::max(worst, std::abs(track->x[i] - expect));
        }
        QVERIFY2(worst < 0.006, qPrintable(QString::number(worst))); // < 2 px no tamanho analisado
        // Corrigido, o tremor quase some: o caminho + correção varia pouco quadro a quadro.
        double jitter = 0, corrected = 0;
        for (int i = 1; i < track->frames(); ++i) {
            const StabCorrection c0 = stabilizationAt(*track, static_cast<Micros>((i - 1) * kSecond / 30.0), 1.0);
            const StabCorrection c1 = stabilizationAt(*track, static_cast<Micros>(i * kSecond / 30.0), 1.0);
            jitter += std::abs(track->x[i] - track->x[i - 1]);
            corrected += std::abs((track->x[i] + c1.dx) - (track->x[i - 1] + c0.dx));
        }
        QVERIFY2(corrected < jitter * 0.2, qPrintable(QStringLiteral("%1 vs %2").arg(corrected).arg(jitter)));
        QVERIFY(track->zoomFor(1.0) > 1.0 && track->zoomFor(1.0) < 1.15);
        // O cache é reaproveitado por quem abrir a mídia depois.
        registerMotion(shaky, nullptr);
        QVERIFY(engine::discoverMotion(shaky));
        QCOMPARE(registeredMotion(shaky)->frames(), track->frames());
    }

    void regionTracking()
    {
        // Quadrado 80×80 (um quadro parado do padrão de teste) andando sobre um fundo de fractal: x = 200 + 260·t,
        // y = 150 + 90·sin(2πt/3), num vídeo 1280×720 de 4 s.
        const QString clip = m_dir.filePath(QStringLiteral("objeto.mp4"));
        QProcess p;
        p.start(QStringLiteral("ffmpeg"), {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                                           "mandelbrot=size=1280x720:rate=30:end_pts=0.01,loop=loop=-1:size=1,trim=duration=4",
                                           "-f", "lavfi", "-i", "testsrc2=size=80x80:rate=30:duration=0.04,loop=loop=-1:size=1,trim=duration=4", "-filter_complex",
                                           "[0][1]overlay=x='200+260*t':y='150+90*sin(2*PI*t/3)':eof_action=pass",
                                           "-c:v", "libx264", "-preset", "ultrafast", "-crf", "14", "-pix_fmt", "yuv420p", clip});
        QVERIFY(p.waitForFinished(120000) && p.exitCode() == 0);
        auto truth = [](double t) {
            return QPointF((200 + 260 * t + 40) / 1280.0, (150 + 90 * std::sin(2 * M_PI * t / 3) + 40) / 720.0);
        };
        const Micros mid = 2 * kSecond;
        const QPointF c = truth(2.0);
        QString err;
        for (const Micros to : {Micros(3'800'000), Micros(200'000)}) { // para a frente e para trás
            const auto pts = engine::trackRegion(clip, mid, to, c.x(), c.y(), 80 / 1280.0, 80 / 720.0, {}, &err);
            QVERIFY2(pts.size() > 40, qPrintable(err));
            double worst = 0;
            for (const auto &pt : pts) {
                const QPointF e = truth(static_cast<double>(pt.t) / kSecond);
                worst = std::max(worst, std::hypot((pt.u - e.x()) * 1280, (pt.v - e.y()) * 720));
            }
            qInfo() << "erro máximo (px):" << worst;
            QVERIFY2(worst < 4, qPrintable(QStringLiteral("erro máximo %1 px").arg(worst)));
        }
    }

    void cancel()
    {
        const QString out = m_dir.filePath(QStringLiteral("cancelado.mp4"));
        QString err;
        QVERIFY(!engine::generateProxy(m_video, out, [](double f) { return f < 0.3; }, &err));
        QVERIFY(!QFileInfo::exists(out));
        QVERIFY(!QFileInfo::exists(out + QStringLiteral(".part.mp4")));
    }
};

QTEST_GUILESS_MAIN(ProxyTest)
#include "tst_proxy.moc"
