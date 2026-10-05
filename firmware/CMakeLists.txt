cmake_minimum_required(VERSION 3.16)

# Structure calquée sur les exemples esp-matter (examples/*/CMakeLists.txt).
# L'ordre des include() est significatif : esp_matter_device.cmake doit être
# inclus après project.cmake, mais ESP_MATTER_DEVICE_PATH est résolu avant.

if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "ESP_MATTER_PATH n'est pas défini. Source esp-matter/export.sh.")
endif()

# À ce stade, project.cmake n'a pas encore tourné : la variable CMake
# IDF_TARGET n'est renseignée que si idf.py l'a passée en -D. Sinon, on lit
# l'environnement (tools/build.sh exporte IDF_TARGET), puis on retombe sur
# l'H2, cible du produit.
set(_sb_target "${IDF_TARGET}")
if("${_sb_target}" STREQUAL "")
    set(_sb_target "$ENV{IDF_TARGET}")
endif()
if("${_sb_target}" STREQUAL "")
    set(_sb_target "esp32h2")
endif()

if(NOT DEFINED ENV{ESP_MATTER_DEVICE_PATH})
    if("${_sb_target}" STREQUAL "esp32c6")
        set(ENV{ESP_MATTER_DEVICE_PATH} $ENV{ESP_MATTER_PATH}/device_hal/device/esp32c6_devkit_c)
    elseif("${_sb_target}" STREQUAL "esp32h2")
        set(ENV{ESP_MATTER_DEVICE_PATH} $ENV{ESP_MATTER_PATH}/device_hal/device/esp32h2_devkit_c)
    elseif("${_sb_target}" STREQUAL "esp32h21")
        set(ENV{ESP_MATTER_DEVICE_PATH} $ENV{ESP_MATTER_PATH}/device_hal/device/esp32h21_devkit_c)
    else()
        message(FATAL_ERROR "Cible non supportée par ce projet : ${_sb_target}")
    endif()
endif()

set(PROJECT_VER "0.1.0")
set(PROJECT_VER_NUMBER 1)

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${ESP_MATTER_PATH}/examples/common/cmake_common/components_include.cmake)
include($ENV{ESP_MATTER_DEVICE_PATH}/esp_matter_device.cmake)

set(EXTRA_COMPONENT_DIRS
    "${ESP_MATTER_PATH}/examples/common"
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components"
    "${ESP_MATTER_PATH}/device_hal/device"
    ${extra_components_dirs_append})

project(smartbutton)

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
