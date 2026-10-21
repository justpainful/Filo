// Registration entry points. Every engine library defines exactly one of these in its own directory;
// engines/register.cpp calls them. If an engine directory has no sources yet, its function is provided as a weak stub
// there (so the project builds while engines are developed independently).
#pragma once
namespace filo {
void RegisterImageEngine();      // src/engines/image   - WIC + ffmpeg codecs: image <-> image, image -> pdf/docx
void RegisterMediaEngine();      // src/engines/media   - ffmpeg: video/audio conversion
void RegisterPdfEngine();        // src/engines/pdf     - PDFium: PDF -> images/text/markdown/docx/pptx/xlsx/epub
void RegisterDocEngine();        // src/engines/doc     - documents, presentations, spreadsheets, subtitles (native + LibreOffice)
void RegisterArchiveEngine();    // src/engines/archive - zip/tar/gz/rar
}
