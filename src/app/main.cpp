// SPDX-License-Identifier: GPL-3.0-or-later
#include "ImageProviders.h"
#include "Language.h"
#include "ProjectController.h"
#include "media/FFmpegUtil.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

#include <clocale>

int main(int argc, char **argv)
{
    // Se o Qt Quick usar OpenGL, precisa de 3.3 Core (shaders GLSL 330 com texturas 3D).
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    if (fmt.renderableType() != QSurfaceFormat::OpenGLES) {
        fmt.setVersion(3, 3);
        fmt.setProfile(QSurfaceFormat::CoreProfile);
        QSurfaceFormat::setDefaultFormat(fmt);
    }
    QGuiApplication app(argc, argv);
    // O QGuiApplication adota o locale do sistema; o FFmpeg lê os números das cadeias de filtros
    // com strtod, que em pt_BR esperaria vírgula. Números do processo ficam no formato "C".
    std::setlocale(LC_NUMERIC, "C");
    clv::media::quietFFmpegLogs();
    QGuiApplication::setApplicationName(QStringLiteral("clivcut"));
    QGuiApplication::setOrganizationName(QStringLiteral("clivcut"));
    QGuiApplication::setApplicationVersion(QStringLiteral(CLIVCUT_VERSION));
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.clivcut"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/qt/qml/Clivcut/icons/clivcut.svg")));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    clv::app::installLanguage(clv::app::savedLanguage());

    QCommandLineParser cli;
    cli.addHelpOption();
    cli.addVersionOption();
    cli.addPositionalArgument(QStringLiteral("project"), QStringLiteral(".clivcut project, draft JSON folder or media files"));
    cli.process(app);

    ProjectController controller(nullptr);
    QObject::connect(&controller, &ProjectController::errorOccurred, [](const QString &m) { qWarning("%s", qPrintable(m)); });
    QObject::connect(&controller, &ProjectController::statusMessageChanged, &controller,
                     [&controller] { qInfo("%s", qPrintable(controller.statusMessage())); });

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("thumb"), new ThumbnailProvider);
    engine.addImageProvider(QStringLiteral("wave"), new WaveformProvider);
    engine.addImageProvider(QStringLiteral("filter"), new FilterPreviewProvider);
    engine.addImageProvider(QStringLiteral("icon"), new IconProvider);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
                     Qt::QueuedConnection);
    engine.loadFromModule("Clivcut", "Main");

    // Argumentos: projeto/draft abre; mídias são importadas.
    QList<QUrl> media;
    for (const QString &arg : cli.positionalArguments()) {
        const QFileInfo fi(arg);
        const QUrl url = QUrl::fromLocalFile(fi.absoluteFilePath());
        if (fi.suffix() == QLatin1String("clivcut") || fi.isDir() || fi.fileName().startsWith(QLatin1String("draft_")))
            controller.openProject(url);
        else
            media << url;
    }
    if (!media.isEmpty())
        controller.importMedia(media);

    // Captura automática da janela (testes visuais/CI): CLIVCUT_SCREENSHOT=arquivo.png
    // CLIVCUT_SCREENSHOT_DELAY=ms, CLIVCUT_SCREENSHOT_SEEK=µs, CLIVCUT_SCREENSHOT_SELECT=1 (seleciona 1º clipe),
    // CLIVCUT_SCREENSHOT_PANEL=índice do painel lateral, CLIVCUT_SCREENSHOT_TAB=aba do inspetor,
    // CLIVCUT_SCREENSHOT_PALETTE=1 (abre a paleta de comandos), CLIVCUT_SCREENSHOT_EXPORT=aba do diálogo de exportação,
    // CLIVCUT_SCREENSHOT_SHORTCUTS=1 (abre o editor de atalhos), CLIVCUT_SCREENSHOT_RENDER=1 (renderiza a pré-visualização),
    // CLIVCUT_SCREENSHOT_OPEN=objectName de um diálogo para abrir, CLIVCUT_SCREENSHOT_TRANSCRIBE=modelo (transcreve em inglês),
    // CLIVCUT_SCREENSHOT_SCROLL=px (rola o inspetor), CLIVCUT_SCREENSHOT_SCOPES=modo (mostra os escopos),
    // CLIVCUT_SCREENSHOT_COMPOUND=1 (abre o primeiro clipe composto para edição).
    const QString shot = qEnvironmentVariable("CLIVCUT_SCREENSHOT");
    if (!shot.isEmpty() && !engine.rootObjects().isEmpty()) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        bool customDelay = false;
        int delay = qEnvironmentVariableIntValue("CLIVCUT_SCREENSHOT_DELAY", &customDelay);
        if (!customDelay)
            delay = 2500;
        if (qEnvironmentVariableIsSet("CLIVCUT_SCREENSHOT_SEEK"))
            controller.seek(qEnvironmentVariable("CLIVCUT_SCREENSHOT_SEEK").toDouble());
        if (qEnvironmentVariableIsSet("CLIVCUT_SCREENSHOT_TRANSCRIBE"))
            QTimer::singleShot(500, &controller, [&controller] {
                controller.generateTranscript(qEnvironmentVariable("CLIVCUT_SCREENSHOT_TRANSCRIBE"), QStringLiteral("en"));
            });
        if (qEnvironmentVariableIsSet("CLIVCUT_SCREENSHOT_RENDER"))
            QTimer::singleShot(500, &controller, [&controller] { controller.renderPreview(); });
        if (qEnvironmentVariableIsSet("CLIVCUT_SCREENSHOT_COMPOUND")) {
            for (const auto &t : controller.tracks())
                for (const auto &c : t.toMap().value(QStringLiteral("clips")).toList())
                    if (c.toMap().value(QStringLiteral("mediaKind")).toString() == QLatin1String("compound")) {
                        controller.openCompound(c.toMap().value(QStringLiteral("id")).toString());
                        break;
                    }
        }
        if (qEnvironmentVariableIsSet("CLIVCUT_SCREENSHOT_SELECT")) {
            const auto tracks = controller.tracks();
            for (const auto &t : tracks) {
                const auto clips = t.toMap().value(QStringLiteral("clips")).toList();
                if (!clips.isEmpty()) {
                    controller.select(clips.first().toMap().value(QStringLiteral("id")).toString());
                    break;
                }
            }
        }
        // Metade do atraso: dá tempo de a interface reagir antes da captura.
        QTimer::singleShot(delay / 2, window, [window] {
            bool ok = false;
            const int panel = qEnvironmentVariableIntValue("CLIVCUT_SCREENSHOT_PANEL", &ok);
            if (ok)
                if (auto *rail = window->findChild<QObject *>(QStringLiteral("rail")))
                    QMetaObject::invokeMethod(rail, "activate", Q_ARG(QVariant, panel));
            const int scroll = qEnvironmentVariableIntValue("CLIVCUT_SCREENSHOT_SCROLL", &ok);
            if (ok)
                if (auto *flick = window->findChild<QObject *>(QStringLiteral("inspectorScroll")))
                    QTimer::singleShot(300, flick, [flick, scroll] { flick->setProperty("contentY", scroll); });
            const QString tab = qEnvironmentVariable("CLIVCUT_SCREENSHOT_TAB");
            if (!tab.isEmpty())
                if (auto *inspector = window->findChild<QObject *>(QStringLiteral("inspector")))
                    inspector->setProperty("page", tab);
            if (qEnvironmentVariableIsSet("CLIVCUT_SCREENSHOT_PALETTE"))
                if (auto *palette = window->findChild<QObject *>(QStringLiteral("commandPalette")))
                    QMetaObject::invokeMethod(palette, "open");
            const int scopeMode = qEnvironmentVariableIntValue("CLIVCUT_SCREENSHOT_SCOPES", &ok);
            if (ok) {
                if (auto *ctrl = ProjectController::instance())
                    ctrl->setScopesEnabled(true);
                if (auto *scope = window->findChild<QObject *>(QStringLiteral("scopeView")))
                    scope->setProperty("mode", scopeMode);
            }
            const QString openName = qEnvironmentVariable("CLIVCUT_SCREENSHOT_OPEN");
            if (!openName.isEmpty())
                if (auto *dialog = window->findChild<QObject *>(openName))
                    QMetaObject::invokeMethod(dialog, "open");
            if (qEnvironmentVariableIsSet("CLIVCUT_SCREENSHOT_SHORTCUTS"))
                if (auto *dialog = window->findChild<QObject *>(QStringLiteral("shortcutsDialog")))
                    QMetaObject::invokeMethod(dialog, "open");
            const int exportTab = qEnvironmentVariableIntValue("CLIVCUT_SCREENSHOT_EXPORT", &ok);
            if (ok)
                if (auto *dialog = window->findChild<QObject *>(QStringLiteral("exportDialog"))) {
                    QMetaObject::invokeMethod(dialog, "open");
                    if (auto *tabs = dialog->findChild<QObject *>(QStringLiteral("exportModes")))
                        tabs->setProperty("currentIndex", exportTab);
                }
        });
        QTimer::singleShot(delay, window, [window, shot] {
            window->grabWindow().save(shot);
            QCoreApplication::exit(0); // quit() pediria para fechar a janela (e perguntaria sobre alterações)
        });
    }

    const int rc = QGuiApplication::exec();
    // Miniaturas e formas de onda em andamento terminam antes de destruir o resto.
    waitForImageJobs();
    QThreadPool::globalInstance()->clear();
    QThreadPool::globalInstance()->waitForDone();
    return rc;
}
