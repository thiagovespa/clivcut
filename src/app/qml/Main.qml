import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtCore
import Clivcut

ApplicationWindow {
    id: window
    width: 1480
    height: 920
    minimumWidth: 860
    minimumHeight: 600
    visible: true
    color: Theme.bg
    title: (Project.modified ? "• " : "") + Project.projectName + " — clivcut"

    palette.window: Theme.panel
    palette.windowText: Theme.text
    palette.base: Theme.surface
    palette.text: Theme.text
    palette.button: Theme.surface
    palette.buttonText: Theme.text
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.accentInk
    palette.toolTipBase: Theme.surfaceHi
    palette.toolTipText: Theme.text
    palette.placeholderText: Theme.textFaint
    palette.mid: Theme.border
    palette.light: Theme.surfaceHi
    palette.dark: Theme.bg

    // Atalhos não disparam enquanto se digita num campo de texto.
    readonly property bool typing: activeFocusItem !== null && activeFocusItem.hasOwnProperty("cursorPosition")

    Settings {
        id: settings
        category: "window"
        property alias x: window.x
        property alias y: window.y
        property alias width: window.width
        property alias height: window.height
        property string lastFolder: StandardPaths.writableLocation(StandardPaths.MoviesLocation)
    }

    // Ações que trocam/fecham o projeto passam por aqui: pergunta antes de perder alterações.
    property string pendingAction: ""
    property bool closeConfirmed: false
    function guarded(action) {
        if (!Project.modified) {
            runAction(action);
            return;
        }
        pendingAction = action;
        unsavedDialog.open();
    }
    function runAction(action) {
        pendingAction = "";
        if (action === "new") newDialog.open();
        else if (action === "open") openDialog.open();
        else if (action === "draft") draftDialog.open();
        else if (action === "quit") {
            closeConfirmed = true;
            window.close();
        }
    }
    onClosing: (close) => {
        if (!closeConfirmed && Project.modified) {
            close.accepted = false;
            guarded("quit");
        }
    }
    Component.onCompleted: Qt.callLater(() => {
        recoveryDialog.refresh();
        if (recoveryDialog.items.length > 0)
            recoveryDialog.open();
    })

    header: TopBar {
        onNewRequested: window.guarded("new")
        onOpenRequested: window.guarded("open")
        onSaveRequested: window.save()
        onSaveAsRequested: saveDialog.open()
        onImportDraftRequested: window.guarded("draft")
        onExportRequested: exportDialog.open()
        onCommandsRequested: commandPalette.open()
        onHomeRequested: window.showWelcome = true
    }

    // Tela inicial enquanto o projeto está vazio (some ao importar/abrir).
    property bool showWelcome: true
    Connections {
        target: Project
        function onAssetsChanged() { if (Project.assets.length > 0) window.showWelcome = false }
        function onFileChanged() { if (Project.filePath !== "" || Project.duration > 0) window.showWelcome = false }
    }

    ActivityRail {
        id: rail
        objectName: "rail"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        visible: !window.showWelcome
        onShortcutsRequested: shortcutsDialog.open()
        Settings {
            category: "panels"
            property alias section: rail.current
            property alias collapsed: rail.collapsed
        }
    }

    WelcomeScreen {
        anchors.fill: parent
        visible: window.showWelcome
        z: 10
        onImportRequested: mediaDialog.open()
        onOpenRequested: openDialog.open()
        onDraftRequested: draftDialog.open()
        onDismissed: window.showWelcome = false
    }

    SplitView {
        anchors.left: rail.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        orientation: Qt.Vertical
        handle: Rectangle {
            implicitHeight: 4
            color: SplitHandle.pressed ? Theme.accent : Theme.bg
        }

        SplitView {
            SplitView.fillHeight: true
            SplitView.minimumHeight: 240
            orientation: Qt.Horizontal
            handle: Rectangle {
                implicitWidth: 4
                color: SplitHandle.pressed ? Theme.accent : Theme.bg
            }

            MediaPanel {
                visible: !rail.collapsed
                section: rail.current
                SplitView.preferredWidth: Math.max(240, window.width * 0.22)
                SplitView.minimumWidth: 220
                onImportRequested: mediaDialog.open()
                onImportDraftRequested: window.guarded("draft")
                onRelinkRequested: (assetId) => {
                    relinkDialog.assetId = assetId;
                    relinkDialog.open();
                }
            }
            PreviewPanel {
                id: previewPanel
                SplitView.fillWidth: true
                SplitView.minimumWidth: 260
            }
            Inspector {
                objectName: "inspector"
                SplitView.preferredWidth: Math.max(250, window.width * 0.23)
                SplitView.minimumWidth: 230
            }
        }

        Timeline {
            id: timeline
            onSilencesRequested: silenceDialog.open()
            onVoiceoverRequested: previewPanel.openVoiceover()
            SplitView.preferredHeight: Math.max(220, window.height * 0.36)
            SplitView.minimumHeight: 160
        }
    }

    // --- arrastar arquivos do gerenciador para a janela ---
    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list"]
        onDropped: (drop) => {
            if (!drop.hasUrls)
                return;
            const files = [];
            for (const u of drop.urls) {
                const s = u.toString();
                if (s.endsWith(".clivcut") || s.indexOf("draft_content.json") >= 0 || s.indexOf("draft_info.json") >= 0)
                    Project.openProject(u);
                else
                    files.push(u);
            }
            if (files.length > 0)
                Project.importMedia(files);
            drop.accept();
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: 8
            visible: parent.containsDrag
            color: "#2022d3c5"
            border.color: Theme.accent
            border.width: 2
            radius: 12
            Text {
                anchors.centerIn: parent
                text: qsTr("Drop to import")
                color: Theme.text
                font.pixelSize: 22
            }
        }
    }

    // --- atalhos: padrões e personalizações vêm do Keymap; aqui só o que cada ação faz ---
    readonly property var handlers: ({
        play: () => Project.togglePlay(),
        prevFrame: () => Project.stepFrames(-1),
        nextFrame: () => Project.stepFrames(1),
        back1s: () => Project.stepFrames(-Math.round(Project.fps)),
        forward1s: () => Project.stepFrames(Math.round(Project.fps)),
        start: () => Project.jumpToEdge(false),
        end: () => Project.jumpToEdge(true),
        split: () => Project.splitAtPlayhead(),
        deleteLeft: () => Project.deleteLeft(),
        deleteRight: () => Project.deleteRight(),
        delete: () => Project.deleteSelection(false),
        rippleDelete: () => Project.deleteSelection(true),
        undo: () => Project.undo(),
        redo: () => Project.redo(),
        duplicate: () => Project.duplicateSelection(),
        copy: () => Project.copySelection(),
        cut: () => Project.cutSelection(),
        paste: () => Project.paste(),
        copyAttributes: () => Project.copyAttributes(),
        pasteAttributes: () => Project.pasteAttributesToSelection(),
        group: () => Project.groupSelection(),
        ungroup: () => Project.ungroupSelection(),
        detachAudio: () => Project.detachAudio(),
        freeze: () => Project.freezeFrame(),
        addText: () => Project.addText("", ""),
        selectAll: () => Project.selectAll(),
        deselect: () => Project.clearSelection(),
        marker: () => Project.addMarker(),
        markIn: () => Project.markIn(),
        markOut: () => Project.markOut(),
        clearInOut: () => Project.clearInOut(),
        goIn: () => Project.jumpToInOut(false),
        goOut: () => Project.jumpToInOut(true),
        snapping: () => Project.snapping = !Project.snapping,
        magnet: () => Project.magnet = !Project.magnet,
        zoomIn: () => timeline.zoomBy(1.25),
        zoomOut: () => timeline.zoomBy(0.8),
        zoomFit: () => { timeline.userZoomed = false; timeline.zoomToFit() },
        removeSilences: () => silenceDialog.open(),
        recordVoiceover: () => previewPanel.openVoiceover(),
        renderPreview: () => Project.renderingPreview ? Project.cancelRenderPreview() : Project.renderPreview(),
        clearRenderCache: () => Project.clearRenderCache(),
        palette: () => commandPalette.open(),
        scopes: () => Project.scopesEnabled = !Project.scopesEnabled,
        shortcuts: () => shortcutsDialog.open(),
        panel0: () => rail.activate(0),
        panel1: () => rail.activate(1),
        panel2: () => rail.activate(2),
        panel3: () => rail.activate(3),
        panel4: () => rail.activate(4),
        panel5: () => rail.activate(5),
        panel6: () => rail.activate(6),
        panel7: () => rail.activate(7),
        newProject: () => window.guarded("new"),
        open: () => window.guarded("open"),
        save: () => window.save(),
        saveAs: () => saveDialog.open(),
        import: () => mediaDialog.open(),
        export: () => exportDialog.open()
    })
    readonly property var actionIcons: ({
        play: "play", prevFrame: "chevron-left", nextFrame: "chevron-right", start: "skip-back", end: "skip-forward",
        split: "scissors", deleteLeft: "arrow-left-to-line", deleteRight: "arrow-right-to-line", delete: "trash-2",
        rippleDelete: "trash-2", undo: "undo-2", redo: "redo-2", duplicate: "copy", copy: "copy", cut: "scissors",
        paste: "copy", group: "link", ungroup: "link", detachAudio: "audio-lines", freeze: "snowflake", addText: "type",
        selectAll: "layers", marker: "bookmark", markIn: "arrow-right-to-line", markOut: "arrow-left-to-line",
        clearInOut: "x", goIn: "skip-back", goOut: "skip-forward", snapping: "crosshair", magnet: "magnet",
        zoomIn: "zoom-in", zoomOut: "zoom-out", zoomFit: "maximize-2", renderPreview: "zap", removeSilences: "scissors-line-dashed", recordVoiceover: "mic", clearRenderCache: "trash-2", shortcuts: "keyboard", scopes: "activity", panel0: "image-play",
        panel1: "type", panel2: "captions", panel3: "palette", panel4: "arrow-right-left", panel5: "sparkles", panel6: "file-text", panel7: "wand-sparkles",
        newProject: "file-plus-2", open: "folder-open", save: "download", saveAs: "download", import: "plus", export: "upload"
    })
    Repeater {
        model: Keymap.actions
        Item {
            id: binding
            required property var modelData
            Shortcut {
                sequences: Keymap.keys(binding.modelData.id)
                // Desligado ao editar atalhos e, para ações de edição, ao digitar num campo.
                enabled: !shortcutsDialog.opened && !(binding.modelData.edit && window.typing)
                onActivated: window.handlers[binding.modelData.id]()
            }
        }
    }

    function save() {
        if (Project.filePath === "")
            saveDialog.open();
        else
            Project.saveProject();
    }

    // --- diálogos ---
    FileDialog {
        id: mediaDialog
        title: qsTr("Import media")
        fileMode: FileDialog.OpenFiles
        currentFolder: settings.lastFolder
        nameFilters: [qsTr("Media (*.mp4 *.mov *.mkv *.webm *.avi *.m4v *.mts *.mp3 *.wav *.m4a *.aac *.flac *.ogg *.opus *.png *.jpg *.jpeg *.webp *.gif *.bmp *.tif *.tiff *.heic)"), qsTr("All files (*)")]
        onAccepted: {
            settings.lastFolder = currentFolder;
            Project.importMedia(selectedFiles);
        }
    }
    FileDialog {
        id: openDialog
        title: qsTr("Open project")
        nameFilters: [qsTr("clivcut projects (*.clivcut)"), qsTr("Draft JSON (draft_content.json draft_info.json)")]
        onAccepted: Project.openProject(selectedFile)
    }
    FileDialog {
        id: saveDialog
        title: qsTr("Save project")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "clivcut"
        nameFilters: [qsTr("clivcut projects (*.clivcut)")]
        onAccepted: {
            if (Project.saveProjectAs(selectedFile) && window.pendingAction !== "")
                window.runAction(window.pendingAction);
        }
        onRejected: window.pendingAction = ""
    }
    UnsavedDialog {
        id: unsavedDialog
        onSaveRequested: {
            if (Project.filePath === "")
                saveDialog.open(); // continua a ação depois de salvar
            else if (Project.saveProject())
                window.runAction(window.pendingAction);
        }
        onDiscardRequested: {
            Project.discardChanges();
            window.runAction(window.pendingAction);
        }
        onRejected: window.pendingAction = ""
    }
    RecoveryDialog { id: recoveryDialog }
    FolderDialog {
        id: draftDialog
        title: qsTr("Draft folder")
        onAccepted: Project.importDraft(selectedFolder)
    }
    FileDialog {
        id: relinkDialog
        property string assetId
        title: qsTr("Relink media")
        onAccepted: Project.relinkAsset(assetId, selectedFile)
    }
    ExportDialog { id: exportDialog; objectName: "exportDialog" }

    CommandPalette {
        id: commandPalette
        objectName: "commandPalette"
        parent: Overlay.overlay
        commands: Keymap.actions.filter(a => a.id !== "palette").map(a => ({
            name: a.name, shortcut: Keymap.label(a.id), icon: window.actionIcons[a.id] || "", run: window.handlers[a.id]
        })).concat([
            { name: qsTr("Add adjustment layer"), shortcut: "", icon: "sliders-horizontal", run: () => Project.addFilterClip("") },
            { name: qsTr("Normalize loudness (-14 LUFS)"), shortcut: "", icon: "volume-2", run: () => Project.normalizeLoudness(-14) },
            { name: qsTr("Generate auto captions"), shortcut: Keymap.label("panel2"), icon: "captions", run: () => rail.activate(2) },
            { name: qsTr("Import captions (SRT/VTT)"), shortcut: "", icon: "captions", run: () => rail.activate(2) },
            { name: qsTr("Home screen"), shortcut: "", icon: "clapperboard", run: () => window.showWelcome = true },
            { name: qsTr("Use proxies for playback"), shortcut: "", icon: "gauge", run: () => Project.useProxies = !Project.useProxies },
            { name: Theme.dark ? qsTr("Light theme") : qsTr("Dark theme"), shortcut: "", icon: Theme.dark ? "sun" : "moon", run: () => Theme.dark = !Theme.dark },
            { name: qsTr("Language: %1").arg(qsTr("System default")), shortcut: "", icon: "languages", run: () => Project.language = "" }
        ].concat(Project.languages.map(l => ({ name: qsTr("Language: %1").arg(l.name), shortcut: "", icon: "languages",
                                                run: () => Project.language = l.code }))))
    }
    SilenceDialog { id: silenceDialog; objectName: "silenceDialog"; parent: Overlay.overlay }
    ShortcutsDialog { id: shortcutsDialog; objectName: "shortcutsDialog"; parent: Overlay.overlay }
    NewProjectDialog { id: newDialog }

    // --- mensagens ---
    Connections {
        target: Project
        function onErrorOccurred(message) { toast.show(message, true) }
        function onStatusMessageChanged() { toast.show(Project.statusMessage, false) }
        function onExportFinished(ok, message) {
            toast.show(ok ? qsTr("Video exported: %1").arg(message) : message, !ok);
        }
    }

    Rectangle {
        id: toast
        property bool error: false
        function show(msg, isError) {
            label.text = msg;
            error = isError;
            opacity = 1;
            hideTimer.restart();
        }
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        width: Math.min(label.implicitWidth + 32, window.width - 80)
        height: label.implicitHeight + 18
        radius: 8
        color: error ? "#5a1f27" : Theme.surfaceHi
        border.color: error ? Theme.danger : Theme.border
        opacity: 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        Text {
            id: label
            anchors.centerIn: parent
            width: Math.min(implicitWidth, window.width - 112)
            color: Theme.text
            font.pixelSize: Theme.fontNormal
            wrapMode: Text.Wrap
        }
        Timer {
            id: hideTimer
            interval: 4000
            onTriggered: toast.opacity = 0
        }
    }
}
