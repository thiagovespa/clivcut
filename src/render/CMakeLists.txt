add_library(clivcut_render STATIC
    Compositor.h Compositor.cpp
    TextRaster.h TextRaster.cpp
    OffscreenRhi.h OffscreenRhi.cpp
)
target_include_directories(clivcut_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(clivcut_render PUBLIC clivcut_core Qt6::Gui Qt6::GuiPrivate)

# GLSL ES 3.00 / GLSL 330 no mínimo: texturas 3D (LUTs) e textureSize.
qt_add_shaders(clivcut_render "clivcut_shaders"
    PREFIX "/clivcut"
    GLSL "300es,330"
    HLSL 50
    MSL 12
    FILES
        shaders/layer.vert
        shaders/layer.frag
        shaders/blit.vert
        shaders/blit.frag
        shaders/yuv.vert
        shaders/yuv.frag
)
