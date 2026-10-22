import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Clivcut

ToolBar {
    id: root
    signal newRequested()
    signal openRequested()
    signal saveRequested()
    signal saveAsRequested()
    signal importDraftRequested()
    signal exportRequested()
    signal commandsRequested()
    signal homeRequested()
    signal tourRequested()
    signal saveTemplateRequested()
    signal whatsNewRequested()
    readonly property Item searchItem: search
    readonly property Item exportItem: exportButton

    height: 48
    background: Rectangle {
        color: Theme.panel
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 6

        // Logo abre o menu do projeto.
        AbstractButton {
            id: logo
            implicitWidth: logoRow.implicitWidth + 12
            implicitHeight: 34
            hoverEnabled: true
            onClicked: fileMenu.popup(0, height)
            background: Rectangle { radius: Theme.radius; color: logo.hovered ? Theme.surface : "transparent" }
            contentItem: Row {
                id: logoRow
                spacing: 8
                leftPadding: 6
                Image {
                    anchors.verticalCenter: parent.verticalCenter
                    source: "qrc:/qt/qml/Clivcut/icons/clivcut.svg"
                    sourceSize: Qt.size(24, 24)
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "clivcut"
                    color: Theme.text
                    font.pixelSize: Theme.fontLarge
                    font.bold: true
                }
                Icon { anchors.verticalCenter: parent.verticalCenter; name: "chevron-right"; size: 14; color: Theme.textFaint; rotation: 90 }
            }
            Menu {
                id: fileMenu
                MenuItem { text: qsTr("Home"); onTriggered: root.homeRequested() }
                MenuSeparator {}
                MenuItem { text: qsTr("New project…") + "\t" + Keymap.label("newProject"); onTriggered: root.newRequested() }
                MenuItem { text: qsTr("Open…") + "\t" + Keymap.label("open"); onTriggered: root.openRequested() }
                MenuItem { text: qsTr("Save") + "\t" + Keymap.label("save"); onTriggered: root.saveRequested() }
                MenuItem { text: qsTr("Save as…") + "\t" + Keymap.label("saveAs"); onTriggered: root.saveAsRequested() }
                MenuSeparator {}
                MenuItem { text: qsTr("Import draft JSON…"); onTriggered: root.importDraftRequested() }
                MenuItem { text: qsTr("Export…") + "\t" + Keymap.label("export"); onTriggered: root.exportRequested() }
                MenuSeparator {}
                MenuItem { text: qsTr("New from template…"); onTriggered: root.homeRequested() }
                MenuItem { text: qsTr("Save as template…"); onTriggered: root.saveTemplateRequested() }
                MenuSeparator {}
                Menu {
                    title: qsTr("Help")
                    MenuItem { text: qsTr("Take the tour"); onTriggered: root.tourRequested() }
                    MenuItem { text: qsTr("What's new"); onTriggered: root.whatsNewRequested() }
                    MenuItem { text: qsTr("User guide"); onTriggered: Qt.openUrlExternally(Keymap.userGuideUrl) }
                }
                Menu {
                    title: qsTr("Language")
                    MenuItem {
                        text: qsTr("System default")
                        checkable: true
                        checked: Project.language === ""
                        onTriggered: Project.language = ""
                    }
                    Repeater {
                        model: Project.languages
                        MenuItem {
                            required property var modelData
                            text: modelData.name
                            checkable: true
                            checked: Project.language === modelData.code
                            onTriggered: Project.language = modelData.code
                        }
                    }
                }
            }
        }

        Rectangle { width: 1; height: 22; color: Theme.border }

        TextField {
            id: nameField
            Layout.preferredWidth: 220
            text: Project.projectName
            color: Theme.text
            font.pixelSize: Theme.fontNormal
            selectByMouse: true
            background: Rectangle {
                radius: Theme.radius
                color: nameField.activeFocus || nameHover.hovered ? Theme.surface : "transparent"
                border.color: nameField.activeFocus ? Theme.accent : "transparent"
            }
            HoverHandler { id: nameHover }
            onEditingFinished: {
                Project.projectName = text;
                focus = false;
            }
        }
        Rectangle {
            width: 7
            height: 7
            radius: 4
            color: Project.modified ? Theme.warning : Theme.success
            ToolTip.visible: statusHover.hovered
            ToolTip.text: Project.modified ? qsTr("Unsaved changes (recovery copy active)") : qsTr("All saved")
            HoverHandler { id: statusHover }
        }

        Item { Layout.fillWidth: true }

        // Busca de comandos
        AbstractButton {
            id: search
            Layout.fillWidth: true
            Layout.maximumWidth: 340
            Layout.minimumWidth: 160
            implicitHeight: 32
            hoverEnabled: true
            onClicked: root.commandsRequested()
            background: Rectangle {
                radius: 8
                color: Theme.surface
                border.color: search.hovered ? Theme.accent : Theme.border
            }
            contentItem: RowLayout {
                spacing: 8
                Item { width: 2 }
                Icon { name: "search"; size: 15; color: Theme.textFaint }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Search actions…")
                    color: Theme.textFaint
                    font.pixelSize: Theme.fontNormal
                }
                Rectangle {
                    radius: 4
                    color: Theme.surfaceHi
                    implicitWidth: kText.implicitWidth + 10
                    implicitHeight: 20
                    Text { id: kText; anchors.centerIn: parent; text: Keymap.label("palette"); color: Theme.textDim; font.pixelSize: 10 }
                }
                Item { width: 2 }
            }
        }

        Item { Layout.fillWidth: true }

        IconButton {
            iconName: "undo-2"
            tip: Project.canUndo ? qsTr("Undo: %1").arg(Project.undoText) : qsTr("Undo")
            shortcut: Keymap.label("undo")
            enabled: Project.canUndo
            onClicked: Project.undo()
        }
        IconButton {
            iconName: "redo-2"
            tip: Project.canRedo ? qsTr("Redo: %1").arg(Project.redoText) : qsTr("Redo")
            shortcut: Keymap.label("redo")
            enabled: Project.canRedo
            onClicked: Project.redo()
        }
        Item { width: 6 }
        Button {
            id: exportButton
            focusPolicy: Qt.NoFocus
            onClicked: root.exportRequested()
            contentItem: Row {
                spacing: 8
                Icon { anchors.verticalCenter: parent.verticalCenter; name: "upload"; size: 16; color: Theme.accentInk }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: Project.exporting ? qsTr("Exporting %1%").arg(Math.round(Project.exportProgress * 100)) : qsTr("Export")
                    color: Theme.accentInk
                    font.pixelSize: Theme.fontNormal
                    font.bold: true
                }
            }
            background: Rectangle {
                implicitWidth: 120
                implicitHeight: 34
                radius: 8
                color: exportButton.down ? Theme.accentDim : Theme.accent
            }
        }
    }
}
