import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Clivcut

Rectangle {
    id: root
    signal importRequested()
    signal importDraftRequested()
    signal relinkRequested(string assetId)

    color: Theme.panel

    property int section: 0

    readonly property var sel: Project.selectedClip
    readonly property bool hasSel: sel.id !== undefined

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Cabeçalho da seção (escolhida no trilho lateral).
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 10
            Layout.topMargin: 12
            Layout.bottomMargin: 4
            Text {
                Layout.fillWidth: true
                text: [qsTr("Media"), qsTr("Text"), qsTr("Captions"), qsTr("Filters"), qsTr("Transitions"), qsTr("Animations"), qsTr("Transcript"), qsTr("Effects")][root.section]
                color: Theme.text
                font.pixelSize: Theme.fontLarge
                font.bold: true
            }
            IconButton {
                visible: root.section === 0
                iconName: "plus"
                tip: qsTr("Import media")
                shortcut: Keymap.label("import")
                onClicked: root.importRequested()
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.section

            // ---------------- Mídia ----------------
            ColumnLayout {
                spacing: 8
                // Área de importação: clique ou arraste arquivos.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.leftMargin: 10
                    Layout.rightMargin: 10
                    implicitHeight: Project.assets.length === 0 ? 150 : 44
                    radius: 10
                    color: dropHover.hovered ? Theme.accentSoft : "transparent"
                    border.color: dropHover.hovered ? Theme.accent : Theme.border
                    border.width: 1
                    Behavior on implicitHeight { NumberAnimation { duration: 150 } }
                    Column {
                        anchors.centerIn: parent
                        spacing: 8
                        Icon {
                            anchors.horizontalCenter: parent.horizontalCenter
                            visible: Project.assets.length === 0
                            name: "download"
                            size: 30
                            color: Theme.textFaint
                        }
                        Row {
                            anchors.horizontalCenter: parent.horizontalCenter
                            spacing: 6
                            Icon { name: "plus"; size: 16; color: Theme.accent }
                            Text { text: qsTr("Import media"); color: Theme.text; font.pixelSize: Theme.fontNormal; font.bold: true }
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            visible: Project.assets.length === 0
                            width: 200
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            text: qsTr("or drag videos, audio and images into the window")
                            color: Theme.textFaint
                            font.pixelSize: Theme.fontSmall
                        }
                    }
                    HoverHandler { id: dropHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: root.importRequested() }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: Project.assets.length === 0
                }

                GridView {
                    id: grid
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.leftMargin: 10
                    visible: Project.assets.length > 0
                    clip: true
                    cellWidth: Math.floor((width - 4) / Math.max(1, Math.floor((width - 4) / 150)))
                    cellHeight: cellWidth * 9 / 16 + 30
                    model: Project.assets
                    ScrollBar.vertical: ScrollBar {}

                    delegate: Item {
                        id: tile
                        required property var modelData
                        width: grid.cellWidth
                        height: grid.cellHeight

                        Rectangle {
                            id: card
                            z: 1 // acima da área de arraste: o botão "+" recebe cliques
                            anchors.fill: parent
                            anchors.margins: 4
                            radius: Theme.radius
                            color: Theme.surface
                            border.color: dragArea.containsMouse ? Theme.accent : "transparent"
                            clip: true

                            Image {
                                id: thumb
                                width: parent.width
                                height: parent.width * 9 / 16
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                visible: tile.modelData.kind !== "audio"
                                source: tile.modelData.kind !== "audio" ? "image://thumb/" + tile.modelData.id + "/" + Math.min(1000000, tile.modelData.duration / 2) : ""
                                sourceSize.width: 240
                            }
                            Rectangle {
                                visible: tile.modelData.kind === "audio"
                                width: parent.width
                                height: parent.width * 9 / 16
                                color: Theme.clipAudio
                                Image {
                                    anchors.fill: parent
                                    anchors.margins: 6
                                    source: tile.modelData.kind === "audio" ? "image://wave/" + tile.modelData.id + "/0/" + tile.modelData.duration + "/bfffe3" : ""
                                    sourceSize: Qt.size(240, 60)
                                    asynchronous: true
                                }
                            }
                            Rectangle {
                                visible: tile.modelData.kind !== "image"
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 4
                                width: durText.implicitWidth + 8
                                height: 16
                                radius: 3
                                color: "#b0000000"
                                Text {
                                    id: durText
                                    anchors.centerIn: parent
                                    text: Project.formatTime(tile.modelData.duration).substring(0, 5)
                                    color: "white"
                                    font.pixelSize: 10
                                }
                            }
                            Rectangle {
                                visible: tile.modelData.uses > 0
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.margins: 4
                                width: 16
                                height: 16
                                radius: 8
                                color: Theme.accent
                                Text {
                                    anchors.centerIn: parent
                                    text: "✓"
                                    color: "black"
                                    font.pixelSize: 10
                                }
                            }
                            // Proxy: pronto (P), na fila (P…) ou gerando (barra).
                            Rectangle {
                                visible: tile.modelData.proxyState === "ready" || tile.modelData.proxyState === "queued"
                                anchors.left: parent.left
                                anchors.bottom: thumb.bottom
                                anchors.margins: 4
                                width: proxyText.implicitWidth + 8
                                height: 16
                                radius: 3
                                color: tile.modelData.proxyState === "ready" ? Theme.accent : "#b0000000"
                                Text {
                                    id: proxyText
                                    anchors.centerIn: parent
                                    text: tile.modelData.proxyState === "ready" ? "P" : "P…"
                                    color: tile.modelData.proxyState === "ready" ? "black" : Theme.textDim
                                    font.pixelSize: 10
                                    font.bold: true
                                }
                                ToolTip.visible: proxyHover.hovered
                                ToolTip.text: tile.modelData.proxyState === "ready" ? qsTr("Proxy ready: lightweight playback")
                                                                                   : qsTr("Proxy queued")
                                HoverHandler { id: proxyHover }
                            }
                            ProgressBar {
                                visible: tile.modelData.proxyState === "building"
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: thumb.bottom
                                anchors.margins: 4
                                value: tile.modelData.proxyProgress
                                ToolTip.visible: visible && barHover.hovered
                                ToolTip.text: qsTr("Generating proxy… %1%").arg(Math.round(value * 100))
                                HoverHandler { id: barHover }
                            }
                            Text {
                                anchors.bottom: parent.bottom
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.margins: 6
                                text: tile.modelData.missing ? "⚠ " + tile.modelData.name : tile.modelData.name
                                color: tile.modelData.missing ? Theme.danger : Theme.textDim
                                font.pixelSize: Theme.fontSmall
                                elide: Text.ElideMiddle
                            }
                            Rectangle {
                                visible: dragArea.containsMouse
                                anchors.right: parent.right
                                anchors.bottom: thumb.bottom
                                anchors.margins: 4
                                width: 22
                                height: 22
                                radius: 11
                                color: Theme.accent
                                Text {
                                    anchors.centerIn: parent
                                    text: "+"
                                    color: "black"
                                    font.bold: true
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: Project.addAssetToTimeline(tile.modelData.id, -1, -1)
                                }
                                z: 2
                            }
                        }

                        Menu {
                            id: tileMenu
                            MenuItem {
                                text: qsTr("Add to timeline")
                                enabled: !tile.modelData.missing
                                onTriggered: Project.addAssetToTimeline(tile.modelData.id, -1, -1)
                            }
                            MenuItem {
                                text: tile.modelData.proxyState === "ready" ? qsTr("Proxy ready") : qsTr("Generate proxy")
                                visible: tile.modelData.kind === "video"
                                height: visible ? implicitHeight : 0
                                enabled: tile.modelData.proxyState === "none" && !tile.modelData.missing
                                onTriggered: Project.generateProxy(tile.modelData.id)
                            }
                            MenuItem {
                                text: qsTr("Relink media…")
                                onTriggered: root.relinkRequested(tile.modelData.id)
                            }
                        }
                        MouseArea {
                            id: dragArea
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onClicked: (m) => { if (m.button === Qt.RightButton) tileMenu.popup() }
                            drag.target: dragProxy
                            onDoubleClicked: {
                                if (tile.modelData.missing)
                                    root.relinkRequested(tile.modelData.id);
                                else
                                    Project.addAssetToTimeline(tile.modelData.id, -1, -1);
                            }
                        }
                        // Arraste nativo (atravessa painéis) com o id da mídia.
                        Item {
                            id: dragProxy
                            width: 1
                            height: 1
                            Drag.active: dragArea.drag.active
                            Drag.dragType: Drag.Automatic
                            Drag.supportedActions: Qt.CopyAction
                            Drag.mimeData: { "clivcut/asset": tile.modelData.id }
                            Drag.keys: ["clivcut/asset"]
                        }
                    }
                }
            }

            // ---------------- Texto ----------------
            Flickable {
                contentHeight: textCol.implicitHeight + 20
                clip: true
                ColumnLayout {
                    id: textCol
                    x: 10
                    y: 10
                    width: parent.width - 20
                    SectionLabel { text: qsTr("Add text") }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 8
                        TileButton { glyph: "Aa"; caption: qsTr("Default text"); onClicked: Project.addText("", "") }
                        TileButton { glyph: "T"; caption: qsTr("Title"); tint: "#3a2a55"; onClicked: Project.addText("", "title") }
                        TileButton { glyph: "▭"; caption: qsTr("Subtitle"); tint: "#202024"; onClicked: Project.addText("", "subtitle") }
                        TileButton { glyph: "★"; caption: qsTr("Highlight"); tint: "#4d4310"; onClicked: Project.addText("", "highlight") }
                    }
                    SectionLabel { text: qsTr("Shapes") }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: [
                                { kind: "rect", glyph: "▬", name: qsTr("Rectangle") },
                                { kind: "rounded", glyph: "▢", name: qsTr("Rounded") },
                                { kind: "circle", glyph: "●", name: qsTr("Circle") },
                                { kind: "triangle", glyph: "▲", name: qsTr("Triangle") },
                                { kind: "star", glyph: "★", name: qsTr("Star") },
                                { kind: "arrow", glyph: "➜", name: qsTr("Arrow") },
                                { kind: "heart", glyph: "♥", name: qsTr("Heart") },
                                { kind: "bubble", glyph: "❝", name: qsTr("Speech bubble") }
                            ]
                            TileButton {
                                required property var modelData
                                glyph: modelData.glyph
                                caption: modelData.name
                                onClicked: Project.addShape(modelData.kind)
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        Layout.topMargin: 12
                        wrapMode: Text.Wrap
                        color: Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                        text: qsTr("Text is added at the timeline playhead, on a text track. Edit the content and style in the right panel.")
                    }
                }
            }

            // ---------------- Legendas ----------------
            CaptionsPanel {}

            // ---------------- Filtros ----------------
            FiltersPanel {}

            // ---------------- Transições ----------------
            Flickable {
                contentHeight: trCol.implicitHeight + 20
                clip: true
                ColumnLayout {
                    id: trCol
                    x: 10
                    y: 10
                    width: parent.width - 20
                    SectionLabel { text: qsTr("In transition of the selected clip") }
                    Text {
                        Layout.fillWidth: true
                        visible: !root.hasSel || root.sel.type !== "media"
                        wrapMode: Text.Wrap
                        color: Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                        text: qsTr("Select a clip that comes right after another on the same track.")
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 8
                        enabled: root.hasSel && root.sel.type === "media"
                        opacity: enabled ? 1 : 0.4
                        Repeater {
                            model: [
                                { kind: "none", glyph: "∅", name: qsTr("None", "transition, mask or animation") },
                                { kind: "dissolve", glyph: "◐", name: qsTr("Dissolve") },
                                { kind: "fadeBlack", glyph: "■", name: qsTr("Darken") },
                                { kind: "wipeLeft", glyph: "⇥", name: qsTr("Wipe →") },
                                { kind: "wipeRight", glyph: "⇤", name: qsTr("Wipe ←") },
                                { kind: "slideLeft", glyph: "⇠", name: qsTr("Slide ←") },
                                { kind: "slideRight", glyph: "⇢", name: qsTr("Slide →") },
                                { kind: "circleOpen", glyph: "◎", name: qsTr("Circle") },
                                { kind: "zoomIn", glyph: "⤢", name: qsTr("Zoom") },
                                { kind: "zoomOut", glyph: "⤡", name: qsTr("Zoom out") },
                                { kind: "slideUp", glyph: "⇡", name: qsTr("Slide ↑") },
                                { kind: "slideDown", glyph: "⇣", name: qsTr("Slide ↓") },
                                { kind: "wipeUp", glyph: "⤒", name: qsTr("Wipe ↑") },
                                { kind: "wipeDown", glyph: "⤓", name: qsTr("Wipe ↓") },
                                { kind: "spin", glyph: "↻", name: qsTr("Spin") },
                                { kind: "flashWhite", glyph: "☀", name: qsTr("Flash") },
                                { kind: "glitch", glyph: "▚", name: qsTr("Glitch") },
                                { kind: "blurDissolve", glyph: "◌", name: qsTr("Blur") }
                            ]
                            TileButton {
                                required property var modelData
                                glyph: modelData.glyph
                                caption: modelData.name
                                selected: root.hasSel && (root.sel.transition === modelData.kind || (modelData.kind === "none" && !root.sel.transition))
                                onClicked: Project.setTransition(root.sel.id, modelData.kind, root.sel.transitionDuration > 0 ? root.sel.transitionDuration : 500000)
                            }
                        }
                    }
                    PropSlider {
                        visible: root.hasSel && root.sel.transition !== undefined && root.sel.transition !== ""
                        label: qsTr("Duration")
                        from: 0.1
                        to: 3
                        stepSize: 0.1
                        decimals: 1
                        suffix: "s"
                        value: root.hasSel ? (root.sel.transitionDuration || 500000) / 1e6 : 0.5
                        onMoved: (v) => Project.setTransition(root.sel.id, root.sel.transition, v * 1e6)
                    }
                }
            }

            // ---------------- Animações ----------------
            Flickable {
                contentHeight: anCol.implicitHeight + 20
                clip: true
                ColumnLayout {
                    id: anCol
                    x: 10
                    y: 10
                    width: parent.width - 20
                    Text {
                        Layout.fillWidth: true
                        visible: !root.hasSel
                        wrapMode: Text.Wrap
                        color: Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                        text: qsTr("Select a clip to apply in and out animations.")
                    }
                    Repeater {
                        model: [{ isIn: true, title: qsTr("In") }, { isIn: false, title: qsTr("Out") }]
                        ColumnLayout {
                            id: animGroup
                            required property var modelData
                            Layout.fillWidth: true
                            enabled: root.hasSel
                            opacity: enabled ? 1 : 0.4
                            readonly property string current: root.hasSel ? (modelData.isIn ? root.sel.animIn : root.sel.animOut) : "none"
                            readonly property real dur: root.hasSel ? (modelData.isIn ? root.sel.animInDuration : root.sel.animOutDuration) : 500000
                            SectionLabel { text: animGroup.modelData.title }
                            Flow {
                                Layout.fillWidth: true
                                spacing: 8
                                Repeater {
                                    model: [
                                        { kind: "none", glyph: "∅", name: qsTr("None", "transition, mask or animation") },
                                        { kind: "fade", glyph: "◐", name: qsTr("Fade") },
                                        { kind: "zoom", glyph: "⤢", name: qsTr("Zoom") },
                                        { kind: "slideUp", glyph: "↑", name: qsTr("Rise") },
                                        { kind: "slideDown", glyph: "↓", name: qsTr("Drop") },
                                        { kind: "slideLeft", glyph: "←", name: qsTr("Left") },
                                        { kind: "slideRight", glyph: "→", name: qsTr("Right") }
                                    ]
                                    TileButton {
                                        required property var modelData
                                        glyph: modelData.glyph
                                        caption: modelData.name
                                        selected: animGroup.current === modelData.kind
                                        onClicked: Project.setAnimation(root.sel.id, animGroup.modelData.isIn, modelData.kind, animGroup.dur)
                                    }
                                }
                            }
                            PropSlider {
                                visible: animGroup.current !== "none"
                                label: qsTr("Duration")
                                from: 0.1
                                to: 3
                                stepSize: 0.1
                                decimals: 1
                                suffix: "s"
                                value: animGroup.dur / 1e6
                                onMoved: (v) => Project.setAnimation(root.sel.id, animGroup.modelData.isIn, animGroup.current, v * 1e6)
                            }
                        }
                    }
                }
            }
        
            // ---------------- Transcrição ----------------
            TranscriptPanel {}

            // ---------------- Efeitos ----------------
            EffectsPanel {}
        }
    }
}
