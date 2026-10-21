// Registration of the image tool group (after Edit Photo). Wheel order on the reference: Compress, Metadata, Edit, Annotate,
// Add BG, Crop, Redact - then the Windows additions (Collage, QR, PDF).
#include "tools/tools.h"

namespace filo::tools {
ToolDialog* MakeAnnotatePhoto(const Selection& sel);
ToolDialog* MakeAddBackground(const Selection& sel);
ToolDialog* MakeCropImage(const Selection& sel);
ToolDialog* MakeRedactPhoto(const Selection& sel);
ToolDialog* MakeImagesToPdf(const Selection& sel);
ToolDialog* MakeReadQr(const Selection& sel);
ToolDialog* MakeCollage(const Selection& sel);
}

using namespace filo;
using namespace filo::tools;

FILO_TOOL_GROUP(Image) {
  ToolInfo an;
  an.id = "annotate"; an.label = L"Annotate"; an.wheelLabel = L"Annotate"; an.icon = "annotate"; an.category = "image";
  an.accepts = [](const Selection& s, std::wstring* why) {
    if (s.count() != 1 || !SelAllGroup(s, Group::Image)) { SelReason(why, L"Select one image."); return false; }
    return true;
  };
  add(an, &MakeAnnotatePhoto);

  ToolInfo bg;
  bg.id = "add-background"; bg.label = L"Add a Background"; bg.wheelLabel = L"Add BG"; bg.icon = "addbg"; bg.category = "image";
  bg.accepts = [](const Selection& s, std::wstring* why) {
    if (s.count() != 1 || !SelAllGroup(s, Group::Image)) { SelReason(why, L"Select one image."); return false; }
    return true;
  };
  add(bg, &MakeAddBackground);

  ToolInfo crop;
  crop.id = "crop-image"; crop.label = L"Crop Images"; crop.wheelLabel = L"Crop"; crop.icon = "crop"; crop.category = "image";
  crop.accepts = [](const Selection& s, std::wstring* why) {
    if (s.count() < 1 || !SelAllGroup(s, Group::Image)) { SelReason(why, L"Select one or more images."); return false; }
    return true;
  };
  add(crop, &MakeCropImage);

  ToolInfo red;
  red.id = "redact-photo"; red.label = L"Redact Photos"; red.wheelLabel = L"Redact"; red.icon = "redact"; red.category = "image";
  red.accepts = [](const Selection& s, std::wstring* why) {
    if (s.count() != 1 || !SelAllGroup(s, Group::Image)) { SelReason(why, L"Select one image."); return false; }
    return true;
  };
  add(red, &MakeRedactPhoto);

  ToolInfo pdf;
  pdf.id = "images-to-pdf"; pdf.label = L"Create PDF"; pdf.wheelLabel = L"PDF"; pdf.icon = "document"; pdf.category = "image";
  pdf.accepts = [](const Selection& s, std::wstring* why) {
    if (s.count() < 1 || !SelAllGroup(s, Group::Image)) { SelReason(why, L"Select one or more images."); return false; }
    return true;
  };
  add(pdf, &MakeImagesToPdf);

  ToolInfo qr;
  qr.id = "read-qr"; qr.label = L"Read QR Code"; qr.wheelLabel = L"QR"; qr.icon = "qr"; qr.category = "image";
  qr.accepts = [](const Selection& s, std::wstring* why) {
    if (s.count() != 1 || !SelAllGroup(s, Group::Image)) { SelReason(why, L"Select one image."); return false; }
    return true;
  };
  add(qr, &MakeReadQr);

  ToolInfo col;
  col.id = "collage"; col.label = L"Collage"; col.wheelLabel = L"Collage"; col.icon = "organize"; col.category = "image";
  col.accepts = [](const Selection& s, std::wstring* why) {
    if (s.count() < 2 || !SelAllGroup(s, Group::Image)) { SelReason(why, L"Select two or more images."); return false; }
    return true;
  };
  add(col, &MakeCollage);
}
