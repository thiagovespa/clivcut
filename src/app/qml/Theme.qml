pragma Singleton
import QtQuick
import QtCore

// Identidade visual "Estúdio": grafite + âmbar, com tema claro opcional.
QtObject {
    id: theme
    property bool dark: true
    property Settings prefs: Settings {
        category: "appearance"
        property alias darkTheme: theme.dark
    }

    readonly property color bg: dark ? "#111113" : "#eef0f3"
    readonly property color panel: dark ? "#18181b" : "#ffffff"
    readonly property color rail: dark ? "#0c0c0e" : "#e4e6ea"
    readonly property color surface: dark ? "#212125" : "#f2f3f6"
    readonly property color surfaceHi: dark ? "#2b2b30" : "#e5e7ec"
    readonly property color border: dark ? "#2e2e34" : "#d9dce2"
    readonly property color text: dark ? "#ececef" : "#1a1b20"
    readonly property color textDim: dark ? "#a3a3ad" : "#5b5e69"
    readonly property color textFaint: dark ? "#6c6c76" : "#979aa4"
    readonly property color accent: dark ? "#f5a524" : "#d98500"
    readonly property color accentDim: dark ? "#c9850f" : "#b56f00"
    readonly property color accentSoft: dark ? "#33f5a524" : "#24d98500"
    readonly property color accentInk: "#1b1203"
    readonly property color danger: dark ? "#f0555b" : "#d93a40"
    readonly property color warning: dark ? "#ff8a3d" : "#e0681a"
    readonly property color success: dark ? "#4fc08d" : "#1f9d63"
    readonly property color playhead: accent
    readonly property color previewBg: dark ? "#0a0a0b" : "#d8dbe1"
    readonly property color mainLane: dark ? "#1f2a2c" : "#dfeaea"

    readonly property color clipVideo: dark ? "#34537f" : "#5878aa"
    readonly property color clipImage: dark ? "#3f5f8f" : "#6587b8"
    readonly property color clipAudio: dark ? "#2f6e57" : "#3f8f6f"
    readonly property color clipText: dark ? "#7a54a8" : "#8d68bd"
    readonly property color clipAdjust: dark ? "#a3612f" : "#c27a43"
    readonly property color clipCompound: dark ? "#4a4c8c" : "#6a6db5"
    readonly property color clipSelected: accent

    readonly property int radius: 7
    readonly property int fontSmall: 11
    readonly property int fontNormal: 13
    readonly property int fontLarge: 15
    readonly property int fontTitle: 22

    /// Cor no formato que o provedor de ícones aceita (sem '#').
    function hex(c) { return c.toString().replace("#", "").slice(-6) }
}
