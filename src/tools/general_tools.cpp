// Registration of the general tool group: Compress, Metadata (first two entries of every tools wheel in the reference).
#include "tools/tools.h"

namespace filo::tools {
ToolDialog* MakeCompress(const Selection& sel);
ToolDialog* MakeEditMetadata(const Selection& sel);
}

using namespace filo;
using namespace filo::tools;

FILO_TOOL_GROUP(General) {
  ToolInfo c;
  c.id = "compress"; c.label = L"Compress"; c.wheelLabel = L"Compress"; c.icon = "compress"; c.category = "general";
  c.accepts = [](const Selection& s, std::wstring* why) {
    if (!SelAllIn(s, {Group::Image, Group::Video, Group::Audio, Group::Pdf})) { SelReason(why, L"Select images, videos, audio files or PDFs."); return false; }
    return true;
  };
  add(c, &MakeCompress);

  ToolInfo m;
  m.id = "edit-metadata"; m.label = L"Edit Metadata"; m.wheelLabel = L"Metadata"; m.icon = "metadata"; m.category = "general";
  m.accepts = [](const Selection& s, std::wstring* why) {
    if (s.count() != 1 || s.items[0].isDir || s.items[0].group == Group::Unknown || s.items[0].group == Group::Folder) { SelReason(why, L"Select one file."); return false; }
    return true;
  };
  add(m, &MakeEditMetadata);
}
