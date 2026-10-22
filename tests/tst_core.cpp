// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/DraftImport.h"
#include "core/Captions.h"
#include "core/Clipboard.h"
#include "core/Edit.h"
#include "core/Evaluate.h"
#include "core/History.h"
#include "core/Keyframes.h"
#include "core/Lut.h"
#include "core/Serialize.h"
#include "core/SpeedCurve.h"
#include "core/Stabilize.h"
#include "core/Tracking.h"

#include <QFile>
#include <QTest>

using namespace clv;

namespace {

MediaAsset videoAsset(Project &p, Micros duration = 10 * kSecond)
{
    MediaAsset a;
    a.id = newId("ast");
    a.kind = MediaKind::Video;
    a.path = QStringLiteral("/tmp/x.mp4");
    a.duration = duration;
    a.width = 1920;
    a.height = 1080;
    a.hasAudio = true;
    a.hasVideo = true;
    p.assets.emplace(a.id, a);
    return a;
}

} // namespace

class CoreTest : public QObject {
    Q_OBJECT

private slots:
    void colorGrading()
    {
        ColorAdjust c;
        QVERIFY(!c.hasGrade());
        QVERIFY(c.gradeKey().isEmpty());
        // Neutro: LUT identidade.
        Lut3D id = buildGradeLut(c, 9);
        auto near = [](std::array<float, 3> a, std::array<float, 3> b, float tol = 0.01f) {
            return std::abs(a[0] - b[0]) < tol && std::abs(a[1] - b[1]) < tol && std::abs(a[2] - b[2]) < tol;
        };
        QVERIFY(near(id.sample(0.3f, 0.6f, 0.9f), {0.3f, 0.6f, 0.9f}));

        // Curva mestre levantando os meios-tons: monotônica, passa pelos pontos.
        const auto curve = sampleCurve({{0.5, 0.7}});
        QCOMPARE(curve.size(), size_t(1024));
        QVERIFY(std::abs(curve[512] - 0.7f) < 0.01f);
        for (size_t i = 1; i < curve.size(); ++i)
            QVERIFY(curve[i] >= curve[i - 1]);
        c.curves[0] = {{0.5, 0.7}};
        QVERIFY(c.hasGrade());
        Lut3D lifted = buildGradeLut(c, 17);
        QVERIFY(lifted.sample(0.5f, 0.5f, 0.5f)[0] > 0.65f);
        QVERIFY(near(lifted.sample(0, 0, 0), {0, 0, 0}));

        // HSL: tira a saturação dos vermelhos e deixa o azul em paz.
        ColorAdjust h;
        h.hsl[0].saturation = -1;
        Lut3D hsl = buildGradeLut(h, 17);
        const auto red = hsl.sample(0.9f, 0.1f, 0.1f);
        QVERIFY(std::abs(red[0] - red[1]) < 0.05f);
        QVERIFY(near(hsl.sample(0.1f, 0.1f, 0.9f), {0.1f, 0.1f, 0.9f}, 0.03f));

        // Roda de ganho para o azul deixa os realces azulados; o preto não muda.
        ColorAdjust w;
        w.gain = {std::cos(4.18879), std::sin(4.18879), 0}; // 240° = azul
        Lut3D wheel = buildGradeLut(w, 17);
        const auto white = wheel.sample(0.8f, 0.8f, 0.8f);
        QVERIFY(white[2] > white[0] + 0.1f);
        QVERIFY(near(wheel.sample(0, 0, 0), {0, 0, 0}));

        // Serialização e chave estável.
        Project p = createProject();
        auto a = videoAsset(p);
        Clip clip = createMediaClip(a, 0);
        clip.color = c;
        clip.color.hsl = h.hsl;
        clip.color.gain = w.gain;
        edit::insertClip(p, clip, 0);
        Project q;
        QVERIFY(parseProject(serializeProject(p), q));
        const ColorAdjust &back = q.tracks[0].clips[0].color;
        QCOMPARE(back.curves[0].size(), size_t(1));
        QCOMPARE(back.hsl[0].saturation, -1.0);
        QVERIFY(std::abs(back.gain.x - w.gain.x) < 1e-9);
        QCOMPARE(back.gradeKey(), clip.color.gradeKey());
    }

    void removeRangesRipples()
    {
        // Vídeo de 10 s na principal, texto de 4 a 8 s em cima, marcador em 9 s.
        Project p = createProject();
        auto a = videoAsset(p);
        edit::insertClip(p, createMediaClip(a, 0), 0);
        auto t = createTextClip(QStringLiteral("x"), 4 * kSecond);
        t.duration = 4 * kSecond;
        edit::insertClip(p, t, -1);
        p.markers.push_back({newId("mrk"), 9 * kSecond, {}, QStringLiteral("#fff")});
        p.outPoint = 9 * kSecond;
        // Dois cortes (fora de ordem e um vazio): 1–2 s e 5–6 s.
        const Micros removed = edit::removeRanges(p, {{5 * kSecond, 6 * kSecond}, {1 * kSecond, 2 * kSecond}, {3 * kSecond, 3 * kSecond}});
        QCOMPARE(removed, 2 * kSecond);
        QCOMPARE(p.duration(), 8 * kSecond);
        QCOMPARE(p.tracks[0].clips.size(), size_t(3)); // 0–1, 2–5, 6–10 da fonte
        QCOMPARE(p.tracks[0].clips[1].sourceIn, 2 * kSecond);
        QCOMPARE(p.tracks[0].clips[2].start, 4 * kSecond);
        QCOMPARE(p.tracks[0].clips[2].sourceIn, 6 * kSecond);
        // Texto 4–8 perde 5–6 e recua 1 s: 3–4 e 4–6.
        Micros textLen = 0;
        for (const auto &c : p.tracks[1].clips)
            textLen += c.duration;
        QCOMPARE(textLen, 3 * kSecond);
        QCOMPARE(p.tracks[1].clips.front().start, 3 * kSecond);
        QCOMPARE(p.markers.front().t, 7 * kSecond);
        QCOMPARE(p.outPoint, 7 * kSecond);
    }

    void keyframeInterpolation()
    {
        std::vector<Keyframe> k{{0, 0, Easing::Linear}, {kSecond, 10, Easing::Linear}};
        QCOMPARE(valueAt(k, -5, 99), 0.0);
        QCOMPARE(valueAt(k, kSecond / 2, 99), 5.0);
        QCOMPARE(valueAt(k, 2 * kSecond, 99), 10.0);
        QCOMPARE(valueAt({}, 0, 99), 99.0);
        k[0].ease = Easing::Hold;
        QCOMPARE(valueAt(k, kSecond / 2, 99), 0.0);
    }

    void bezierEasing()
    {
        // Diagonal = linear; curvas clássicas do CSS nos pontos conhecidos.
        for (double u : {0.0, 0.1, 0.5, 0.9, 1.0})
            QVERIFY(std::abs(cubicBezier(0, 0, 1, 1, u) - u) < 1e-6);
        QVERIFY(std::abs(cubicBezier(0.42, 0, 0.58, 1, 0.5) - 0.5) < 1e-6); // ease-in-out é simétrica
        QVERIFY(cubicBezier(0.42, 0, 1, 1, 0.25) < 0.25);                   // ease-in começa devagar
        QVERIFY(cubicBezier(0, 0, 0.58, 1, 0.25) > 0.25);                   // ease-out começa rápido
        // Ultrapassagem (y > 1) é permitida: efeito "mola".
        QVERIFY(cubicBezier(0.3, 1.6, 0.6, 1.4, 0.6) > 1.0);
        // Monotônica em x mesmo com pontos extremos.
        double last = -1;
        for (int i = 0; i <= 100; ++i) {
            const double v = cubicBezier(0.9, 0.1, 0.1, 0.9, i / 100.0);
            QVERIFY(v >= last - 1e-9);
            last = v;
        }
        // No keyframe e na serialização.
        Keyframe a{0, 0, Easing::Bezier, 0.42, 0, 0.58, 1}, b{kSecond, 10, Easing::Linear};
        QVERIFY(std::abs(valueAt({a, b}, kSecond / 2, 0) - 5.0) < 1e-4);
        QVERIFY(valueAt({a, b}, kSecond / 4, 0) < 2.5);
        Project p = createProject();
        auto t = createTextClip(QStringLiteral("x"), 0);
        t.keyframes[static_cast<int>(Prop::Scale)] = {a, b};
        edit::insertClip(p, t, -1);
        Project q;
        QVERIFY(parseProject(serializeProject(p), q));
        const Keyframe &k = q.tracks.back().clips[0].keyframes[static_cast<int>(Prop::Scale)][0];
        QCOMPARE(k.ease, Easing::Bezier);
        QCOMPARE(k.bx1, 0.42);
        QCOMPARE(k.by2, 1.0);
    }

    void splitKeyframesKeepsValue()
    {
        std::vector<Keyframe> k{{0, 0, Easing::Linear}, {kSecond, 10, Easing::Linear}};
        auto [l, r] = splitKeyframes(k, kSecond / 4);
        QCOMPARE(l.back().v, 2.5);
        QCOMPARE(r.front().t, Micros(0));
        QCOMPARE(r.front().v, 2.5);
        QCOMPARE(r.back().t, kSecond * 3 / 4);
    }

    void mainTrackMagnetInsert()
    {
        Project p = createProject();
        p.mainTrackMagnet = true;
        auto a = videoAsset(p, 4 * kSecond);
        edit::insertClip(p, createMediaClip(a, 0), 0);
        edit::insertClip(p, createMediaClip(a, 100 * kSecond), 0); // vai para o fim, sem buraco
        QCOMPARE(p.tracks[0].clips.size(), size_t(2));
        QCOMPARE(p.tracks[0].clips[1].start, 4 * kSecond);
        // Inserir no meio do primeiro clipe (antes do ponto médio) entra antes dele.
        auto mid = createMediaClip(a, kSecond);
        const QString id = edit::insertClip(p, mid, 0);
        QCOMPARE(p.tracks[0].clips[0].id, id);
        QCOMPARE(p.duration(), 12 * kSecond);
    }

    void mainTrackFreeMove()
    {
        // Sem ímã (padrão), a faixa principal aceita buracos e clipes no meio deles.
        Project p = createProject();
        QVERIFY(!p.mainTrackMagnet);
        auto a = videoAsset(p, 2 * kSecond);
        const QString first = edit::insertClip(p, createMediaClip(a, 0), 0);
        const QString second = edit::insertClip(p, createMediaClip(a, 2 * kSecond), 0);
        QVERIFY(edit::moveClip(p, second, 0, 6 * kSecond));
        QCOMPARE(edit::clipById(p, second)->start, 6 * kSecond);
        QCOMPARE(edit::clipById(p, first)->start, Micros(0));
        const QString gap = edit::insertClip(p, createMediaClip(a, 3 * kSecond), 0);
        QCOMPARE(edit::findClip(p, gap)->track, 0);
        QCOMPARE(p.tracks.size(), size_t(1));
    }

    void splitAndSource()
    {
        Project p = createProject();
        auto a = videoAsset(p);
        const QString id = edit::insertClip(p, createMediaClip(a, 0), 0);
        auto right = edit::splitClip(p, id, 3 * kSecond);
        QVERIFY(right);
        const Clip *l = edit::clipById(p, id);
        const Clip *r = edit::clipById(p, *right);
        QCOMPARE(l->duration, 3 * kSecond);
        QCOMPARE(r->start, 3 * kSecond);
        QCOMPARE(r->sourceIn, 3 * kSecond);
        QCOMPARE(r->duration, 7 * kSecond);
        // Não divide na borda.
        QVERIFY(!edit::splitClip(p, id, 0));
    }

    void splitReverse()
    {
        Project p = createProject();
        auto a = videoAsset(p);
        auto c = createMediaClip(a, 0);
        c.reverse = true;
        const QString id = edit::insertClip(p, c, 0);
        auto right = edit::splitClip(p, id, 4 * kSecond);
        QVERIFY(right);
        // Em reverso, o início da timeline mostra o fim da mídia.
        QCOMPARE(sourceTimeAt(p, *edit::clipById(p, id), 0), 10 * kSecond - 1);
        QCOMPARE(sourceTimeAt(p, *edit::clipById(p, *right), 0), 6 * kSecond - 1);
    }

    void trimRespectsSource()
    {
        Project p = createProject();
        p.mainTrackMagnet = true;
        auto a = videoAsset(p, 5 * kSecond);
        const QString id = edit::insertClip(p, createMediaClip(a, 0), 0);
        QVERIFY(edit::trimClip(p, id, edit::Edge::End, 20 * kSecond));
        QCOMPARE(edit::clipById(p, id)->duration, 5 * kSecond); // limitado pela mídia
        QVERIFY(edit::trimClip(p, id, edit::Edge::Start, 2 * kSecond));
        const Clip *c = edit::clipById(p, id);
        QCOMPARE(c->sourceIn, 2 * kSecond);
        QCOMPARE(c->start, Micros(0)); // faixa principal magnética recompacta
        QCOMPARE(c->duration, 3 * kSecond);
    }

    void overlayTrackPlacement()
    {
        Project p = createProject();
        auto a = videoAsset(p, 5 * kSecond);
        edit::insertClip(p, createMediaClip(a, 0), 0);
        edit::insertClip(p, createMediaClip(a, kSecond), -1);
        edit::insertClip(p, createMediaClip(a, 2 * kSecond), -1); // sobrepõe: nova faixa
        QCOMPARE(p.tracks.size(), size_t(3));
        edit::insertClip(p, createTextClip(QStringLiteral("Oi"), 0), -1);
        QCOMPARE(p.tracks.back().kind, TrackKind::Text);
    }

    void deleteWithRipple()
    {
        Project p = createProject();
        p.mainTrackMagnet = true;
        auto a = videoAsset(p, 2 * kSecond);
        edit::insertClip(p, createMediaClip(a, 0), 0);
        const QString b = edit::insertClip(p, createMediaClip(a, 10 * kSecond), 0);
        const QString c = edit::insertClip(p, createMediaClip(a, 10 * kSecond), 0);
        QVERIFY(edit::deleteClips(p, {b}, true));
        QCOMPARE(edit::clipById(p, c)->start, 2 * kSecond);
    }

    void speedChangesDuration()
    {
        Project p = createProject();
        auto a = videoAsset(p, 10 * kSecond);
        const QString id = edit::insertClip(p, createMediaClip(a, 0), 0);
        QVERIFY(edit::setSpeed(p, id, 2));
        QCOMPARE(edit::clipById(p, id)->duration, 5 * kSecond);
        QCOMPARE(sourceTimeAt(p, *edit::clipById(p, id), kSecond), 2 * kSecond);
    }

    void evaluateLayersAndTransition()
    {
        Project p = createProject();
        auto a = videoAsset(p, 4 * kSecond);
        edit::insertClip(p, createMediaClip(a, 0), 0);
        auto second = createMediaClip(a, 4 * kSecond);
        second.transitionIn = Transition{TransitionKind::Dissolve, kSecond};
        edit::insertClip(p, second, 0);
        edit::insertClip(p, createTextClip(QStringLiteral("T"), 0, 8 * kSecond), -1);

        auto f = evaluate(p, 2 * kSecond);
        QCOMPARE(f.layers.size(), size_t(2)); // vídeo + texto
        QCOMPARE(f.audio.size(), size_t(1));

        // No corte (4s), a transição está na metade: duas camadas de vídeo.
        f = evaluate(p, 4 * kSecond);
        QCOMPARE(f.layers.size(), size_t(3));
        QCOMPARE(f.layers[1].opacity, 0.5);
        // Antes do corte, o clipe que entra também aparece.
        f = evaluate(p, 4 * kSecond - kSecond / 4);
        QCOMPARE(f.layers.size(), size_t(3));
        QCOMPARE(f.layers[1].opacity, 0.25);
    }

    void transitionAcrossTinyGap()
    {
        // Clipes soltos a olho deixam uma folga menor que meio quadro: o corte ainda vale.
        Project p = createProject(); // 30 fps
        auto a = videoAsset(p, 4 * kSecond);
        edit::insertClip(p, createMediaClip(a, 0), 0);
        auto second = createMediaClip(a, 4 * kSecond + 10'000);
        second.transitionIn = Transition{TransitionKind::Dissolve, kSecond};
        edit::insertClip(p, second, 0);
        QVERIFY(touchesPrevious(p, p.tracks[0].clips, 1));
        auto f = evaluate(p, 4 * kSecond + 10'000);
        QCOMPARE(f.layers.size(), size_t(2));
        QCOMPARE(f.layers[1].opacity, 0.5);
        // Dentro da folga, o clipe que sai continua na tela junto do que entra.
        f = evaluate(p, 4 * kSecond + 5'000);
        QCOMPARE(f.layers.size(), size_t(2));

        // Um espaço de verdade não tem corte.
        p.tracks[0].clips[1].start = 5 * kSecond;
        QVERIFY(!touchesPrevious(p, p.tracks[0].clips, 1));
        QCOMPARE(evaluate(p, 5 * kSecond).layers.size(), size_t(1));
    }

    void audioFades()
    {
        Project p = createProject();
        auto a = videoAsset(p, 4 * kSecond);
        auto c = createMediaClip(a, 0);
        c.fadeIn = kSecond;
        c.fadeOut = kSecond;
        QCOMPARE(audioGainAt(c, kSecond / 2), 0.5);
        QCOMPARE(audioGainAt(c, 2 * kSecond), 1.0);
        QCOMPARE(audioGainAt(c, 4 * kSecond - kSecond / 4), 0.25);
    }

    void serializationRoundTrip()
    {
        Project p = createProject(QStringLiteral("Teste"), 1080, 1920, 60);
        auto a = videoAsset(p);
        auto c = createMediaClip(a, 0);
        c.keyframes[static_cast<int>(Prop::Scale)] = {{0, 1, Easing::EaseIn}, {kSecond, 2, Easing::Linear}};
        c.transitionIn = Transition{TransitionKind::WipeLeft, 700'000};
        c.chroma.enabled = true;
        c.blend = BlendMode::Screen;
        c.removeBackground = true;
        c.voiceIsolation = 0.7;
        p.inPoint = 250'000;
        p.outPoint = 900'000;
        c.mask = Mask{MaskShape::Rectangle, 0.3, 0.6, 0.2, 0.4, 15, 0.1, 0.5, true};
        edit::insertClip(p, c, 0);
        auto t = createTextClip(QStringLiteral("Olá ✓"), 0);
        t.animIn = {AnimationKind::Zoom, 300'000};
        t.effect = {QStringLiteral("glow"), 0.4};
        edit::insertClip(p, t, -1);
        auto fx = createAdjustClip(0, kSecond);
        fx.effect = {QStringLiteral("rgbSplit"), 0.7};
        edit::insertClip(p, fx, -1);
        p.markers.push_back({newId("mrk"), kSecond, QStringLiteral("m"), QStringLiteral("#ff0000")});
        p.markers.push_back({newId("mrk"), 2 * kSecond, QString(), QStringLiteral("#22d3ee"), true});

        Project q;
        QString err;
        QVERIFY2(parseProject(serializeProject(p), q, &err), qPrintable(err));
        QCOMPARE(serializeProject(q), serializeProject(p));
        QCOMPARE(q.tracks[0].clips[0].keyframes[static_cast<int>(Prop::Scale)][0].ease, Easing::EaseIn);
        QCOMPARE(q.tracks[1].clips[0].text, QStringLiteral("Olá ✓"));
        QVERIFY(q.tracks[0].clips[0].removeBackground);
        QCOMPARE(q.tracks[0].clips[0].voiceIsolation, 0.7);
        QCOMPARE(q.inPoint, 250'000);
        QCOMPARE(q.outPoint, 900'000);
        QVERIFY(!q.tracks[1].clips[0].removeBackground);
        QCOMPARE(q.tracks[0].clips[0].mask.shape, MaskShape::Rectangle);
        QVERIFY(q.tracks[0].clips[0].mask.invert);
        QCOMPARE(q.tracks[0].clips[0].mask.rotation, 15.0);
        // O efeito sobrevive em texto e em camadas de ajuste (que saem cedo da serialização).
        QCOMPARE(q.tracks[1].clips[0].effect.id, QStringLiteral("glow"));
        QCOMPARE(q.tracks[2].clips[0].effect.id, QStringLiteral("rgbSplit"));
        QCOMPARE(q.tracks[2].clips[0].effect.intensity, 0.7);
        QVERIFY(!q.markers[0].beat);
        QVERIFY(q.markers[1].beat);
    }

    void compoundClips()
    {
        Project p = createProject();
        p.mainTrackMagnet = true;
        auto a = videoAsset(p, 10 * kSecond);
        auto c1 = createMediaClip(a, 0);
        c1.duration = 4 * kSecond;
        auto c2 = createMediaClip(a, 4 * kSecond);
        c2.duration = 3 * kSecond;
        c2.sourceIn = 5 * kSecond;
        auto c3 = createMediaClip(a, 7 * kSecond);
        c3.duration = 2 * kSecond;
        const QString id1 = edit::insertClip(p, c1, 0);
        const QString id2 = edit::insertClip(p, c2, 0);
        const QString id3 = edit::insertClip(p, c3, 0);
        const QString title = edit::insertClip(p, createTextClip(QStringLiteral("T"), 5 * kSecond, 2 * kSecond), -1);
        QCOMPARE(p.tracks.size(), size_t(2));

        // Clipes da principal que não são vizinhos: recusa.
        edit::CompoundError err;
        QVERIFY(edit::createCompound(p, {id1, id3}, QStringLiteral("x"), &err).isEmpty());
        QCOMPARE(err, edit::CompoundError::MainTrackGap);

        const QString cmp = edit::createCompound(p, {id2, title, id1}, QStringLiteral("Compound 1"), &err);
        QVERIFY(!cmp.isEmpty());
        QCOMPARE(p.tracks.size(), size_t(1)); // a faixa de texto ficou vazia e saiu
        QCOMPARE(p.tracks[0].clips.size(), size_t(2));
        const Clip &cc = p.tracks[0].clips[0];
        QVERIFY(cc.isCompound());
        QCOMPARE(cc.duration, 7 * kSecond);
        QCOMPARE(p.tracks[0].clips[1].start, 7 * kSecond); // c3 continua onde estava
        const Sequence *seq = p.compound(cc.compoundId);
        QVERIFY(seq);
        QCOMPARE(seq->tracks.size(), size_t(2));
        QCOMPARE(seq->duration(), 7 * kSecond);

        // Avaliação: a camada do composto traz o quadro interno (vídeo + texto aos 5,5 s).
        const FrameDesc f = evaluate(p, 5'500'000);
        QCOMPARE(f.layers.size(), size_t(1));
        QVERIFY(f.layers[0].nested);
        QCOMPARE(f.layers[0].nested->layers.size(), size_t(2));
        QCOMPARE(f.layers[0].nested->layers[0].sourceTime, 6'500'000); // c2 começa em 4 s com origem 5 s
        QVERIFY(f.layers[0].nested->layers[1].clip->isText());

        // Salvar e reabrir preserva a sequência.
        Project q;
        QVERIFY(parseProject(serializeProject(p), q));
        QCOMPARE(serializeProject(q), serializeProject(p));
        QCOMPARE(q.compounds.size(), size_t(1));

        // O fim não passa do conteúdo; dividir gera dois compostos com a mesma sequência.
        QVERIFY(edit::trimClip(p, cmp, edit::Edge::End, 20 * kSecond) == false || p.tracks[0].clips[0].duration <= 7 * kSecond);
        const auto right = edit::splitClip(p, cmp, 2 * kSecond);
        QVERIFY(right);
        const Clip *r = edit::clipById(p, *right);
        QVERIFY(r && r->isCompound());
        QCOMPARE(r->sourceIn, 2 * kSecond);
        QCOMPARE(r->compoundId, p.tracks[0].clips[0].compoundId);

        // Desfazer o da direita: volta o trecho de 2 s a 7 s, nos mesmos lugares.
        const QStringList back = edit::breakCompound(p, *right);
        QCOMPARE(back.size(), 3); // fim de c1, c2 e o título
        QCOMPARE(p.compounds.size(), size_t(1)); // a metade esquerda ainda usa a sequência
        const Clip *b1 = edit::clipById(p, back[0]);
        QVERIFY(b1 && b1->hasMedia());
        QCOMPARE(b1->start, 2 * kSecond);
        QCOMPARE(b1->sourceIn, 2 * kSecond);
        QCOMPARE(b1->duration, 2 * kSecond);
        const Clip *t = edit::clipById(p, back[2]);
        QVERIFY(t && t->isText());
        QCOMPARE(t->start, 5 * kSecond);
        QCOMPARE(p.tracks.size(), size_t(2));
        QVERIFY(p.tracks[1].kind == TrackKind::Text);
        QCOMPARE(edit::clipById(p, id3)->start, 7 * kSecond);
        // Desfazer o outro: a sequência some.
        edit::breakCompound(p, p.tracks[0].clips[0].id);
        QVERIFY(p.compounds.empty());
        QCOMPARE(p.tracks[0].clips.size(), size_t(4));
    }

    void compoundWorkspaceRoundTrip()
    {
        Project p = createProject();
        p.mainTrackMagnet = true;
        auto a = videoAsset(p, 10 * kSecond);
        auto c1 = createMediaClip(a, 0);
        c1.duration = 3 * kSecond;
        auto c2 = createMediaClip(a, 3 * kSecond);
        c2.duration = 3 * kSecond;
        auto c3 = createMediaClip(a, 6 * kSecond);
        c3.duration = 2 * kSecond;
        const QString id1 = edit::insertClip(p, c1, 0), id2 = edit::insertClip(p, c2, 0), id3 = edit::insertClip(p, c3, 0);
        const QString title = edit::insertClip(p, createTextClip(QStringLiteral("T"), 0, 6 * kSecond), -1);
        const QString cmpA = edit::createCompound(p, {id1, id2, title}, QStringLiteral("A"));
        // Outro composto, usado só fora (não pode sumir ao editar o A).
        const QString cmpB = edit::createCompound(p, {id3}, QStringLiteral("B"));
        QVERIFY(!cmpA.isEmpty() && !cmpB.isEmpty());
        const QString seqA = edit::clipById(p, cmpA)->compoundId;
        QCOMPARE(p.compounds.size(), size_t(2));

        auto w = edit::compoundWorkspace(p, seqA);
        QVERIFY(w);
        QCOMPARE(w->tracks.size(), size_t(2));
        QVERIFY(w->tracks[0].kind == TrackKind::Video);
        QVERIFY(!w->mainTrackMagnet);
        QCOMPARE(w->duration(), 6 * kSecond);
        // Dentro: apaga o segundo vídeo e encurta o título (o conteúdo fica com 3 s),
        // importa uma mídia nova e roda uma limpeza que não enxerga os clipes de fora.
        QVERIFY(edit::deleteClips(*w, {id2}, false));
        QVERIFY(edit::trimClip(*w, title, edit::Edge::End, 2 * kSecond));
        auto extra = videoAsset(*w, kSecond);
        w->background = QStringLiteral("#123456");
        edit::pruneCompounds(*w);
        QVERIFY(w->compounds.empty());

        edit::mergeCompoundWorkspace(p, seqA, *w);
        QCOMPARE(p.compounds.size(), size_t(2)); // A e B continuam
        QCOMPARE(p.compound(seqA)->duration(), 3 * kSecond);
        QCOMPARE(edit::clipById(p, cmpA)->duration, 3 * kSecond); // composto encurtado
        QCOMPARE(edit::clipById(p, cmpB)->start, 3 * kSecond);    // principal magnética fecha o buraco
        QVERIFY(p.assets.count(extra.id));
        QCOMPARE(p.background, QStringLiteral("#123456"));

        // Esvaziar o conteúdo remove o composto e a sequência.
        auto w2 = edit::compoundWorkspace(p, seqA);
        for (auto &t : w2->tracks)
            t.clips.clear();
        edit::mergeCompoundWorkspace(p, seqA, *w2);
        QVERIFY(!edit::clipById(p, cmpA));
        QVERIFY(!p.compound(seqA));
        QVERIFY(edit::clipById(p, cmpB));
    }

    void stabilizationMath()
    {
        // Correção de +1% da largura para a direita: o ponto mostrado vem de 1% à esquerda na origem.
        StabCorrection c;
        c.dx = 0.01;
        double u = 0.5, v = 0.5;
        stabilizedUv(c, 16.0 / 9, u, v);
        QVERIFY(std::abs(u - 0.49) < 1e-9 && std::abs(v - 0.5) < 1e-9);
        // Zoom 2×: a borda mostrada vem de um quarto para dentro.
        c = {};
        c.zoom = 2;
        u = 1;
        v = 0;
        stabilizedUv(c, 16.0 / 9, u, v);
        QVERIFY(std::abs(u - 0.75) < 1e-9 && std::abs(v - 0.25) < 1e-9);

        // Caminho com tremor rápido sobre uma panorâmica lenta: a correção desfaz o tremor e mantém a panorâmica.
        auto track = std::make_shared<MotionTrack>();
        track->fps = 30;
        for (int i = 0; i < 300; ++i) {
            track->x.push_back(0.0005 * i + 0.01 * ((i % 2) ? 1 : -1));
            track->y.push_back(0);
            track->angle.push_back(0);
            track->logScale.push_back(0);
        }
        const StabCorrection mid = stabilizationAt(*track, 150 * kSecond / 30, 1.0);
        QVERIFY2(std::abs(mid.dx - 0.01) < 0.002, qPrintable(QString::number(mid.dx))); // quadro par: tremor de -1%, correção de +1%
        QVERIFY(mid.zoom > 1.0 && mid.zoom < 1.1);
        QVERIFY(!stabilizationAt(*track, 0, 0).active());

        const auto back = motionFromJson(motionToJson(*track));
        QVERIFY(back && back->frames() == 300);
        QVERIFY(std::abs(back->x[7] - track->x[7]) < 1e-6);
        QVERIFY(!motionFromJson("{}"));
    }

    void trackedPathToKeyframes()
    {
        Project p = createProject(QStringLiteral("t"), 1920, 1080, 30);
        auto a = videoAsset(p, 10 * kSecond); // 1920×1080
        p.assets[a.id].width = 1280;
        p.assets[a.id].height = 720;
        a = p.assets[a.id];
        auto vc = createMediaClip(a, 0);
        vc.duration = 2 * kSecond;
        vc.transform.scale = 0.5;
        vc.transform.x = 0.5; // metade direita
        const QString vid = edit::insertClip(p, vc, 0);
        auto t = createTextClip(QStringLiteral("Hi"), 0, 2 * kSecond);
        t.transform.x = 0.1;
        const QString txt = edit::insertClip(p, t, -1);

        // Ida e volta imagem ↔ canvas, com rotação e espelho.
        Clip *v = edit::clipById(p, vid);
        v->transform.rotation = 30;
        v->transform.flipH = true;
        const QPointF c = clipImageToCanvas(p, *v, 0, 0.3, 0.7);
        double u = 0, w = 0;
        QVERIFY(canvasToClipImage(p, *v, 0, c, u, w));
        QVERIFY(std::abs(u - 0.3) < 1e-9 && std::abs(w - 0.7) < 1e-9);
        v->transform.rotation = 0;
        v->transform.flipH = false;

        // O ponto anda de u = 0,2 a 0,8 em 2 s: na tela isso são 0,6 × 960 px = 0,6 em X normalizado.
        std::vector<TrackedPoint> path;
        for (int i = 0; i <= 60; ++i)
            path.push_back({i * kSecond / 30, 0.2 + 0.6 * i / 60.0, 0.5});
        QVERIFY(applyTrackedPath(p, txt, vid, 0, path));
        const Clip *f = edit::clipById(p, txt);
        const auto &kx = f->keyframes[static_cast<int>(Prop::X)];
        QVERIFY2(kx.size() <= 3, qPrintable(QString::number(kx.size()))); // reta: poucos keyframes
        QVERIFY(std::abs(propAt(*f, Prop::X, 0) - 0.1) < 1e-6);
        QVERIFY(std::abs(propAt(*f, Prop::X, kSecond) - 0.4) < 0.002);
        QVERIFY(std::abs(propAt(*f, Prop::Y, kSecond) - t.transform.y) < 1e-6); // y não muda
        // Sem trecho em comum: nada muda.
        QVERIFY(!applyTrackedPath(p, txt, QStringLiteral("nada"), 0, path));
    }

    void templatesAndSlots()
    {
        Project p = createProject();
        p.mainTrackMagnet = true;
        auto video = videoAsset(p, 10 * kSecond);
        auto c1 = createMediaClip(video, 0);
        c1.duration = 4 * kSecond;
        c1.transform.scale = 0.8;
        c1.color.filter = QStringLiteral("vivid");
        auto c2 = createMediaClip(video, 4 * kSecond);
        c2.duration = 3 * kSecond;
        c2.transitionIn = Transition{TransitionKind::Dissolve, 500'000};
        const QString id1 = edit::insertClip(p, c1, 0), id2 = edit::insertClip(p, c2, 0);
        edit::insertClip(p, createTextClip(QStringLiteral("Title"), 0, 2 * kSecond), -1);

        edit::makeTemplate(p, [](MediaKind k, int n) {
            return QStringLiteral("%1 %2").arg(k == MediaKind::Image ? QStringLiteral("Photo") : QStringLiteral("Video")).arg(n);
        });
        QVERIFY(p.assets.empty());
        const QStringList empty = edit::emptySlots(p);
        QCOMPARE(empty, (QStringList{id1, id2}));
        QCOMPARE(edit::clipById(p, id1)->slot, QStringLiteral("Video 1"));
        QCOMPARE(edit::clipById(p, id2)->slot, QStringLiteral("Video 2"));
        QVERIFY(edit::clipById(p, id1)->isEmptySlot());
        // Texto continua; salvar e reabrir mantém os espaços.
        Project q;
        QVERIFY(parseProject(serializeProject(p), q));
        QCOMPARE(edit::emptySlots(q), empty);

        // Vídeo curto no primeiro espaço: o espaço encolhe e a principal fecha o buraco;
        // escala, filtro e transição ficam.
        MediaAsset shortClip = videoAsset(p, 2 * kSecond);
        QVERIFY(edit::setClipMedia(p, id1, shortClip));
        const Clip *f1 = edit::clipById(p, id1);
        QVERIFY(!f1->isEmptySlot());
        QCOMPARE(f1->duration, 2 * kSecond);
        QCOMPARE(f1->transform.scale, 0.8);
        QCOMPARE(f1->color.filter, QStringLiteral("vivid"));
        QCOMPARE(edit::clipById(p, id2)->start, 2 * kSecond);
        // Foto num espaço de vídeo: mantém a duração do espaço.
        MediaAsset photo;
        photo.id = newId("ast");
        photo.kind = MediaKind::Image;
        photo.width = 800;
        photo.height = 600;
        QVERIFY(edit::setClipMedia(p, id2, photo));
        QCOMPARE(edit::clipById(p, id2)->duration, 3 * kSecond);
        QVERIFY(edit::clipById(p, id2)->transitionIn.has_value());
        QVERIFY(edit::emptySlots(p).isEmpty());
        // Texto não recebe mídia.
        QVERIFY(!edit::setClipMedia(p, p.tracks[1].clips[0].id, photo));
    }

    void historyUndoRedo()
    {
        Project p = createProject();
        auto a = videoAsset(p);
        History h;
        h.push(p, QStringLiteral("inserir"));
        edit::insertClip(p, createMediaClip(a, 0), 0);
        QVERIFY(h.undo(p));
        QCOMPARE(p.tracks[0].clips.size(), size_t(0));
        QVERIFY(h.redo(p));
        QCOMPARE(p.tracks[0].clips.size(), size_t(1));
    }

    void snapping()
    {
        Project p = createProject();
        auto a = videoAsset(p, 4 * kSecond);
        edit::insertClip(p, createMediaClip(a, 0), 0);
        QCOMPARE(edit::snap(p, 4 * kSecond + 30'000, 50'000, {}, -1), 4 * kSecond);
        QCOMPARE(edit::snap(p, 3 * kSecond, 50'000, {}, -1), 3 * kSecond);
    }

    void draftImport()
    {
        QFile f(QFINDTESTDATA("data/draft_sample.json"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray json = f.readAll();
        auto r = importDraftJson(json, QStringLiteral("/drafts/demo"));
        QVERIFY2(r.ok, qPrintable(r.error));
        const Project &p = r.project;
        QCOMPARE(p.width, 1080);
        QCOMPARE(p.height, 1920);
        QCOMPARE(p.tracks.size(), size_t(3)); // vídeo, texto, áudio (effect ignorada)
        QVERIFY(p.mainTrackMagnet);
        const Clip &v = p.tracks[0].clips[0];
        QCOMPARE(v.sourceIn, kSecond);
        QCOMPARE(v.transform.scale, 1.5);
        QCOMPARE(v.transform.y, -0.5); // eixo Y invertido
        QVERIFY(v.transform.flipH);
        QCOMPARE(v.keyframes[static_cast<int>(Prop::Y)][0].v, -0.5);
        QVERIFY(!v.transitionIn);
        QVERIFY(p.tracks[0].clips[1].transitionIn); // transição movida para o clipe que entra
        QCOMPARE(p.tracks[0].clips[1].speedCurve.size(), size_t(2)); // curva de velocidade
        QCOMPARE(p.tracks[0].clips[1].speed, 2.0);                  // 6 s de origem em 3 s
        QCOMPARE(p.tracks[0].clips[1].transitionIn->duration, Micros(400000));
        QCOMPARE(p.asset(QStringLiteral("V1"))->path, QStringLiteral("/drafts/demo/a.mp4"));
        QCOMPARE(p.asset(QStringLiteral("A1"))->path, QStringLiteral("C:/music/b.mp3"));
        QCOMPARE(p.tracks[1].clips[0].text, QStringLiteral("Oi"));
        QCOMPARE(p.tracks[1].clips[0].style.color, QStringLiteral("#ff0000"));
        QCOMPARE(p.tracks[2].clips[0].fadeIn, Micros(500000));
        QCOMPARE(p.tracks[2].clips[0].volume, 0.5);
        QVERIFY(!r.warnings.isEmpty());
    }

    void subtitlesParseAndWrite()
    {
        const QString srt = QStringLiteral(
            "1\r\n00:00:01,000 --> 00:00:02,500\r\nOlá <i>mundo</i>\r\n\r\n"
            "2\n00:00:03,000 --> 00:00:04,000\nSegunda\nlinha\n\n"
            "3\n00:00:05,000 --> 00:00:04,000\ninválida\n");
        auto segs = parseSubtitles(srt);
        QCOMPARE(segs.size(), size_t(2));
        QCOMPARE(segs[0].start, kSecond);
        QCOMPARE(segs[0].end, 2'500'000);
        QCOMPARE(segs[0].text, QStringLiteral("Olá mundo"));
        QCOMPARE(segs[1].text, QStringLiteral("Segunda\nlinha"));
        QCOMPARE(parseSubtitles(toSrt(segs)).size(), size_t(2));
        QVERIFY(toSrt(segs).contains(QStringLiteral("00:00:01,000 --> 00:00:02,500")));

        const QString vtt = QStringLiteral("WEBVTT\n\n00:01.200 --> 00:02.000\nCurto\n\n01:00:00.000 --> 01:00:01.000\nUma hora\n");
        segs = parseSubtitles(vtt);
        QCOMPARE(segs.size(), size_t(2));
        QCOMPARE(segs[0].start, Micros(1'200'000));
        QCOMPARE(segs[1].start, 3600 * kSecond);
    }

    void captionTrack()
    {
        Project p = createProject();
        std::vector<CaptionSegment> segs{{0, 2 * kSecond, QStringLiteral("a")},
                                         {kSecond, 3 * kSecond, QStringLiteral("b")}, // sobrepõe: é aparado
                                         {4 * kSecond, 5 * kSecond, QStringLiteral("  ")}};
        const int ti = addCaptionTrack(p, segs);
        const Track &t = p.tracks[ti];
        QVERIFY(t.captions);
        QCOMPARE(t.kind, TrackKind::Text);
        QCOMPARE(t.clips.size(), size_t(2));
        QCOMPARE(t.clips[0].end(), kSecond);
        QCOMPARE(captionsFromTrack(t).back().text, QStringLiteral("b"));
        // Persistência da marcação de faixa de legendas.
        Project q;
        QVERIFY(parseProject(serializeProject(p), q));
        QVERIFY(q.tracks[ti].captions);
    }

    void captionReflow()
    {
        std::vector<CaptionSegment> segs{{0, 10 * kSecond, QStringLiteral("um dois tres quatro cinco seis sete oito nove dez")}};
        auto out = reflowCaptions(segs, 20);
        QVERIFY(out.size() >= 3);
        for (const auto &s : out)
            QVERIFY(s.text.size() <= 20);
        QCOMPARE(out.front().start, Micros(0));
        QCOMPARE(out.back().end, 10 * kSecond);
        for (size_t i = 1; i < out.size(); ++i)
            QCOMPARE(out[i].start, out[i - 1].end);
    }

    void cubeParse()
    {
        // LUT 2³ que inverte as cores.
        const QByteArray cube = "# comentário\nTITLE \"Inverter\"\nLUT_3D_SIZE 2\n"
                                "1 1 1\n0 1 1\n1 0 1\n0 0 1\n1 1 0\n0 1 0\n1 0 0\n0 0 0\n";
        QString err;
        Lut3D lut = parseCube(cube, &err);
        QVERIFY2(lut.isValid(), qPrintable(err));
        QCOMPARE(lut.title, QStringLiteral("Inverter"));
        auto o = lut.sample(0.25f, 0.5f, 1.0f);
        QVERIFY(std::abs(o[0] - 0.75f) < 1e-5 && std::abs(o[1] - 0.5f) < 1e-5 && std::abs(o[2]) < 1e-5);
        // Ida e volta pelo formato.
        QCOMPARE(parseCube(toCube(lut)).rgb, lut.rgb);
        // LUT 1D vira 3D.
        Lut3D one = parseCube("LUT_1D_SIZE 2\n0 0 0\n0.5 0.5 0.5\n");
        QVERIFY(one.isValid());
        QVERIFY(std::abs(one.sample(1, 1, 1)[0] - 0.5f) < 1e-4);
        // Erros.
        QVERIFY(!parseCube("LUT_3D_SIZE 2\n0 0 0\n", &err).isValid());
        QVERIFY(err.contains(QStringLiteral("entries")));
    }

    void builtinFilters()
    {
        QVERIFY(clv::builtinFilters().size() >= 12);
        for (const auto &f : clv::builtinFilters()) {
            const Lut3D lut = builtinFilterLut(f.id);
            QVERIFY2(lut.isValid(), qPrintable(f.id));
            QCOMPARE(lut.size, 33);
        }
        // P&B: saída sem cor.
        const auto bw = builtinFilterLut(QStringLiteral("bw")).sample(0.9f, 0.2f, 0.1f);
        QVERIFY(std::abs(bw[0] - bw[1]) < 1e-3 && std::abs(bw[1] - bw[2]) < 1e-3);
        QVERIFY(!builtinFilterLut(QStringLiteral("nao-existe")).isValid());
        QVERIFY(lutForKey(QStringLiteral("warm")) == lutForKey(QStringLiteral("warm"))); // cache
        // Intensidade 0 na CPU não altera a imagem.
        QImage img(4, 4, QImage::Format_RGBA8888);
        img.fill(QColor(200, 30, 90));
        QCOMPARE(applyLut(img, builtinFilterLut(QStringLiteral("bw")), 0.0f), img);
        QVERIFY(applyLut(img, builtinFilterLut(QStringLiteral("bw")), 1.0f) != img);
    }

    void adjustClips()
    {
        Project p = createProject();
        Clip adj = createAdjustClip(kSecond, 2 * kSecond, QStringLiteral("vintage"));
        adj.color.vignette = 0.4;
        adj.color.lutPath = QStringLiteral("/tmp/a.cube");
        const QString id = edit::insertClip(p, adj, -1);
        QVERIFY(!id.isEmpty());
        QCOMPARE(p.tracks.back().kind, TrackKind::Adjust);
        // Dividir e aparar funcionam como em texto (sem mídia de origem).
        QVERIFY(edit::splitClip(p, id, 2 * kSecond));
        QVERIFY(edit::trimClip(p, id, edit::Edge::Start, kSecond / 2));
        // Aparece como camada visual.
        QCOMPARE(evaluate(p, kSecond).layers.size(), size_t(1));
        QVERIFY(evaluate(p, kSecond).audio.empty());
        // Serialização.
        Project q;
        QVERIFY(parseProject(serializeProject(p), q));
        const Clip &c = q.tracks.back().clips.front();
        QVERIFY(c.isAdjust());
        QCOMPARE(c.color.filter, QStringLiteral("vintage"));
        QCOMPARE(c.color.vignette, 0.4);
        QCOMPARE(c.color.lutPath, QStringLiteral("/tmp/a.cube"));
        QCOMPARE(q.tracks.back().kind, TrackKind::Adjust);
    }

    void copyPasteClips()
    {
        Project p = createProject();
        auto a = videoAsset(p, 4 * kSecond);
        const QString main = edit::insertClip(p, createMediaClip(a, 0), 0);
        auto t = createTextClip(QStringLiteral("legenda"), kSecond, kSecond);
        const QString text = edit::insertClip(p, t, -1);
        const Clipboard cb = copyClips(p, {main, text});
        QCOMPARE(cb.items.size(), size_t(2));
        QCOMPARE(cb.assets.size(), size_t(1));

        // Colar no fim: mantém o deslocamento de 1 s do texto e vai para as mesmas faixas.
        const QStringList ids = pasteClips(p, cb, 4 * kSecond);
        QCOMPARE(ids.size(), 2);
        QCOMPARE(p.tracks[0].clips.size(), size_t(2));
        QCOMPARE(p.tracks[0].clips[1].start, 4 * kSecond);
        const Clip *pastedText = edit::clipById(p, ids.contains(text) ? QString() : ids.last());
        QVERIFY(pastedText);
        QVERIFY(pastedText->isText());
        QCOMPARE(pastedText->start, 5 * kSecond);
        QCOMPARE(p.assets.size(), size_t(1)); // mesma mídia reaproveitada

        // JSON (área de transferência do sistema) para outro projeto: leva a mídia.
        Clipboard back;
        QVERIFY(clipboardFromJson(clipboardToJson(cb), back));
        Project other = createProject();
        QCOMPARE(pasteClips(other, back, 0).size(), 2);
        QCOMPARE(other.assets.size(), size_t(1));
        QCOMPARE(other.tracks[0].clips.front().assetId, other.assets.begin()->first);
        QVERIFY(!clipboardFromJson("lixo", back));
    }

    void pasteAttributesBetweenClips()
    {
        Project p = createProject();
        auto a = videoAsset(p);
        Clip src = createMediaClip(a, 0);
        src.transform.scale = 1.5;
        src.color.filter = QStringLiteral("vintage");
        src.mask.shape = MaskShape::Circle;
        src.volume = 0.3;
        src.keyframes[static_cast<int>(Prop::X)] = {{0, 0, Easing::Linear}, {20 * kSecond, 1, Easing::Linear}};
        Clip dst = createMediaClip(a, 0);
        dst.duration = 2 * kSecond;
        pasteAttributes(dst, src);
        QCOMPARE(dst.transform.scale, 1.5);
        QCOMPARE(dst.color.filter, QStringLiteral("vintage"));
        QCOMPARE(dst.mask.shape, MaskShape::Circle);
        QCOMPARE(dst.volume, 0.3);
        QCOMPARE(dst.duration, 2 * kSecond); // tempo não muda
        QVERIFY(dst.keyframes[static_cast<int>(Prop::X)].back().t <= dst.duration);
        // Texto ← mídia: só transformação; texto ← texto: estilo.
        Clip t1 = createTextClip(QStringLiteral("a"), 0), t2 = createTextClip(QStringLiteral("b"), 0);
        t1.style.color = QStringLiteral("#ff0000");
        pasteAttributes(t2, t1);
        QCOMPARE(t2.style.color, QStringLiteral("#ff0000"));
        QCOMPARE(t2.text, QStringLiteral("b"));
    }

    void groupsAndMultiMove()
    {
        Project p = createProject();
        auto a = videoAsset(p, 2 * kSecond);
        edit::insertClip(p, createMediaClip(a, 0), 0);
        const QString t1 = edit::insertClip(p, createTextClip(QStringLiteral("x"), kSecond), -1);
        const QString ov = edit::insertClip(p, createMediaClip(a, kSecond), -1);
        const QString gid = edit::groupClips(p, {t1, ov});
        QVERIFY(!gid.isEmpty());
        QCOMPARE(edit::expandToGroups(p, {t1}).size(), 2);
        QVERIFY(edit::groupClips(p, {t1}).isEmpty()); // precisa de 2+

        // Mover o grupo 2 s para a frente.
        QVERIFY(edit::moveClips(p, edit::expandToGroups(p, {t1}), 2 * kSecond));
        QCOMPARE(edit::clipById(p, t1)->start, 3 * kSecond);
        QCOMPARE(edit::clipById(p, ov)->start, 3 * kSecond);
        // Não passa de zero: delta limitado.
        QVERIFY(edit::moveClips(p, {t1, ov}, -10 * kSecond));
        QCOMPARE(edit::clipById(p, t1)->start, Micros(0));
        // Dividir mantém o grupo; duplicar não.
        auto right = edit::splitClip(p, ov, kSecond);
        QVERIFY(right);
        QCOMPARE(edit::clipById(p, *right)->groupId, gid);
        auto dup = edit::duplicateClip(p, t1);
        QVERIFY(dup && edit::clipById(p, *dup)->groupId.isEmpty());
        QVERIFY(edit::ungroupClips(p, {ov}));
        QCOMPARE(edit::expandToGroups(p, {t1}).size(), 1);
        // Persistência do grupo.
        edit::groupClips(p, {t1, ov});
        Project q;
        QVERIFY(parseProject(serializeProject(p), q));
        QVERIFY(!edit::clipById(q, t1)->groupId.isEmpty());
    }

    void speedCurveMath()
    {
        const std::vector<SpeedPoint> flat{{0, 2}, {1, 2}};
        QCOMPARE(curveMean(flat), 2.0);
        QCOMPARE(curveProgress(flat, 0.25), 0.25);
        // Rampa 1→3: média 2; na metade, consumiu (1+2)/2*0.5 / 2 = 37,5% da origem.
        const std::vector<SpeedPoint> ramp{{0, 1}, {1, 3}};
        QCOMPARE(curveMean(ramp), 2.0);
        QVERIFY(std::abs(curveProgress(ramp, 0.5) - 0.375) < 1e-9);
        QCOMPARE(curveValue(ramp, 0.5), 2.0);
        // Normalização: ordena, cria pontas e limita valores.
        const auto n = normalizedCurve({{0.5, 50}, {0.2, 0.01}});
        QCOMPARE(n.size(), size_t(4));
        QCOMPARE(n.front().u, 0.0);
        QCOMPARE(n.back().u, 1.0);
        QCOMPARE(n[2].v, 10.0);
        QCOMPARE(n[1].v, 0.1);
        QVERIFY(speedPresets().size() >= 6);
    }

    void speedCurveOnClip()
    {
        Project p = createProject();
        auto a = videoAsset(p, 10 * kSecond);
        const QString id = edit::insertClip(p, createMediaClip(a, 0), 0);
        // Rampa 1→3 (média 2): mesma origem de 10 s em 5 s de timeline.
        QVERIFY(edit::setSpeedCurve(p, id, {{0, 1}, {1, 3}}));
        const Clip *c = edit::clipById(p, id);
        QCOMPARE(c->duration, 5 * kSecond);
        QCOMPARE(c->speed, 2.0);
        QCOMPARE(c->sourceDuration(), 10 * kSecond);
        // Origem: começa lento, termina rápido; monotônico e cobre tudo.
        QCOMPARE(sourceTimeAt(p, *c, 0), Micros(0));
        QVERIFY(std::llabs(sourceTimeAt(p, *c, c->duration / 2) - 3'750'000) < 2);
        QVERIFY(sourceTimeAt(p, *c, c->duration) >= 10 * kSecond - 2);
        QCOMPARE(speedAt(*c, 0), 1.0);
        QCOMPARE(speedAt(*c, c->duration), 3.0);

        // Dividir no meio: origem contínua e velocidades médias corretas em cada lado.
        auto right = edit::splitClip(p, id, c->duration / 2);
        QVERIFY(right);
        const Clip *l = edit::clipById(p, id), *r = edit::clipById(p, *right);
        QVERIFY(std::llabs(l->sourceOut() - r->sourceIn) <= 1);
        QVERIFY(std::llabs(l->sourceDuration() - 3'750'000) <= 2);
        QVERIFY(std::llabs(r->sourceOut() - 10 * kSecond) <= 2);
        QVERIFY(std::abs(speedAt(*l, l->duration) - 2.0) < 1e-6);
        QVERIFY(std::abs(speedAt(*r, 0) - 2.0) < 1e-6);
        // Mesmo tempo da timeline, mesma origem antes e depois de dividir.
        // u=0,75 do original: (0,75+0,75²)/2 da origem.
        QVERIFY(std::llabs(sourceTimeAt(p, *r, r->duration / 2) - 6'562'500) < 3);

        // Velocidade normal remove a curva.
        QVERIFY(edit::setSpeed(p, id, 1.0));
        QVERIFY(edit::clipById(p, id)->speedCurve.empty());

        // Persistência.
        QVERIFY(edit::setSpeedCurve(p, *right, speedPresets().front().points));
        Project q;
        QVERIFY(parseProject(serializeProject(p), q));
        QCOMPARE(edit::clipById(q, *right)->speedCurve, edit::clipById(p, *right)->speedCurve);
    }

    void detachAudioFromVideo()
    {
        Project p = createProject();
        auto a = videoAsset(p, 4 * kSecond);
        Clip c = createMediaClip(a, 0);
        c.volume = 0.5;
        c.fadeIn = kSecond;
        c.speed = 2;
        c.duration = 2 * kSecond;
        c.transform.scale = 1.5;
        const QString id = edit::insertClip(p, c, 0);
        auto aid = edit::detachAudio(p, id);
        QVERIFY(aid);
        const Clip *v = edit::clipById(p, id), *au = edit::clipById(p, *aid);
        QVERIFY(v->audioMuted);
        QVERIFY(au->audioOnly);
        QCOMPARE(au->start, v->start);
        QCOMPARE(au->duration, v->duration);
        QCOMPARE(au->speed, 2.0);
        QCOMPARE(au->volume, 0.5);
        QCOMPARE(au->fadeIn, kSecond);
        QCOMPARE(au->transform.scale, 1.0); // atributos visuais não vão para o áudio
        QCOMPARE(p.tracks[edit::findClip(p, *aid)->track].kind, TrackKind::Audio);
        // Avaliação: uma camada visual (o vídeo) e uma fonte de áudio (o clipe separado).
        const FrameDesc f = evaluate(p, kSecond);
        QCOMPARE(f.layers.size(), size_t(1));
        QCOMPARE(f.audio.size(), size_t(1));
        QCOMPARE(f.audio.front().clip->id, *aid);
        // Não separa de novo, nem de imagem/áudio.
        QVERIFY(!edit::detachAudio(p, id));
        QVERIFY(!edit::detachAudio(p, *aid));
        // Persistência.
        Project q;
        QVERIFY(parseProject(serializeProject(p), q));
        QVERIFY(edit::clipById(q, id)->audioMuted);
        QVERIFY(edit::clipById(q, *aid)->audioOnly);
    }

    void draftEncryptedRejected()
    {
        auto r = importDraftJson(QByteArray("\x8f\x12garbage"), {});
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains(QStringLiteral("encrypted")));
    }
};

QTEST_GUILESS_MAIN(CoreTest)
#include "tst_core.moc"
