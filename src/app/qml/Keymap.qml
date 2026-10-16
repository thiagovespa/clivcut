pragma Singleton
import QtQuick
import QtCore
import Clivcut

// Registro dos atalhos: padrões de cada ação e as trocas do usuário (salvas nas preferências).
QtObject {
    id: keymap

    property Settings store: Settings {
        category: "shortcuts"
        property string custom: "{}" // { idDaAção: ["Ctrl+B", ...] }
    }
    readonly property var overrides: {
        try {
            return JSON.parse(store.custom);
        } catch (e) {
            return {};
        }
    }

    // edit: desativado enquanto se digita num campo de texto.
    readonly property var actions: [
        { id: "play", group: qsTr("Playback"), name: qsTr("Play / pause"), keys: ["Space"], edit: true },
        { id: "prevFrame", group: qsTr("Playback"), name: qsTr("Previous frame"), keys: ["Left"], edit: true },
        { id: "nextFrame", group: qsTr("Playback"), name: qsTr("Next frame"), keys: ["Right"], edit: true },
        { id: "back1s", group: qsTr("Playback"), name: qsTr("Back 1 second"), keys: ["Shift+Left"], edit: true },
        { id: "forward1s", group: qsTr("Playback"), name: qsTr("Forward 1 second"), keys: ["Shift+Right"], edit: true },
        { id: "start", group: qsTr("Playback"), name: qsTr("Go to start"), keys: ["Home"], edit: true },
        { id: "end", group: qsTr("Playback"), name: qsTr("Go to end"), keys: ["End"], edit: true },

        { id: "split", group: qsTr("Editing"), name: qsTr("Split at playhead"), keys: ["Ctrl+B"], edit: true },
        { id: "deleteLeft", group: qsTr("Editing"), name: qsTr("Delete left of playhead"), keys: ["Q"], edit: true },
        { id: "deleteRight", group: qsTr("Editing"), name: qsTr("Delete right of playhead"), keys: ["W"], edit: true },
        { id: "delete", group: qsTr("Editing"), name: qsTr("Delete selection"), keys: ["Del", "Backspace"], edit: true },
        { id: "rippleDelete", group: qsTr("Editing"), name: qsTr("Ripple delete"), keys: ["Shift+Del"], edit: true },
        { id: "undo", group: qsTr("Editing"), name: qsTr("Undo"), keys: ["Ctrl+Z"], edit: true },
        { id: "redo", group: qsTr("Editing"), name: qsTr("Redo"), keys: ["Ctrl+Shift+Z", "Ctrl+Y"], edit: true },
        { id: "duplicate", group: qsTr("Editing"), name: qsTr("Duplicate"), keys: ["Ctrl+D"], edit: true },
        { id: "copy", group: qsTr("Editing"), name: qsTr("Copy"), keys: ["Ctrl+C"], edit: true },
        { id: "cut", group: qsTr("Editing"), name: qsTr("Cut"), keys: ["Ctrl+X"], edit: true },
        { id: "paste", group: qsTr("Editing"), name: qsTr("Paste at playhead"), keys: ["Ctrl+V"], edit: true },
        { id: "copyAttributes", group: qsTr("Editing"), name: qsTr("Copy attributes"), keys: ["Ctrl+Alt+C"], edit: true },
        { id: "pasteAttributes", group: qsTr("Editing"), name: qsTr("Paste attributes"), keys: ["Ctrl+Alt+V"], edit: true },
        { id: "group", group: qsTr("Editing"), name: qsTr("Group clips"), keys: ["Ctrl+G"], edit: true },
        { id: "ungroup", group: qsTr("Editing"), name: qsTr("Ungroup"), keys: ["Ctrl+Shift+G"], edit: true },
        { id: "detachAudio", group: qsTr("Editing"), name: qsTr("Detach audio from video"), keys: ["Ctrl+Shift+D"], edit: true },
        { id: "freeze", group: qsTr("Editing"), name: qsTr("Freeze frame"), keys: ["Alt+F"], edit: true },
        { id: "addText", group: qsTr("Editing"), name: qsTr("Add text"), keys: ["Ctrl+T"], edit: true },
        { id: "selectAll", group: qsTr("Editing"), name: qsTr("Select all"), keys: ["Ctrl+A"], edit: true },
        { id: "deselect", group: qsTr("Editing"), name: qsTr("Clear selection"), keys: ["Esc"], edit: true },

        { id: "marker", group: qsTr("Timeline"), name: qsTr("Add marker"), keys: ["M"], edit: true },
        { id: "markIn", group: qsTr("Timeline"), name: qsTr("Mark in"), keys: ["I"], edit: true },
        { id: "markOut", group: qsTr("Timeline"), name: qsTr("Mark out"), keys: ["O"], edit: true },
        { id: "clearInOut", group: qsTr("Timeline"), name: qsTr("Clear in/out"), keys: ["Alt+X"], edit: true },
        { id: "goIn", group: qsTr("Timeline"), name: qsTr("Go to in point"), keys: ["Shift+I"], edit: true },
        { id: "goOut", group: qsTr("Timeline"), name: qsTr("Go to out point"), keys: ["Shift+O"], edit: true },
        { id: "snapping", group: qsTr("Timeline"), name: qsTr("Toggle auto snapping"), keys: ["N"], edit: true },
        { id: "magnet", group: qsTr("Timeline"), name: qsTr("Toggle main track magnet"), keys: [], edit: true },
        { id: "zoomIn", group: qsTr("Timeline"), name: qsTr("Zoom in"), keys: ["Ctrl+=", "Ctrl++"], edit: true },
        { id: "zoomOut", group: qsTr("Timeline"), name: qsTr("Zoom out"), keys: ["Ctrl+-"], edit: true },
        { id: "zoomFit", group: qsTr("Timeline"), name: qsTr("Fit timeline to window"), keys: ["Shift+Z"], edit: true },
        { id: "recordVoiceover", group: qsTr("Editing"), name: qsTr("Record voiceover"), keys: ["Alt+R"], edit: true },
        { id: "removeSilences", group: qsTr("Editing"), name: qsTr("Remove silences"), keys: ["Ctrl+Shift+R"], edit: true },
        { id: "detectBeats", group: qsTr("Editing"), name: qsTr("Mark the beats of the selected music"), keys: [], edit: true },
        { id: "clearBeats", group: qsTr("Editing"), name: qsTr("Clear beat markers"), keys: [], edit: true },
        { id: "renderPreview", group: qsTr("Timeline"), name: qsTr("Render preview"), keys: ["Ctrl+R"], edit: true },
        { id: "clearRenderCache", group: qsTr("Timeline"), name: qsTr("Clear render cache"), keys: [], edit: true },

        { id: "scopes", group: qsTr("Interface"), name: qsTr("Show scopes"), keys: ["Ctrl+Shift+V"], edit: false },
        { id: "palette", group: qsTr("Interface"), name: qsTr("Command palette"), keys: ["Ctrl+K", "Ctrl+Shift+P"], edit: false },
        { id: "shortcuts", group: qsTr("Interface"), name: qsTr("Keyboard shortcuts"), keys: [], edit: false },
        { id: "panel0", group: qsTr("Interface"), name: qsTr("Panel: Media"), keys: ["Alt+1"], edit: false },
        { id: "panel1", group: qsTr("Interface"), name: qsTr("Panel: Text"), keys: ["Alt+2"], edit: false },
        { id: "panel2", group: qsTr("Interface"), name: qsTr("Panel: Captions"), keys: ["Alt+3"], edit: false },
        { id: "panel3", group: qsTr("Interface"), name: qsTr("Panel: Filters"), keys: ["Alt+4"], edit: false },
        { id: "panel4", group: qsTr("Interface"), name: qsTr("Panel: Transitions"), keys: ["Alt+5"], edit: false },
        { id: "panel5", group: qsTr("Interface"), name: qsTr("Panel: Animations"), keys: ["Alt+6"], edit: false },
        { id: "panel6", group: qsTr("Interface"), name: qsTr("Panel: Transcript"), keys: ["Alt+7"], edit: false },
        { id: "panel7", group: qsTr("Interface"), name: qsTr("Panel: Effects"), keys: ["Alt+8"], edit: false },

        { id: "newProject", group: qsTr("File"), name: qsTr("New project"), keys: ["Ctrl+N"], edit: false },
        { id: "open", group: qsTr("File"), name: qsTr("Open project"), keys: ["Ctrl+O"], edit: false },
        { id: "save", group: qsTr("File"), name: qsTr("Save project"), keys: ["Ctrl+S"], edit: false },
        { id: "saveAs", group: qsTr("File"), name: qsTr("Save project as"), keys: ["Ctrl+Shift+S"], edit: false },
        { id: "import", group: qsTr("File"), name: qsTr("Import media"), keys: ["Ctrl+I"], edit: false },
        { id: "export", group: qsTr("File"), name: qsTr("Export"), keys: ["Ctrl+E"], edit: false }
    ]

    function action(id) { return actions.find(a => a.id === id) }
    function defaults(id) {
        const a = action(id);
        return a ? a.keys : [];
    }
    function keys(id) {
        const o = overrides[id];
        return o !== undefined ? o : defaults(id);
    }
    function isCustom(id) { return overrides[id] !== undefined }
    /// Primeiro atalho da ação no formato da plataforma ("Ctrl+B", "⌘B"), ou "".
    function label(id) {
        const k = keys(id);
        return k.length > 0 ? Project.shortcutText(k[0]) : "";
    }
    function setKeys(id, list) {
        const o = Object.assign({}, overrides);
        // Igual ao padrão: volta a seguir o padrão (atualizações futuras dele valem).
        if (JSON.stringify(list) === JSON.stringify(defaults(id)))
            delete o[id];
        else
            o[id] = list;
        store.custom = JSON.stringify(o);
    }
    function reset(id) {
        const o = Object.assign({}, overrides);
        delete o[id];
        store.custom = JSON.stringify(o);
    }
    function resetAll() { store.custom = "{}" }
    /// Ação que já usa `seq` (exceto `exceptId`), ou null.
    function owner(seq, exceptId) {
        for (const a of actions)
            if (a.id !== exceptId && keys(a.id).some(k => Project.sameShortcut(k, seq)))
                return a;
        return null;
    }
    /// Atribui `seq` a `id`, tirando-o de quem já o usava. Devolve o nome da ação afetada ou "".
    function assign(id, seq) {
        const other = owner(seq, id);
        if (other)
            setKeys(other.id, keys(other.id).filter(k => !Project.sameShortcut(k, seq)));
        const mine = keys(id);
        if (!mine.some(k => Project.sameShortcut(k, seq)))
            setKeys(id, mine.concat([seq]));
        return other ? other.name : "";
    }
    function remove(id, seq) { setKeys(id, keys(id).filter(k => k !== seq)) }
}
