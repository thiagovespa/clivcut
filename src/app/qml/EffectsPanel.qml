import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Clivcut

// Efeitos de vídeo: com um clipe selecionado, aplica nele; sem seleção, cria uma
// camada de efeito que afeta tudo o que está abaixo, a partir do cursor.
Flickable {
    id: root
    readonly property var sel: Project.selectedClip
    readonly property bool hasTarget: sel.id !== undefined && sel.mediaKind !== "audio"
    readonly property string current: hasTarget ? (sel.effect || "") : ""
    readonly property var glyphs: ({
        shake: "≋", blur: "◌", glitch: "▚", rgbSplit: "◑", vhs: "▤", glow: "✦",
        grain: "⁘", pixelate: "▦", mirror: "◫", snow: "❄", zoomPulse: "◎", strobe: "ϟ"
    })

    contentHeight: col.implicitHeight + 20
    clip: true
    ScrollBar.vertical: ScrollBar {}

    ColumnLayout {
        id: col
        x: 10
        y: 10
        width: root.width - 20
        spacing: 8

        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textFaint
            font.pixelSize: Theme.fontSmall
            text: root.hasTarget ? qsTr("Click an effect to apply it to the selected clip; click it again to remove.")
                                 : qsTr("No clip selected: the effect is added as an effect layer from the playhead and affects everything below it.")
        }

        Flow {
            Layout.fillWidth: true
            spacing: 6
            Repeater {
                model: Project.videoEffects
                TileButton {
                    required property var modelData
                    glyph: root.glyphs[modelData.id] || "✶"
                    caption: modelData.name
                    selected: root.current === modelData.id
                    onClicked: {
                        if (!root.hasTarget)
                            Project.addEffectClip(modelData.id);
                        else
                            Project.setClipProperty(root.sel.id, "effect", selected ? "" : modelData.id);
                    }
                }
            }
        }

        PropSlider {
            visible: root.current !== ""
            label: qsTr("Intensity")
            from: 0; to: 1; displayScale: 100; decimals: 0; suffix: "%"
            value: root.hasTarget ? root.sel.effectIntensity : 0.6
            onMoved: (v) => Project.setClipProperty(root.sel.id, "effectIntensity", v)
        }
    }
}
