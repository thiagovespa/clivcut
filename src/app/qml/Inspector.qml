import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Clivcut

Rectangle {
    id: root
    color: Theme.panel

    readonly property var sel: Project.selectedClip
    readonly property bool hasSel: sel.id !== undefined
    readonly property bool isText: hasSel && sel.type === "text"
    readonly property bool isMedia: hasSel && sel.type === "media"
    readonly property bool isAdjust: hasSel && sel.type === "adjust"
    readonly property bool hasColor: isAdjust || (isMedia && sel.mediaKind !== "audio")
    readonly property bool isAudioOnly: isMedia && (sel.mediaKind === "audio" || sel.audioOnly)
    readonly property bool hasVisual: hasSel && !isAudioOnly && !isAdjust
    /// Clipe composto: sem arquivo de origem, então sem velocidade nem processamentos de áudio por mídia.
    readonly property bool isCompound: isMedia && sel.mediaKind === "compound"
    readonly property bool perMediaAudio: isMedia && sel.hasAudio && !isCompound

    function setProp(key, value) { Project.setClipProperty(sel.id, key, value) }

    // Abas disponíveis conforme o tipo de clipe.
    readonly property var pages: !hasSel ? ["project"]
        : isText ? ["text", "transform", "keyframes", "animation"]
        : isAdjust ? ["color", "mask", "keyframes"]
        : isCompound ? ["transform", "color", "mask", "keyframes", "audio", "advanced"]
        : isAudioOnly ? ["audio", "speed", "keyframes"]
        : sel.mediaKind === "image" ? ["transform", "color", "mask", "keyframes", "advanced"]
        : ["transform", "color", "mask", "keyframes", "audio", "speed", "advanced"]
    readonly property var pageNames: ({
        project: qsTr("Project"), text: qsTr("Text"), transform: qsTr("Video"), color: isAdjust ? qsTr("Adjustment/Filter") : qsTr("Adjust"),
        audio: qsTr("Audio"), speed: qsTr("Speed"), advanced: qsTr("Advanced"), animation: qsTr("Animation"),
        mask: qsTr("Mask"), keyframes: qsTr("Keyframes")
    })
    property string page: pages[0]
    onPagesChanged: {
        if (Project.screenshotPage !== "" && pages.indexOf(Project.screenshotPage) >= 0)
            page = Project.screenshotPage;
        else if (pages.indexOf(page) < 0)
            page = pages[0];
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Abas
        Flow {
            Layout.fillWidth: true
            Layout.margins: 8
            spacing: 4
            Repeater {
                model: root.pages
                Rectangle {
                    required property string modelData
                    width: tabText.implicitWidth + 18
                    height: 26
                    radius: 13
                    color: root.page === modelData ? Theme.surfaceHi : "transparent"
                    Text {
                        id: tabText
                        anchors.centerIn: parent
                        text: root.pageNames[parent.modelData]
                        color: root.page === parent.modelData ? Theme.text : Theme.textDim
                        font.pixelSize: Theme.fontNormal
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.page = parent.modelData
                    }
                }
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

        Flickable {
            objectName: "inspectorScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentHeight: content.implicitHeight + 24
            clip: true
            ScrollBar.vertical: ScrollBar {}

            ColumnLayout {
                id: content
                x: 12
                y: 8
                width: parent.width - 24
                spacing: 8

                // ---------------- Projeto ----------------
                ColumnLayout {
                    visible: root.page === "project"
                    Layout.fillWidth: true
                    SectionLabel { text: qsTr("Project") }
                    GridLayout {
                        columns: 2
                        columnSpacing: 10
                        Text { text: qsTr("Resolution"); color: Theme.textDim; font.pixelSize: Theme.fontSmall }
                        Text { text: Project.canvasWidth + " × " + Project.canvasHeight; color: Theme.text; font.pixelSize: Theme.fontNormal }
                        Text { text: qsTr("Frame rate"); color: Theme.textDim; font.pixelSize: Theme.fontSmall }
                        Text { text: Project.fps; color: Theme.text; font.pixelSize: Theme.fontNormal }
                        Text { text: qsTr("Duration"); color: Theme.textDim; font.pixelSize: Theme.fontSmall }
                        Text { text: Project.formatTime(Project.duration); color: Theme.text; font.pixelSize: Theme.fontNormal }
                        Text { text: qsTr("Media files"); color: Theme.textDim; font.pixelSize: Theme.fontSmall }
                        Text { text: Project.assets.length; color: Theme.text; font.pixelSize: Theme.fontNormal }
                    }
                    SectionLabel { text: qsTr("Canvas background") }
                    ColorField {
                        label: qsTr("Color")
                        color: Project.background
                        onPicked: (c) => Project.setBackground(c === "" ? "#000000" : c)
                    }
                    PropSlider {
                        label: qsTr("Blur")
                        from: 0; to: 1; displayScale: 100; decimals: 0; suffix: "%"
                        value: Project.backgroundBlur
                        onMoved: (v) => Project.setBackgroundBlur(v)
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                        text: qsTr("With blur, the empty canvas area shows a blurred copy of the main track video (useful for vertical videos in landscape and vice versa).")
                    }
                    Text {
                        Layout.fillWidth: true
                        Layout.topMargin: 16
                        wrapMode: Text.Wrap
                        color: Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                        text: qsTr("Select a clip on the timeline to edit position, color, audio, speed and animations.\n\nShortcuts: Space plays · Ctrl+B splits · Q/W delete left/right · Del deletes · Ctrl+D duplicates · M marker · ←/→ frame by frame.")
                    }
                }

                // ---------------- Texto ----------------
                ColumnLayout {
                    visible: root.page === "text" && root.isText
                    Layout.fillWidth: true
                    SectionLabel { text: qsTr("Content") }
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 80
                        TextArea {
                            id: textEdit
                            text: root.isText ? root.sel.text : ""
                            color: Theme.text
                            wrapMode: TextEdit.Wrap
                            selectByMouse: true
                            background: Rectangle { color: Theme.surface; radius: Theme.radius; border.color: textEdit.activeFocus ? Theme.accent : "transparent" }
                            onTextChanged: if (activeFocus && root.isText && text !== root.sel.text) root.setProp("text", text)
                        }
                    }
                    SectionLabel { text: qsTr("Style") }
                    RowLayout {
                        Layout.fillWidth: true
                        ComboBox {
                            id: fontBox
                            Layout.fillWidth: true
                            focusPolicy: Qt.NoFocus
                            model: Qt.fontFamilies()
                            currentIndex: root.isText ? Math.max(0, model.indexOf(root.sel.fontFamily)) : 0
                            onActivated: (i) => root.setProp("fontFamily", model[i])
                        }
                        IconButton { iconName: "bold"; tip: qsTr("Bold"); active: root.isText && root.sel.bold; onClicked: root.setProp("bold", !root.sel.bold) }
                        IconButton { iconName: "italic"; tip: qsTr("Italic"); active: root.isText && root.sel.italic; onClicked: root.setProp("italic", !root.sel.italic) }
                    }
                    RowLayout {
                        Text { text: qsTr("Alignment"); color: Theme.textDim; font.pixelSize: Theme.fontSmall; Layout.preferredWidth: 78 }
                        IconButton { iconName: "text-align-start"; tip: qsTr("Left"); active: root.isText && root.sel.align === 0; onClicked: root.setProp("align", 0) }
                        IconButton { iconName: "text-align-center"; tip: qsTr("Center"); active: root.isText && root.sel.align === 1; onClicked: root.setProp("align", 1) }
                        IconButton { iconName: "text-align-end"; tip: qsTr("Right"); active: root.isText && root.sel.align === 2; onClicked: root.setProp("align", 2) }
                    }
                    PropSlider {
                        label: qsTr("Size")
                        from: 10; to: 400; stepSize: 1; decimals: 0
                        value: root.isText ? root.sel.fontSize : 72
                        onMoved: (v) => root.setProp("fontSize", v)
                    }
                    ColorField { label: qsTr("Color"); color: root.isText ? root.sel.color : "white"; onPicked: (c) => root.setProp("color", c === "" ? "#ffffff" : c) }
                    ColorField { label: qsTr("Outline"); color: root.isText ? root.sel.strokeColor : "black"; onPicked: (c) => root.setProp("strokeColor", c === "" ? "#000000" : c) }
                    PropSlider {
                        label: qsTr("Thickness")
                        from: 0; to: 20; stepSize: 0.5; decimals: 1
                        value: root.isText ? root.sel.strokeWidth : 0
                        onMoved: (v) => root.setProp("strokeWidth", v)
                    }
                    ColorField {
                        label: qsTr("Background")
                        allowNone: true
                        isNone: !root.isText || root.sel.background === ""
                        color: root.isText && root.sel.background !== "" ? root.sel.background : "black"
                        onPicked: (c) => root.setProp("background", c)
                    }
                    CheckBox {
                        text: qsTr("Shadow")
                        checked: root.isText && root.sel.shadow
                        focusPolicy: Qt.NoFocus
                        onToggled: root.setProp("shadow", checked)
                    }
                    ColorField {
                        label: qsTr("Highlight")
                        color: root.isText ? root.sel.highlightColor : "#ffe14d"
                        onPicked: (c) => root.setProp("highlightColor", c === "" ? "#ffe14d" : c)
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                        text: qsTr("Put *asterisks* around words to show them in the highlight color.")
                    }
                    PropSlider {
                        label: qsTr("Curve")
                        from: -1; to: 1; stepSize: 0.05; displayScale: 100; decimals: 0; suffix: "%"
                        value: root.isText ? root.sel.curve : 0
                        onMoved: (v) => root.setProp("curve", v)
                    }

                    SectionLabel {
                        text: qsTr("Shape")
                        actionText: root.isText && root.sel.shape !== "" ? qsTr("Remove") : ""
                        onActionClicked: root.setProp("shape", "")
                    }
                    ComboBox {
                        id: shapeBox
                        Layout.fillWidth: true
                        focusPolicy: Qt.NoFocus
                        readonly property var kinds: ["", "rect", "rounded", "circle", "triangle", "star", "arrow", "heart", "bubble"]
                        model: [qsTr("None", "transition, mask or animation"), qsTr("Rectangle"), qsTr("Rounded"), qsTr("Circle"), qsTr("Triangle"), qsTr("Star"),
                                qsTr("Arrow"), qsTr("Heart"), qsTr("Speech bubble")]
                        currentIndex: root.isText ? Math.max(0, kinds.indexOf(root.sel.shape)) : 0
                        onActivated: (i) => root.setProp("shape", kinds[i])
                    }
                    ColorField {
                        visible: root.isText && root.sel.shape !== ""
                        label: qsTr("Fill")
                        color: root.isText ? root.sel.shapeColor : "#f5a524"
                        onPicked: (c) => root.setProp("shapeColor", c === "" ? "#f5a524" : c)
                    }
                    PropSlider {
                        visible: root.isText && root.sel.shape !== ""
                        label: qsTr("Width")
                        from: 20; to: 1920; stepSize: 10; decimals: 0; suffix: " px"
                        value: root.isText ? root.sel.shapeWidth : 480
                        onMoved: (v) => root.setProp("shapeWidth", v)
                    }
                    PropSlider {
                        visible: root.isText && root.sel.shape !== ""
                        label: qsTr("Height")
                        from: 20; to: 1080; stepSize: 10; decimals: 0; suffix: " px"
                        value: root.isText ? root.sel.shapeHeight : 270
                        onMoved: (v) => root.setProp("shapeHeight", v)
                    }
                    Button {
                        visible: root.isText && root.sel.inCaptionTrack
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        text: qsTr("Apply to all captions")
                        focusPolicy: Qt.NoFocus
                        onClicked: Project.applyStyleToTrack(root.sel.id)
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Copies this text's style, position and animations to the whole track")
                    }
                }

                // ---------------- Transformação ----------------
                ColumnLayout {
                    visible: root.page === "transform" && root.hasVisual
                    Layout.fillWidth: true
                    SectionLabel {
                        text: qsTr("Transform")
                        actionText: qsTr("Reset")
                        onActionClicked: Project.resetClipProperties(root.sel.id, "transform")
                    }
                    Repeater {
                        model: [
                            { key: "scale", label: qsTr("Scale"), from: 0.05, to: 5, scale: 100, suffix: "%", dec: 0 },
                            { key: "x", label: qsTr("Position X"), from: -2, to: 2, scale: 100, suffix: "", dec: 0 },
                            { key: "y", label: qsTr("Position Y"), from: -2, to: 2, scale: 100, suffix: "", dec: 0 },
                            { key: "rotation", label: qsTr("Rotation"), from: -360, to: 360, scale: 1, suffix: "°", dec: 0 },
                            { key: "opacity", label: qsTr("Opacity"), from: 0, to: 1, scale: 100, suffix: "%", dec: 0 }
                        ]
                        PropSlider {
                            required property var modelData
                            label: modelData.label
                            from: modelData.from
                            to: modelData.to
                            stepSize: (modelData.to - modelData.from) / 1000
                            displayScale: modelData.scale
                            decimals: modelData.dec
                            suffix: modelData.suffix
                            value: root.hasSel ? root.sel[modelData.key] : 0
                            keyframable: true
                            hasKeyframes: root.hasSel && root.sel["kf_" + modelData.key]
                            keyframeHere: root.hasSel && root.sel["kfAt_" + modelData.key]
                            onMoved: (v) => root.setProp(modelData.key, v)
                            onKeyframeToggled: Project.toggleKeyframe(root.sel.id, modelData.key)
                        }
                    }
                    RowLayout {
                        visible: root.isMedia
                        Text { text: qsTr("Mirror"); color: Theme.textDim; font.pixelSize: Theme.fontSmall; Layout.preferredWidth: 78 }
                        IconButton { iconName: "flip-horizontal-2"; tip: qsTr("Horizontal"); active: root.hasSel && root.sel.flipH; onClicked: root.setProp("flipH", !root.sel.flipH) }
                        IconButton { iconName: "flip-vertical-2"; tip: qsTr("Vertical"); active: root.hasSel && root.sel.flipV; onClicked: root.setProp("flipV", !root.sel.flipV) }
                    }
                    SectionLabel { visible: root.isMedia; text: qsTr("Crop") }
                    Repeater {
                        model: root.isMedia ? [
                            { key: "cropLeft", label: qsTr("Left") }, { key: "cropRight", label: qsTr("Right") },
                            { key: "cropTop", label: qsTr("Top") }, { key: "cropBottom", label: qsTr("Bottom") }
                        ] : []
                        PropSlider {
                            required property var modelData
                            label: modelData.label
                            from: 0; to: 0.9; stepSize: 0.005
                            displayScale: 100; decimals: 0; suffix: "%"
                            value: root.isMedia ? root.sel[modelData.key] : 0
                            onMoved: (v) => root.setProp(modelData.key, v)
                        }
                    }
                    SectionLabel {
                        visible: root.isMedia && root.sel.mediaKind === "video"
                        text: qsTr("Stabilization")
                        actionText: root.isMedia && root.sel.stabilize > 0 ? qsTr("Off") : ""
                        onActionClicked: root.setProp("stabilize", 0)
                    }
                    PropSlider {
                        visible: root.isMedia && root.sel.mediaKind === "video"
                        label: qsTr("Strength")
                        from: 0; to: 1; stepSize: 0.05; displayScale: 100; decimals: 0; suffix: "%"
                        value: root.isMedia ? root.sel.stabilize : 0
                        onMoved: (v) => root.setProp("stabilize", v)
                    }
                    ProgressBar {
                        visible: root.isMedia && root.sel.stabilizeState === "processing"
                        Layout.fillWidth: true
                        value: root.isMedia ? root.sel.stabilizeProgress : 0
                    }
                    Text {
                        visible: root.isMedia && root.sel.mediaKind === "video"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: root.isMedia && root.sel.stabilizeState === "ready" ? Theme.accent : Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                        text: !root.isMedia ? ""
                              : root.sel.stabilizeState === "processing" ? qsTr("Analyzing the camera motion… %1%").arg(Math.round(root.sel.stabilizeProgress * 100))
                              : root.sel.stabilizeState === "queued" ? qsTr("Queued…")
                              : root.sel.stabilizeState === "ready" ? qsTr("✓ Stabilized (zooms in slightly to hide the moving edges)")
                              : qsTr("Smooths out camera shake. The video is analyzed once in the background; more strength smooths more and zooms in a bit more.")
                    }
                }

                // ---------------- Ajustes de cor ----------------
                ColumnLayout {
                    visible: root.page === "color" && root.hasColor
                    Layout.fillWidth: true

                    PropSlider {
                        visible: root.isAdjust
                        label: qsTr("Intensity")
                        from: 0; to: 1; displayScale: 100; decimals: 0; suffix: "%"
                        value: root.isAdjust ? root.sel.opacity : 1
                        keyframable: true
                        hasKeyframes: root.isAdjust && root.sel.kf_opacity
                        keyframeHere: root.isAdjust && root.sel.kfAt_opacity
                        onMoved: (v) => root.setProp("opacity", v)
                        onKeyframeToggled: Project.toggleKeyframe(root.sel.id, "opacity")
                    }

                    // Filtro
                    SectionLabel {
                        text: root.hasColor && root.sel.filter !== "" ? qsTr("Filter: %1").arg(root.sel.filterName) : qsTr("Filter")
                        actionText: root.hasColor && root.sel.filter !== "" ? qsTr("Remove") : ""
                        onActionClicked: Project.applyFilter(root.sel.id, "")
                    }
                    Text {
                        visible: root.hasColor && root.sel.filter === ""
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: qsTr("Pick a filter in the Filters tab of the left panel.")
                        color: Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                    }
                    PropSlider {
                        visible: root.hasColor && root.sel.filter !== ""
                        label: qsTr("Intensity")
                        from: 0; to: 1; displayScale: 100; decimals: 0; suffix: "%"
                        value: root.hasColor ? root.sel.filterIntensity : 0.8
                        onMoved: (v) => root.setProp("filterIntensity", v)
                    }

                    SectionLabel {
                        text: qsTr("Basic adjustments")
                        actionText: qsTr("Reset")
                        onActionClicked: Project.resetClipProperties(root.sel.id, "color")
                    }
                    Repeater {
                        model: [
                            { key: "exposure", label: qsTr("Exposure"), kf: false },
                            { key: "brightness", label: qsTr("Brightness"), kf: true },
                            { key: "contrast", label: qsTr("Contrast"), kf: true },
                            { key: "saturation", label: qsTr("Saturation"), kf: true },
                            { key: "highlights", label: qsTr("Highlights"), kf: false },
                            { key: "shadows", label: qsTr("Shadows"), kf: false },
                            { key: "temperature", label: qsTr("Temperature"), kf: false },
                            { key: "tint", label: qsTr("Tint"), kf: false }
                        ]
                        PropSlider {
                            required property var modelData
                            label: modelData.label
                            from: -1; to: 1; stepSize: 0.01
                            displayScale: 100; decimals: 0
                            value: root.hasColor ? root.sel[modelData.key] : 0
                            keyframable: modelData.kf
                            hasKeyframes: root.hasColor && !!root.sel["kf_" + modelData.key]
                            keyframeHere: root.hasColor && !!root.sel["kfAt_" + modelData.key]
                            onMoved: (v) => root.setProp(modelData.key, v)
                            onKeyframeToggled: Project.toggleKeyframe(root.sel.id, modelData.key)
                        }
                    }
                    SectionLabel { text: qsTr("Image effects") }
                    PropSlider {
                        label: qsTr("Vignette")
                        from: 0; to: 1; displayScale: 100; decimals: 0
                        value: root.hasColor ? root.sel.vignette : 0
                        onMoved: (v) => root.setProp("vignette", v)
                    }
                    PropSlider {
                        visible: root.isMedia
                        label: qsTr("Sharpen")
                        from: 0; to: 1; displayScale: 100; decimals: 0
                        value: root.hasColor ? root.sel.sharpen : 0
                        onMoved: (v) => root.setProp("sharpen", v)
                    }

                    // LUT do usuário
                    SectionLabel {
                        text: qsTr("LUT (.cube)")
                        actionText: root.hasColor && root.sel.lutPath !== "" ? qsTr("Remove") : ""
                        onActionClicked: root.setProp("lutPath", "")
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            Layout.fillWidth: true
                            text: root.hasColor && root.sel.lutPath !== "" ? root.sel.lutName : qsTr("No LUT loaded")
                            color: root.hasColor && root.sel.lutPath !== "" ? Theme.text : Theme.textFaint
                            font.pixelSize: Theme.fontSmall
                            elide: Text.ElideMiddle
                        }
                        Button {
                            text: qsTr("Load…")
                            focusPolicy: Qt.NoFocus
                            onClicked: lutDialog.open()
                        }
                    }
                    PropSlider {
                        visible: root.hasColor && root.sel.lutPath !== ""
                        label: qsTr("Intensity")
                        from: 0; to: 1; displayScale: 100; decimals: 0; suffix: "%"
                        value: root.hasColor ? root.sel.lutIntensity : 1
                        onMoved: (v) => root.setProp("lutIntensity", v)
                    }

                    // Curvas de tons
                    SectionLabel {
                        id: curvesLabel
                        property int channel: 0 // 0 mestre, 1 R, 2 G, 3 B
                        text: qsTr("Curves")
                        actionText: root.hasColor && root.sel.curves && root.sel.curves[channel].length > 0 ? qsTr("Reset") : ""
                        onActionClicked: Project.setCurve(root.sel.id, channel, [])
                    }
                    RowLayout {
                        spacing: 4
                        Repeater {
                            model: [{ name: qsTr("Luma"), color: Theme.text }, { name: "R", color: "#ff5a5a" },
                                    { name: "G", color: "#4fd17a" }, { name: "B", color: "#5a9bff" }]
                            Rectangle {
                                id: chip
                                required property var modelData
                                required property int index
                                readonly property bool on: curvesLabel.channel === index
                                implicitWidth: chipText.implicitWidth + 16
                                implicitHeight: 24
                                radius: 12
                                color: on ? Theme.accentSoft : Theme.surface
                                border.color: on ? Theme.accent : "transparent"
                                Text {
                                    id: chipText
                                    anchors.centerIn: parent
                                    text: chip.modelData.name
                                    color: chip.modelData.color
                                    font.pixelSize: Theme.fontSmall
                                    font.bold: chip.on
                                }
                                TapHandler { onTapped: curvesLabel.channel = chip.index }
                            }
                        }
                    }
                    ToneCurveEditor {
                        Layout.fillWidth: true
                        Layout.maximumWidth: 280
                        Layout.preferredHeight: width
                        points: root.hasColor && root.sel.curves ? root.sel.curves[curvesLabel.channel] : []
                        lineColor: ["#e8e8ea", "#ff5a5a", "#4fd17a", "#5a9bff"][curvesLabel.channel]
                        onEdited: (pts) => Project.setCurve(root.sel.id, curvesLabel.channel, pts)
                    }

                    // HSL por faixa de cor
                    SectionLabel {
                        id: hslLabel
                        property int band: 0
                        readonly property var value: root.hasColor && root.sel.hsl ? root.sel.hsl[band] : ({ hue: 0, saturation: 0, luminance: 0 })
                        text: qsTr("HSL")
                        actionText: value.hue !== 0 || value.saturation !== 0 || value.luminance !== 0 ? qsTr("Reset") : ""
                        onActionClicked: Project.setHsl(root.sel.id, band, 0, 0, 0)
                    }
                    RowLayout {
                        spacing: 6
                        Repeater {
                            model: ["#ff3b3b", "#ff9a2e", "#ffe03b", "#3bd16a", "#3be0e0", "#3b7bff", "#9a5bff", "#ff4fd8"]
                            Rectangle {
                                id: dot
                                required property string modelData
                                required property int index
                                readonly property var v: root.hasColor && root.sel.hsl ? root.sel.hsl[index] : null
                                readonly property bool changed: v && (v.hue !== 0 || v.saturation !== 0 || v.luminance !== 0)
                                implicitWidth: 22
                                implicitHeight: 22
                                radius: 11
                                color: modelData
                                border.width: hslLabel.band === index ? 3 : (changed ? 2 : 0)
                                border.color: hslLabel.band === index ? Theme.text : Theme.textDim
                                TapHandler { onTapped: hslLabel.band = dot.index }
                                ToolTip.visible: dotHover.hovered
                                ToolTip.text: [qsTr("Reds"), qsTr("Oranges"), qsTr("Yellows"), qsTr("Greens"), qsTr("Cyans"),
                                               qsTr("Blues"), qsTr("Purples"), qsTr("Magentas")][dot.index]
                                HoverHandler { id: dotHover }
                            }
                        }
                    }
                    PropSlider {
                        label: qsTr("Hue")
                        from: -1; to: 1; displayScale: 100; decimals: 0
                        value: hslLabel.value.hue
                        onMoved: (v) => Project.setHsl(root.sel.id, hslLabel.band, v, hslLabel.value.saturation, hslLabel.value.luminance)
                    }
                    PropSlider {
                        label: qsTr("Saturation")
                        from: -1; to: 1; displayScale: 100; decimals: 0
                        value: hslLabel.value.saturation
                        onMoved: (v) => Project.setHsl(root.sel.id, hslLabel.band, hslLabel.value.hue, v, hslLabel.value.luminance)
                    }
                    PropSlider {
                        label: qsTr("Luminance")
                        from: -1; to: 1; displayScale: 100; decimals: 0
                        value: hslLabel.value.luminance
                        onMoved: (v) => Project.setHsl(root.sel.id, hslLabel.band, hslLabel.value.hue, hslLabel.value.saturation, v)
                    }

                    // Rodas de cor
                    SectionLabel {
                        text: qsTr("Color wheels")
                        actionText: root.hasColor && root.sel.hasGrade ? qsTr("Reset all grading") : ""
                        onActionClicked: Project.resetGrade(root.sel.id)
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Repeater {
                            model: [{ key: "lift", name: qsTr("Shadows") }, { key: "gamma", name: qsTr("Midtones") },
                                    { key: "gain", name: qsTr("Highlights") }]
                            ColorWheelPad {
                                required property var modelData
                                readonly property var w: root.hasColor && root.sel.wheels ? root.sel.wheels[modelData.key] : ({ x: 0, y: 0, luma: 0 })
                                Layout.fillWidth: true
                                Layout.preferredWidth: 1
                                label: modelData.name
                                wx: w.x
                                wy: w.y
                                luma: w.luma
                                onEdited: (x, y, l) => Project.setColorWheel(root.sel.id, modelData.key, x, y, l)
                            }
                        }
                    }
                }

                // ---------------- Máscara ----------------
                ColumnLayout {
                    id: maskPage
                    visible: root.page === "mask" && root.hasColor
                    Layout.fillWidth: true
                    readonly property string shape: root.hasColor ? root.sel.maskShape : "none"
                    SectionLabel {
                        text: qsTr("Mask")
                        actionText: maskPage.shape !== "none" ? qsTr("Reset") : ""
                        onActionClicked: Project.resetClipProperties(root.sel.id, "mask")
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: [
                                { shape: "none", glyph: "∅", name: qsTr("None", "transition, mask or animation") },
                                { shape: "linear", glyph: "⊟", name: qsTr("Linear") },
                                { shape: "mirror", glyph: "☰", name: qsTr("Mirrored") },
                                { shape: "circle", glyph: "◯", name: qsTr("Circle") },
                                { shape: "rectangle", glyph: "▢", name: qsTr("Rectangle") }
                            ]
                            TileButton {
                                required property var modelData
                                width: 58
                                implicitHeight: 64
                                glyph: modelData.glyph
                                caption: modelData.name
                                selected: maskPage.shape === modelData.shape
                                onClicked: root.setProp("maskShape", modelData.shape)
                            }
                        }
                    }
                    Repeater {
                        model: [
                            { key: "maskX", label: qsTr("Position X"), from: -0.5, to: 1.5, shapes: ["linear", "mirror", "circle", "rectangle"] },
                            { key: "maskY", label: qsTr("Position Y"), from: -0.5, to: 1.5, shapes: ["linear", "mirror", "circle", "rectangle"] },
                            { key: "maskWidth", label: qsTr("Width"), from: 0.01, to: 2, shapes: ["circle", "rectangle"] },
                            { key: "maskHeight", label: qsTr("Height"), from: 0.01, to: 2, shapes: ["mirror", "circle", "rectangle"] },
                            { key: "maskRotation", label: qsTr("Rotation"), from: -180, to: 180, shapes: ["linear", "mirror", "circle", "rectangle"] },
                            { key: "maskFeather", label: qsTr("Feather"), from: 0, to: 1, shapes: ["linear", "mirror", "circle", "rectangle"] },
                            { key: "maskRoundness", label: qsTr("Roundness"), from: 0, to: 1, shapes: ["rectangle"] }
                        ]
                        PropSlider {
                            required property var modelData
                            visible: modelData.shapes.indexOf(maskPage.shape) >= 0
                            label: modelData.label
                            from: modelData.from
                            to: modelData.to
                            stepSize: (modelData.to - modelData.from) / 400
                            displayScale: modelData.key === "maskRotation" ? 1 : 100
                            decimals: 0
                            suffix: modelData.key === "maskRotation" ? "°" : (modelData.key === "maskX" || modelData.key === "maskY" ? "" : "%")
                            value: root.hasColor ? root.sel[modelData.key] : 0
                            onMoved: (v) => root.setProp(modelData.key, v)
                        }
                    }
                    CheckBox {
                        visible: maskPage.shape !== "none"
                        text: qsTr("Invert")
                        checked: root.hasColor && root.sel.maskInvert
                        focusPolicy: Qt.NoFocus
                        onToggled: root.setProp("maskInvert", checked)
                    }
                    Text {
                        visible: maskPage.shape === "none"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                        text: root.isAdjust ? qsTr("With a mask, the adjustment/filter applies only inside the shape (e.g. darken the sky).")
                                            : qsTr("Show only part of the clip: useful for split screens, frames and cutout effects.")
                    }
                }

                // ---------------- Keyframes ----------------
                ColumnLayout {
                    id: kfPage
                    visible: root.page === "keyframes" && root.hasSel
                    Layout.fillWidth: true
                    readonly property var labels: ({
                        x: qsTr("Position X"), y: qsTr("Position Y"), scale: qsTr("Scale"), rotation: qsTr("Rotation"),
                        opacity: root.isAdjust ? qsTr("Intensity") : qsTr("Opacity"), volume: qsTr("Volume"),
                        brightness: qsTr("Brightness"), contrast: qsTr("Contrast"), saturation: qsTr("Saturation")
                    })
                    readonly property var animated: root.hasSel
                        ? ["x", "y", "scale", "rotation", "opacity", "volume", "brightness", "contrast", "saturation"].filter(p => root.sel["kf_" + p])
                        : []
                    property string current: ""
                    readonly property string prop: animated.indexOf(current) >= 0 ? current : (animated.length > 0 ? animated[0] : "")

                    SectionLabel { text: qsTr("Animated properties") }
                    Text {
                        visible: kfPage.animated.length === 0
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                        text: qsTr("No animated properties. Use the ◇ next to Scale, Position, Opacity, Volume or Color (in the other tabs) to create keyframes at the playhead.")
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 4
                        Repeater {
                            model: kfPage.animated
                            Rectangle {
                                required property string modelData
                                width: propText.implicitWidth + 16
                                height: 24
                                radius: 12
                                color: kfPage.prop === modelData ? Theme.surfaceHi : "transparent"
                                border.color: kfPage.prop === modelData ? Theme.accent : Theme.border
                                Text {
                                    id: propText
                                    anchors.centerIn: parent
                                    text: kfPage.labels[parent.modelData]
                                    color: Theme.text
                                    font.pixelSize: Theme.fontSmall
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: kfPage.current = parent.modelData
                                }
                            }
                        }
                    }
                    KeyframeGraph {
                        visible: kfPage.prop !== ""
                        Layout.fillWidth: true
                        clipId: root.hasSel ? root.sel.id : ""
                        prop: kfPage.prop
                        duration: root.hasSel ? root.sel.duration : 1
                        playheadU: root.hasSel && root.sel.underPlayhead ? root.sel.playheadU : -1
                    }
                }

                // ---------------- Áudio ----------------
                ColumnLayout {
                    visible: root.page === "audio" && root.isMedia
                    Layout.fillWidth: true
                    SectionLabel {
                        text: qsTr("Audio")
                        actionText: qsTr("Reset")
                        onActionClicked: Project.resetClipProperties(root.sel.id, "audio")
                    }
                    Text {
                        visible: root.isMedia && !root.sel.hasAudio
                        text: qsTr("This clip has no audio.")
                        color: Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                    }
                    RowLayout {
                        visible: root.isMedia && root.sel.hasAudio
                        CheckBox {
                            text: qsTr("Mute clip")
                            checked: root.isMedia && root.sel.audioMuted
                            focusPolicy: Qt.NoFocus
                            onToggled: root.setProp("audioMuted", checked)
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            visible: root.isMedia && root.sel.mediaKind === "video" && !root.sel.audioOnly && !root.sel.audioMuted
                            text: qsTr("Detach audio")
                            focusPolicy: Qt.NoFocus
                            onClicked: Project.detachAudio()
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Creates a separate audio clip and mutes the video")
                        }
                    }
                    PropSlider {
                        enabled: root.isMedia && root.sel.hasAudio
                        label: qsTr("Volume")
                        from: 0; to: 2; stepSize: 0.01
                        displayScale: 100; decimals: 0; suffix: "%"
                        value: root.isMedia ? root.sel.volume : 1
                        keyframable: true
                        hasKeyframes: root.isMedia && root.sel.kf_volume
                        keyframeHere: root.isMedia && root.sel.kfAt_volume
                        onMoved: (v) => root.setProp("volume", v)
                        onKeyframeToggled: Project.toggleKeyframe(root.sel.id, "volume")
                    }
                    PropSlider {
                        enabled: root.isMedia && root.sel.hasAudio
                        label: qsTr("Gain")
                        from: -24; to: 24; stepSize: 0.5; decimals: 1; suffix: " dB"
                        value: root.isMedia ? root.sel.gainDb : 0
                        onMoved: (v) => root.setProp("gainDb", v)
                    }
                    RowLayout {
                        visible: root.perMediaAudio
                        Layout.fillWidth: true
                        ComboBox {
                            id: loudTarget
                            Layout.fillWidth: true
                            focusPolicy: Qt.NoFocus
                            model: [qsTr("Social media (-14 LUFS)"), qsTr("Podcast (-16 LUFS)"), qsTr("TV/broadcast (-23 LUFS)")]
                            readonly property var values: [-14, -16, -23]
                        }
                        Button {
                            text: Project.normalizing ? qsTr("Measuring…") : qsTr("Normalize")
                            enabled: !Project.normalizing
                            focusPolicy: Qt.NoFocus
                            onClicked: Project.normalizeLoudness(loudTarget.values[loudTarget.currentIndex])
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Measures the loudness (EBU R128) of the selected clips and adjusts the gain to the target")
                        }
                    }
                    SectionLabel {
                        visible: root.perMediaAudio
                        text: qsTr("Noise reduction")
                    }
                    PropSlider {
                        visible: root.perMediaAudio
                        label: qsTr("Intensity")
                        from: 0; to: 1; stepSize: 0.1; displayScale: 100; decimals: 0; suffix: "%"
                        value: root.isMedia ? root.sel.denoise : 0
                        onMoved: (v) => root.setProp("denoise", v)
                    }
                    Text {
                        visible: root.perMediaAudio
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: root.isMedia && root.sel.denoiseState === "ready" ? Theme.accent : Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                        text: !root.isMedia ? ""
                              : root.sel.denoiseState === "processing" ? qsTr("Processing… %1%").arg(Math.round(root.sel.denoiseProgress * 100))
                              : root.sel.denoiseState === "queued" ? qsTr("Queued…")
                              : root.sel.denoiseState === "ready" ? qsTr("✓ Clean audio in use (processed locally)")
                              : qsTr("Removes hiss, hum and constant background noise. Processing runs in the background.")
                    }
                    SectionLabel {
                        visible: root.perMediaAudio
                        text: qsTr("Voice effect")
                        actionText: root.isMedia && root.sel.voiceEffect !== "" ? qsTr("Remove") : ""
                        onActionClicked: root.setProp("voiceEffect", "")
                    }
                    Flow {
                        visible: root.perMediaAudio
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: Project.voiceEffects
                            Rectangle {
                                id: fxChip
                                required property var modelData
                                readonly property bool on: root.isMedia && root.sel.voiceEffect === modelData.id
                                implicitWidth: fxText.implicitWidth + 18
                                implicitHeight: 26
                                radius: 13
                                color: on ? Theme.accentSoft : Theme.surface
                                border.color: on ? Theme.accent : "transparent"
                                Text {
                                    id: fxText
                                    anchors.centerIn: parent
                                    text: fxChip.modelData.name
                                    color: fxChip.on ? Theme.accent : Theme.text
                                    font.pixelSize: Theme.fontSmall
                                }
                                TapHandler { onTapped: root.setProp("voiceEffect", fxChip.on ? "" : fxChip.modelData.id) }
                            }
                        }
                    }
                    Text {
                        visible: root.perMediaAudio && root.sel.voiceEffect !== ""
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: root.isMedia && root.sel.effectState === "ready" ? Theme.accent : Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                        text: !root.isMedia ? ""
                              : root.sel.effectState === "processing" ? qsTr("Applying the effect… %1%").arg(Math.round(root.sel.effectProgress * 100))
                              : root.sel.effectState === "queued" ? qsTr("Queued…")
                              : qsTr("✓ Effect in use")
                    }
                    SectionLabel {
                        visible: root.isMedia && root.sel.hasAudio
                        text: qsTr("Duck under voice")
                    }
                    PropSlider {
                        visible: root.isMedia && root.sel.hasAudio
                        label: qsTr("Amount")
                        from: 0; to: 1; stepSize: 0.05; displayScale: 100; decimals: 0; suffix: "%"
                        value: root.isMedia ? root.sel.ducking : 0
                        onMoved: (v) => root.setProp("ducking", v)
                    }
                    Text {
                        visible: root.isMedia && root.sel.hasAudio
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                        text: qsTr("For background music: this clip gets quieter (up to -16 dB) whenever the other clips have speech, and comes back up in the pauses.")
                    }
                    SectionLabel {
                        visible: root.perMediaAudio
                        text: qsTr("Beats")
                        actionText: Project.hasBeatMarkers ? qsTr("Clear") : ""
                        onActionClicked: Project.clearBeatMarkers()
                    }
                    RowLayout {
                        visible: root.perMediaAudio
                        Layout.fillWidth: true
                        ComboBox {
                            id: beatEvery
                            Layout.fillWidth: true
                            focusPolicy: Qt.NoFocus
                            model: [qsTr("Every beat"), qsTr("Every 2 beats"), qsTr("Every bar (4 beats)")]
                            readonly property var values: [1, 2, 4]
                        }
                        Button {
                            text: Project.detectingBeats ? qsTr("Detecting…") : qsTr("Mark beats")
                            enabled: !Project.detectingBeats
                            focusPolicy: Qt.NoFocus
                            onClicked: Project.detectBeats(beatEvery.values[beatEvery.currentIndex])
                        }
                    }
                    Text {
                        visible: root.perMediaAudio
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                        text: qsTr("Finds the rhythm of the music and adds markers on its beats; clips snap to them, so cuts land on the beat.")
                    }
                    SectionLabel {
                        visible: root.perMediaAudio
                        text: qsTr("Voice isolation (AI)")
                    }
                    PropSlider {
                        visible: root.perMediaAudio
                        enabled: root.isMedia && root.sel.voiceState !== "unavailable"
                        label: qsTr("Intensity")
                        from: 0; to: 1; stepSize: 0.1; displayScale: 100; decimals: 0; suffix: "%"
                        value: root.isMedia ? root.sel.voiceIsolation : 0
                        onMoved: (v) => root.setProp("voiceIsolation", v)
                    }
                    ProgressBar {
                        visible: root.isMedia && (root.sel.voiceState === "processing"
                                                  || (root.sel.voiceState === "needsModel" && Project.downloadingModel === "dfn3"))
                        Layout.fillWidth: true
                        value: root.sel.voiceState === "processing" ? root.sel.voiceProgress : Project.modelDownloadProgress
                    }
                    PillButton {
                        visible: root.isMedia && root.sel.voiceState === "needsModel" && Project.downloadingModel !== "dfn3"
                        enabled: Project.downloadingModel === ""
                        text: qsTr("Download model (16 MB)")
                        iconName: "download"
                        onClicked: Project.downloadModel("dfn3")
                    }
                    Text {
                        visible: root.perMediaAudio
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: root.isMedia && root.sel.voiceState === "ready" ? Theme.accent : Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                        text: !root.isMedia ? ""
                              : root.sel.voiceState === "unavailable" ? qsTr("Not available in this build (compiled without ONNX Runtime).")
                              : root.sel.voiceState === "needsModel" ? (Project.downloadingModel === "dfn3" ? qsTr("Downloading the voice model…")
                                                                        : qsTr("Requires the voice isolation model (DeepFilterNet3, MIT/Apache-2.0), downloaded only once."))
                              : root.sel.voiceState === "processing" ? qsTr("Isolating the voice… %1%").arg(Math.round(root.sel.voiceProgress * 100))
                              : root.sel.voiceState === "queued" ? qsTr("Queued…")
                              : root.sel.voiceState === "ready" ? qsTr("✓ Isolated voice in use (processed locally)")
                              : qsTr("Keeps only the speech: removes background music, noise and room sound. Runs on your computer.")
                    }
                    PropSlider {
                        enabled: root.isMedia && root.sel.hasAudio
                        label: qsTr("Fade in")
                        from: 0; to: 10; stepSize: 0.1; decimals: 1; suffix: "s"
                        value: root.isMedia ? root.sel.fadeIn / 1e6 : 0
                        onMoved: (v) => root.setProp("fadeIn", v * 1e6)
                    }
                    PropSlider {
                        enabled: root.isMedia && root.sel.hasAudio
                        label: qsTr("Fade out")
                        from: 0; to: 10; stepSize: 0.1; decimals: 1; suffix: "s"
                        value: root.isMedia ? root.sel.fadeOut / 1e6 : 0
                        onMoved: (v) => root.setProp("fadeOut", v * 1e6)
                    }
                }

                // ---------------- Velocidade ----------------
                ColumnLayout {
                    visible: root.page === "speed" && root.isMedia
                    Layout.fillWidth: true
                    SectionLabel { text: qsTr("Speed") }
                    Text {
                        visible: root.isMedia && root.sel.speed <= 0
                        text: qsTr("Frozen frame")
                        color: Theme.textDim
                        font.pixelSize: Theme.fontSmall
                    }
                    // Normal | Curva
                    Row {
                        id: speedMode
                        visible: root.isMedia && root.sel.speed > 0 && root.sel.mediaKind !== "image"
                        spacing: 4
                        property bool curve: root.isMedia && root.sel.speedCurve !== undefined && root.sel.speedCurve.length >= 2
                        property bool showCurve: curve
                        onCurveChanged: showCurve = curve
                        Repeater {
                            model: [{ c: false, t: qsTr("Normal") }, { c: true, t: qsTr("Curve") }]
                            Rectangle {
                                required property var modelData
                                width: modeText.implicitWidth + 22
                                height: 26
                                radius: 13
                                color: speedMode.showCurve === modelData.c ? Theme.surfaceHi : "transparent"
                                border.color: speedMode.showCurve === modelData.c ? Theme.accent : Theme.border
                                Text {
                                    id: modeText
                                    anchors.centerIn: parent
                                    text: parent.modelData.t
                                    color: Theme.text
                                    font.pixelSize: Theme.fontSmall
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        speedMode.showCurve = parent.modelData.c;
                                        if (!parent.modelData.c && speedMode.curve)
                                            Project.setSpeed(root.sel.id, root.sel.speed); // volta a constante
                                    }
                                }
                            }
                        }
                    }
                    // --- Normal ---
                    PropSlider {
                        visible: speedMode.visible && !speedMode.showCurve
                        label: qsTr("Speed")
                        from: 0.1; to: 10; stepSize: 0.05; decimals: 2; suffix: "x"
                        value: root.isMedia ? root.sel.speed : 1
                        onMoved: (v) => Project.setSpeed(root.sel.id, v)
                    }
                    Flow {
                        visible: speedMode.visible && !speedMode.showCurve
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: [0.25, 0.5, 1, 1.5, 2, 4]
                            Button {
                                required property real modelData
                                text: modelData + "x"
                                focusPolicy: Qt.NoFocus
                                highlighted: root.isMedia && Math.abs(root.sel.speed - modelData) < 0.001
                                onClicked: Project.setSpeed(root.sel.id, modelData)
                            }
                        }
                    }
                    // --- Curva ---
                    Flow {
                        visible: speedMode.visible && speedMode.showCurve
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: Project.speedPresets
                            TileButton {
                                required property var modelData
                                width: 72
                                implicitHeight: 58
                                glyph: "〰"
                                caption: modelData.name
                                onClicked: Project.applySpeedPreset(root.sel.id, modelData.id)
                            }
                        }
                    }
                    CurveEditor {
                        visible: speedMode.visible && speedMode.showCurve
                        Layout.fillWidth: true
                        points: speedMode.curve ? root.sel.speedCurve : [{ u: 0, v: 1 }, { u: 1, v: 1 }]
                        playheadU: root.isMedia && root.sel.underPlayhead ? root.sel.playheadU : -1
                        onEdited: (pts) => Project.setSpeedCurve(root.sel.id, pts)
                    }
                    Text {
                        visible: speedMode.visible && speedMode.showCurve
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                        text: qsTr("Drag the points; double-click adds or removes one. Duration: %1 (average %2×)")
                              .arg(root.isMedia ? Project.formatTime(root.sel.duration) : "")
                              .arg(root.isMedia ? root.sel.speed.toFixed(2) : "1")
                    }
                    CheckBox {
                        visible: root.isMedia && root.sel.mediaKind === "video" && root.sel.speed > 0
                        text: qsTr("Play in reverse")
                        checked: root.isMedia && root.sel.reverse
                        focusPolicy: Qt.NoFocus
                        onToggled: root.setProp("reverse", checked)
                    }
                    CheckBox {
                        visible: root.isMedia && root.sel.hasAudio && root.sel.speed > 0
                        text: qsTr("Keep original pitch")
                        checked: root.isMedia && root.sel.keepPitch
                        focusPolicy: Qt.NoFocus
                        onToggled: root.setProp("keepPitch", checked)
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: qsTr("When off, the voice gets higher when sped up and lower when slowed down")
                    }
                    Button {
                        visible: root.isMedia && root.sel.mediaKind === "video"
                        text: qsTr("❄ Freeze frame at playhead") + (Keymap.label("freeze") !== "" ? " (" + Keymap.label("freeze") + ")" : "")
                        focusPolicy: Qt.NoFocus
                        onClicked: Project.freezeFrame()
                    }
                }

                // ---------------- Avançado ----------------
                ColumnLayout {
                    visible: root.page === "advanced" && root.isMedia
                    Layout.fillWidth: true
                    SectionLabel { text: qsTr("Blend mode") }
                    ComboBox {
                        Layout.fillWidth: true
                        focusPolicy: Qt.NoFocus
                        readonly property var modes: ["normal", "multiply", "screen", "overlay", "darken", "lighten", "colorDodge", "colorBurn", "softLight", "difference"]
                        model: [qsTr("Normal"), qsTr("Multiply"), qsTr("Screen"), qsTr("Overlay"), qsTr("Darken"), qsTr("Lighten"), qsTr("Color dodge"), qsTr("Color burn"), qsTr("Soft light"), qsTr("Difference")]
                        currentIndex: root.isMedia ? Math.max(0, modes.indexOf(root.sel.blend)) : 0
                        onActivated: (i) => root.setProp("blend", modes[i])
                    }
                    SectionLabel { text: qsTr("Chroma key (green screen)") }
                    CheckBox {
                        text: qsTr("Remove color")
                        checked: root.isMedia && root.sel.chromaEnabled
                        focusPolicy: Qt.NoFocus
                        onToggled: root.setProp("chromaEnabled", checked)
                    }
                    ColorField {
                        visible: root.isMedia && root.sel.chromaEnabled
                        label: qsTr("Key color")
                        color: root.isMedia ? root.sel.chromaColor : "#00ff00"
                        onPicked: (c) => root.setProp("chromaColor", c === "" ? "#00ff00" : c)
                    }
                    PropSlider {
                        visible: root.isMedia && root.sel.chromaEnabled
                        label: qsTr("Intensity")
                        from: 0; to: 1; displayScale: 100; decimals: 0
                        value: root.isMedia ? root.sel.chromaSimilarity : 0.3
                        onMoved: (v) => root.setProp("chromaSimilarity", v)
                    }
                    PropSlider {
                        visible: root.isMedia && root.sel.chromaEnabled
                        label: qsTr("Smoothness")
                        from: 0; to: 1; displayScale: 100; decimals: 0
                        value: root.isMedia ? root.sel.chromaSmoothness : 0.1
                        onMoved: (v) => root.setProp("chromaSmoothness", v)
                    }

                    SectionLabel {
                        visible: root.isMedia && root.sel.mediaKind === "video"
                        text: qsTr("Remove background (AI)")
                    }
                    CheckBox {
                        visible: root.isMedia && root.sel.mediaKind === "video"
                        text: qsTr("Cut out people automatically")
                        checked: root.isMedia && root.sel.removeBackground
                        enabled: root.isMedia && root.sel.matteState !== "unavailable"
                        focusPolicy: Qt.NoFocus
                        onToggled: root.setProp("removeBackground", checked)
                    }
                    ProgressBar {
                        visible: root.isMedia && (root.sel.matteState === "processing"
                                                  || (root.sel.matteState === "needsModel" && Project.downloadingModel === "rvm"))
                        Layout.fillWidth: true
                        value: root.sel.matteState === "processing" ? root.sel.matteProgress : Project.modelDownloadProgress
                    }
                    PillButton {
                        visible: root.isMedia && root.sel.matteState === "needsModel" && Project.downloadingModel !== "rvm"
                        enabled: Project.downloadingModel === ""
                        text: qsTr("Download model (15 MB)")
                        iconName: "download"
                        onClicked: Project.downloadModel("rvm")
                    }
                    Text {
                        visible: root.isMedia && root.sel.mediaKind === "video"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: root.isMedia && root.sel.matteState === "ready" ? Theme.accent : Theme.textFaint
                        font.pixelSize: Theme.fontSmall
                        text: !root.isMedia ? ""
                              : root.sel.matteState === "unavailable" ? qsTr("Not available in this build (compiled without ONNX Runtime).")
                              : root.sel.matteState === "needsModel" ? (Project.downloadingModel === "rvm" ? qsTr("Downloading the cutout model…")
                                                                         : qsTr("Requires the people cutout model (RobustVideoMatting, GPL-3.0), downloaded only once."))
                              : root.sel.matteState === "processing" ? qsTr("Analyzing the video… %1%").arg(Math.round(root.sel.matteProgress * 100))
                              : root.sel.matteState === "queued" ? qsTr("Queued…")
                              : root.sel.matteState === "ready" ? qsTr("✓ Background removed (processed locally)")
                              : qsTr("Separates people from the background without a green screen. Everything runs on your computer, in the background.")
                    }
                }

                // ---------------- Animação (texto) ----------------
                ColumnLayout {
                    visible: root.page === "animation"
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: Theme.textDim
                        font.pixelSize: Theme.fontSmall
                        text: qsTr("Pick in and out animations in the \"Animations\" tab of the left panel.")
                    }
                }
            }
        }
    }

    FileDialog {
        id: lutDialog
        title: qsTr("Load LUT")
        nameFilters: [qsTr("3D LUT (*.cube)")]
        onAccepted: Project.loadLut(root.sel.id, selectedFile)
    }
}
