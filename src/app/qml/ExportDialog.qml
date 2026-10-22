import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtCore
import Clivcut

// Exportação: vídeo (com GPU e codecs com transparência), só áudio ou o quadro atual.
Dialog {
    id: root
    title: qsTr("Export")
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 480
    closePolicy: Project.exporting ? Popup.NoAutoClose : Popup.CloseOnEscape

    background: Rectangle { color: Theme.panel; radius: 10; border.color: Theme.border }
    header: Text {
        text: root.title
        color: Theme.text
        font.pixelSize: Theme.fontLarge
        font.bold: true
        padding: 16
    }

    Settings {
        id: prefs
        category: "export"
        property alias mode: modeBar.currentIndex
        property alias resolution: resBox.currentIndex
        property alias fpsIndex: fpsBox.currentIndex
        property alias qualityIndex: qualityBox.currentIndex
        property alias hardware: gpuBox.checked
        property alias audioFormatIndex: audioBox.currentIndex
        property string codec: "h264"
        property string folder: StandardPaths.writableLocation(StandardPaths.MoviesLocation)
    }

    readonly property int mode: modeBar.currentIndex // 0 vídeo, 1 áudio, 2 quadro
    readonly property var codecLabels: ({
        h264: "H.264 (MP4)", hevc: "HEVC / H.265 (MP4)", av1: "AV1 (MP4)", prores: "ProRes 422 HQ (MOV)",
        prores4444: qsTr("ProRes 4444 with transparency (MOV)"), vp9alpha: qsTr("VP9 with transparency (WebM)")
    })
    readonly property var codecExt: ({ prores: "mov", prores4444: "mov", vp9alpha: "webm" })
    readonly property var audioLabels: ({ wav: "WAV", flac: "FLAC", m4a: "AAC (M4A)", opus: "Opus", mp3: "MP3" })
    readonly property string extension: mode === 1 ? audioBox.currentValue || "wav"
                                       : mode === 2 ? "png"
                                       : codecExt[codecBox.currentValue] || "mp4"
    readonly property bool alphaCodec: codecBox.currentValue === "prores4444" || codecBox.currentValue === "vp9alpha"
    readonly property bool hardwareAvailable: Project.hardwareCodecs.indexOf(codecBox.currentValue) >= 0

    function suggestedName() {
        const base = Project.projectName.replace(/[\\/:*?"<>|]/g, "_");
        return prefs.folder.replace(/^file:\/\//, "") + "/" + base + "." + extension;
    }
    // Troca a extensão do caminho digitado quando o formato muda.
    function fixExtension() {
        if (pathField.text === "")
            return;
        const slash = pathField.text.lastIndexOf("/");
        const dot = pathField.text.lastIndexOf(".");
        const stem = dot > slash ? pathField.text.substring(0, dot) : pathField.text;
        pathField.text = stem + "." + extension;
    }
    onExtensionChanged: fixExtension()

    onOpened: {
        codecBox.currentIndex = Math.max(0, Project.videoCodecs.indexOf(prefs.codec));
        pathField.text = suggestedName();
    }

    contentItem: ColumnLayout {
        spacing: 12
        enabled: !Project.exporting

        TabBar {
            id: modeBar
            objectName: "exportModes"
            Layout.fillWidth: true
            background: Rectangle { color: "transparent" }
            Repeater {
                model: [qsTr("Video"), qsTr("Audio only"), qsTr("Current frame")]
                TabButton {
                    id: tab
                    required property string modelData
                    text: modelData
                    focusPolicy: Qt.NoFocus
                    contentItem: Text {
                        text: tab.text
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: tab.checked ? Theme.accent : Theme.textDim
                        font.pixelSize: Theme.fontNormal
                        font.bold: tab.checked
                    }
                    background: Rectangle {
                        radius: Theme.radius
                        color: tab.checked ? Theme.accentSoft : (tab.hovered ? Theme.surface : "transparent")
                    }
                }
            }
        }

        GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 10
            Layout.fillWidth: true

            Text { text: qsTr("File"); color: Theme.textDim; font.pixelSize: Theme.fontNormal }
            RowLayout {
                Layout.fillWidth: true
                TextField {
                    id: pathField
                    Layout.fillWidth: true
                    color: Theme.text
                    selectByMouse: true
                }
                Button {
                    text: "…"
                    focusPolicy: Qt.NoFocus
                    onClicked: fileDialog.open()
                }
            }

            Text { visible: root.mode === 0; text: qsTr("Resolution"); color: Theme.textDim; font.pixelSize: Theme.fontNormal }
            ComboBox {
                id: resBox
                visible: root.mode === 0
                Layout.fillWidth: true
                model: ["480p", "720p", "1080p", "2K (1440p)", "4K (2160p)"]
                currentIndex: 2
                readonly property var values: [480, 720, 1080, 1440, 2160]
            }

            Text { visible: root.mode === 0; text: qsTr("Frame rate"); color: Theme.textDim; font.pixelSize: Theme.fontNormal }
            ComboBox {
                id: fpsBox
                visible: root.mode === 0
                Layout.fillWidth: true
                model: [qsTr("From project (%1)").arg(Project.fps), "24", "25", "30", "50", "60"]
                readonly property var values: [0, 24, 25, 30, 50, 60]
            }

            Text { visible: root.mode === 0; text: qsTr("Codec"); color: Theme.textDim; font.pixelSize: Theme.fontNormal }
            ComboBox {
                id: codecBox
                visible: root.mode === 0
                Layout.fillWidth: true
                model: Project.videoCodecs.map(c => ({ value: c, text: root.codecLabels[c] || c }))
                textRole: "text"
                valueRole: "value"
                onActivated: prefs.codec = currentValue
            }

            Text { visible: root.mode === 0; text: qsTr("Quality"); color: Theme.textDim; font.pixelSize: Theme.fontNormal }
            ComboBox {
                id: qualityBox
                visible: root.mode === 0
                Layout.fillWidth: true
                model: [qsTr("High"), qsTr("Recommended"), qsTr("Smaller file")]
                currentIndex: 1
                readonly property var crf: [16, 20, 26]
            }

            Text { visible: root.mode === 1; text: qsTr("Format"); color: Theme.textDim; font.pixelSize: Theme.fontNormal }
            ComboBox {
                id: audioBox
                visible: root.mode === 1
                Layout.fillWidth: true
                model: Project.audioFormats.map(f => ({ value: f, text: root.audioLabels[f] || f }))
                textRole: "text"
                valueRole: "value"
            }
        }

        CheckBox {
            id: gpuBox
            visible: root.mode === 0 && root.hardwareAvailable
            checked: true
            text: qsTr("Encode on the GPU (faster)")
        }

        CheckBox {
            id: rangeBox
            readonly property bool hasRange: Project.inPoint >= 0 || Project.outPoint > 0
            readonly property real rangeIn: Math.max(0, Project.inPoint)
            readonly property real rangeOut: Project.outPoint > 0 ? Project.outPoint : Project.duration
            visible: hasRange && root.mode !== 2
            checked: true
            text: qsTr("Only the in/out range (%1 – %2)").arg(Project.formatTime(rangeIn)).arg(Project.formatTime(rangeOut))
        }

        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textFaint
            font.pixelSize: Theme.fontSmall
            text: root.mode === 2
                  ? qsTr("Saves the frame under the playhead (%1) as an image at the canvas resolution (%2×%3).")
                        .arg(Project.formatTime(Project.playhead)).arg(Project.canvasWidth).arg(Project.canvasHeight)
                  : qsTr("Duration: %1 · Canvas %2×%3")
                        .arg(Project.formatTime(rangeBox.hasRange && rangeBox.checked ? rangeBox.rangeOut - rangeBox.rangeIn : Project.duration))
                        .arg(Project.canvasWidth).arg(Project.canvasHeight)
                    + (root.mode === 0 && root.alphaCodec ? " · " + qsTr("areas without clips stay transparent") : "")
        }

        Text {
            visible: Project.emptySlotCount > 0 && root.mode !== 1
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.warning
            font.pixelSize: Theme.fontSmall
            text: qsTr("%n template slot(s) still empty: they will show as placeholders in the video.", "", Project.emptySlotCount)
        }

        ProgressBar {
            Layout.fillWidth: true
            visible: Project.exporting
            value: Project.exportProgress
        }
    }

    footer: DialogButtonBox {
        background: Rectangle { color: "transparent" }
        Button {
            text: Project.exporting ? qsTr("Cancel export") : qsTr("Close")
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: Project.exporting ? Project.cancelExport() : root.close()
        }
        Button {
            text: Project.exporting ? qsTr("%1%").arg(Math.round(Project.exportProgress * 100)) : qsTr("Export")
            enabled: !Project.exporting && Project.duration > 0 && pathField.text !== ""
            highlighted: true
            DialogButtonBox.buttonRole: DialogButtonBox.ApplyRole
            onClicked: {
                const path = pathField.text;
                prefs.folder = "file://" + path.substring(0, path.lastIndexOf("/"));
                const range = rangeBox.hasRange && rangeBox.checked;
                if (root.mode === 1)
                    Project.exportAudio("file://" + path, range);
                else if (root.mode === 2)
                    Project.exportFrame("file://" + path);
                else
                    Project.exportVideo("file://" + path, resBox.values[resBox.currentIndex], fpsBox.values[fpsBox.currentIndex],
                                        codecBox.currentValue, qualityBox.crf[qualityBox.currentIndex], range,
                                        gpuBox.visible && gpuBox.checked);
            }
        }
    }

    Connections {
        target: Project
        function onExportFinished(ok, message) {
            if (ok)
                root.close();
        }
    }

    FileDialog {
        id: fileDialog
        fileMode: FileDialog.SaveFile
        currentFolder: prefs.folder
        nameFilters: root.mode === 1 ? [qsTr("Audio (*.%1)").arg(root.extension)]
                   : root.mode === 2 ? [qsTr("Image (*.png *.jpg)")]
                   : [qsTr("Video (*.%1)").arg(root.extension)]
        onAccepted: pathField.text = selectedFile.toString().replace(/^file:\/\//, "")
    }
}
