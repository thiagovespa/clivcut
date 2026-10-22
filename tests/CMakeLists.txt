add_executable(tst_core tst_core.cpp)
target_link_libraries(tst_core PRIVATE clivcut_core Qt6::Test)
add_test(NAME core COMMAND tst_core)

add_executable(tst_media tst_media.cpp)
target_link_libraries(tst_media PRIVATE clivcut_media Qt6::Test)
add_test(NAME media COMMAND tst_media)

add_executable(tst_render tst_render.cpp)
target_link_libraries(tst_render PRIVATE clivcut_render Qt6::Test)
add_test(NAME render COMMAND tst_render)
set_tests_properties(render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

if(CLIVCUT_BUILD_APP)
    add_executable(tst_controller tst_controller.cpp
        ${CMAKE_SOURCE_DIR}/src/app/ProjectController.cpp
        ${CMAKE_SOURCE_DIR}/src/app/AudioOutput.cpp
        ${CMAKE_SOURCE_DIR}/src/app/AudioRecorder.cpp
        ${CMAKE_SOURCE_DIR}/src/app/Language.cpp
        ${CMAKE_SOURCE_DIR}/src/app/Scopes.cpp)
    target_include_directories(tst_controller PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
    file(GLOB CLIVCUT_TEST_STICKERS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/app/stickers/*.svg)
    file(GLOB CLIVCUT_TEST_TEMPLATES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/app/templates/*.clivcut
         ${CMAKE_SOURCE_DIR}/src/app/templates/text/*.json)
    qt_add_resources(tst_controller stickers PREFIX /qt/qml/Clivcut BASE ${CMAKE_SOURCE_DIR}/src/app
        FILES ${CLIVCUT_TEST_STICKERS} ${CLIVCUT_TEST_TEMPLATES})
    target_link_libraries(tst_controller PRIVATE clivcut_engine Qt6::Qml Qt6::Network Qt6::Svg Qt6::Test clivcut::sdl2)
    if(TARGET whisper)
        target_compile_definitions(tst_controller PRIVATE CLIVCUT_WHISPER_SAMPLES="${CLIVCUT_WHISPER_SAMPLES}")
    endif()
    add_test(NAME controller COMMAND tst_controller)
    set_tests_properties(controller PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SDL_AUDIODRIVER=dummy")
endif()

if(TARGET whisper)
    add_executable(tst_transcribe tst_transcribe.cpp)
    target_link_libraries(tst_transcribe PRIVATE clivcut_engine Qt6::Test)
    target_compile_definitions(tst_transcribe PRIVATE CLIVCUT_WHISPER_SAMPLES="${CLIVCUT_WHISPER_SAMPLES}")
    add_test(NAME transcribe COMMAND tst_transcribe)
endif()

add_executable(tst_proxy tst_proxy.cpp)
target_link_libraries(tst_proxy PRIVATE clivcut_engine Qt6::Test)
add_test(NAME proxy COMMAND tst_proxy)

add_executable(tst_audio tst_audio.cpp)
target_link_libraries(tst_audio PRIVATE clivcut_engine Qt6::Test)
if(CLIVCUT_WHISPER_SAMPLES)
    target_compile_definitions(tst_audio PRIVATE CLIVCUT_WHISPER_SAMPLES="${CLIVCUT_WHISPER_SAMPLES}")
endif()
add_test(NAME audio COMMAND tst_audio)
