# Document engine: XmlLite (well-formedness verification), PDF engine (PDFium wrapper used for slide rendering / PDF verification).
target_link_libraries(filo_eng_doc PUBLIC xmllite shlwapi ole32 windowscodecs crypt32 filo_eng_pdf filo_eng_image)
