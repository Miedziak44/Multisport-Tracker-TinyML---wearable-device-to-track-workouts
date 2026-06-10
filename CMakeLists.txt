cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(MultiSport_Tracker)

# ==============================================================================
# 1. ŹRÓDŁA APLIKACJI GŁÓWNEJ
# ==============================================================================
FILE(GLOB app_sources src/*.c src/*.cpp)
target_sources(app PRIVATE ${app_sources})

# ==============================================================================
# 2. KONFIGURACJA ŚCIEŻEK MODELU EDGE IMPULSE
# ==============================================================================
set(EI_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/ei_model)

# ==============================================================================
# 3. DOŁĄCZANIE ZALEŻNOŚCI (INCLUDE DIRECTORIES)
# ==============================================================================
target_include_directories(app PRIVATE
    ${EI_ROOT}
    ${EI_ROOT}/model-parameters
    ${EI_ROOT}/tflite-model
    ${EI_ROOT}/edge-impulse-sdk
    ${EI_ROOT}/edge-impulse-sdk/classifier
    ${EI_ROOT}/edge-impulse-sdk/porting
    ${EI_ROOT}/edge-impulse-sdk/dsp
    ${EI_ROOT}/edge-impulse-sdk/third_party/flatbuffers/include
    ${EI_ROOT}/edge-impulse-sdk/third_party/gemmlowp
    ${EI_ROOT}/edge-impulse-sdk/third_party/ruy
)

# ==============================================================================
# 4. SKANOWANIE REKURENCYJNE PLIKÓW ŹRÓDŁOWYCH MODELU (C/C++)
# ==============================================================================
FILE(GLOB_RECURSE ei_sources
    ${EI_ROOT}/edge-impulse-sdk/*.cpp
    ${EI_ROOT}/edge-impulse-sdk/*.cc
    ${EI_ROOT}/edge-impulse-sdk/*.c
    ${EI_ROOT}/tflite-model/*.cpp
    ${EI_ROOT}/tflite-model/*.cc
    ${EI_ROOT}/tflite-model/*.c
    ${EI_ROOT}/model-parameters/*.cpp
)

# ==============================================================================
# 5. INTEGRACJA MODELU Z PROCESEM BUDOWANIA (LINKING)
# ==============================================================================
target_sources(app PRIVATE ${ei_sources})

# ==============================================================================
# 6. FLAGI KOMPILATORA (OPTYMALIZACJA WYJŚCIA TERMINALA)
# ==============================================================================
# Wyłączenie warningów z bibliotek zewnętrznych dla zachowania czytelności logów
zephyr_compile_options(-w -Wno-return-type)