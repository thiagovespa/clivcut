import QtQuick
import Clivcut

// Tour guiado da primeira abertura: um passo por área da janela.
Item {
    id: root
    anchors.fill: parent
    readonly property bool running: spot.visible

    property Item railItem
    property Item mediaItem
    property Item previewItem
    property Item inspectorItem
    property Item timelineItem
    property Item searchItem
    property Item exportItem

    signal finished()

    readonly property var steps: [
        { target: null, title: qsTr("Welcome to clivcut"),
          body: qsTr("A one-minute tour of where things are. You can skip it now and take it again later from the menu (Help → Take the tour).") },
        { target: railItem, title: qsTr("Panels"),
          body: qsTr("Media, text and stickers, captions, filters, transitions, animations, transcript and effects. <b>Alt+1…8</b> switch between them; clicking the open one hides it.") },
        { target: mediaItem, title: qsTr("Your media"),
          body: qsTr("Import videos, photos and music here (or drag files into the window), then drag them to the timeline. In the other panels, click an item to add it.") },
        { target: previewItem, title: qsTr("Player"),
          body: qsTr("Shows exactly what will be exported. Drag in it to move the selected clip; <b>Ctrl+wheel</b> resizes it.") },
        { target: inspectorItem, title: qsTr("Inspector"),
          body: qsTr("Select a clip to edit it here: position, color, mask, keyframes, audio and speed. The advanced tools are here too: <b>stabilization</b> and <b>tracking</b> on the Video tab, <b>music separation</b> and <b>beat markers</b> on the Audio tab.") },
        { target: timelineItem, title: qsTr("Timeline"),
          body: qsTr("The main track sits at the bottom and closes gaps by itself. <b>Ctrl+B</b> splits, <b>Q/W</b> delete left/right of the playhead, <b>Alt+G</b> combines clips into a compound clip (double-click opens it).") },
        { target: searchItem, title: qsTr("Find any action"),
          body: qsTr("<b>Ctrl+K</b> searches every command by name, with its shortcut — the quickest way to discover what clivcut can do.") },
        { target: exportItem, title: qsTr("Export"),
          body: qsTr("Export the video (on the GPU when possible), only the audio, a transparent video or the current frame. That's it — enjoy!") }
    ]
    property int index: 0

    function start(at) {
        index = Math.max(0, Math.min(steps.length - 1, at || 0));
        show();
    }
    function show() {
        const s = steps[index];
        spot.target = s.target;
        spot.title = s.title;
        spot.body = s.body;
        spot.stepText = qsTr("%1 of %2").arg(index + 1).arg(steps.length);
        spot.canGoBack = index > 0;
        spot.nextText = index === steps.length - 1 ? qsTr("Done") : qsTr("Next");
        spot.visible = true;
        spot.forceActiveFocus();
    }
    function close() {
        spot.visible = false;
        root.finished();
    }

    Spotlight {
        id: spot
        onNextClicked: {
            if (root.index >= root.steps.length - 1) {
                root.close();
                return;
            }
            root.index++;
            root.show();
        }
        onBackClicked: {
            root.index--;
            root.show();
        }
        onSkipClicked: root.close()
    }
}
