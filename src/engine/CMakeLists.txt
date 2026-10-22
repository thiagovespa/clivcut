add_library(clivcut_engine STATIC
    DecoderFrameSource.h DecoderFrameSource.cpp
    AudioMixer.h AudioMixer.cpp
    TimeStretch.h TimeStretch.cpp
    RenderCache.h RenderCache.cpp
    VoiceIsolation.h VoiceIsolation.cpp
    Silence.h Silence.cpp
    PhaseCorrelation.h PhaseCorrelation.cpp
    Stabilization.h Stabilization.cpp
    MotionTracking.h MotionTracking.cpp
    StemSeparation.h StemSeparation.cpp
    TextToSpeech.h TextToSpeech.cpp
    Beats.h Beats.cpp
    VoiceEffects.h VoiceEffects.cpp
    Exporter.h Exporter.cpp
    Transcriber.h Transcriber.cpp
    WhisperModels.h WhisperModels.cpp
    AiModels.h AiModels.cpp
    Proxy.h Proxy.cpp
    AudioCleanup.h AudioCleanup.cpp
    Matting.h Matting.cpp
)
target_include_directories(clivcut_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(clivcut_engine PUBLIC clivcut_core clivcut_media clivcut_render)
if(TARGET whisper)
    target_link_libraries(clivcut_engine PRIVATE whisper)
    target_compile_definitions(clivcut_engine PRIVATE CLIVCUT_HAVE_WHISPER)
endif()
if(TARGET clivcut_onnxruntime)
    target_link_libraries(clivcut_engine PUBLIC clivcut_onnxruntime)
    target_compile_definitions(clivcut_engine PRIVATE CLIVCUT_HAVE_ONNX)
    if(TARGET clivcut_espeak)
        target_link_libraries(clivcut_engine PRIVATE clivcut_espeak)
        target_compile_definitions(clivcut_engine PRIVATE CLIVCUT_HAVE_TTS)
    endif()
endif()
