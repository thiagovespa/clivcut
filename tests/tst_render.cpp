// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Edit.h"
#include "core/Evaluate.h"
#include "core/Lut.h"
#include "render/Compositor.h"
#include "render/OffscreenRhi.h"
#include "render/TextRaster.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace clv;

namespace {

class SolidSource : public render::FrameSource {
public:
    QHash<QString, VideoImage> images; // por assetId
    VideoImage frameFor(const VisualLayer &l) override { return images.value(l.clip->assetId); }
};

QImage solid(QColor c, QSize s = {160, 90})
{
    QImage img(s, QImage::Format_RGBA8888);
    img.fill(c);
    return img;
}

/** Converte RGB em YUV 4:2:0 (cor média de cada bloco 2×2), como sairia de um decodificador. */
VideoImage toYuv(const QImage &rgb, bool nv12, bool bt709, bool fullRange, int rotation = 0)
{
    const QImage src = rgb.convertToFormat(QImage::Format_RGBA8888);
    const int w = src.width(), h = src.height();
    VideoImage v;
    v.format = nv12 ? VideoImage::Format::Nv12 : VideoImage::Format::I420;
    v.size = src.size();
    v.bt709 = bt709;
    v.fullRange = fullRange;
    v.rotation = rotation;
    v.key = VideoImage::newKey();
    const double kr = bt709 ? 0.2126 : 0.299, kb = bt709 ? 0.0722 : 0.114;
    const auto yuv = [&](QColor c, double &y, double &cb, double &cr) {
        y = kr * c.redF() + (1 - kr - kb) * c.greenF() + kb * c.blueF();
        cb = (c.blueF() - y) / (2 * (1 - kb));
        cr = (c.redF() - y) / (2 * (1 - kr));
    };
    const auto Y8 = [&](double y) { return static_cast<char>(std::lround(fullRange ? y * 255 : 16 + y * 219)); };
    const auto C8 = [&](double c) { return static_cast<char>(std::lround(fullRange ? 128 + c * 255 : 128 + c * 224)); };
    const QSize cs = v.chromaSize();
    v.planes[0].resize(w * h);
    v.planes[1].resize(cs.width() * cs.height() * (nv12 ? 2 : 1));
    if (!nv12)
        v.planes[2].resize(cs.width() * cs.height());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double l, cb, cr;
            yuv(src.pixelColor(x, y), l, cb, cr);
            v.planes[0][y * w + x] = Y8(l);
        }
    for (int cy = 0; cy < cs.height(); ++cy)
        for (int cx = 0; cx < cs.width(); ++cx) {
            double l, cb, cr;
            yuv(src.pixelColor(std::min(cx * 2, w - 1), std::min(cy * 2, h - 1)), l, cb, cr);
            const int i = cy * cs.width() + cx;
            if (nv12) {
                v.planes[1][i * 2] = C8(cb);
                v.planes[1][i * 2 + 1] = C8(cr);
            } else {
                v.planes[1][i] = C8(cb);
                v.planes[2][i] = C8(cr);
            }
        }
    return v;
}

MediaAsset imageAsset(Project &p, QSize s = {160, 90})
{
    MediaAsset a;
    a.id = newId("ast");
    a.kind = MediaKind::Image;
    a.width = s.width();
    a.height = s.height();
    a.hasVideo = true;
    p.assets.emplace(a.id, a);
    return a;
}

} // namespace

class RenderTest : public QObject {
    Q_OBJECT

    render::OffscreenRhi m_ctx;
    std::unique_ptr<render::Compositor> m_comp;

    QImage renderAt(const Project &p, Micros t, render::FrameSource &src)
    {
        m_comp->setRenderSize(QSize(p.width, p.height));
        QRhi *rhi = m_ctx.rhi();
        QRhiCommandBuffer *cb = nullptr;
        if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
            return {};
        m_comp->composite(cb, p, evaluate(p, t), src);
        QRhiReadbackResult rb;
        m_comp->readback(cb, &rb);
        rhi->endOffscreenFrame();
        return m_comp->imageFromReadback(rb);
    }

    static QColor px(const QImage &img, int x, int y) { return img.pixelColor(x, y); }
    static bool near(QColor a, QColor b, int tol = 6)
    {
        return std::abs(a.red() - b.red()) <= tol && std::abs(a.green() - b.green()) <= tol &&
               std::abs(a.blue() - b.blue()) <= tol;
    }

private slots:
    void initTestCase()
    {
        QString err;
        if (!m_ctx.create(&err))
            QSKIP(qPrintable(err));
        qInfo() << "backend:" << m_ctx.backendName();
        m_comp = std::make_unique<render::Compositor>(m_ctx.rhi());
    }

    void orientationAndScale()
    {
        Project p = createProject(QStringLiteral("t"), 320, 180, 30);
        auto a = imageAsset(p);
        auto c = createMediaClip(a, 0);
        c.transform.scale = 0.5;
        c.transform.x = -0.5;
        c.transform.y = -0.5; // quadrante superior esquerdo
        edit::insertClip(p, c, 0);
        SolidSource src;
        src.images[a.id] = solid(Qt::red);
        const QImage img = renderAt(p, 0, src);
        QCOMPARE(img.size(), QSize(320, 180));
        QVERIFY2(near(px(img, 20, 20), Qt::red), qPrintable(px(img, 20, 20).name()));
        QVERIFY(near(px(img, 300, 170), Qt::black));
        QVERIFY(near(px(img, 300, 20), Qt::black));
        QVERIFY(near(px(img, 20, 170), Qt::black));
    }

    void blurredBackground()
    {
        // Canvas vertical com vídeo paisagem: as faixas vazias mostram o próprio vídeo borrado.
        Project p = createProject(QStringLiteral("t"), 180, 320, 30);
        auto a = imageAsset(p, {160, 90});
        edit::insertClip(p, createMediaClip(a, 0), 0);
        SolidSource src;
        src.images[a.id] = solid(QColor(40, 200, 90));
        QImage img = renderAt(p, 0, src);
        QVERIFY(near(px(img, 90, 10), Qt::black)); // sem desfoque: cor de fundo
        p.backgroundBlur = 0.5;
        img = renderAt(p, 0, src);
        QVERIFY2(near(px(img, 90, 10), QColor(40, 200, 90), 10), qPrintable(px(img, 90, 10).name()));
        QVERIFY(near(px(img, 90, 160), QColor(40, 200, 90))); // o clipe continua por cima
    }

    void transparentBackground()
    {
        // Exportação com alfa: fora dos clipes o canvas fica transparente (cor e desfoque ignorados).
        Project p = createProject(QStringLiteral("t"), 320, 180, 30);
        p.background = QStringLiteral("#ff0000");
        p.backgroundBlur = 0.5;
        auto a = imageAsset(p);
        auto c = createMediaClip(a, 0);
        c.transform.scale = 0.5;
        edit::insertClip(p, c, 0);
        SolidSource src;
        src.images[a.id] = solid(Qt::blue);
        m_comp->setTransparentBackground(true);
        const QImage img = renderAt(p, 0, src);
        m_comp->setTransparentBackground(false);
        QCOMPARE(qAlpha(img.pixel(5, 5)), 0);
        QCOMPARE(qAlpha(img.pixel(160, 90)), 255);
        QVERIFY(near(px(img, 160, 90), Qt::blue));
    }

    void colorGradeLut()
    {
        Project p = createProject(QStringLiteral("t"), 160, 90, 30);
        auto a = imageAsset(p);
        auto c = createMediaClip(a, 0);
        c.color.curves[0] = {{0.5, 0.8}}; // clareia os meios-tons
        edit::insertClip(p, c, 0);
        SolidSource src;
        src.images[a.id] = solid(QColor(128, 128, 128));
        const QImage img = renderAt(p, 0, src);
        QVERIFY2(px(img, 80, 45).red() > 185, qPrintable(px(img, 80, 45).name()));
        // Outra curva: novo LUT; a anterior sai de uso sem quebrar nada.
        p.tracks[0].clips[0].color.curves[0] = {{0.5, 0.3}};
        const QImage darker = renderAt(p, 0, src);
        QVERIFY2(px(darker, 80, 45).red() < 90, qPrintable(px(darker, 80, 45).name()));
    }

    void videoEffects()
    {
        Project p = createProject(QStringLiteral("t"), 160, 90, 30);
        auto a = imageAsset(p);
        auto c = createMediaClip(a, 0);
        c.effect = {QStringLiteral("strobe"), 1.0}; // em t = 0 o clarão está no máximo
        const QString id = edit::insertClip(p, c, 0);
        SolidSource src;
        src.images[a.id] = solid(QColor(200, 0, 0));
        QImage img = renderAt(p, 0, src);
        QVERIFY2(px(img, 80, 45).green() > 180, qPrintable(px(img, 80, 45).name()));

        // Desfoque mistura as cores na divisa entre duas metades.
        QImage half(160, 90, QImage::Format_RGBA8888);
        half.fill(Qt::red);
        for (int y = 0; y < 90; ++y)
            for (int x = 80; x < 160; ++x)
                half.setPixelColor(x, y, Qt::blue);
        src.images[a.id] = half;
        edit::clipById(p, id)->effect = {QStringLiteral("blur"), 1.0};
        img = renderAt(p, 0, src);
        const QColor edge = px(img, 80, 45);
        QVERIFY2(edge.red() > 40 && edge.blue() > 40, qPrintable(edge.name()));
        QVERIFY(near(px(img, 10, 45), Qt::red, 10)); // longe da divisa continua vermelho

        // Pixelado: blocos de cor única.
        edit::clipById(p, id)->effect = {QStringLiteral("pixelate"), 1.0};
        img = renderAt(p, 0, src);
        QCOMPARE(px(img, 82, 45), px(img, 86, 45));
    }

    void flashTransition()
    {
        Project p = createProject(QStringLiteral("t"), 160, 90, 30);
        auto a = imageAsset(p);
        auto c1 = createMediaClip(a, 0);
        c1.duration = kSecond;
        edit::insertClip(p, c1, 0);
        auto c2 = createMediaClip(a, kSecond);
        c2.duration = kSecond;
        c2.transitionIn = Transition{TransitionKind::FlashWhite, 600'000};
        edit::insertClip(p, c2, 0);
        SolidSource src;
        src.images[a.id] = solid(QColor(20, 20, 160));
        // No corte, o clarão está no branco; longe dele, a imagem original.
        QVERIFY2(px(renderAt(p, kSecond, src), 80, 45).red() > 230, qPrintable(px(renderAt(p, kSecond, src), 80, 45).name()));
        QVERIFY(near(px(renderAt(p, 300'000, src), 80, 45), QColor(20, 20, 160), 6));
    }

    void opacityAndMultiply()
    {
        Project p = createProject(QStringLiteral("t"), 320, 180, 30);
        p.background = QStringLiteral("#ffffff");
        auto a = imageAsset(p);
        auto b = imageAsset(p);
        auto base = createMediaClip(a, 0);
        edit::insertClip(p, base, 0); // amarelo em tela cheia
        auto top = createMediaClip(b, 0);
        top.blend = BlendMode::Multiply; // magenta * amarelo = vermelho
        edit::insertClip(p, top, -1);
        SolidSource src;
        src.images[a.id] = solid(Qt::yellow);
        src.images[b.id] = solid(Qt::magenta);
        QImage img = renderAt(p, 0, src);
        QVERIFY2(near(px(img, 160, 90), Qt::red), qPrintable(px(img, 160, 90).name()));

        // Opacidade 50% em modo normal sobre fundo branco.
        p.tracks[1].clips[0].blend = BlendMode::Normal;
        p.tracks[1].clips[0].transform.opacity = 0.5;
        img = renderAt(p, 0, src);
        QVERIFY2(near(px(img, 160, 90), QColor(255, 128, 128)), qPrintable(px(img, 160, 90).name()));
    }

    void textRenders()
    {
        Project p = createProject(QStringLiteral("t"), 320, 180, 30);
        auto t = createTextClip(QStringLiteral("CLIVCUT"), 0);
        t.transform.y = 0;
        t.style.fontSize = 300;
        t.style.color = QStringLiteral("#00ff00");
        t.style.strokeWidth = 0;
        t.style.shadow = false;
        edit::insertClip(p, t, -1);
        SolidSource src;
        const QImage img = renderAt(p, 0, src);
        int green = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
                if (img.pixelColor(x, y).green() > 200 && img.pixelColor(x, y).red() < 60)
                    ++green;
        QVERIFY2(green > 500, qPrintable(QString::number(green)));
    }

    void textHighlightCurveAndShapes()
    {
        Clip t = createTextClip(QStringLiteral("AAA *BBB*"), 0);
        t.style.fontSize = 80;
        t.style.color = QStringLiteral("#00ff00");
        t.style.highlightColor = QStringLiteral("#ff0000");
        t.style.strokeWidth = 0;
        t.style.shadow = false;
        auto count = [](const QImage &img, auto pred) {
            int n = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    n += pred(img.pixelColor(x, y)) ? 1 : 0;
            return n;
        };
        auto isGreen = [](QColor c) { return c.alpha() > 200 && c.green() > 200 && c.red() < 60; };
        auto isRed = [](QColor c) { return c.alpha() > 200 && c.red() > 200 && c.green() < 60; };
        QImage img = render::rasterizeText(t, 1.0);
        // As duas palavras aparecem, cada uma com sua cor; os asteriscos somem.
        QVERIFY(count(img, isGreen) > 300);
        QVERIFY(count(img, isRed) > 300);
        t.text = QStringLiteral("AAA BBB");
        const QImage plain = render::rasterizeText(t, 1.0);
        QVERIFY(img.width() < plain.width() + 4); // sem largura extra dos asteriscos

        // Curvado: o texto ocupa mais altura (arco) que reto.
        t.style.curve = 0.8;
        const QImage curved = render::rasterizeText(t, 1.0);
        QVERIFY2(curved.height() > plain.height() + 20, qPrintable(QStringLiteral("%1 vs %2").arg(curved.height()).arg(plain.height())));
        t.style.curve = -0.8;
        QVERIFY(render::rasterizeText(t, 1.0).height() > plain.height() + 20);

        // Forma sem texto: imagem do tamanho da forma, preenchida no centro.
        Clip shape = createTextClip(QString(), 0);
        shape.style.shape = QStringLiteral("rounded");
        shape.style.shapeColor = QStringLiteral("#3366ff");
        shape.style.shapeWidth = 200;
        shape.style.shapeHeight = 100;
        shape.style.shadow = false;
        const QImage s = render::rasterizeText(shape, 1.0);
        QVERIFY(std::abs(s.width() - 200) < 12 && std::abs(s.height() - 100) < 12);
        QCOMPARE(s.pixelColor(s.width() / 2, s.height() / 2), QColor(0x33, 0x66, 0xff));
        for (const char *kind : {"rect", "circle", "triangle", "star", "arrow", "heart", "bubble"}) {
            shape.style.shape = QString::fromLatin1(kind);
            QVERIFY2(!render::rasterizeText(shape, 1.0).isNull(), kind);
        }
    }

    void filtersAndLuts()
    {
        Project p = createProject(QStringLiteral("t"), 320, 180, 30);
        auto a = imageAsset(p);
        auto c = createMediaClip(a, 0);
        edit::insertClip(p, c, 0);
        SolidSource src;
        src.images[a.id] = solid(QColor(200, 40, 40));
        Clip &clip = p.tracks[0].clips[0];

        // P&B embutido: pixel fica cinza.
        clip.color.filter = QStringLiteral("bw");
        clip.color.filterIntensity = 1;
        QImage img = renderAt(p, 0, src);
        QColor c0 = px(img, 160, 90);
        QVERIFY2(std::abs(c0.red() - c0.green()) <= 3 && std::abs(c0.green() - c0.blue()) <= 3, qPrintable(c0.name()));

        // Intensidade 0: inalterado.
        clip.color.filterIntensity = 0;
        img = renderAt(p, 0, src);
        QVERIFY2(near(px(img, 160, 90), QColor(200, 40, 40), 3), qPrintable(px(img, 160, 90).name()));

        // LUT .cube do usuário que inverte as cores.
        QTemporaryDir dir;
        const QString cube = dir.filePath(QStringLiteral("inv.cube"));
        QFile f(cube);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("LUT_3D_SIZE 2\n1 1 1\n0 1 1\n1 0 1\n0 0 1\n1 1 0\n0 1 0\n1 0 0\n0 0 0\n");
        f.close();
        clip.color.lutPath = cube;
        clip.color.lutIntensity = 1;
        img = renderAt(p, 0, src);
        QVERIFY2(near(px(img, 160, 90), QColor(55, 215, 215), 4), qPrintable(px(img, 160, 90).name()));
    }

    void adjustLayerAffectsOnlyBelow()
    {
        Project p = createProject(QStringLiteral("t"), 320, 180, 30);
        auto a = imageAsset(p);
        auto b = imageAsset(p);
        edit::insertClip(p, createMediaClip(a, 0), 0); // vermelho em tela cheia
        Clip adj = createAdjustClip(0, kSecond, QStringLiteral("bw"));
        adj.color.filterIntensity = 1;
        edit::insertClip(p, adj, -1);                  // P&B sobre o vermelho
        auto top = createMediaClip(b, 0);
        top.transform.scale = 0.5;
        top.transform.x = 0.5;                         // azul à direita, acima do ajuste
        edit::insertClip(p, top, -1);
        QCOMPARE(p.tracks.size(), size_t(3));

        SolidSource src;
        src.images[a.id] = solid(QColor(220, 30, 30));
        src.images[b.id] = solid(QColor(30, 60, 220));
        QImage img = renderAt(p, kSecond / 2, src);
        const QColor left = px(img, 40, 90);
        QVERIFY2(std::abs(left.red() - left.blue()) <= 3, qPrintable(left.name())); // vermelho ficou cinza
        QVERIFY2(near(px(img, 280, 90), QColor(30, 60, 220), 4), qPrintable(px(img, 280, 90).name())); // azul intacto

        // Opacidade do ajuste = mistura parcial.
        p.tracks[1].clips[0].transform.opacity = 0.5;
        img = renderAt(p, kSecond / 2, src);
        const QColor half = px(img, 40, 90);
        QVERIFY2(half.red() > half.blue() + 40 && half.red() < 210, qPrintable(half.name()));
    }

    void shapeMasks()
    {
        Project p = createProject(QStringLiteral("t"), 320, 180, 30);
        auto a = imageAsset(p);
        edit::insertClip(p, createMediaClip(a, 0), 0); // vermelho em tela cheia sobre preto
        SolidSource src;
        src.images[a.id] = solid(Qt::red);
        Mask &m = p.tracks[0].clips[0].mask;

        // Círculo central: centro vermelho, cantos pretos.
        m.shape = MaskShape::Circle;
        m.width = 0.4;
        m.height = 0.6;
        m.feather = 0;
        QImage img = renderAt(p, 0, src);
        QVERIFY(near(px(img, 160, 90), Qt::red));
        QVERIFY(near(px(img, 5, 5), Qt::black));
        QVERIFY(near(px(img, 160 + 70, 90), Qt::black)); // fora do raio horizontal (0,2 × 320 = 64 px)
        QVERIFY(near(px(img, 160 + 55, 90), Qt::red));

        // Invertido.
        m.invert = true;
        img = renderAt(p, 0, src);
        QVERIFY(near(px(img, 160, 90), Qt::black));
        QVERIFY(near(px(img, 5, 5), Qt::red));
        m.invert = false;

        // Linear: mantém a metade de cima; girado 180° mantém a de baixo.
        m.shape = MaskShape::Linear;
        img = renderAt(p, 0, src);
        QVERIFY(near(px(img, 160, 30), Qt::red));
        QVERIFY(near(px(img, 160, 150), Qt::black));
        m.rotation = 180;
        img = renderAt(p, 0, src);
        QVERIFY(near(px(img, 160, 30), Qt::black));
        QVERIFY(near(px(img, 160, 150), Qt::red));
        m.rotation = 0;

        // Retângulo deslocado para a direita.
        m.shape = MaskShape::Rectangle;
        m.x = 0.75;
        m.width = 0.3;
        m.height = 0.5;
        img = renderAt(p, 0, src);
        QVERIFY(near(px(img, 240, 90), Qt::red));
        QVERIFY(near(px(img, 80, 90), Qt::black));
        QVERIFY(near(px(img, 240, 10), Qt::black));

        // Suavização gera valores intermediários na borda.
        m.feather = 0.5;
        img = renderAt(p, 0, src);
        const int edge = px(img, 240 + 48, 90).red(); // perto da borda direita do retângulo
        QVERIFY2(edge > 20 && edge < 235, qPrintable(QString::number(edge)));
    }

    void maskedAdjustLayer()
    {
        // Ajuste P&B só no círculo central: cor fora, cinza dentro.
        Project p = createProject(QStringLiteral("t"), 320, 180, 30);
        auto a = imageAsset(p);
        edit::insertClip(p, createMediaClip(a, 0), 0);
        Clip adj = createAdjustClip(0, kSecond, QStringLiteral("bw"));
        adj.color.filterIntensity = 1;
        adj.mask.shape = MaskShape::Circle;
        adj.mask.width = 0.3;
        adj.mask.height = 0.5;
        adj.mask.feather = 0;
        edit::insertClip(p, adj, -1);
        SolidSource src;
        src.images[a.id] = solid(QColor(220, 30, 30));
        const QImage img = renderAt(p, kSecond / 2, src);
        const QColor in = px(img, 160, 90), out = px(img, 10, 10);
        QVERIFY2(std::abs(in.red() - in.blue()) <= 3, qPrintable(in.name()));
        QVERIFY2(near(out, QColor(220, 30, 30), 4), qPrintable(out.name()));
    }

    void compoundClip()
    {
        // Composto: vermelho em tela cheia com verde no quadrante superior esquerdo,
        // reduzido à metade e levado para o quadrante inferior direito.
        Project p = createProject(QStringLiteral("t"), 320, 180, 30);
        auto red = imageAsset(p);
        auto green = imageAsset(p);
        const QString r = edit::insertClip(p, createMediaClip(red, 0), 0);
        auto g = createMediaClip(green, 0);
        g.transform.scale = 0.5;
        g.transform.x = -0.5;
        g.transform.y = -0.5;
        const QString gid = edit::insertClip(p, g, -1);
        const QString cmp = edit::createCompound(p, {r, gid}, QStringLiteral("c"));
        QVERIFY(!cmp.isEmpty());
        Clip *cc = edit::clipById(p, cmp);
        cc->transform.scale = 0.5;
        cc->transform.x = 0.5;
        cc->transform.y = 0.5;
        SolidSource src;
        src.images[red.id] = solid(Qt::red);
        src.images[green.id] = solid(Qt::green);
        QImage img = renderAt(p, 0, src);
        QVERIFY2(near(px(img, 180, 100), Qt::green), qPrintable(px(img, 180, 100).name()));
        QVERIFY2(near(px(img, 300, 170), Qt::red), qPrintable(px(img, 300, 170).name()));
        QVERIFY2(near(px(img, 230, 160), Qt::red), qPrintable(px(img, 230, 160).name()));
        QVERIFY(near(px(img, 20, 20), Qt::black));
        QVERIFY(near(px(img, 300, 20), Qt::black));

        // Por dentro transparente: só o verde, a 50% de opacidade, sobre azul.
        Project q = createProject(QStringLiteral("t"), 320, 180, 30);
        auto blue = imageAsset(q);
        auto green2 = imageAsset(q);
        edit::insertClip(q, createMediaClip(blue, 0), 0);
        auto g2 = createMediaClip(green2, 0);
        g2.transform.scale = 0.5;
        const QString g2id = edit::insertClip(q, g2, -1);
        const QString cmp2 = edit::createCompound(q, {g2id}, QStringLiteral("c"));
        QVERIFY(!cmp2.isEmpty());
        edit::clipById(q, cmp2)->transform.opacity = 0.5;
        src.images[blue.id] = solid(Qt::blue);
        src.images[green2.id] = solid(Qt::green);
        img = renderAt(q, 0, src);
        QVERIFY2(near(px(img, 20, 20), Qt::blue), qPrintable(px(img, 20, 20).name()));
        QVERIFY2(near(px(img, 160, 90), QColor(0, 128, 127), 8), qPrintable(px(img, 160, 90).name()));
    }

    void emptyTemplateSlot()
    {
        Project p = createProject(QStringLiteral("t"), 320, 180, 30);
        Clip c;
        c.id = newId("clp");
        c.type = ClipType::Media;
        c.slot = QStringLiteral("Video 1");
        c.duration = kSecond;
        c.transform.scale = 0.5;
        p.tracks[0].clips.push_back(c);
        SolidSource src;
        const QImage img = renderAt(p, 0, src);
        // "+" laranja no centro do cartão; fora do cartão, a cor de fundo.
        const QColor mid = px(img, 163, 83); // dentro do círculo (centro ~160,85; raio 8), fora da cruz
        QVERIFY2(mid.red() > 200 && mid.green() > 120 && mid.blue() < 90, qPrintable(mid.name()));
        QVERIFY(near(px(img, 10, 10), Qt::black));
        QVERIFY(!near(px(img, 100, 60), Qt::black)); // dentro do cartão
    }

    void yuvFrames()
    {
        // Metade esquerda vermelha, direita azul: a GPU converte o YUV com a matriz, a faixa e a
        // rotação do quadro, e o resultado bate com a conversão na CPU.
        QImage pattern(160, 90, QImage::Format_RGBA8888);
        pattern.fill(QColor(220, 40, 30));
        for (int y = 0; y < 90; ++y)
            for (int x = 80; x < 160; ++x)
                pattern.setPixelColor(x, y, QColor(30, 60, 210));

        struct Case {
            bool nv12, bt709, full;
        };
        for (const Case &k : {Case{false, false, false}, Case{true, true, false}, Case{false, true, true}, Case{true, false, true}}) {
            Project p = createProject(QStringLiteral("t"), 160, 90, 30);
            auto a = imageAsset(p);
            edit::insertClip(p, createMediaClip(a, 0), 0);
            SolidSource src;
            const VideoImage frame = toYuv(pattern, k.nv12, k.bt709, k.full);
            src.images[a.id] = frame;
            const QImage out = renderAt(p, 0, src);
            const QImage cpu = frame.toImage();
            for (const QPoint &pt : {QPoint(30, 45), QPoint(130, 45)}) {
                QVERIFY2(near(px(out, pt.x(), pt.y()), pattern.pixelColor(pt), 8),
                         qPrintable(QStringLiteral("nv12=%1 709=%2 full=%3 at %4,%5: %6")
                                        .arg(k.nv12).arg(k.bt709).arg(k.full).arg(pt.x()).arg(pt.y())
                                        .arg(px(out, pt.x(), pt.y()).name())));
                QVERIFY(near(px(out, pt.x(), pt.y()), cpu.pixelColor(pt), 3));
            }
        }

        // Vídeo de celular (90° horário): o lado esquerdo vai para cima.
        Project p = createProject(QStringLiteral("t"), 90, 160, 30);
        auto a = imageAsset(p, {90, 160});
        edit::insertClip(p, createMediaClip(a, 0), 0);
        SolidSource src;
        const VideoImage rotated = toYuv(pattern, false, false, false, 90);
        QCOMPARE(rotated.displaySize(), QSize(90, 160));
        src.images[a.id] = rotated;
        const QImage out = renderAt(p, 0, src);
        QVERIFY2(near(px(out, 45, 30), pattern.pixelColor(30, 45), 8), qPrintable(px(out, 45, 30).name()));
        QVERIFY2(near(px(out, 45, 130), pattern.pixelColor(130, 45), 8), qPrintable(px(out, 45, 130).name()));
        QVERIFY(near(rotated.toImage().pixelColor(45, 30), pattern.pixelColor(30, 45), 8));
    }

    void cleanupTestCase() { m_comp.reset(); }
};

QTEST_MAIN(RenderTest)
#include "tst_render.moc"
