import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore
import Clivcut

Rectangle {
    id: root
    function openVoiceover() { voiceover.armed ? voiceover.stop() : voiceover.open() }
    color: Theme.bg

    readonly property var sel: Project.selectedClip
    readonly property bool canMove: sel.id !== undefined && sel.underPlayhead && (sel.type === "text" || sel.mediaKind !== "audio")

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Cabeçalho: proporção e qualidade
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            Layout.leftMargin: 10
            Layout.rightMargin: 10
            Text {
                text: qsTr("Player")
                color: Theme.textDim
                font.pixelSize: Theme.fontNormal
                visible: root.width > 420
            }
            Item { Layout.fillWidth: true }
            IconButton {
                iconName: "activity"
                active: Project.scopesEnabled
                tip: qsTr("Scopes: histogram, waveform and vectorscope")
                shortcut: Keymap.label("scopes")
                onClicked: Project.scopesEnabled = !Project.scopesEnabled
            }
            ComboBox {
                id: ratio
                focusPolicy: Qt.NoFocus
                implicitWidth: 130
                model: [
                    { label: "16:9 · " + qsTr("Landscape"), w: 1920, h: 1080 },
                    { label: "9:16 · " + qsTr("Vertical"), w: 1080, h: 1920 },
                    { label: "1:1 · " + qsTr("Square"), w: 1080, h: 1080 },
                    { label: "4:3", w: 1440, h: 1080 },
                    { label: "3:4", w: 1080, h: 1440 },
                    { label: "4:5 · " + qsTr("Feed"), w: 1080, h: 1350 },
                    { label: "21:9 · " + qsTr("Cinema"), w: 2560, h: 1080 }
                ]
                textRole: "label"
                displayText: Project.canvasWidth + "×" + Project.canvasHeight
                onActivated: (i) => Project.setCanvas(model[i].w, model[i].h)
            }
            IconButton {
                text: Project.proxiesPending > 0 ? "P ⟳" + Project.proxiesPending : "Proxy"
                active: Project.useProxies
                onClicked: Project.useProxies = !Project.useProxies
                tip: Project.useProxies ? qsTr("Playing with proxies (lightweight copies of large videos). Click to use the originals.")
                                        : qsTr("Playing the original files. Click to use proxies.")
            }
            ComboBox {
                id: quality
                focusPolicy: Qt.NoFocus
                implicitContentWidthPolicy: ComboBox.WidestText
                model: [qsTr("Preview 540p"), qsTr("Preview 720p"), qsTr("Preview 1080p")]
                readonly property var sides: [960, 1280, 1920]
                currentIndex: Math.max(0, sides.indexOf(Project.previewResolution))
                onActivated: (i) => Project.previewResolution = sides[i]
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Playback resolution (does not affect export)")
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            PreviewItem {
                id: preview
                anchors.fill: parent
                maxResolution: Project.previewResolution
            }

            VoiceoverPanel {
                id: voiceover
                objectName: "voiceoverPanel"
                z: 6
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 10
                width: Math.min(implicitWidth, parent.width - 20)
            }

            // Escopos do quadro atual (o mesmo que vai para o export).
            Rectangle {
                id: scopes
                objectName: "scopesPanel"
                visible: Project.scopesEnabled
                z: 5
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 8
                width: Math.min(300, parent.width * 0.45)
                height: width * 0.78
                radius: Theme.radius
                color: Qt.rgba(0.04, 0.04, 0.05, 0.86)
                border.color: Theme.border
                Settings {
                    category: "scopes"
                    property alias mode: scopeView.mode
                }
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 6
                    spacing: 4
                    RowLayout {
                        spacing: 2
                        Repeater {
                            model: [qsTr("Histogram"), qsTr("Waveform"), qsTr("Vectorscope")]
                            Rectangle {
                                id: scopeTab
                                required property string modelData
                                required property int index
                                readonly property bool on: scopeView.mode === index
                                implicitWidth: tabText.implicitWidth + 12
                                implicitHeight: 20
                                radius: 4
                                color: on ? Theme.accentSoft : "transparent"
                                Text {
                                    id: tabText
                                    anchors.centerIn: parent
                                    text: scopeTab.modelData
                                    color: scopeTab.on ? Theme.accent : "#c8c8cc"
                                    font.pixelSize: Theme.fontSmall
                                }
                                TapHandler { onTapped: scopeView.mode = scopeTab.index }
                            }
                        }
                        Item { Layout.fillWidth: true }
                        IconButton {
                            implicitWidth: 22; implicitHeight: 20
                            iconName: "x"
                            iconSize: 12
                            glyphColor: "#c8c8cc"
                            tip: qsTr("Close")
                            onClicked: Project.scopesEnabled = false
                        }
                    }
                    ScopeItem {
                        id: scopeView
                        objectName: "scopeView"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                    }
                }
            }

            // Moldura do canvas + arrastar para mover o clipe selecionado.
            Rectangle {
                id: frame
                x: preview.canvasRect.x
                y: preview.canvasRect.y
                width: preview.canvasRect.width
                height: preview.canvasRect.height
                color: "transparent"
                border.color: dragMover.containsMouse && root.canMove ? Theme.accent : "transparent"
                border.width: 1

                MouseArea {
                    id: dragMover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: root.canMove ? (pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor) : Qt.ArrowCursor
                    property real startX: 0
                    property real startY: 0
                    property point origin
                    onPressed: (mouse) => {
                        if (!root.canMove) {
                            mouse.accepted = false;
                            return;
                        }
                        startX = root.sel.x;
                        startY = root.sel.y;
                        origin = Qt.point(mouse.x, mouse.y);
                    }
                    onPositionChanged: (mouse) => {
                        if (!pressed || !root.canMove)
                            return;
                        const nx = startX + (mouse.x - origin.x) / (width / 2);
                        const ny = startY + (mouse.y - origin.y) / (height / 2);
                        // Encaixa no centro quando perto.
                        Project.setClipProperties(root.sel.id, { x: Math.abs(nx) < 0.02 ? 0 : nx, y: Math.abs(ny) < 0.02 ? 0 : ny });
                    }
                    onWheel: (wheel) => {
                        if (!root.canMove || !(wheel.modifiers & Qt.ControlModifier)) {
                            wheel.accepted = false;
                            return;
                        }
                        const f = wheel.angleDelta.y > 0 ? 1.05 : 1 / 1.05;
                        Project.setClipProperty(root.sel.id, "scale", Math.max(0.05, Math.min(10, root.sel.scale * f)));
                    }
                }
                // Guias de centro durante o arraste.
                Rectangle {
                    visible: dragMover.pressed && root.canMove && Math.abs(root.sel.x) < 0.001
                    x: parent.width / 2
                    width: 1
                    height: parent.height
                    color: Theme.accent
                }
                Rectangle {
                    visible: dragMover.pressed && root.canMove && Math.abs(root.sel.y) < 0.001
                    y: parent.height / 2
                    height: 1
                    width: parent.width
                    color: Theme.accent
                }
            }

            // Rastreamento: desenha a caixa em volta do que o clipe vai seguir.
            Item {
                id: trackPick
                visible: Project.trackingPick
                x: preview.canvasRect.x
                y: preview.canvasRect.y
                width: preview.canvasRect.width
                height: preview.canvasRect.height
                property point a
                property point b
                property bool drawing: false
                Rectangle { anchors.fill: parent; color: "#55000000" }
                Rectangle {
                    visible: trackPick.drawing
                    x: Math.min(trackPick.a.x, trackPick.b.x)
                    y: Math.min(trackPick.a.y, trackPick.b.y)
                    width: Math.abs(trackPick.b.x - trackPick.a.x)
                    height: Math.abs(trackPick.b.y - trackPick.a.y)
                    color: Theme.accentSoft
                    border.color: Theme.accent
                    border.width: 2
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.CrossCursor
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onPressed: (mouse) => {
                        if (mouse.button === Qt.RightButton) {
                            Project.cancelTrackingPick();
                            return;
                        }
                        trackPick.a = Qt.point(mouse.x, mouse.y);
                        trackPick.b = trackPick.a;
                        trackPick.drawing = true;
                    }
                    onPositionChanged: (mouse) => trackPick.b = Qt.point(Math.max(0, Math.min(width, mouse.x)), Math.max(0, Math.min(height, mouse.y)))
                    onReleased: {
                        if (!trackPick.drawing)
                            return;
                        trackPick.drawing = false;
                        if (Math.abs(trackPick.b.x - trackPick.a.x) < 6 || Math.abs(trackPick.b.y - trackPick.a.y) < 6)
                            return;
                        Project.trackRegion(trackPick.a.x / width, trackPick.a.y / height, trackPick.b.x / width, trackPick.b.y / height);
                    }
                }
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: 10
                    width: hint.implicitWidth + 20
                    height: 28
                    radius: 14
                    color: "#cc000000"
                    Text {
                        id: hint
                        anchors.centerIn: parent
                        color: "white"
                        font.pixelSize: Theme.fontSmall
                        text: qsTr("Drag a box around what the clip should follow · right-click or Esc cancels")
                    }
                }
                Keys.onEscapePressed: Project.cancelTrackingPick()
                onVisibleChanged: if (visible) forceActiveFocus()
            }

            Text {
                anchors.centerIn: parent
                visible: Project.duration <= 0
                text: qsTr("Import media and drag it to the timeline")
                color: Theme.textFaint
                font.pixelSize: Theme.fontLarge
            }
        }

        // Transporte
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Text {
                text: Project.formatTime(Project.playhead)
                color: Theme.accent
                font.pixelSize: Theme.fontNormal
                font.family: "monospace"
            }
            Text {
                text: " / " + Project.formatTime(Project.duration)
                color: Theme.textDim
                font.pixelSize: Theme.fontNormal
                font.family: "monospace"
            }
            Item { Layout.fillWidth: true }
            IconButton { iconName: "skip-back"; tip: qsTr("Go to start"); shortcut: Keymap.label("start"); onClicked: Project.jumpToEdge(false) }
            IconButton { iconName: "chevron-left"; tip: qsTr("Previous frame"); shortcut: Keymap.label("prevFrame"); onClicked: Project.stepFrames(-1) }
            // Play em destaque
            AbstractButton {
                id: playButton
                implicitWidth: 40
                implicitHeight: 40
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                onClicked: Project.togglePlay()
                background: Rectangle {
                    radius: 20
                    color: playButton.down ? Theme.accentDim : Theme.accent
                }
                contentItem: Item {
                    Icon {
                        anchors.centerIn: parent
                        anchors.horizontalCenterOffset: Project.playing ? 0 : 1
                        name: Project.playing ? "pause" : "play"
                        size: 18
                        color: Theme.accentInk
                    }
                }
                ToolTip.visible: hovered
                ToolTip.delay: 450
                ToolTip.text: qsTr("Play/pause") + "   " + Keymap.label("play")
            }
            IconButton { iconName: "chevron-right"; tip: qsTr("Next frame"); shortcut: Keymap.label("nextFrame"); onClicked: Project.stepFrames(1) }
            IconButton { iconName: "skip-forward"; tip: qsTr("Go to end"); shortcut: Keymap.label("end"); onClicked: Project.jumpToEdge(true) }
            Item { Layout.fillWidth: true }
            Text {
                text: Project.fps.toFixed(Project.fps % 1 === 0 ? 0 : 2) + " fps"
                color: Theme.textFaint
                font.pixelSize: Theme.fontSmall
            }
        }
    }
}
