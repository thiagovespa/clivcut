# espeak-ng (GPL-3.0-or-later): texto → fonemas para a voz neural (Kokoro).
#
# Três formas de obter:
#  - CLIVCUT_SYSTEM_ESPEAK=ON: a do sistema (pkg-config espeak-ng), com os dados no lugar padrão
#    dela (Flatpak, distros).
#  - padrão: compilada aqui a partir do código-fonte fixado (biblioteca estática, sem áudio).
#    Os dados de fonemas são gerados rodando o próprio espeak-ng; em build cruzado isso acontece
#    num segundo build com o compilador nativo (CLIVCUT_HOST_C_COMPILER). Os dados são iguais em
#    todas as plataformas suportadas.
#  - CLIVCUT_ESPEAK_DATA_DIR: usa dados já compilados (pasta que contém espeak-ng-data).
#
# Resultado: alvo clivcut_espeak e, quando os dados vão no pacote, CLIVCUT_ESPEAK_DATA_STAGE
# (pasta que contém espeak-ng-data, só com as línguas usadas).

option(CLIVCUT_SYSTEM_ESPEAK "Use the system espeak-ng instead of building it" OFF)
set(CLIVCUT_ESPEAK_DATA_DIR "" CACHE PATH "Prebuilt espeak-ng data (folder containing espeak-ng-data)")
set(CLIVCUT_HOST_C_COMPILER "cc" CACHE STRING "Native C compiler used to build espeak-ng data when cross-compiling")
# O espeak-ng sem extras é só C: o mesmo compilador serve para a linguagem C++ que o projeto declara.
set(CLIVCUT_HOST_CXX_COMPILER "${CLIVCUT_HOST_C_COMPILER}" CACHE STRING "Native C++ compiler used to build espeak-ng data when cross-compiling")

# Línguas das vozes do Kokoro que passam pelo espeak-ng.
set(CLIVCUT_ESPEAK_DICTS en es pt fr it hi)

add_library(clivcut_espeak INTERFACE)

if(CLIVCUT_SYSTEM_ESPEAK)
    # O build CMake do espeak-ng não instala o .pc: procura direto.
    find_path(ESPEAK_NG_INCLUDE_DIR espeak-ng/speak_lib.h REQUIRED)
    find_library(ESPEAK_NG_LIBRARY espeak-ng REQUIRED)
    target_include_directories(clivcut_espeak INTERFACE ${ESPEAK_NG_INCLUDE_DIR})
    target_link_libraries(clivcut_espeak INTERFACE ${ESPEAK_NG_LIBRARY})
    target_compile_definitions(clivcut_espeak INTERFACE CLIVCUT_ESPEAK_SYSTEM_DATA)
    return()
endif()

include(ExternalProject)
set(ESPEAK_URL https://github.com/espeak-ng/espeak-ng/archive/refs/tags/1.52.0.tar.gz)
set(ESPEAK_SHA256 bb4338102ff3b49a81423da8a1a158b420124b055b60fa76cfb4b18677130a23)
set(ESPEAK_COMMON_ARGS
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_SHARED_LIBS=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DUSE_MBROLA=OFF -DUSE_LIBSONIC=OFF -DUSE_LIBPCAUDIO=OFF
    -DUSE_KLATT=OFF -DUSE_SPEECHPLAYER=OFF -DUSE_ASYNC=OFF
    -DBUILD_TESTING=OFF -DESPEAK_BUILD_MANPAGES=OFF)

set(_espeak_data_here OFF)
if(NOT CMAKE_CROSSCOMPILING AND NOT CLIVCUT_ESPEAK_DATA_DIR)
    set(_espeak_data_here ON)
endif()

get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(MSVC AND _multi_config)
    set(_espeak_libs src/libespeak-ng/Release/espeak-ng.lib src/ucd-tools/Release/ucd.lib)
elseif(MSVC)
    set(_espeak_libs src/libespeak-ng/espeak-ng.lib src/ucd-tools/ucd.lib)
else()
    set(_espeak_libs src/libespeak-ng/libespeak-ng.a src/ucd-tools/libucd.a)
endif()

set(_espeak_target_args ${ESPEAK_COMMON_ARGS}
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
if(CMAKE_TOOLCHAIN_FILE)
    list(APPEND _espeak_target_args -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
endif()
if(CMAKE_OSX_ARCHITECTURES)
    list(APPEND _espeak_target_args "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
endif()
if(CMAKE_OSX_DEPLOYMENT_TARGET)
    list(APPEND _espeak_target_args -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET})
endif()
if(MSVC)
    list(APPEND _espeak_target_args -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL)
endif()

set(_espeak_targets --target espeak-ng)
if(_espeak_data_here)
    list(APPEND _espeak_targets --target data)
endif()

set(_espeak_bin ${CMAKE_BINARY_DIR}/espeak-ng-build)
set(_espeak_byproducts)
foreach(lib ${_espeak_libs})
    list(APPEND _espeak_byproducts ${_espeak_bin}/${lib})
endforeach()
ExternalProject_Add(espeak_ng
    URL ${ESPEAK_URL}
    URL_HASH SHA256=${ESPEAK_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR ${CMAKE_BINARY_DIR}/espeak-ng-src
    BINARY_DIR ${_espeak_bin}
    CMAKE_ARGS ${_espeak_target_args}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release ${_espeak_targets}
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${_espeak_byproducts})

# Cabeçalhos e bibliotecas estáticas (o diretório precisa existir já na configuração).
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/espeak-ng-src/src/include)
target_include_directories(clivcut_espeak INTERFACE ${CMAKE_BINARY_DIR}/espeak-ng-src/src/include)
target_link_libraries(clivcut_espeak INTERFACE ${_espeak_byproducts})
if(NOT MSVC)
    target_link_libraries(clivcut_espeak INTERFACE m)
endif()
if(WIN32)
    # Biblioteca estática: sem dllimport nos cabeçalhos.
    target_compile_definitions(clivcut_espeak INTERFACE LIBESPEAK_NG_EXPORT)
endif()
add_dependencies(clivcut_espeak espeak_ng)

# --- dados ---
if(CLIVCUT_ESPEAK_DATA_DIR)
    set(_espeak_data_src ${CLIVCUT_ESPEAK_DATA_DIR}/espeak-ng-data)
    set(_espeak_data_dep)
elseif(_espeak_data_here)
    set(_espeak_data_src ${_espeak_bin}/espeak-ng-data)
    set(_espeak_data_dep espeak_ng)
else()
    # Build cruzado: um segundo espeak-ng, nativo, só para gerar os dados.
    ExternalProject_Add(espeak_ng_host
        URL ${ESPEAK_URL}
        URL_HASH SHA256=${ESPEAK_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_DIR ${CMAKE_BINARY_DIR}/espeak-ng-host-src
        BINARY_DIR ${CMAKE_BINARY_DIR}/espeak-ng-host-build
        CMAKE_ARGS ${ESPEAK_COMMON_ARGS}
            -DCMAKE_C_COMPILER=${CLIVCUT_HOST_C_COMPILER}
            -DCMAKE_CXX_COMPILER=${CLIVCUT_HOST_CXX_COMPILER}
            -DCMAKE_CXX_COMPILER_WORKS=TRUE
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release --target data
        INSTALL_COMMAND "")
    set(_espeak_data_src ${CMAKE_BINARY_DIR}/espeak-ng-host-build/espeak-ng-data)
    set(_espeak_data_dep espeak_ng_host)
endif()

# Só o que a voz usa: tabelas de fonemas, definições de línguas e os dicionários das línguas do Kokoro.
set(CLIVCUT_ESPEAK_DATA_STAGE ${CMAKE_BINARY_DIR}/tts)
set(_stage ${CLIVCUT_ESPEAK_DATA_STAGE}/espeak-ng-data)
set(_copy_cmds
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${_stage}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_stage}
    COMMAND ${CMAKE_COMMAND} -E copy ${_espeak_data_src}/phontab ${_espeak_data_src}/phonindex
            ${_espeak_data_src}/phondata ${_espeak_data_src}/intonations ${_stage}/
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${_espeak_data_src}/lang ${_stage}/lang
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${_espeak_data_src}/voices ${_stage}/voices)
foreach(d ${CLIVCUT_ESPEAK_DICTS})
    list(APPEND _copy_cmds COMMAND ${CMAKE_COMMAND} -E copy ${_espeak_data_src}/${d}_dict ${_stage}/)
endforeach()
add_custom_command(OUTPUT ${_stage}/phontab ${_copy_cmds} DEPENDS ${_espeak_data_dep} VERBATIM)
add_custom_target(clivcut_espeak_data ALL DEPENDS ${_stage}/phontab)
if(_espeak_data_dep)
    add_dependencies(clivcut_espeak_data ${_espeak_data_dep})
endif()
add_dependencies(clivcut_espeak clivcut_espeak_data)
target_compile_definitions(clivcut_espeak INTERFACE CLIVCUT_ESPEAK_DATA_BUILD="${CLIVCUT_ESPEAK_DATA_STAGE}")

# Instalação: ao lado do executável (Windows), em share/clivcut (Linux) ou nos recursos do .app.
if(WIN32)
    install(DIRECTORY ${_stage} DESTINATION ${CMAKE_INSTALL_BINDIR})
elseif(APPLE)
    install(DIRECTORY ${_stage} DESTINATION clivcut.app/Contents/Resources)
else()
    install(DIRECTORY ${_stage} DESTINATION ${CMAKE_INSTALL_DATADIR}/clivcut)
endif()
