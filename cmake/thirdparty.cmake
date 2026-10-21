# miniz (MIT) -- ZIP/deflate for OOXML/EPUB writing
add_library(filo_miniz STATIC ${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c)
target_include_directories(filo_miniz PUBLIC ${CMAKE_SOURCE_DIR}/third_party/miniz)
target_compile_definitions(filo_miniz PUBLIC MINIZ_NO_STDIO_DISABLED=0)

# PDFium (BSD-3/Apache-2) -- prebuilt, loaded via import lib; dll copied next to the exes
set(PDFIUM_DIR ${CMAKE_SOURCE_DIR}/third_party/pdfium)
add_library(filo_pdfium INTERFACE)
target_include_directories(filo_pdfium INTERFACE ${PDFIUM_DIR}/include)
target_link_libraries(filo_pdfium INTERFACE ${PDFIUM_DIR}/lib/pdfium.dll.lib)

function(filo_embed_manifest target)
  target_sources(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src/app/filo.manifest)
endfunction()
function(filo_copy_runtime target)
  add_dependencies(${target} filo_icon_packs)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PDFIUM_DIR}/bin/pdfium.dll $<TARGET_FILE_DIR:${target}>)
endfunction()
