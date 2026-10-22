import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Clivcut

// Tela inicial: começar um projeto pelo formato, importar, abrir ou retomar um recente.
Rectangle {
    id: root
    color: Theme.bg
    signal importRequested()
    signal openRequested()
    signal draftRequested()
    signal dismissed()
    /// Um template virou o projeto: falta escolher as mídias para os espaços.
    signal templateOpened()

    readonly property var formats: [
        { name: qsTr("Landscape"), sub: "16:9 · 1920×1080", w: 1920, h: 1080, rw: 64, rh: 36 },
        { name: qsTr("Vertical"), sub: "9:16 · 1080×1920", w: 1080, h: 1920, rw: 27, rh: 48 },
        { name: qsTr("Square"), sub: "1:1 · 1080×1080", w: 1080, h: 1080, rw: 42, rh: 42 },
        { name: qsTr("Feed"), sub: "4:5 · 1080×1350", w: 1080, h: 1350, rw: 36, rh: 45 },
        { name: qsTr("Cinema"), sub: "21:9 · 2560×1080", w: 2560, h: 1080, rw: 68, rh: 29 }
    ]

    Flickable {
        anchors.fill: parent
        contentHeight: content.implicitHeight + 80
        clip: true
        ColumnLayout {
            id: content
            width: Math.min(parent.width - 64, 900)
            x: (parent.width - width) / 2
            y: 48
            spacing: 18

            RowLayout {
                spacing: 14
                Image {
                    source: "qrc:/qt/qml/Clivcut/icons/clivcut.svg"
                    sourceSize: Qt.size(48, 48)
                }
                ColumnLayout {
                    spacing: 2
                    Text { text: "clivcut"; color: Theme.text; font.pixelSize: Theme.fontTitle + 6; font.bold: true }
                    Text { text: qsTr("Start with the video format"); color: Theme.textDim; font.pixelSize: Theme.fontLarge }
                }
            }

            Flow {
                Layout.fillWidth: true
                spacing: 12
                Repeater {
                    model: root.formats
                    Rectangle {
                        id: card
                        required property var modelData
                        width: 160
                        height: 136
                        radius: 12
                        color: cardHover.hovered ? Theme.surfaceHi : Theme.panel
                        border.color: cardHover.hovered ? Theme.accent : Theme.border
                        Column {
                            anchors.centerIn: parent
                            spacing: 10
                            Item {
                                width: 70
                                height: 50
                                anchors.horizontalCenter: parent.horizontalCenter
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: card.modelData.rw
                                    height: card.modelData.rh
                                    radius: 4
                                    color: "transparent"
                                    border.color: cardHover.hovered ? Theme.accent : Theme.textDim
                                    border.width: 2
                                }
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: card.modelData.name
                                color: Theme.text
                                font.pixelSize: Theme.fontNormal
                                font.bold: true
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: card.modelData.sub
                                color: Theme.textFaint
                                font.pixelSize: Theme.fontSmall
                            }
                        }
                        HoverHandler { id: cardHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: {
                                Project.newProject(card.modelData.w, card.modelData.h, 30);
                                root.dismissed();
                            }
                        }
                    }
                }
            }

            Text {
                Layout.topMargin: 8
                text: qsTr("Or start from a template")
                color: Theme.textDim
                font.pixelSize: Theme.fontLarge
                font.bold: true
            }
            Flow {
                Layout.fillWidth: true
                spacing: 12
                Repeater {
                    model: Project.templates
                    Rectangle {
                        id: tpl
                        required property var modelData
                        width: 214
                        height: 150
                        radius: 12
                        color: tplHover.hovered ? Theme.surfaceHi : Theme.panel
                        border.color: tplHover.hovered ? Theme.accent : Theme.border
                        readonly property real ratio: modelData.width / modelData.height
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 4
                            RowLayout {
                                spacing: 8
                                // Formato do template em miniatura.
                                Rectangle {
                                    implicitWidth: tpl.ratio >= 1 ? 40 : 40 * tpl.ratio
                                    implicitHeight: tpl.ratio >= 1 ? 40 / tpl.ratio : 40
                                    radius: 3
                                    color: "transparent"
                                    border.color: tplHover.hovered ? Theme.accent : Theme.textDim
                                    border.width: 2
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: qsTr("%n slot(s)", "", tpl.modelData.slots) + " · " + Project.formatTime(tpl.modelData.duration).substring(0, 5)
                                    color: Theme.textFaint
                                    font.pixelSize: Theme.fontSmall
                                }
                                IconButton {
                                    visible: !tpl.modelData.builtin
                                    implicitWidth: 22; implicitHeight: 22
                                    iconName: "x"
                                    iconSize: 12
                                    tip: qsTr("Delete template")
                                    onClicked: Project.deleteTemplate(tpl.modelData.id)
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: tpl.modelData.name
                                color: Theme.text
                                font.pixelSize: Theme.fontNormal
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                text: tpl.modelData.description
                                color: Theme.textDim
                                font.pixelSize: Theme.fontSmall
                                wrapMode: Text.Wrap
                                elide: Text.ElideRight
                                maximumLineCount: 3
                            }
                        }
                        HoverHandler { id: tplHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: {
                                if (Project.newFromTemplate(tpl.modelData.id)) {
                                    root.dismissed();
                                    root.templateOpened();
                                }
                            }
                        }
                    }
                }
            }

            RowLayout {
                spacing: 10
                PillButton { text: qsTr("Import media"); iconName: "plus"; primary: true; onClicked: root.importRequested() }
                PillButton { text: qsTr("Open project"); iconName: "folder-open"; onClicked: root.openRequested() }
                PillButton { text: qsTr("Import draft JSON"); iconName: "download"; onClicked: root.draftRequested() }
                Item { Layout.fillWidth: true }
                PillButton { text: qsTr("Go to the editor"); iconName: "chevron-right"; onClicked: root.dismissed() }
            }

            Text {
                visible: Project.recentProjects.length > 0
                Layout.topMargin: 12
                text: qsTr("Recent")
                color: Theme.textDim
                font.pixelSize: Theme.fontLarge
                font.bold: true
            }
            Repeater {
                model: Project.recentProjects
                Rectangle {
                    id: recent
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: 52
                    radius: Theme.radius
                    color: recentHover.hovered ? Theme.surface : "transparent"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 12
                        Icon { name: "clapperboard"; size: 20; color: Theme.accent }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Text { text: recent.modelData.name; color: Theme.text; font.pixelSize: Theme.fontNormal; font.bold: true }
                            Text {
                                Layout.fillWidth: true
                                text: recent.modelData.folder
                                color: Theme.textFaint
                                font.pixelSize: Theme.fontSmall
                                elide: Text.ElideMiddle
                            }
                        }
                        Text { text: recent.modelData.modified; color: Theme.textFaint; font.pixelSize: Theme.fontSmall }
                    }
                    HoverHandler { id: recentHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: {
                            if (Project.openProject("file://" + recent.modelData.path))
                                root.dismissed();
                        }
                    }
                }
            }
            RowLayout {
                Layout.topMargin: 12
                Layout.fillWidth: true
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Tip: drag videos into the window at any time · Ctrl+K opens the command search")
                    color: Theme.textFaint
                    font.pixelSize: Theme.fontSmall
                    elide: Text.ElideRight
                }
                Icon { name: "languages"; size: 16; color: Theme.textDim }
                ComboBox {
                    id: languageBox
                    // Primeiro item: seguir o idioma do sistema.
                    readonly property var entries: [{ code: "", name: qsTr("System default") }].concat(Project.languages)
                    implicitWidth: 200
                    focusPolicy: Qt.NoFocus
                    model: entries.map(e => e.name)
                    currentIndex: Math.max(0, entries.findIndex(e => e.code === Project.language))
                    onActivated: (i) => Project.language = entries[i].code
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Interface language")
                }
            }
        }
    }
}
