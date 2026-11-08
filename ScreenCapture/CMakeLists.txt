cmake_minimum_required(VERSION 3.21)
project(ResolumeScreenCapture LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
   NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    message(FATAL_ERROR "Build with the Visual Studio x64 toolchain on Windows.")
endif()

set(FFGL_SDK_DIR "" CACHE PATH "Path to the pinned Resolume FFGL SDK")
set(FFGL_SOURCE "${FFGL_SDK_DIR}/source/lib")
set(FFGL_GLEW_DIR "${FFGL_SDK_DIR}/deps/glew-2.1.0")
set(FFGL_GLEW_LIBRARY "${FFGL_GLEW_DIR}/lib/Release/x64/glew32s.lib")
if(NOT EXISTS "${FFGL_SOURCE}/FFGLSDK.cpp" OR NOT EXISTS "${FFGL_GLEW_LIBRARY}")
    message(FATAL_ERROR "FFGL SDK and bundled GLEW not found. Run build.ps1 to download them and build the DLL.")
endif()

add_library(ScreenCapture MODULE
    src/DesktopCapture.cpp
    src/ScreenCapture.cpp
    src/WindowCapture.cpp
    "${FFGL_SOURCE}/FFGLSDK.cpp"
    "${FFGL_SDK_DIR}/build/windows/FFGLPlugins.def"
)
target_compile_features(ScreenCapture PRIVATE cxx_std_17)
target_compile_definitions(ScreenCapture PRIVATE GLEW_STATIC)
target_include_directories(ScreenCapture SYSTEM PRIVATE "${FFGL_SOURCE}" "${FFGL_GLEW_DIR}/include")
target_link_libraries(ScreenCapture PRIVATE
    "${FFGL_GLEW_LIBRARY}" opengl32 gdi32 user32 d3d11 dwmapi runtimeobject ole32 oleaut32
)
set_source_files_properties(src/DesktopCapture.cpp src/ScreenCapture.cpp src/WindowCapture.cpp
    PROPERTIES COMPILE_OPTIONS "/W4")
set_target_properties(ScreenCapture PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/dist"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/lib"
)
add_custom_command(TARGET ScreenCapture POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt" "$<TARGET_FILE_DIR:ScreenCapture>"
)
