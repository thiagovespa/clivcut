import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Clivcut

// Trilho vertical de seções. Clicar na seção ativa recolhe o painel.
Rectangle {
    id: root
    property int current: 0
    property bool collapsed: false
    signal settingsRequested()
    signal shortcutsRequested()

    readonly property var sections: [
        { icon: "image-play", label: qsTr("Media"), action: "panel0" },
        { icon: "type", label: qsTr("Text"), action: "panel1" },
        { icon: "captions", label: qsTr("Captions"), action: "panel2" },
        { icon: "palette", label: qsTr("Filters"), action: "panel3" },
        { icon: "arrow-right-left", label: qsTr("Transitions"), action: "panel4" },
        { icon: "sparkles", label: qsTr("Animations"), action: "panel5" },
        { icon: "file-text", label: qsTr("Transcript"), action: "panel6" },
        { icon: "wand-sparkles", label: qsTr("Effects"), action: "panel7" }
    ]
    function activate(i) {
        if (i === current && !collapsed)
            collapsed = true;
        else {
            current = i;
            collapsed = false;
        }
    }

    implicitWidth: 56
    color: Theme.rail

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 8
        anchors.bottomMargin: 8
        spacing: 2

        Repeater {
            model: root.sections
            Item {
                id: entry
                required property var modelData
                required property int index
                readonly property bool on: root.current === index && !root.collapsed
                Layout.alignment: Qt.AlignHCenter
                implicitWidth: 52
                implicitHeight: 52
                Rectangle {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: 3
                    height: entry.on ? 26 : 0
                    radius: 2
                    color: Theme.accent
                    Behavior on height { NumberAnimation { duration: 120 } }
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: 44
                    height: 44
                    radius: 10
                    color: entry.on ? Theme.accentSoft : (hover.hovered ? Theme.surface : "transparent")
                }
                Column {
                    anchors.centerIn: parent
                    spacing: 3
                    Icon {
                        anchors.horizontalCenter: parent.horizontalCenter
                        name: entry.modelData.icon
                        size: 19
                        color: entry.on ? Theme.accent : Theme.textDim
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: entry.modelData.label
                        color: entry.on ? Theme.text : Theme.textFaint
                        font.pixelSize: 9
                    }
                }
                HoverHandler { id: hover }
                TapHandler { onTapped: root.activate(entry.index) }
                ToolTip.visible: hover.hovered
                ToolTip.delay: 600
                ToolTip.text: entry.modelData.label + "   " + Keymap.label(entry.modelData.action)
            }
        }

        Item { Layout.fillHeight: true }

        IconButton {
            Layout.alignment: Qt.AlignHCenter
            iconName: "keyboard"
            tip: qsTr("Keyboard shortcuts")
            shortcut: Keymap.label("shortcuts")
            onClicked: root.shortcutsRequested()
        }
        IconButton {
            Layout.alignment: Qt.AlignHCenter
            iconName: Theme.dark ? "sun" : "moon"
            tip: Theme.dark ? qsTr("Light theme") : qsTr("Dark theme")
            onClicked: Theme.dark = !Theme.dark
        }
    }
}
