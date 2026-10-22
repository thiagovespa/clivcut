import QtQuick
import QtQuick.Controls
import Clivcut

// Um clipe na timeline: selecionar, mover (inclusive entre faixas) e aparar pelas bordas.
Item {
    id: root
    property var clipData
    property var track
    property var timeline

    readonly property bool selected: clipData.selected
    readonly property bool locked: track.locked
    readonly property bool isText: clipData.type === "text"
    readonly property bool isAdjust: clipData.type === "adjust"
    readonly property bool isAudio: clipData.mediaKind === "audio" || clipData.audioOnly
    readonly property bool isImage: clipData.mediaKind === "image"
    readonly property bool isCompound: clipData.mediaKind === "compound"
    readonly property bool isSlot: clipData.mediaKind === "slot"

    // Estado de interação (pré-visualização local até soltar).
    property real dragDx: 0
    property real dragDy: 0
    property real trimStartDx: 0
    property real trimEndDx: 0

    // Acompanha o arraste de outro clipe da mesma seleção.
    readonly property real followDx: !body.drag.active && timeline.dragSet.indexOf(clipData.id) >= 0 ? timeline.dragSetDx : 0
    x: timeline.xOf(clipData.start) + dragDx + trimStartDx + followDx
    y: dragDy
    width: Math.max(4, timeline.xOf(clipData.duration) - trimStartDx + trimEndDx)
    z: body.drag.active || dragDx !== 0 || dragDy !== 0 ? 50 : (selected ? 2 : 1)

    readonly property color baseColor: isText ? Theme.clipText : isAdjust ? Theme.clipAdjust : isCompound ? Theme.clipCompound
                                       : isSlot ? Theme.surfaceHi
                                       : isAudio ? Theme.clipAudio : isImage ? Theme.clipImage : Theme.clipVideo

    Rectangle {
        id: bg
        anchors.fill: parent
        radius: 5
        color: clipData.missing ? "#5a1f27" : root.baseColor
        border.color: root.selected ? Theme.clipSelected : clipData.groupId !== "" ? Theme.warning : Qt.darker(root.baseColor, 1.4)
        border.width: root.selected ? 2 : 1
        clip: true

        // Tira de miniaturas (vídeo/imagem)
        Row {
            visible: !root.isText && !root.isAdjust && !root.isAudio && !root.isCompound && !root.isSlot && !clipData.missing
            anchors.fill: parent
            anchors.margins: 1
            readonly property real tileW: Math.max(24, height * 16 / 9)
            Repeater {
                model: parent.visible ? Math.min(80, Math.ceil(parent.width / parent.tileW)) : 0
                Image {
                    required property int index
                    width: parent.tileW
                    height: parent.height
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    cache: true
                    sourceSize.height: 72
                    readonly property real relUs: root.timeline.tOf(index * width + width / 2)
                    source: "image://thumb/" + root.clipData.assetId + "/" +
                            Math.round(root.isImage ? 0 : (root.clipData.reverse ? root.clipData.sourceIn + (root.clipData.duration - relUs) * root.clipData.speed
                                                                              : root.clipData.sourceIn + relUs * root.clipData.speed))
                    opacity: 0.9
                }
            }
        }

        // Forma de onda (áudio)
        Image {
            visible: root.isAudio
            anchors.fill: parent
            anchors.margins: 3
            asynchronous: true
            sourceSize.width: Math.min(4096, Math.max(1, Math.round(width)))
            sourceSize.height: Math.max(1, Math.round(height))
            source: root.isAudio && width > 1 ? "image://wave/" + root.clipData.assetId + "/" + Math.round(root.clipData.sourceIn) + "/" +
                                                Math.round(root.clipData.duration * Math.max(root.clipData.speed, 0.0001)) + "/7fe6b4" : ""
        }

        // Fades de áudio
        Canvas {
            anchors.fill: parent
            visible: clipData.fadeIn > 0 || clipData.fadeOut > 0
            property real fi: root.timeline.xOf(clipData.fadeIn)
            property real fo: root.timeline.xOf(clipData.fadeOut)
            onFiChanged: requestPaint()
            onFoChanged: requestPaint()
            onWidthChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d");
                ctx.reset();
                ctx.fillStyle = "rgba(0,0,0,0.35)";
                if (fi > 0) { ctx.beginPath(); ctx.moveTo(0, 0); ctx.lineTo(fi, 0); ctx.lineTo(0, height); ctx.fill(); }
                if (fo > 0) { ctx.beginPath(); ctx.moveTo(width, 0); ctx.lineTo(width - fo, 0); ctx.lineTo(width, height); ctx.fill(); }
            }
        }

        // Soltar uma mídia da biblioteca em cima do clipe troca a mídia (ou preenche o espaço).
        DropArea {
            id: mediaDrop
            anchors.fill: parent
            enabled: !root.isText && !root.isAdjust && !root.isCompound
            keys: ["clivcut/asset"]
            onDropped: (d) => {
                Project.replaceClipMedia(root.clipData.id, d.getDataAsString("clivcut/asset"));
                d.accept(Qt.CopyAction);
            }
        }
        Rectangle {
            anchors.fill: parent
            visible: mediaDrop.containsDrag || root.isSlot
            color: mediaDrop.containsDrag ? Theme.accentSoft : "transparent"
            border.color: Theme.accent
            border.width: mediaDrop.containsDrag ? 2 : 1
            opacity: mediaDrop.containsDrag ? 1 : 0.6
            radius: 5
        }

        // Rótulo
        Rectangle {
            x: 4
            y: 3
            width: Math.min(labelText.implicitWidth + 10, parent.width - 8)
            height: 16
            radius: 3
            color: "#99000000"
            visible: parent.width > 30
            Text {
                id: labelText
                anchors.verticalCenter: parent.verticalCenter
                x: 5
                width: parent.width - 8
                text: (root.clipData.audioMuted ? "🔇 " : "") + (root.clipData.groupId !== "" ? "⛓ " : "") + (root.isText ? "T  " : root.isAdjust ? "◐  " : root.isCompound ? "▣  " : root.isSlot ? "+  " : "") + root.clipData.label + "  " + Project.formatTime(root.clipData.duration).substring(0, 5)
                color: "white"
                font.pixelSize: 10
                elide: Text.ElideRight
            }
        }

        // Keyframes
        Repeater {
            model: root.clipData.keyframeTimes
            Rectangle {
                required property real modelData
                x: root.timeline.xOf(modelData) - 4
                y: bg.height - 11
                width: 7
                height: 7
                rotation: 45
                color: Theme.warning
                border.color: "black"
            }
        }
    }

    // Indicador de transição no corte esquerdo
    Rectangle {
        visible: root.clipData.transition !== ""
        x: -9
        anchors.verticalCenter: parent.verticalCenter
        width: 18
        height: 18
        radius: 4
        rotation: 45
        color: Theme.accent
        border.color: "black"
        z: 3
        Text {
            anchors.centerIn: parent
            rotation: -45
            text: "⇄"
            font.pixelSize: 10
            color: "black"
        }
        ToolTip.visible: trHover.hovered
        ToolTip.text: qsTr("Transition: %1 (%2 s)").arg(root.clipData.transition).arg((root.clipData.transitionDuration / 1e6).toFixed(1))
        HoverHandler { id: trHover }
    }

    // Corpo: selecionar e mover
    MouseArea {
        id: body
        anchors.fill: parent
        anchors.leftMargin: 7
        anchors.rightMargin: 7
        enabled: !root.locked
        cursorShape: drag.active ? Qt.ClosedHandCursor : Qt.PointingHandCursor
        drag.target: dragTarget
        drag.threshold: 4
        property point pressPos
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: (m) => {
            pressPos = Qt.point(m.x, m.y);
            if (m.button === Qt.RightButton) {
                if (!root.selected)
                    Project.select(root.clipData.id, false);
                root.timeline.openClipMenu(root.clipData.id);
                return;
            }
            if (!root.selected || (m.modifiers & (Qt.ControlModifier | Qt.ShiftModifier)))
                Project.select(root.clipData.id, (m.modifiers & (Qt.ControlModifier | Qt.ShiftModifier)) !== 0);
            // Vários selecionados: os demais acompanham o arraste.
            root.timeline.dragSet = Project.selection.length > 1 ? Project.selection : [];
        }
        onPositionChanged: (m) => {
            if (!drag.active)
                return;
            root.dragDx = m.x - pressPos.x;
            root.dragDy = m.y - pressPos.y;
            // Snap visual das bordas.
            const t = root.timeline.tOf(root.timeline.xOf(root.clipData.start) + root.dragDx);
            const s1 = root.timeline.snap(t, [root.clipData.id]);
            const s2 = root.timeline.snap(t + root.clipData.duration, [root.clipData.id]) - root.clipData.duration;
            const best = Math.abs(s1 - t) <= Math.abs(s2 - t) ? s1 : s2;
            root.dragDx = root.timeline.xOf(Math.max(0, best)) - root.timeline.xOf(root.clipData.start);
            if (root.timeline.dragSet.length > 0) {
                root.dragDy = 0; // em grupo, só no tempo
                root.timeline.dragSetDx = root.dragDx;
            }
        }
        onReleased: (m) => {
            if (root.dragDx !== 0 || root.dragDy !== 0) {
                const newStart = Math.max(0, root.clipData.start + root.timeline.tOf(root.dragDx));
                const centerY = root.parent.y + root.height / 2 + root.dragDy;
                let target = root.timeline.trackAtY(centerY + 6);
                if (target < 0 || root.timeline.trackKind(target) !== root.track.kind)
                    target = root.track.index;
                root.dragDx = 0;
                root.dragDy = 0;
                root.timeline.dragSetDx = 0;
                root.timeline.dragSet = [];
                Project.moveClip(root.clipData.id, target, newStart);
            }
            root.timeline.dragSet = [];
            root.timeline.dragSetDx = 0;
            dragTarget.x = 0;
            dragTarget.y = 0;
        }
        // Composto: abre o conteúdo para edição; os demais levam o cursor ao início.
        onDoubleClicked: root.isCompound ? Project.openCompound(root.clipData.id) : Project.seek(root.clipData.start)
        Item { id: dragTarget }
    }

    // Alças de corte
    Repeater {
        model: [true, false]
        Rectangle {
            id: handle
            required property bool modelData // true = borda inicial
            readonly property bool startEdge: modelData
            x: startEdge ? 0 : root.width - width
            width: 7
            height: root.height
            radius: 3
            visible: !root.locked && (root.selected || handleArea.containsMouse || handleArea.pressed)
            color: handleArea.pressed ? Theme.accent : "#ccffffff"
            Rectangle {
                anchors.centerIn: parent
                width: 1
                height: parent.height * 0.4
                color: "black"
            }
            MouseArea {
                id: handleArea
                anchors.fill: parent
                anchors.margins: -2
                hoverEnabled: true
                cursorShape: Qt.SizeHorCursor
                property real pressX
                onPressed: (m) => {
                    pressX = mapToItem(root.parent, m.x, 0).x;
                    Project.select(root.clipData.id, false);
                }
                onPositionChanged: (m) => {
                    if (!pressed)
                        return;
                    const dx = mapToItem(root.parent, m.x, 0).x - pressX;
                    const edgeT = handle.startEdge ? root.clipData.start : root.clipData.end;
                    const snapped = root.timeline.snap(edgeT + root.timeline.tOf(dx), [root.clipData.id]);
                    let d = root.timeline.xOf(snapped) - root.timeline.xOf(edgeT);
                    // Limites: mínimo de um quadro e material de origem disponível.
                    const minW = root.timeline.xOf(1e6 / Project.fps);
                    const full = root.timeline.xOf(root.clipData.duration);
                    if (handle.startEdge) {
                        d = Math.min(d, full - minW);
                        if (!root.isText && !root.isAdjust && !root.isImage && root.clipData.speed > 0 && !root.clipData.reverse)
                            d = Math.max(d, -root.timeline.xOf(root.clipData.sourceIn / root.clipData.speed));
                        d = Math.max(d, -root.timeline.xOf(root.clipData.start));
                        root.trimStartDx = d;
                    } else {
                        d = Math.max(d, -(full - minW));
                        if (!root.isText && !root.isAdjust && !root.isImage && root.clipData.speed > 0 && !root.clipData.reverse && root.clipData.assetDuration > 0)
                            d = Math.min(d, root.timeline.xOf((root.clipData.assetDuration - root.clipData.sourceIn) / root.clipData.speed) - full);
                        root.trimEndDx = d;
                    }
                }
                onReleased: {
                    // Zera antes de aparar: trimClip recria o modelo e destrói este delegate.
                    const id = root.clipData.id;
                    const startT = root.clipData.start + root.timeline.tOf(root.trimStartDx);
                    const endT = root.clipData.end + root.timeline.tOf(root.trimEndDx);
                    const trimStart = handle.startEdge && root.trimStartDx !== 0;
                    const trimEnd = !handle.startEdge && root.trimEndDx !== 0;
                    root.trimStartDx = 0;
                    root.trimEndDx = 0;
                    if (trimStart)
                        Project.trimClip(id, true, startT);
                    else if (trimEnd)
                        Project.trimClip(id, false, endT);
                }
            }
        }
    }
}
