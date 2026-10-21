# Image engine extras: ZXing-cpp (Apache-2.0) built as a static library, readers only, QR codes only.
# Included from the root CMakeLists.txt inside the per-engine loop, so everything here is in the root directory scope.
set(ZXING_ROOT ${CMAKE_SOURCE_DIR}/third_party/zxing/zxing-cpp-zxing-cpp-287c85d)
if(EXISTS ${ZXING_ROOT}/core/CMakeLists.txt AND NOT TARGET ZXing)
  set(ZXING_READERS ON)
  set(ZXING_WRITERS OFF)
  set(ZXING_C_API OFF)
  set(ZXING_USE_BUNDLED_ZINT OFF)
  set(ZXING_ENABLE_1D OFF CACHE BOOL "" FORCE)
  set(ZXING_ENABLE_AZTEC OFF CACHE BOOL "" FORCE)
  set(ZXING_ENABLE_DATAMATRIX OFF CACHE BOOL "" FORCE)
  set(ZXING_ENABLE_MAXICODE OFF CACHE BOOL "" FORCE)
  set(ZXING_ENABLE_PDF417 OFF CACHE BOOL "" FORCE)
  set(ZXING_ENABLE_QRCODE ON CACHE BOOL "" FORCE)
  add_subdirectory(${ZXING_ROOT}/core ${CMAKE_BINARY_DIR}/third_party_zxing EXCLUDE_FROM_ALL)
endif()
if(TARGET ZXing)
  target_link_libraries(filo_eng_image PUBLIC ZXing)
  target_compile_definitions(filo_eng_image PUBLIC FILO_HAVE_ZXING=1)
endif()
# Direct2D SVG rasteriser + WIC + D3D11 (WARP fallback) are Windows system libraries.
target_link_libraries(filo_eng_image PUBLIC d2d1 d3d11 dxgi dwrite dxguid windowscodecs shlwapi ole32 oleaut32 propsys)
# The image tests need the PDFium import library through filo_pdfium (already linked by the engine) and WIC.

if(TARGET ZXing)
  target_compile_options(ZXing PRIVATE /w)
endif()
