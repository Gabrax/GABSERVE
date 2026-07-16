cmake_minimum_required(VERSION 3.20)
project(gabserve LANGUAGES CXX)

add_executable(gabserve gabserve.cpp mf_h264.cpp)
target_compile_features(gabserve PRIVATE cxx_std_17)
target_compile_definitions(gabserve PRIVATE UNICODE _UNICODE)
target_link_libraries(gabserve PRIVATE
    ws2_32 ole32 oleaut32 windowscodecs user32 gdi32 winmm uuid d3d11 dxgi
    mfplat mf mfuuid)

if(MSVC)
    target_compile_options(gabserve PRIVATE /W4)
endif()
