import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Clivcut

// Camada que escurece a janela e deixa só um elemento à vista, com um balão explicativo.
// Usada pelo tour guiado e pelo "Mostrar" das novidades.
Item {
    id: root
    anchors.fill: parent
    visible: false
    z: 1000

    property Item target: null
    property string title: ""
    property string body: ""
    property string stepText: ""     // "2 de 7"
    property bool canGoBack: false
    property string nextText: qsTr("Next")
    property bool showSkip: true
    readonly property int pad: 6

    signal nextClicked()
    signal backClicked()
    signal skipClicked()

    // Buraco (em coordenadas desta camada); sem alvo, o balão fica no centro.
    property rect hole: Qt.rect(width / 2, height / 2, 0, 0)
    function refresh() {
        if (!target || !target.visible) {
            hole = Qt.rect(width / 2, height / 2, 0, 0);
            return;
        }
        const p = target.mapToItem(root, 0, 0);
        hole = Qt.rect(Math.max(0, p.x - pad), Math.max(0, p.y - pad),
                       Math.min(width, target.width + pad * 2), Math.min(height, target.height + pad * 2));
    }
    onTargetChanged: refresh()
    onVisibleChanged: if (visible) refresh()
    onWidthChanged: refresh()
    onHeightChanged: refresh()
    Timer { interval: 250; repeat: true; running: root.visible; onTriggered: root.refresh() }

    readonly property color dim: "#b0000000"
    // Quatro faixas em volta do buraco (o buraco continua visível, mas não clicável).
    Rectangle { color: root.dim; x: 0; y: 0; width: root.width; height: root.hole.y }
    Rectangle { color: root.dim; x: 0; y: root.hole.y + root.hole.height; width: root.width; height: root.height - y }
    Rectangle { color: root.dim; x: 0; y: root.hole.y; width: root.hole.x; height: root.hole.height }
    Rectangle { color: root.dim; x: root.hole.x + root.hole.width; y: root.hole.y; width: root.width - x; height: root.hole.height }
    Rectangle {
        visible: root.hole.width > 0
        x: root.hole.x; y: root.hole.y; width: root.hole.width; height: root.hole.height
        color: "transparent"
        radius: 8
        border.color: Theme.accent
        border.width: 2
    }
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = true }

    // Balão: abaixo do buraco se couber, senão acima, senão ao lado; sempre dentro da janela.
    Rectangle {
        id: bubble
        width: Math.min(380, root.width - 32)
        height: content.implicitHeight + 32
        radius: 12
        color: Theme.panel
        border.color: Theme.border
        readonly property real gap: 14
        readonly property bool noHole: root.hole.width <= 0
        readonly property bool fitsBelow: root.hole.y + root.hole.height + gap + height < root.height - 8
        readonly property bool fitsAbove: root.hole.y - gap - height > 8
        readonly property bool fitsRight: root.hole.x + root.hole.width + gap + width < root.width - 8
        x: {
            let v = noHole ? (root.width - width) / 2
                  : (fitsBelow || fitsAbove) ? root.hole.x + root.hole.width / 2 - width / 2
                  : fitsRight ? root.hole.x + root.hole.width + gap : root.hole.x - gap - width;
            return Math.max(16, Math.min(root.width - width - 16, v));
        }
        y: {
            let v = noHole ? (root.height - height) / 2
                  : fitsBelow ? root.hole.y + root.hole.height + gap
                  : fitsAbove ? root.hole.y - gap - height
                  : root.hole.y + root.hole.height / 2 - height / 2;
            return Math.max(16, Math.min(root.height - height - 16, v));
        }

        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.margins: 16
            spacing: 8
            Text {
                Layout.fillWidth: true
                text: root.title
                color: Theme.text
                font.pixelSize: Theme.fontLarge
                font.bold: true
                wrapMode: Text.Wrap
            }
            Text {
                Layout.fillWidth: true
                text: root.body
                color: Theme.textDim
                font.pixelSize: Theme.fontNormal
                wrapMode: Text.Wrap
                textFormat: Text.StyledText
                lineHeight: 1.15
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 8
                Text { text: root.stepText; color: Theme.textFaint; font.pixelSize: Theme.fontSmall }
                Item { Layout.fillWidth: true }
                Button {
                    visible: root.showSkip
                    flat: true
                    text: qsTr("Skip")
                    focusPolicy: Qt.NoFocus
                    onClicked: root.skipClicked()
                }
                Button {
                    visible: root.canGoBack
                    text: qsTr("Back")
                    focusPolicy: Qt.NoFocus
                    onClicked: root.backClicked()
                }
                Button {
                    text: root.nextText
                    highlighted: true
                    focusPolicy: Qt.NoFocus
                    onClicked: root.nextClicked()
                }
            }
        }
    }

    focus: visible
    Keys.onEscapePressed: root.skipClicked()
    Keys.onRightPressed: root.nextClicked()
    Keys.onReturnPressed: root.nextClicked()
    Keys.onLeftPressed: if (root.canGoBack) root.backClicked()
}
