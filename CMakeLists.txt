cmake_minimum_required(VERSION 3.11)
project(wavezbg)

add_prx_module(${PROJECT_NAME}
    src/main.c
    exports.exp
)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -G0 -Wall -D__PSP__ -ffast-math -fno-strict-aliasing -funroll-loops -fomit-frame-pointer -fweb -frename-registers -fgcse-after-reload -fipa-pta -mtune=allegrex -march=allegrex")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -fno-exceptions -fno-rtti")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -O3 -fipa-pta")

target_link_libraries(${PROJECT_NAME} PRIVATE
    pspuser
)
