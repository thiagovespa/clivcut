# Ícones de plataforma: .ico (Windows, versionado) e .icns (macOS, gerado no CI com iconutil).
set(CLIVCUT_PLATFORM_ICON)
if(WIN32)
    set(CLIVCUT_PLATFORM_ICON ${CMAKE_CURRENT_BINARY_DIR}/clivcut.rc)
    file(WRITE ${CLIVCUT_PLATFORM_ICON} "IDI_ICON1 ICON \"${CMAKE_SOURCE_DIR}/packaging/windows/clivcut.ico\"\n")
elseif(APPLE AND EXISTS ${CMAKE_SOURCE_DIR}/packaging/macos/clivcut.icns)
    set(CLIVCUT_PLATFORM_ICON ${CMAKE_SOURCE_DIR}/packaging/macos/clivcut.icns)
    set_source_files_properties(${CLIVCUT_PLATFORM_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()

qt_add_executable(clivcut
    ${CLIVCUT_PLATFORM_ICON}
    main.cpp
    ProjectController.h ProjectController.cpp
    PreviewItem.h PreviewItem.cpp
    ImageProviders.h ImageProviders.cpp
    AudioOutput.h AudioOutput.cpp
    AudioRecorder.h AudioRecorder.cpp
    Language.h Language.cpp
    Scopes.h Scopes.cpp
    ScopeItem.h ScopeItem.cpp
)

set_source_files_properties(qml/Theme.qml qml/Keymap.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# Ícones da interface (Lucide, ISC: icons/ui/LICENSE).
file(GLOB CLIVCUT_UI_ICONS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS icons/ui/*.svg)
# Stickers (Fluent Emoji, MIT: stickers/LICENSE).
file(GLOB CLIVCUT_STICKERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS stickers/*.svg)
file(GLOB CLIVCUT_TEMPLATES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS templates/*.clivcut templates/text/*.json)

qt_add_qml_module(clivcut
    URI Clivcut
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/Keymap.qml
        qml/ShortcutsDialog.qml
        qml/WhatsNewDialog.qml
        qml/GuidedTour.qml
        qml/SilenceDialog.qml
        qml/TranscriptPanel.qml
        qml/EffectsPanel.qml
        qml/VoiceoverPanel.qml
        qml/TopBar.qml
        qml/MediaPanel.qml
        qml/CaptionsPanel.qml
        qml/FiltersPanel.qml
        qml/PreviewPanel.qml
        qml/Inspector.qml
        qml/Timeline.qml
        qml/TimelineClip.qml
        qml/ExportDialog.qml
        qml/NewProjectDialog.qml
        qml/RecoveryDialog.qml
        qml/UnsavedDialog.qml
        qml/ActivityRail.qml
        qml/CommandPalette.qml
        qml/WelcomeScreen.qml
        qml/components/IconButton.qml
        qml/components/Icon.qml
        qml/components/PillButton.qml
        qml/components/Spotlight.qml
        qml/components/AiVoiceControls.qml
        qml/components/PropSlider.qml
        qml/components/SectionLabel.qml
        qml/components/ColorField.qml
        qml/components/TileButton.qml
        qml/components/CurveEditor.qml
        qml/components/KeyframeGraph.qml
        qml/components/ToneCurveEditor.qml
        qml/components/ColorWheelPad.qml
    RESOURCES
        icons/clivcut.svg
        ${CLIVCUT_UI_ICONS}
        ${CLIVCUT_STICKERS}
        ${CLIVCUT_TEMPLATES}
)

target_compile_definitions(clivcut PRIVATE CLIVCUT_VERSION="${PROJECT_VERSION}")
set_target_properties(clivcut PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_GUI_IDENTIFIER io.github.clivcut
    MACOSX_BUNDLE_BUNDLE_NAME clivcut
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_ICON_FILE clivcut.icns)
target_link_libraries(clivcut PRIVATE
    clivcut_engine
    Qt6::Quick Qt6::QuickControls2 Qt6::Concurrent Qt6::Network Qt6::Svg
    clivcut::sdl2
)

# Traduções: o código está em inglês; clivcut_en.ts guarda só os plurais.
# Atualize os .ts com: cmake --build build --target update_translations
qt_add_translations(TARGETS clivcut
    SOURCE_TARGETS clivcut clivcut_core clivcut_media clivcut_render clivcut_engine
    TS_FILES ${CMAKE_SOURCE_DIR}/i18n/clivcut_pt_BR.ts ${CMAKE_SOURCE_DIR}/i18n/clivcut_es.ts
    PLURALS_TS_FILE ${CMAKE_SOURCE_DIR}/i18n/clivcut_en.ts
    RESOURCE_PREFIX /i18n
    LUPDATE_OPTIONS -no-obsolete -locations none)

install(TARGETS clivcut BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(UNIX AND NOT APPLE)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/io.github.clivcut.desktop DESTINATION share/applications)
    install(FILES icons/clivcut.svg DESTINATION share/icons/hicolor/scalable/apps RENAME io.github.clivcut.svg)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/io.github.clivcut.metainfo.xml DESTINATION share/metainfo)
endif()

# Windows/macOS: copia Qt, plugins e módulos QML para o pacote (windeployqt/macdeployqt).
if(WIN32 OR APPLE)
    option(CLIVCUT_DEPLOY "Incluir Qt e dependências na instalação" ON)
    if(CLIVCUT_DEPLOY)
        qt_generate_deploy_qml_app_script(TARGET clivcut OUTPUT_SCRIPT clivcut_deploy NO_UNSUPPORTED_PLATFORM_ERROR)
        install(SCRIPT ${clivcut_deploy})
        if(WIN32)
            # DLLs de terceiros (FFmpeg, SDL2, x264...) ao lado do executável.
            install(TARGETS clivcut RUNTIME_DEPENDENCIES
                PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*" "[Qq]t6.*" # no Windows os nomes chegam em minúsculas
                POST_EXCLUDE_REGEXES ".*[Ss]ystem32/.*\\.dll"
                DIRECTORIES $<TARGET_FILE_DIR:clivcut> ${CMAKE_PREFIX_PATH}
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        endif()
    endif()
endif()
