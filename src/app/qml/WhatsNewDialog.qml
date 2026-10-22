import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Clivcut

// Novidades: os destaques das versões que a pessoa ainda não viu (no máximo as 3 mais
// recentes), cada um com "Mostrar" para levar até o recurso.
Popup {
    id: root
    modal: true
    focus: true
    width: Math.min(640, parent ? parent.width - 80 : 640)
    height: Math.min(720, parent ? parent.height - 80 : 720, list.implicitHeight + 140)
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? (parent.height - height) / 2 : 0
    padding: 16
    background: Rectangle { radius: 12; color: Theme.panel; border.color: Theme.border }

    /// Versão a partir da qual mostrar (exclusiva); vazia = todas.
    property string since: ""
    /// Convite ao tour (para quem atualizou e nunca o fez).
    property bool offerTour: false
    readonly property int maxVersions: 3

    signal showRequested(var feature)
    signal tourRequested()

    // Destaques por versão, da mais nova para a mais antiga. "show": área a destacar
    // (rail, media, preview, inspector, timeline, search, export) e painel a abrir (opcional).
    readonly property var releases: [
        { version: "0.4.0", items: [
            { title: qsTr("AI voice narration"), text: qsTr("Turn texts and captions into natural speech, generated on your computer, in Portuguese, Spanish, English and more: select a text and use “Read aloud” on the Text tab, or “Read all captions” in the Captions panel."), show: { area: "rail", panel: 2 } }
        ] },
        { version: "0.3.0", items: [
            { title: qsTr("Guided tour and this window"), text: qsTr("A short tour of the editor for first-time users, and this summary after each update. Both live in the menu, under Help."), show: { area: "search" } },
            { title: qsTr("Compound clips"), text: qsTr("Alt+G combines clips from several tracks into one clip that you can move, cut and style as a whole; double-click it to edit what is inside."), show: { area: "timeline" } },
            { title: qsTr("Motion tracking"), text: qsTr("Make a text or sticker follow something that moves: select it and use “Follow the video below” on the Video tab."), show: { area: "inspector" } },
            { title: qsTr("Stabilization"), text: qsTr("Smooth out shaky footage with one slider on the Video tab; the analysis runs once in the background."), show: { area: "inspector" } },
            { title: qsTr("Music separation"), text: qsTr("Split a song into vocals, drums, bass and other instruments (or just the vocals or the instrumental), locally with AI, from the Audio tab."), show: { area: "inspector" } },
            { title: qsTr("Beat markers"), text: qsTr("Mark the beats of a song (every beat, 2 beats or bar) from the Audio tab; clips snap to them so cuts land on the beat."), show: { area: "inspector" } }
        ] },
        { version: "0.2.0", items: [
            { title: qsTr("Edit by transcript"), text: qsTr("Transcribe a video and delete words (or all filler words) to cut them from the timeline."), show: { area: "rail", panel: 6 } },
            { title: qsTr("Color grading and scopes"), text: qsTr("Tone curves, HSL and color wheels on the Adjust tab, with histogram, waveform and vectorscope over the player."), show: { area: "inspector" } },
            { title: qsTr("Video effects and transitions"), text: qsTr("12 effects you can apply to clips or as effect layers, and 9 new transitions."), show: { area: "rail", panel: 7 } },
            { title: qsTr("Shapes, curved text and stickers"), text: qsTr("Speech bubbles and shapes, curved text, highlighted *words* and a sticker library in the Text panel."), show: { area: "rail", panel: 1 } },
            { title: qsTr("Voice tools"), text: qsTr("AI voice isolation, voice effects, music ducking under speech and voiceover recording from the microphone."), show: { area: "inspector" } },
            { title: qsTr("Faster export"), text: qsTr("GPU encoding, transparent video, audio-only and current-frame export, plus a render cache for smooth playback."), show: { area: "export" } }
        ] }
    ]

    /** Compara versões "a.b.c": negativo se a < b. */
    function compareVersions(a, b) {
        const pa = a.split(".").map(Number), pb = b.split(".").map(Number);
        for (let i = 0; i < Math.max(pa.length, pb.length); ++i) {
            const d = (pa[i] || 0) - (pb[i] || 0);
            if (d !== 0)
                return d;
        }
        return 0;
    }
    readonly property var current: Qt.application.version
    /// Versões a mostrar: lançadas até a atual e mais novas que `since`.
    readonly property var pending: releases.filter(r => compareVersions(r.version, current) <= 0
                                                        && (since === "" || compareVersions(r.version, since) > 0))
    readonly property var shown: pending.slice(0, maxVersions)
    function hasNews(fromVersion) {
        return releases.some(r => compareVersions(r.version, current) <= 0 && compareVersions(r.version, fromVersion) > 0);
    }

    contentItem: ColumnLayout {
        spacing: 12
        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: qsTr("What's new")
                color: Theme.text
                font.pixelSize: Theme.fontLarge + 2
                font.bold: true
            }
            IconButton { iconName: "x"; tip: qsTr("Close"); onClicked: root.close() }
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                id: list
                width: parent.width
                spacing: 6
                Repeater {
                    model: root.shown
                    ColumnLayout {
                        id: release
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 6
                        Text {
                            Layout.topMargin: 6
                            text: qsTr("New in %1").arg(release.modelData.version)
                            color: Theme.accent
                            font.pixelSize: Theme.fontNormal
                            font.bold: true
                        }
                        Repeater {
                            model: release.modelData.items
                            Rectangle {
                                id: card
                                required property var modelData
                                Layout.fillWidth: true
                                implicitHeight: cardRow.implicitHeight + 20
                                radius: 8
                                color: Theme.surface
                                RowLayout {
                                    id: cardRow
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 10
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Text {
                                            Layout.fillWidth: true
                                            text: card.modelData.title
                                            color: Theme.text
                                            font.pixelSize: Theme.fontNormal
                                            font.bold: true
                                            wrapMode: Text.Wrap
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: card.modelData.text
                                            color: Theme.textDim
                                            font.pixelSize: Theme.fontSmall
                                            wrapMode: Text.Wrap
                                        }
                                    }
                                    Button {
                                        visible: card.modelData.show !== undefined
                                        text: qsTr("Show me")
                                        focusPolicy: Qt.NoFocus
                                        onClicked: {
                                            root.close();
                                            root.showRequested(card.modelData);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                Text {
                    visible: root.pending.length > root.shown.length
                    Layout.topMargin: 8
                    text: qsTr("There is more from older versions in the full history.")
                    color: Theme.textFaint
                    font.pixelSize: Theme.fontSmall
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Button {
                flat: true
                text: qsTr("Full history")
                focusPolicy: Qt.NoFocus
                onClicked: Qt.openUrlExternally("https://github.com/thiagovespa/clivcut/releases")
            }
            Item { Layout.fillWidth: true }
            Button {
                visible: root.offerTour
                text: qsTr("Take the tour")
                focusPolicy: Qt.NoFocus
                onClicked: {
                    root.close();
                    root.tourRequested();
                }
            }
            Button {
                text: qsTr("Close")
                highlighted: true
                focusPolicy: Qt.NoFocus
                onClicked: root.close()
            }
        }
    }
}
