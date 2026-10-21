add_library(clivcut_core STATIC
    Model.h Model.cpp
    Keyframes.h Keyframes.cpp
    Edit.h Edit.cpp
    Serialize.h Serialize.cpp
    History.h History.cpp
    Evaluate.h Evaluate.cpp
    DraftImport.h DraftImport.cpp
    Captions.h Captions.cpp
    Lut.h Lut.cpp
    Clipboard.h Clipboard.cpp
    SpeedCurve.h SpeedCurve.cpp
    Effects.h Effects.cpp
    Stabilize.h Stabilize.cpp
)
target_include_directories(clivcut_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(clivcut_core PUBLIC Qt6::Core Qt6::Gui) # Gui: QImage (LUT na CPU)
