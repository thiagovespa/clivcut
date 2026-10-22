import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Clivcut

// Voz neural (Kokoro): escolha da voz e da velocidade, o botão de gerar, progresso e o
// download do modelo quando ele ainda não está no computador.
ColumnLayout {
    id: root
    spacing: 6
    property string actionText: qsTr("Generate voice")
    property bool canGenerate: true
    signal generateRequested()

    readonly property string state: Project.ttsState
    readonly property bool downloading: Project.downloadingModel === "kokoro" || Project.downloadingModel === "kokoro-voices"

    RowLayout {
        visible: root.state !== "unavailable"
        Layout.fillWidth: true
        Text { text: qsTr("Voice"); color: Theme.textDim; font.pixelSize: Theme.fontSmall; Layout.preferredWidth: 78 }
        ComboBox {
            id: voiceBox
            Layout.fillWidth: true
            focusPolicy: Qt.NoFocus
            model: Project.ttsVoices
            textRole: "label"
            valueRole: "id"
            currentIndex: Project.ttsVoices.findIndex(v => v.id === Project.ttsVoice)
            onActivated: Project.ttsVoice = currentValue
        }
    }
    PropSlider {
        visible: root.state !== "unavailable"
        label: qsTr("Speed")
        from: 0.5; to: 2; stepSize: 0.05; decimals: 2; suffix: "×"
        value: Project.ttsSpeed
        onMoved: (v) => Project.ttsSpeed = v
    }
    Button {
        visible: root.state === "ready"
        Layout.fillWidth: true
        enabled: root.canGenerate
        text: root.actionText
        focusPolicy: Qt.NoFocus
        onClicked: root.generateRequested()
    }
    RowLayout {
        visible: root.state === "processing"
        Layout.fillWidth: true
        ProgressBar { Layout.fillWidth: true; value: Math.max(0, Project.ttsProgress) }
        Button { text: qsTr("Cancel"); focusPolicy: Qt.NoFocus; onClicked: Project.cancelSpeech() }
    }
    ProgressBar {
        visible: root.state === "needsModel" && root.downloading
        Layout.fillWidth: true
        value: Project.modelDownloadProgress
    }
    PillButton {
        visible: root.state === "needsModel" && !root.downloading
        enabled: Project.downloadingModel === ""
        text: qsTr("Download AI voice (205 MB)")
        iconName: "download"
        onClicked: Project.downloadTtsModels()
    }
    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.textFaint
        font.pixelSize: Theme.fontSmall
        text: root.state === "unavailable" ? qsTr("Not available in this build (compiled without the AI voice).")
              : root.state === "needsModel" ? (root.downloading ? qsTr("Downloading the AI voice…")
                                               : qsTr("Requires the AI voice (Kokoro, Apache-2.0), downloaded only once."))
              : root.state === "processing" ? qsTr("Generating the voice… %1%").arg(Math.round(Math.max(0, Project.ttsProgress) * 100))
              : qsTr("Natural narration generated on your computer. The audio lands on a Narration track, at the time of each text.")
    }
}
