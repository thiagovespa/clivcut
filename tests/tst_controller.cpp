// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProjectController.h"
#include "core/Evaluate.h"
#include "core/Serialize.h"
#include "Scopes.h"
#include "engine/AudioMixer.h"
#include "engine/AiModels.h"
#include "media/Loudness.h"
#include "engine/WhisperModels.h"
#include "media/MediaProbe.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <clocale>

class ControllerTest : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_video;
    std::unique_ptr<ProjectController> m_ctrl;

    QVariantList clipsOf(int track) const
    {
        for (const auto &t : m_ctrl->tracks())
            if (t.toMap().value("index").toInt() == track)
                return t.toMap().value("clips").toList();
        return {};
    }

private slots:
    void initTestCase()
    {
        std::setlocale(LC_NUMERIC, "C"); // como o app: o FFmpeg lê números com strtod
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg não encontrado");
        m_video = m_dir.filePath(QStringLiteral("v.mp4"));
        QProcess p;
        p.start(QStringLiteral("ffmpeg"), {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                                           "testsrc2=size=320x180:rate=30:duration=3", "-f", "lavfi", "-i",
                                           "sine=duration=3", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-shortest", m_video});
        QVERIFY(p.waitForFinished(60000) && p.exitCode() == 0);
        qputenv("CLIVCUT_CACHE_DIR", m_dir.filePath(QStringLiteral("cache")).toUtf8());
        // Modelos de transcrição continuam vindo da pasta real; o resto vai para ~/.qttest.
        qputenv("CLIVCUT_MODELS_DIR", clv::engine::whisperModelsDir().toUtf8());
        QStandardPaths::setTestModeEnabled(true); // recuperação em ~/.qttest, não nos dados reais
        QDir(ProjectController::recoveryDir()).removeRecursively();
        m_ctrl = std::make_unique<ProjectController>(nullptr);
    }

    void importAndPlace()
    {
        m_ctrl->newProject(640, 360, 30);
        m_ctrl->setMagnet(true); // os testes abaixo usam a faixa principal compacta
        m_ctrl->importMedia({QUrl::fromLocalFile(m_video)});
        QCOMPARE(m_ctrl->assets().size(), 1);
        const QString assetId = m_ctrl->assets().first().toMap().value("id").toString();
        m_ctrl->addAssetToTimeline(assetId);
        m_ctrl->addAssetToTimeline(assetId);
        QCOMPARE(clipsOf(0).size(), 2);
        QCOMPARE(m_ctrl->duration(), 6e6);
        // Importar o mesmo arquivo de novo não duplica.
        m_ctrl->importMedia({QUrl::fromLocalFile(m_video)});
        QCOMPARE(m_ctrl->assets().size(), 1);
    }

    void shortcutHelpers()
    {
        QCOMPARE(m_ctrl->keySequence(Qt::Key_B, Qt::ControlModifier), QStringLiteral("Ctrl+B"));
        QCOMPARE(m_ctrl->keySequence(Qt::Key_5, Qt::KeypadModifier), QStringLiteral("5")); // teclado numérico = mesma tecla
        QCOMPARE(m_ctrl->keySequence(Qt::Key_Delete, Qt::ShiftModifier), QStringLiteral("Shift+Del"));
        QVERIFY(m_ctrl->sameShortcut(QStringLiteral("Ctrl+Shift+Z"), QStringLiteral("Shift+Ctrl+Z")));
        QVERIFY(!m_ctrl->sameShortcut(QStringLiteral("Ctrl+Z"), QStringLiteral("Ctrl+Shift+Z")));
        QVERIFY(!m_ctrl->shortcutText(QStringLiteral("Ctrl+B")).isEmpty());
    }

    void splitUndoRedo()
    {
        m_ctrl->clearSelection();
        m_ctrl->seek(1.5e6);
        m_ctrl->splitAtPlayhead();
        QCOMPARE(clipsOf(0).size(), 3);
        m_ctrl->undo();
        QCOMPARE(clipsOf(0).size(), 2);
        m_ctrl->redo();
        QCOMPARE(clipsOf(0).size(), 3);
    }

    void propertyEditsCoalesce()
    {
        const QString id = clipsOf(0).first().toMap().value("id").toString();
        m_ctrl->select(id);
        const QString before = m_ctrl->undoText();
        for (int i = 1; i <= 10; ++i)
            m_ctrl->setClipProperties(id, {{"x", i * 0.01}, {"y", i * 0.02}});
        QCOMPARE(m_ctrl->selectedClip().value("x").toDouble(), 0.1);
        // Dez movimentos contínuos = um passo de desfazer.
        m_ctrl->undo();
        QCOMPARE(m_ctrl->selectedClip().value("x").toDouble(), 0.0);
        m_ctrl->redo();
        QCOMPARE(m_ctrl->selectedClip().value("y").toDouble(), 0.2);
        Q_UNUSED(before);
    }

    void keyframesViaInspector()
    {
        const QString id = clipsOf(0).first().toMap().value("id").toString();
        m_ctrl->select(id);
        m_ctrl->seek(0);
        m_ctrl->toggleKeyframe(id, "scale");
        QVERIFY(m_ctrl->selectedClip().value("kfAt_scale").toBool());
        m_ctrl->seek(1e6);
        m_ctrl->setClipProperty(id, "scale", 2.0); // com keyframes: cria keyframe em 1 s
        QVERIFY(m_ctrl->selectedClip().value("kfAt_scale").toBool());
        m_ctrl->seek(0.5e6);
        QCOMPARE(m_ctrl->selectedClip().value("scale").toDouble(), 1.5);
    }

    void keyframeEditor()
    {
        const QString id = clipsOf(0).first().toMap().value("id").toString();
        m_ctrl->select(id);
        // Já há keyframes de escala em 0 s (1,0) e 1 s (2,0) do teste anterior.
        QVariantList kfs = m_ctrl->keyframes(id, "scale");
        QCOMPARE(kfs.size(), 2);
        QCOMPARE(m_ctrl->sampleKeyframes(id, "scale", 3).size(), 3);
        // Curva personalizada no primeiro trecho.
        m_ctrl->setKeyframeBezier(id, "scale", 0, 0.9, 0, 1, 1); // começa muito devagar
        QCOMPARE(m_ctrl->keyframes(id, "scale").first().toMap().value("ease").toString(), QStringLiteral("bezier"));
        m_ctrl->seek(0.5e6);
        QVERIFY(m_ctrl->selectedClip().value("scale").toDouble() < 1.3);
        // Mover o segundo keyframe para 1,5 s e valor 3.
        m_ctrl->moveKeyframe(id, "scale", 1e6, 1.5e6, 3.0);
        kfs = m_ctrl->keyframes(id, "scale");
        QCOMPARE(kfs.last().toMap().value("t").toDouble(), 1.5e6);
        QCOMPARE(kfs.last().toMap().value("v").toDouble(), 3.0);
        // Todos suaves.
        m_ctrl->setEasingForAll(id, "scale", "easeInOut");
        for (const auto &k : m_ctrl->keyframes(id, "scale"))
            QCOMPARE(k.toMap().value("ease").toString(), QStringLiteral("easeInOut"));
        m_ctrl->setKeyframeEasing(id, "scale", 0, "hold");
        m_ctrl->seek(1.2e6);
        QCOMPARE(m_ctrl->selectedClip().value("scale").toDouble(), 1.0); // "manter" segura o valor
    }

    void textAndTransition()
    {
        m_ctrl->seek(0);
        m_ctrl->addText(QStringLiteral("Olá"), QStringLiteral("title"));
        QCOMPARE(m_ctrl->selectedClip().value("type").toString(), QStringLiteral("text"));
        QCOMPARE(m_ctrl->selectedClip().value("text").toString(), QStringLiteral("Olá"));
        const QString second = clipsOf(0).at(1).toMap().value("id").toString();
        m_ctrl->setTransition(second, QStringLiteral("dissolve"), 400000);
        QCOMPARE(clipsOf(0).at(1).toMap().value("transition").toString(), QStringLiteral("dissolve"));

        // No primeiro clipe, a transição vai para o corte com o seguinte.
        const QString first = clipsOf(0).at(0).toMap().value("id").toString();
        m_ctrl->select(first);
        QCOMPARE(m_ctrl->selectedClip().value("transitionClip").toString(), second);
        QCOMPARE(m_ctrl->selectedClip().value("transition").toString(), QStringLiteral("dissolve"));
        m_ctrl->setTransition(first, QStringLiteral("wipeLeft"), 400000);
        QCOMPARE(clipsOf(0).at(0).toMap().value("transition").toString(), QString());
        QCOMPARE(clipsOf(0).at(1).toMap().value("transition").toString(), QStringLiteral("wipeLeft"));
        m_ctrl->setTransition(second, QStringLiteral("dissolve"), 400000);
    }

    void freezeAndDeleteLeft()
    {
        m_ctrl->clearSelection();
        const int before = clipsOf(0).size();
        m_ctrl->seek(0.5e6);
        m_ctrl->freezeFrame();
        QCOMPARE(clipsOf(0).size(), before + 2); // divide + quadro congelado
        QCOMPARE(m_ctrl->duration(), 9e6);
        m_ctrl->undo();
        QCOMPARE(m_ctrl->duration(), 6e6);
        m_ctrl->deleteLeft();
        QCOMPARE(m_ctrl->duration(), 5.5e6);
    }

    void saveReopenExport()
    {
        const QString proj = m_dir.filePath(QStringLiteral("p.clivcut"));
        QVERIFY(m_ctrl->saveProjectAs(QUrl::fromLocalFile(proj)));
        QVERIFY(!m_ctrl->modified());
        QVERIFY(m_ctrl->openProject(QUrl::fromLocalFile(proj)));
        QCOMPARE(m_ctrl->duration(), 5.5e6);

        QSignalSpy done(m_ctrl.get(), &ProjectController::exportFinished);
        const QString out = m_dir.filePath(QStringLiteral("out.mp4"));
        m_ctrl->exportVideo(QUrl::fromLocalFile(out), 360, 0, QStringLiteral("h264"), 23);
        QVERIFY(done.wait(60000));
        QVERIFY2(done.first().at(0).toBool(), qPrintable(done.first().at(1).toString()));
        clv::MediaAsset a;
        QVERIFY(clv::media::probe(out, a));
        QCOMPARE(a.width, 640);
        QCOMPARE(a.height, 360);
        QVERIFY(std::abs(a.duration - 5'500'000) < 100'000);
        QVERIFY(a.hasAudio);

        // Entrada/saída: marcar, desfazer, salvar e exportar só o trecho.
        m_ctrl->seek(1e6);
        m_ctrl->markIn();
        m_ctrl->seek(3e6);
        m_ctrl->markOut();
        QCOMPARE(m_ctrl->inPoint(), 1e6);
        QCOMPARE(m_ctrl->outPoint(), 3e6);
        m_ctrl->seek(0.5e6);
        m_ctrl->markOut(); // saída antes da entrada descarta a entrada
        QCOMPARE(m_ctrl->inPoint(), -1.0);
        m_ctrl->undo();
        QCOMPARE(m_ctrl->inPoint(), 1e6);
        QCOMPARE(m_ctrl->outPoint(), 3e6);
        QVERIFY(m_ctrl->saveProjectAs(QUrl::fromLocalFile(proj)));
        QVERIFY(m_ctrl->openProject(QUrl::fromLocalFile(proj)));
        QCOMPARE(m_ctrl->outPoint(), 3e6);
        done.clear();
        const QString part = m_dir.filePath(QStringLiteral("part.mp4"));
        m_ctrl->exportVideo(QUrl::fromLocalFile(part), 360, 0, QStringLiteral("h264"), 23, true);
        QVERIFY(done.wait(60000));
        QVERIFY2(done.first().at(0).toBool(), qPrintable(done.first().at(1).toString()));
        QVERIFY(clv::media::probe(part, a));
        QVERIFY(std::abs(a.duration - 2'000'000) < 100'000);
        m_ctrl->clearInOut();
        QCOMPARE(m_ctrl->outPoint(), -1.0);

        // Só áudio, quadro atual e GPU (cai para software se não houver encoder de hardware).
        // Áudio e quadro vão para pastas que ainda não existem (ex.: ~/Videos ausente).
        done.clear();
        const QString wav = m_dir.filePath(QStringLiteral("missing/sub dir/mix.wav"));
        m_ctrl->exportAudio(QUrl::fromLocalFile(wav));
        QVERIFY(done.wait(60000));
        QVERIFY2(done.first().at(0).toBool(), qPrintable(done.first().at(1).toString()));
        clv::MediaAsset audioOnly;
        QVERIFY(clv::media::probe(wav, audioOnly));
        QVERIFY(audioOnly.hasAudio && !audioOnly.hasVideo);
        QVERIFY(std::abs(audioOnly.duration - 5'500'000) < 30'000);

        done.clear();
        m_ctrl->seek(1e6);
        const QString png = m_dir.filePath(QStringLiteral("missing frames/frame"));
        m_ctrl->exportFrame(QUrl::fromLocalFile(png));
        QVERIFY(done.wait(60000));
        QVERIFY2(done.first().at(0).toBool(), qPrintable(done.first().at(1).toString()));
        const QImage frame(png + QStringLiteral(".png"));
        QCOMPARE(frame.size(), QSize(m_ctrl->canvasWidth(), m_ctrl->canvasHeight()));

        done.clear();
        const QString gpu = m_dir.filePath(QStringLiteral("gpu.mp4"));
        m_ctrl->exportVideo(QUrl::fromLocalFile(gpu), 360, 0, QStringLiteral("h264"), 23, false, true);
        QVERIFY(done.wait(60000));
        QVERIFY2(done.first().at(0).toBool(), qPrintable(done.first().at(1).toString()));
        QVERIFY(clv::media::probe(gpu, a));
        QCOMPARE(a.height, 360);
        qInfo() << "GPU codecs:" << m_ctrl->hardwareCodecs() << "status:" << m_ctrl->statusMessage();
    }

    void filtersWorkflow()
    {
        m_ctrl->newProject(640, 360, 30);
        m_ctrl->importMedia({QUrl::fromLocalFile(m_video)});
        m_ctrl->addAssetToTimeline(m_ctrl->assets().first().toMap().value("id").toString());
        const QString clip = clipsOf(0).first().toMap().value("id").toString();
        QVERIFY(m_ctrl->filters().size() >= 12);
        QVERIFY(m_ctrl->previewFrameRef().contains(QLatin1Char('/')));

        // Filtro no clipe.
        m_ctrl->applyFilter(clip, QStringLiteral("tealorange"));
        m_ctrl->select(clip);
        QCOMPARE(m_ctrl->selectedClip().value("filter").toString(), QStringLiteral("tealorange"));
        QCOMPARE(m_ctrl->selectedClip().value("filterName").toString(), QStringLiteral("Teal & Orange"));
        m_ctrl->applyFilter(clip, QStringLiteral("inexistente")); // ignorado
        QCOMPARE(m_ctrl->selectedClip().value("filter").toString(), QStringLiteral("tealorange"));
        m_ctrl->setClipProperties(clip, {{"exposure", 0.3}, {"vignette", 0.5}});
        QCOMPARE(m_ctrl->selectedClip().value("vignette").toDouble(), 0.5);
        m_ctrl->resetClipProperties(clip, "color"); // mantém o filtro
        QCOMPARE(m_ctrl->selectedClip().value("exposure").toDouble(), 0.0);
        QCOMPARE(m_ctrl->selectedClip().value("filter").toString(), QStringLiteral("tealorange"));

        // Máscara via propriedades.
        m_ctrl->setClipProperties(clip, {{"maskShape", "circle"}, {"maskWidth", 0.3}, {"maskInvert", true}});
        QCOMPARE(m_ctrl->selectedClip().value("maskShape").toString(), QStringLiteral("circle"));
        QVERIFY(m_ctrl->selectedClip().value("maskInvert").toBool());
        m_ctrl->resetClipProperties(clip, "mask");
        QCOMPARE(m_ctrl->selectedClip().value("maskShape").toString(), QStringLiteral("none"));

        // Clipe de filtro (camada de ajuste) no cursor.
        m_ctrl->seek(1e6);
        m_ctrl->addFilterClip(QStringLiteral("bw"));
        QCOMPARE(m_ctrl->selectedClip().value("type").toString(), QStringLiteral("adjust"));
        QCOMPARE(m_ctrl->selectedClip().value("start").toDouble(), 1e6);
        QCOMPARE(m_ctrl->selectedClip().value("duration").toDouble(), 2e6); // até o fim do clipe principal
        bool found = false;
        for (const auto &t : m_ctrl->tracks())
            found = found || t.toMap().value("kind") == QStringLiteral("adjust");
        QVERIFY(found);

        // LUT inválido é recusado com erro; válido é aceito.
        QSignalSpy errors(m_ctrl.get(), &ProjectController::errorOccurred);
        const QString bad = m_dir.filePath(QStringLiteral("bad.cube"));
        QFile bf(bad);
        QVERIFY(bf.open(QIODevice::WriteOnly));
        bf.write("LUT_3D_SIZE 2\n0 0 0\n");
        bf.close();
        QVERIFY(!m_ctrl->loadLut(clip, QUrl::fromLocalFile(bad)));
        QCOMPARE(errors.size(), 1);
        const QString good = m_dir.filePath(QStringLiteral("good.cube"));
        QFile gf(good);
        QVERIFY(gf.open(QIODevice::WriteOnly));
        gf.write("LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
        gf.close();
        QVERIFY(m_ctrl->loadLut(clip, QUrl::fromLocalFile(good)));
        m_ctrl->select(clip);
        QCOMPARE(m_ctrl->selectedClip().value("lutName").toString(), QStringLiteral("good.cube"));

        // Exporta com filtro + ajuste + LUT.
        QSignalSpy done(m_ctrl.get(), &ProjectController::exportFinished);
        m_ctrl->exportVideo(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("filtros.mp4"))), 360, 0,
                            QStringLiteral("h264"), 28);
        QVERIFY(done.wait(60000));
        QVERIFY2(done.first().at(0).toBool(), qPrintable(done.first().at(1).toString()));
    }

    void clipboardAndGroups()
    {
        m_ctrl->newProject(640, 360, 30);
        m_ctrl->importMedia({QUrl::fromLocalFile(m_video)});
        const QString asset = m_ctrl->assets().first().toMap().value("id").toString();
        m_ctrl->addAssetToTimeline(asset);
        m_ctrl->seek(0.5e6);
        m_ctrl->addText(QStringLiteral("A"), "");
        const QString text = m_ctrl->selectedClip().value("id").toString();
        const QString video = clipsOf(0).first().toMap().value("id").toString();

        // Seleção por intervalo nas faixas 0 e 1 pega os dois.
        int textTrack = -1;
        for (const auto &t : m_ctrl->tracks())
            if (t.toMap().value("kind") == QStringLiteral("text"))
                textTrack = t.toMap().value("index").toInt();
        m_ctrl->selectInRange(0, 1e6, {0, textTrack}, false);
        QCOMPARE(m_ctrl->selection().size(), 2);

        // Agrupar: clicar em um seleciona os dois.
        m_ctrl->groupSelection();
        m_ctrl->select(text);
        QCOMPARE(m_ctrl->selection().size(), 2);

        // Copiar e colar no fim: os dois vêm juntos, com grupo novo.
        m_ctrl->copySelection();
        QVERIFY(m_ctrl->canPaste());
        m_ctrl->seek(3e6);
        m_ctrl->paste();
        QCOMPARE(m_ctrl->selection().size(), 2);
        QCOMPARE(clipsOf(0).size(), 2);
        QCOMPARE(m_ctrl->duration(), 6.5e6); // texto colado mantém o deslocamento de 0,5 s
        const auto pasted = m_ctrl->selection();
        QVERIFY(!pasted.contains(text) && !pasted.contains(video));

        // Mover o grupo colado pelo texto: o vídeo da faixa principal vai junto.
        const QString pastedText = clipsOf(textTrack).last().toMap().value("id").toString();
        m_ctrl->select(pastedText);
        QCOMPARE(m_ctrl->selection().size(), 2);
        m_ctrl->ungroupSelection();
        m_ctrl->select(pastedText);
        QCOMPARE(m_ctrl->selection().size(), 1);

        // Recortar remove; colar devolve.
        m_ctrl->cutSelection();
        QCOMPARE(clipsOf(textTrack).size(), 1);
        m_ctrl->seek(4e6); // ponto livre na faixa de texto (sobreposição iria para outra faixa)
        m_ctrl->paste();
        QCOMPARE(clipsOf(textTrack).size(), 2);

        // Atributos: estilo de um texto para o outro.
        m_ctrl->select(text);
        m_ctrl->setClipProperty(text, "color", QStringLiteral("#00ff00"));
        m_ctrl->copyAttributes();
        QVERIFY(m_ctrl->canPasteAttributes());
        const QString other = clipsOf(textTrack).last().toMap().value("id").toString();
        m_ctrl->select(other);
        m_ctrl->pasteAttributesToSelection();
        QCOMPARE(m_ctrl->selectedClip().value("color").toString(), QStringLiteral("#00ff00"));
    }

    void autosaveAndRecovery()
    {
        m_ctrl.reset(); // uma "sessão" de cada vez
        QString recovered;
        {
            ProjectController a(nullptr);
            a.importMedia({QUrl::fromLocalFile(m_video)});
            a.addAssetToTimeline(a.assets().first().toMap().value("id").toString());
            a.addText(QStringLiteral("não salvo"), "");
            QVERIFY(a.modified());
            a.autosaveNow();
            QVERIFY(a.recoverableProjects().isEmpty()); // a própria sessão não aparece
            // Outra janela aberta ao mesmo tempo não vê a cópia em uso.
            ProjectController b(nullptr);
            QVERIFY(b.recoverableProjects().isEmpty());
        } // fim "inesperado": havia alterações, a cópia fica no disco

        auto c = std::make_unique<ProjectController>(nullptr);
        const QVariantList list = c->recoverableProjects();
        QCOMPARE(list.size(), 1);
        QVERIFY(list.first().toMap().value("originalPath").toString().isEmpty()); // nunca salvo
        QVERIFY(c->recoverProject(list.first().toMap().value("file").toString()));
        QVERIFY(c->modified());
        QCOMPARE(c->tracks().size(), 2); // principal + texto
        QVERIFY(c->recoverableProjects().isEmpty());

        // Salvar apaga a cópia de recuperação.
        QVERIFY(c->saveProjectAs(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("recuperado.clivcut")))));
        QVERIFY(QDir(ProjectController::recoveryDir()).entryList({QStringLiteral("*.clivcut")}, QDir::Files).isEmpty());

        // Descartar alterações também.
        c->addText(QStringLiteral("x"), "");
        c->autosaveNow();
        QCOMPARE(QDir(ProjectController::recoveryDir()).entryList({QStringLiteral("*.clivcut")}, QDir::Files).size(), 1);
        c->discardChanges();
        QVERIFY(!c->modified());
        QVERIFY(QDir(ProjectController::recoveryDir()).entryList({QStringLiteral("*.clivcut")}, QDir::Files).isEmpty());
        m_ctrl = std::move(c);
    }

    void detachAudioWorkflow()
    {
        m_ctrl->newProject(640, 360, 30);
        m_ctrl->importMedia({QUrl::fromLocalFile(m_video)});
        m_ctrl->addAssetToTimeline(m_ctrl->assets().first().toMap().value("id").toString());
        const QString video = clipsOf(0).first().toMap().value("id").toString();
        m_ctrl->select(video);
        m_ctrl->detachAudio();
        QVERIFY(clipsOf(0).first().toMap().value("audioMuted").toBool());
        int audioTrack = -1;
        for (const auto &t : m_ctrl->tracks())
            if (t.toMap().value("kind") == QStringLiteral("audio"))
                audioTrack = t.toMap().value("index").toInt();
        QVERIFY(audioTrack > 0);
        QVERIFY(clipsOf(audioTrack).first().toMap().value("audioOnly").toBool());
        // Mixagem: mesmo áudio de antes (o do clipe separado), vídeo mudo.
        clv::engine::AudioMixer mixer;
        std::vector<float> buf(4800 * 2);
        mixer.mix(*m_ctrl->snapshot(), 1'000'000, buf.data(), 4800);
        float peak = 0;
        for (float x : buf)
            peak = std::max(peak, std::abs(x));
        QVERIFY(peak > 0.05f);
        // Desfazer volta ao estado original.
        m_ctrl->undo();
        QVERIFY(!clipsOf(0).first().toMap().value("audioMuted").toBool());
        QCOMPARE(m_ctrl->tracks().size(), 1);
    }

    void loudnessAndDenoise()
    {
        m_ctrl->newProject(640, 360, 30);
        m_ctrl->importMedia({QUrl::fromLocalFile(m_video)});
        m_ctrl->addAssetToTimeline(m_ctrl->assets().first().toMap().value("id").toString());
        const QString id = clipsOf(0).first().toMap().value("id").toString();
        m_ctrl->select(id);
        m_ctrl->normalizeLoudness(-14);
        QVERIFY(m_ctrl->normalizing());
        QTRY_VERIFY_WITH_TIMEOUT(!m_ctrl->normalizing(), 30000);
        QVERIFY(m_ctrl->selectedClip().value("gainDb").toDouble() > 0);
        // A mixagem final fica no alvo.
        clv::engine::AudioMixer mixer;
        clv::media::LoudnessMeter meter;
        std::vector<float> buf(4800 * 2);
        for (clv::Micros t = 0; t < 3'000'000; t += 100'000) {
            mixer.mix(*m_ctrl->snapshot(), t, buf.data(), 4800);
            meter.add(buf.data(), 4800);
        }
        QVERIFY2(std::abs(meter.integrated() + 14) < 0.7, qPrintable(QString::number(meter.integrated())));

        // Redução de ruído: processa em segundo plano até ficar pronta.
        m_ctrl->setClipProperty(id, "denoise", 0.6);
        QCOMPARE(m_ctrl->selectedClip().value("denoise").toDouble(), 0.6);
        QTRY_COMPARE_WITH_TIMEOUT(m_ctrl->selectedClip().value("denoiseState").toString(), QStringLiteral("ready"), 30000);
    }

    void proxiesAutomatic()
    {
        const QString big = m_dir.filePath(QStringLiteral("grande.mp4"));
        QProcess p;
        p.start(QStringLiteral("ffmpeg"), {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                                           "testsrc2=size=2560x1440:rate=30:duration=2", "-c:v", "libx264", "-preset",
                                           "ultrafast", "-pix_fmt", "yuv420p", big});
        QVERIFY(p.waitForFinished(120000) && p.exitCode() == 0);
        m_ctrl->newProject(1920, 1080, 30);
        m_ctrl->importMedia({QUrl::fromLocalFile(big), QUrl::fromLocalFile(m_video)});
        // Só o vídeo > 1080p entra na fila automaticamente.
        QCOMPARE(m_ctrl->proxiesPending(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(m_ctrl->proxiesPending(), 0, 60000);
        QString bigState, smallState;
        for (const auto &a : m_ctrl->assets()) {
            const auto m = a.toMap();
            (m.value("path") == big ? bigState : smallState) = m.value("proxyState").toString();
        }
        QCOMPARE(bigState, QStringLiteral("ready"));
        QCOMPARE(smallState, QStringLiteral("none"));
        // Pedido manual para o pequeno.
        for (const auto &a : m_ctrl->assets())
            if (a.toMap().value("path") == m_video)
                m_ctrl->generateProxy(a.toMap().value("id").toString());
        QTRY_COMPARE_WITH_TIMEOUT(m_ctrl->proxiesPending(), 0, 60000);
        // Reabrir o projeto reencontra os proxies no cache sem gerar de novo.
        const QString proj = m_dir.filePath(QStringLiteral("proxy.clivcut"));
        QVERIFY(m_ctrl->saveProjectAs(QUrl::fromLocalFile(proj)));
        QVERIFY(m_ctrl->openProject(QUrl::fromLocalFile(proj)));
        QCOMPARE(m_ctrl->proxiesPending(), 0);
        for (const auto &a : m_ctrl->assets())
            QCOMPARE(a.toMap().value("proxyState").toString(), QStringLiteral("ready"));
    }

    void renderCacheWorkflow()
    {
        QVERIFY(m_ctrl->openProject(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("p.clivcut")))));
        const double duration = m_ctrl->duration();
        QVERIFY(duration > 0);
        m_ctrl->setPreviewResolution(320);
        m_ctrl->clearRenderCache();
        QVERIFY(m_ctrl->cachedRanges().isEmpty());
        m_ctrl->clearInOut();
        m_ctrl->renderPreview();
        QVERIFY(m_ctrl->renderingPreview());
        QTRY_VERIFY_WITH_TIMEOUT(!m_ctrl->renderingPreview(), 120000);
        // A timeline inteira vira uma faixa renderizada.
        const QVariantList ranges = m_ctrl->cachedRanges();
        QCOMPARE(ranges.size(), 1);
        QCOMPARE(ranges.first().toMap().value("start").toDouble(), 0.0);
        QCOMPARE(ranges.first().toMap().value("end").toDouble(), duration);
        QVERIFY(!m_ctrl->renderCacheView()->fileAt(0).isEmpty());

        // Editar um clipe invalida os trechos que ele toca (após o agrupamento das edições).
        const QString clip = clipsOf(0).first().toMap().value("id").toString();
        m_ctrl->setClipProperty(clip, QStringLiteral("opacity"), 0.5);
        QTRY_VERIFY_WITH_TIMEOUT(m_ctrl->renderCacheView()->fileAt(0).isEmpty(), 5000);
        m_ctrl->undo();
        QTRY_VERIFY_WITH_TIMEOUT(!m_ctrl->renderCacheView()->fileAt(0).isEmpty(), 5000); // mesmo conteúdo, mesmo arquivo
        m_ctrl->clearRenderCache();
        QVERIFY(m_ctrl->cachedRanges().isEmpty());
    }

    void voiceoverRecording()
    {
        QVERIFY(m_ctrl->openProject(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("p.clivcut")))));
        m_ctrl->seek(1e6);
        const int clipsBefore = [&] {
            int n = 0;
            for (const auto &t : m_ctrl->tracks())
                n += t.toMap().value("clips").toList().size();
            return n;
        }();
        if (!m_ctrl->startRecording())
            QSKIP("sem dispositivo de captura neste ambiente");
        QVERIFY(m_ctrl->recording());
        QTest::qWait(800);
        QVERIFY(m_ctrl->recordElapsed() > 0.3e6);
        m_ctrl->stopRecording();
        QVERIFY(!m_ctrl->recording());
        int clipsAfter = 0;
        for (const auto &t : m_ctrl->tracks())
            clipsAfter += t.toMap().value("clips").toList().size();
        QCOMPARE(clipsAfter, clipsBefore + 1);
        // O clipe novo começa onde a gravação começou.
        bool atStart = false;
        for (const auto &t : m_ctrl->tracks())
            for (const auto &c : t.toMap().value("clips").toList())
                if (std::abs(c.toMap().value("start").toDouble() - 1e6) < 1000 && t.toMap().value("kind") == QStringLiteral("audio"))
                    atStart = true;
        QVERIFY(atStart);
    }

    void videoEffectsApi()
    {
        QVERIFY(m_ctrl->openProject(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("p.clivcut")))));
        QCOMPARE(m_ctrl->videoEffects().size(), 12);
        const QString clip = clipsOf(0).first().toMap().value("id").toString();
        m_ctrl->setClipProperty(clip, QStringLiteral("effect"), QStringLiteral("glitch"));
        m_ctrl->setClipProperty(clip, QStringLiteral("effectIntensity"), 2.0); // limitado a 1
        m_ctrl->select(clip);
        QCOMPARE(m_ctrl->selectedClip().value("effect").toString(), QStringLiteral("glitch"));
        QCOMPARE(m_ctrl->selectedClip().value("effectIntensity").toDouble(), 1.0);
        m_ctrl->setClipProperty(clip, QStringLiteral("effect"), QStringLiteral("inexistente"));
        QCOMPARE(m_ctrl->selectedClip().value("effect").toString(), QString());
        // Sem seleção: vira uma camada de efeito selecionada.
        m_ctrl->clearSelection();
        m_ctrl->seek(0);
        m_ctrl->addEffectClip(QStringLiteral("snow"));
        QCOMPARE(m_ctrl->selectedClip().value("type").toString(), QStringLiteral("adjust"));
        QCOMPARE(m_ctrl->selectedClip().value("effect").toString(), QStringLiteral("snow"));
        // Desfazer remove a camada inteira (efeito e clipe juntos).
        m_ctrl->undo();
        m_ctrl->undo();
        for (const auto &t : m_ctrl->tracks())
            QVERIFY(t.toMap().value("kind").toString() != QStringLiteral("adjust"));
    }

    void scopes()
    {
        // Meio quadro vermelho puro, meio cinza médio.
        QImage img(64, 32, QImage::Format_RGBA8888);
        img.fill(QColor(128, 128, 128));
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
                img.setPixelColor(x, y, Qt::red);
        const auto h = clv::app::histogram(img);
        QCOMPARE(h[0][255], 32 * 32);
        QCOMPARE(h[0][128], 32 * 32);
        QCOMPARE(h[1][0], 32 * 32);
        QCOMPARE(h[3][128], 32 * 32); // cinza: luminância = valor
        QCOMPARE(h[3][54], 32 * 32);  // vermelho: 0,2126 × 255
        for (int m = 0; m < 3; ++m) {
            const QImage plot = clv::app::renderScope(img, static_cast<clv::app::ScopeMode>(m), QSize(200, 150));
            QCOMPARE(plot.size(), QSize(200, 150));
        }
        // Forma de onda: o vermelho (luma 54) acende embaixo à esquerda, o cinza no meio à direita.
        const QImage wf = clv::app::renderScope(img, clv::app::ScopeMode::Waveform, QSize(64, 256));
        QVERIFY(qAlpha(wf.pixel(10, 255 - 54)) > 200);
        QVERIFY(qAlpha(wf.pixel(50, 255 - 128)) > 200);
        QCOMPARE(qAlpha(wf.pixel(10, 255 - 128)), 0);
    }

    void colorGradingApi()
    {
        QVERIFY(m_ctrl->openProject(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("p.clivcut")))));
        const QString clip = clipsOf(0).first().toMap().value("id").toString();
        m_ctrl->select(clip);
        QVERIFY(!m_ctrl->selectedClip().value("hasGrade").toBool());
        m_ctrl->setCurve(clip, 0, {QVariantMap{{"x", 0.5}, {"y", 0.7}}});
        m_ctrl->setHsl(clip, 5, 0.2, -0.5, 0);
        m_ctrl->setColorWheel(clip, QStringLiteral("gain"), 3, 4, 0.5); // fora do disco: volta à borda
        const QVariantMap c = m_ctrl->selectedClip();
        QVERIFY(c.value("hasGrade").toBool());
        QCOMPARE(c.value("curves").toList().at(0).toList().size(), 1);
        QCOMPARE(c.value("hsl").toList().at(5).toMap().value("saturation").toDouble(), -0.5);
        const QVariantMap gain = c.value("wheels").toMap().value("gain").toMap();
        QVERIFY(std::abs(gain.value("x").toDouble() - 0.6) < 1e-9 && std::abs(gain.value("y").toDouble() - 0.8) < 1e-9);
        // Curva identidade conta como "sem curva".
        m_ctrl->setCurve(clip, 0, {QVariantMap{{"x", 0.0}, {"y", 0.0}}, QVariantMap{{"x", 1.0}, {"y", 1.0}}});
        QCOMPARE(m_ctrl->selectedClip().value("curves").toList().at(0).toList().size(), 0);
        m_ctrl->resetGrade(clip);
        QVERIFY(!m_ctrl->selectedClip().value("hasGrade").toBool());
        m_ctrl->undo();
        QVERIFY(m_ctrl->selectedClip().value("hasGrade").toBool());
    }

    void stickers()
    {
        QVERIFY(m_ctrl->openProject(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("p.clivcut")))));
        const QVariantList list = m_ctrl->stickers();
        QVERIFY(list.size() > 10);
        const QString id = list.first().toMap().value("id").toString();
        const auto total = [this] {
            qsizetype n = 0;
            for (const auto &t : m_ctrl->tracks())
                n += t.toMap().value("clips").toList().size();
            return n;
        };
        const qsizetype before = total();
        QVERIFY(clipsOf(0).size() > 0);
        const qsizetype mainClips = clipsOf(0).size();
        m_ctrl->addSticker(id);
        // Entra como imagem PNG transparente numa faixa acima da principal.
        QVERIFY(!m_ctrl->selectedClip().isEmpty());
        QCOMPARE(total(), before + 1);
        QCOMPARE(clipsOf(0).size(), mainClips); // nunca na faixa principal
        const QImage png(QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                             .filePath(QStringLiteral("stickers/%1.png").arg(id)));
        QCOMPARE(png.size(), QSize(512, 512));
        QCOMPARE(qAlpha(png.pixel(2, 2)), 0);
        QVERIFY(qAlpha(png.pixel(256, 256)) > 0);
        m_ctrl->undo();
        QCOMPARE(total(), before);
    }

    void silenceRemovalWorkflow()
    {
        // 6 s: tom de 0–2 s, silêncio de 2–4 s, tom de 4–6 s.
        const QString file = m_dir.filePath(QStringLiteral("pausas.wav"));
        QProcess p;
        p.start(QStringLiteral("ffmpeg"), {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                                           "aevalsrc=0.3*sin(2*PI*220*t)*(1-between(t\\,2\\,4)):d=6:s=48000", "-ac", "2", file});
        QVERIFY(p.waitForFinished(60000) && p.exitCode() == 0);
        m_ctrl->newProject(1280, 720, 30);
        m_ctrl->importMedia({QUrl::fromLocalFile(file)});
        m_ctrl->addAssetToTimeline(m_ctrl->assets().first().toMap().value("id").toString());
        QVERIFY(m_ctrl->duration() > 5e6);
        const double before = m_ctrl->duration();

        m_ctrl->analyzeSilences(-40, 500, 100);
        QTRY_VERIFY_WITH_TIMEOUT(!m_ctrl->analyzingSilences(), 30000);
        QCOMPARE(m_ctrl->silences().size(), 1);
        const QVariantMap r = m_ctrl->silences().first().toMap();
        QVERIFY(std::abs(r.value("start").toDouble() - 2.1e6) < 30'000);
        QVERIFY(std::abs(r.value("end").toDouble() - 3.9e6) < 30'000);
        m_ctrl->removeSilences();
        QVERIFY(m_ctrl->silences().isEmpty());
        QVERIFY(std::abs(m_ctrl->duration() - (before - 1.8e6)) < 50'000);
        m_ctrl->undo();
        QCOMPARE(m_ctrl->duration(), before);
    }

    void compoundClipWorkflow()
    {
        m_ctrl->newProject(640, 360, 30);
        m_ctrl->importMedia({QUrl::fromLocalFile(m_video)});
        m_ctrl->addAssetToTimeline(m_ctrl->assets().first().toMap().value("id").toString());
        m_ctrl->seek(500'000);
        m_ctrl->addText(QStringLiteral("Hi"));
        auto total = [this] {
            int n = 0;
            for (const auto &t : m_ctrl->tracks())
                n += t.toMap().value("clips").toList().size();
            return n;
        };
        QCOMPARE(total(), 2);
        m_ctrl->selectAll();
        m_ctrl->createCompound();
        QCOMPARE(total(), 1);
        QVariantMap sel = m_ctrl->selectedClip();
        QCOMPARE(sel.value("mediaKind").toString(), QStringLiteral("compound"));
        QVERIFY(sel.value("hasAudio").toBool()); // o vídeo de teste tem som
        QCOMPARE(clipsOf(0).first().toMap().value("label").toString(), QStringLiteral("Compound clip 1"));
        m_ctrl->undo();
        QCOMPARE(total(), 2);
        m_ctrl->redo();
        QCOMPARE(total(), 1);

        // Copiar o composto e colar num projeto novo leva a sequência e a mídia junto.
        m_ctrl->select(clipsOf(0).first().toMap().value("id").toString());
        m_ctrl->copySelection();
        m_ctrl->newProject(640, 360, 30);
        m_ctrl->paste();
        QCOMPARE(total(), 1);
        const QString pasted = clipsOf(0).first().toMap().value("id").toString();
        QCOMPARE(clipsOf(0).first().toMap().value("mediaKind").toString(), QStringLiteral("compound"));
        QCOMPARE(m_ctrl->assets().size(), 1);

        // Desfazer o composto devolve o vídeo e o texto.
        m_ctrl->select(pasted);
        m_ctrl->breakCompound();
        QCOMPARE(total(), 2);
        QCOMPARE(m_ctrl->selection().size(), 2);
    }

    void editCompoundContents()
    {
        m_ctrl->newProject(640, 360, 30);
        m_ctrl->importMedia({QUrl::fromLocalFile(m_video)});
        m_ctrl->addAssetToTimeline(m_ctrl->assets().first().toMap().value("id").toString());
        m_ctrl->seek(500'000);
        m_ctrl->addText(QStringLiteral("Hi"));
        m_ctrl->selectAll();
        m_ctrl->createCompound();
        const QString cmp = m_ctrl->selectedClip().value("id").toString();
        const QString file = m_dir.filePath(QStringLiteral("compound.clivcut"));
        QVERIFY(m_ctrl->saveProjectAs(QUrl::fromLocalFile(file)));
        QVERIFY(!m_ctrl->modified());
        auto texts = [this] {
            int n = 0;
            for (const auto &t : m_ctrl->tracks())
                for (const auto &c : t.toMap().value("clips").toList())
                    n += c.toMap().value("type").toString() == QLatin1String("text");
            return n;
        };

        // Dentro: a timeline mostra o vídeo e o texto; o cursor vira tempo do conteúdo.
        m_ctrl->seek(1'000'000);
        m_ctrl->openCompound(cmp);
        QCOMPARE(m_ctrl->compoundPath().size(), 1);
        QCOMPARE(texts(), 1);
        QCOMPARE(m_ctrl->playhead(), 1'000'000.0);
        QVERIFY(!m_ctrl->modified());
        QVERIFY(!m_ctrl->canUndo()); // histórico próprio
        for (const auto &t : m_ctrl->tracks())
            for (const auto &c : t.toMap().value("clips").toList())
                if (c.toMap().value("type").toString() == QLatin1String("text"))
                    m_ctrl->select(c.toMap().value("id").toString());
        m_ctrl->deleteSelection(false);
        QCOMPARE(texts(), 0);
        QVERIFY(m_ctrl->modified());
        m_ctrl->undo();
        QCOMPARE(texts(), 1);
        m_ctrl->redo();

        // Salvar de dentro grava o projeto inteiro, já com a mudança.
        QVERIFY(m_ctrl->saveProject());
        clv::Project saved;
        QVERIFY(clv::loadProjectFile(file, saved));
        QCOMPARE(saved.tracks.size(), size_t(1));
        QCOMPARE(saved.compounds.size(), size_t(1));
        int savedTexts = 0;
        saved.forEachClip([&](const clv::Clip &c) { savedTexts += c.isText(); });
        QCOMPARE(savedTexts, 0);

        // Voltar: um passo de desfazer lá fora, e o projeto continua salvo.
        m_ctrl->closeCompound();
        QVERIFY(m_ctrl->compoundPath().isEmpty());
        QCOMPARE(m_ctrl->selectedClip().value("id").toString(), cmp);
        QCOMPARE(m_ctrl->playhead(), 1'000'000.0);
        QVERIFY(!m_ctrl->modified());
        QCOMPARE(m_ctrl->undoText(), QStringLiteral("Edit compound clip"));
        m_ctrl->undo();
        m_ctrl->openCompound(cmp);
        QCOMPARE(texts(), 1); // o texto voltou
        m_ctrl->closeCompound();
        QCOMPARE(m_ctrl->undoText(), QStringLiteral("Create compound clip")); // fechar sem mudar não cria passo
    }

    void stabilizationJob()
    {
        m_ctrl->newProject(640, 360, 30);
        m_ctrl->importMedia({QUrl::fromLocalFile(m_video)});
        m_ctrl->addAssetToTimeline(m_ctrl->assets().first().toMap().value("id").toString());
        const QString clip = clipsOf(0).first().toMap().value("id").toString();
        m_ctrl->select(clip);
        QCOMPARE(m_ctrl->selectedClip().value("stabilizeState").toString(), QStringLiteral("off"));
        m_ctrl->setClipProperty(clip, QStringLiteral("stabilize"), 0.62);
        QCOMPARE(m_ctrl->selectedClip().value("stabilize").toDouble(), 0.6); // passos de 5%
        QTRY_COMPARE_WITH_TIMEOUT(m_ctrl->selectedClip().value("stabilizeState").toString(), QStringLiteral("ready"), 60000);
        QVERIFY(clv::registeredMotion(m_video));
        // O quadro avaliado já traz a correção (o zoom pelo menos 1).
        const auto f = clv::evaluate(*m_ctrl->snapshot(), 1'000'000);
        QVERIFY(!f.layers.empty());
        QVERIFY(f.layers.front().stab.zoom >= 1.0);
        m_ctrl->undo();
        QCOMPARE(m_ctrl->selectedClip().value("stabilize").toDouble(), 0.0);
    }

    void separateMusic()
    {
#ifndef CLIVCUT_WHISPER_SAMPLES
        QSKIP("sem amostras de fala");
#else
        if (!clv::engine::aiModelInstalled(clv::engine::kStemsModel))
            QSKIP("modelo HT-Demucs não instalado");
        const QString song = m_dir.filePath(QStringLiteral("musica.wav"));
        QProcess p;
        p.start(QStringLiteral("ffmpeg"), {"-hide_banner", "-loglevel", "error", "-y", "-i",
                                           QStringLiteral(CLIVCUT_WHISPER_SAMPLES "/jfk.wav"), "-f", "lavfi", "-i",
                                           "aevalsrc='0.5*sin(2*PI*55*t)*exp(-12*mod(t\\,0.5))':s=16000:d=4", "-filter_complex",
                                           "[0]atrim=0:4[v];[v][1]amix=inputs=2:normalize=0:duration=shortest", "-ac", "2", "-ar", "48000", song});
        QVERIFY(p.waitForFinished(60000) && p.exitCode() == 0);
        m_ctrl->newProject(640, 360, 30);
        m_ctrl->importMedia({QUrl::fromLocalFile(song)});
        m_ctrl->addAssetToTimeline(m_ctrl->assets().first().toMap().value("id").toString());
        QString clip;
        for (const auto &t : m_ctrl->tracks())
            for (const auto &c : t.toMap().value("clips").toList())
                clip = c.toMap().value("id").toString();
        m_ctrl->select(clip);
        auto clips = [this] {
            int n = 0;
            for (const auto &t : m_ctrl->tracks())
                n += t.toMap().value("clips").toList().size();
            return n;
        };
        m_ctrl->separateStems(0);
        QCOMPARE(m_ctrl->selectedClip().value("stemsState").toString(), QStringLiteral("processing"));
        QTRY_COMPARE_WITH_TIMEOUT(clips(), 5, 120000);
        QCOMPARE(m_ctrl->selection().size(), 4);
        QCOMPARE(m_ctrl->assets().size(), 5);
        // O original fica mudo, no mesmo lugar.
        m_ctrl->select(clip);
        QVERIFY(m_ctrl->selectedClip().value("audioMuted").toBool());
        m_ctrl->undo();
        QCOMPARE(clips(), 1);
        m_ctrl->select(clip);
        QVERIFY(!m_ctrl->selectedClip().value("audioMuted").toBool());
        // Já em cache: só a voz entra na hora.
        QCOMPARE(m_ctrl->selectedClip().value("stemsState").toString(), QStringLiteral("ready"));
        m_ctrl->separateStems(1);
        QCOMPARE(clips(), 2);
#endif
    }

    void motionTrackingWorkflow()
    {
        m_ctrl->newProject(640, 360, 30);
        m_ctrl->importMedia({QUrl::fromLocalFile(m_video)});
        m_ctrl->addAssetToTimeline(m_ctrl->assets().first().toMap().value("id").toString());
        m_ctrl->seek(1'000'000);
        m_ctrl->addText(QStringLiteral("Hi"));
        const QString text = m_ctrl->selectedClip().value("id").toString();
        QVERIFY(m_ctrl->selectedClip().value("canTrack").toBool());
        m_ctrl->startTrackingPick();
        QVERIFY(m_ctrl->trackingPick());
        m_ctrl->trackRegion(0.4, 0.4, 0.6, 0.6);
        QVERIFY(!m_ctrl->trackingPick());
        QTRY_VERIFY_WITH_TIMEOUT(m_ctrl->trackingProgress() < 0, 60000);
        QCOMPARE(m_ctrl->undoText(), QStringLiteral("Track motion"));
        m_ctrl->select(text);
        QVERIFY(m_ctrl->selectedClip().value("kf_x").toBool());
        m_ctrl->undo();
        m_ctrl->select(text);
        QVERIFY(!m_ctrl->selectedClip().value("kf_x").toBool());
        m_ctrl->redo();
        m_ctrl->select(text);
        m_ctrl->clearTracking();
        QVERIFY(!m_ctrl->selectedClip().value("kf_x").toBool());
        // O vídeo da faixa principal não tem o que seguir por baixo.
        m_ctrl->select(clipsOf(0).first().toMap().value("id").toString());
        QVERIFY(!m_ctrl->selectedClip().value("canTrack").toBool());
    }

    void readTextAloud()
    {
        if (m_ctrl->ttsState() == QLatin1String("unavailable") || m_ctrl->ttsState() == QLatin1String("needsModel"))
            QSKIP("voz neural não instalada");
        m_ctrl->newProject(640, 360, 30);
        m_ctrl->seek(1'000'000);
        m_ctrl->addText(QStringLiteral("Olá! Isto é um *teste* de narração."));
        const double textStart = m_ctrl->selectedClip().value("start").toDouble();
        m_ctrl->setTtsVoice(QStringLiteral("pm_alex"));
        QCOMPARE(m_ctrl->ttsVoice(), QStringLiteral("pm_alex"));
        auto narration = [this]() -> QVariantMap {
            for (const auto &t : m_ctrl->tracks())
                if (t.toMap().value("kind").toString() == QLatin1String("audio") && !t.toMap().value("clips").toList().isEmpty())
                    return t.toMap();
            return {};
        };
        m_ctrl->speakSelection();
        QCOMPARE(m_ctrl->ttsState(), QStringLiteral("processing"));
        QTRY_VERIFY_WITH_TIMEOUT(m_ctrl->ttsProgress() < 0, 60000);
        QVariantMap track = narration();
        QVERIFY(!track.isEmpty());
        QCOMPARE(track.value("name").toString(), QStringLiteral("Narration"));
        const QVariantMap clip = track.value("clips").toList().first().toMap();
        QCOMPARE(clip.value("start").toDouble(), textStart); // no tempo do texto
        QVERIFY(clip.value("duration").toDouble() > 1.5e6);
        m_ctrl->undo();
        QVERIFY(narration().isEmpty());
        // Mesmo texto, voz e velocidade: vem do cache.
        QElapsedTimer t;
        t.start();
        m_ctrl->speakSelection();
        QTRY_VERIFY_WITH_TIMEOUT(m_ctrl->ttsProgress() < 0, 60000);
        QVERIFY(t.elapsed() < 2000);
        QVERIFY(!narration().isEmpty());
        m_ctrl->speakCaptions(); // não há legendas: só avisa
        QVERIFY(m_ctrl->ttsProgress() < 0);
    }

    void beatMarkers()
    {
        // Bumbo a 120 BPM (um golpe a cada 0,5 s) por 8 s.
        const QString file = m_dir.filePath(QStringLiteral("batida.wav"));
        QProcess p;
        p.start(QStringLiteral("ffmpeg"), {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                                           "aevalsrc=0.8*sin(2*PI*60*t)*exp(-18*mod(t\\,0.5)):d=8:s=48000", "-ac", "2", file});
        QVERIFY(p.waitForFinished(60000) && p.exitCode() == 0);
        m_ctrl->newProject(1280, 720, 30);
        m_ctrl->importMedia({QUrl::fromLocalFile(file)});
        m_ctrl->addAssetToTimeline(m_ctrl->assets().first().toMap().value("id").toString());
        QString clip;
        for (const auto &t : m_ctrl->tracks())
            for (const auto &c : t.toMap().value("clips").toList())
                clip = c.toMap().value("id").toString();
        QVERIFY(!clip.isEmpty());
        m_ctrl->select(clip);
        QVERIFY(!m_ctrl->hasBeatMarkers());

        m_ctrl->detectBeats(1);
        QTRY_VERIFY_WITH_TIMEOUT(!m_ctrl->detectingBeats(), 30000);
        const QVariantList beats = m_ctrl->markers();
        QVERIFY2(beats.size() >= 13, qPrintable(QString::number(beats.size())));
        for (const auto &v : beats) {
            const QVariantMap m = v.toMap();
            QVERIFY(m.value("beat").toBool());
            const double t = m.value("t").toDouble() / 1e6;
            QVERIFY2(std::abs(t - 0.5 * std::round(t / 0.5)) < 0.04, qPrintable(QString::number(t)));
        }
        // A cada compasso: menos marcas, e as antigas do clipe são trocadas (não somadas).
        m_ctrl->detectBeats(4);
        QTRY_VERIFY_WITH_TIMEOUT(!m_ctrl->detectingBeats(), 30000);
        QVERIFY(m_ctrl->markers().size() <= beats.size() / 4 + 1);
        m_ctrl->addMarker(); // marcador manual fica ao limpar as batidas
        m_ctrl->clearBeatMarkers();
        QCOMPARE(m_ctrl->markers().size(), 1);
        QVERIFY(!m_ctrl->hasBeatMarkers());
        m_ctrl->undo();
        QVERIFY(m_ctrl->hasBeatMarkers());
    }

    void transcriptWorkflow()
    {
#ifndef CLIVCUT_WHISPER_SAMPLES
        QSKIP("build sem whisper.cpp");
#else
        if (!clv::engine::whisperModelInstalled(QStringLiteral("tiny")))
            QSKIP("modelo tiny ausente");
        m_ctrl->newProject(1280, 720, 30);
        m_ctrl->importMedia({QUrl::fromLocalFile(QStringLiteral(CLIVCUT_WHISPER_SAMPLES "/jfk.wav"))});
        m_ctrl->addAssetToTimeline(m_ctrl->assets().first().toMap().value("id").toString(), -1, 0.0);
        const double before = m_ctrl->duration();
        m_ctrl->generateTranscript(QStringLiteral("tiny"), QStringLiteral("en"));
        QTRY_VERIFY_WITH_TIMEOUT(!m_ctrl->transcribing(), 60000);
        const QVariantList words = m_ctrl->transcript();
        QVERIFY2(words.size() > 15, qPrintable(QString::number(words.size())));
        QVERIFY(!m_ctrl->transcriptStale());
        // Uma palavra por item, com tempos crescentes.
        int ask = -1;
        for (int i = 0; i < words.size(); ++i) {
            const auto w = words[i].toMap();
            QVERIFY(!w.value("text").toString().contains(QLatin1Char(' ')));
            if (i > 0)
                QVERIFY(w.value("start").toDouble() >= words[i - 1].toMap().value("start").toDouble());
            if (ask < 0 && w.value("text").toString().compare(QStringLiteral("ask"), Qt::CaseInsensitive) == 0)
                ask = i;
        }
        QVERIFY(ask > 0 && ask + 1 < words.size());

        // Remover "ask" corta do início dela ao início da palavra seguinte.
        const double cut = words[ask + 1].toMap().value("start").toDouble() - words[ask].toMap().value("start").toDouble();
        QVERIFY(m_ctrl->removeTranscriptWords({ask}));
        QVERIFY(std::abs(m_ctrl->duration() - (before - cut)) < 2000);
        QCOMPARE(m_ctrl->transcript().size(), words.size() - 1);
        QCOMPARE(m_ctrl->transcript()[ask].toMap().value("start").toDouble(), words[ask].toMap().value("start").toDouble());
        QVERIFY(!m_ctrl->transcriptStale());

        // Outra edição deixa a transcrição desatualizada e bloqueia cortes por texto.
        m_ctrl->addMarker();
        QVERIFY(m_ctrl->transcriptStale());
        QVERIFY(!m_ctrl->removeTranscriptWords({0}));
        m_ctrl->clearTranscript();
        QVERIFY(m_ctrl->transcript().isEmpty());
#endif
    }

    void captionsWorkflow()
    {
#ifndef CLIVCUT_WHISPER_SAMPLES
        QSKIP("build sem whisper.cpp");
#else
        if (!clv::engine::whisperModelInstalled(QStringLiteral("tiny")))
            QSKIP("modelo tiny ausente");
        m_ctrl->newProject(1280, 720, 30);
        m_ctrl->importMedia({QUrl::fromLocalFile(m_video), QUrl::fromLocalFile(QStringLiteral(CLIVCUT_WHISPER_SAMPLES "/jfk.wav"))});
        for (const auto &a : m_ctrl->assets()) {
            const auto m = a.toMap();
            m_ctrl->addAssetToTimeline(m.value("id").toString(), -1, m.value("kind") == "audio" ? 0.0 : -1.0);
        }
        QSignalSpy changed(m_ctrl.get(), &ProjectController::transcribeChanged);
        m_ctrl->generateCaptions(QStringLiteral("tiny"), QStringLiteral("en"), -1, true);
        QVERIFY(m_ctrl->transcribing());
        QTRY_VERIFY_WITH_TIMEOUT(!m_ctrl->transcribing(), 60000);

        int captionTrack = -1;
        for (const auto &t : m_ctrl->tracks())
            if (t.toMap().value("name") == QStringLiteral("Captions"))
                captionTrack = t.toMap().value("index").toInt();
        QVERIFY(captionTrack > 0);
        const auto caps = clipsOf(captionTrack);
        QVERIFY(caps.size() >= 2);

        // Gerar de novo com "substituir" não duplica a faixa; um undo remove tudo.
        m_ctrl->undo();
        for (const auto &t : m_ctrl->tracks())
            QVERIFY(t.toMap().value("name") != QStringLiteral("Captions"));
        m_ctrl->redo();

        // Estilo aplicado a todas.
        const QString first = caps.first().toMap().value("id").toString();
        m_ctrl->select(first);
        QVERIFY(m_ctrl->selectedClip().value("inCaptionTrack").toBool());
        m_ctrl->setClipProperty(first, "color", QStringLiteral("#ffe14d"));
        m_ctrl->applyStyleToTrack(first);
        m_ctrl->select(caps.last().toMap().value("id").toString());
        QCOMPARE(m_ctrl->selectedClip().value("color").toString(), QStringLiteral("#ffe14d"));

        // Exporta SRT e reimporta.
        const QString srt = m_dir.filePath(QStringLiteral("legendas.srt"));
        QVERIFY(m_ctrl->exportSubtitles(QUrl::fromLocalFile(srt)));
        QFile f(srt);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(f.readAll());
        QVERIFY2(content.contains(QStringLiteral("country"), Qt::CaseInsensitive), qPrintable(content));
        const int tracksBefore = m_ctrl->tracks().size();
        QVERIFY(m_ctrl->importSubtitles(QUrl::fromLocalFile(srt)));
        QCOMPARE(m_ctrl->tracks().size(), tracksBefore + 1);

        const QString keep = qEnvironmentVariable("CLIVCUT_TEST_KEEP");
        if (!keep.isEmpty()) {
            m_ctrl->undo();
            m_ctrl->saveProjectAs(QUrl::fromLocalFile(keep));
        }
#endif
    }
};

QTEST_MAIN(ControllerTest)
#include "tst_controller.moc"
