#include "shell/browser_url.h"
#include "core/com.h"
#include "core/util.h"
#include <UIAutomation.h>

namespace filo::shell {

std::wstring NormalizeAddressBarText(const std::wstring& raw) {
  std::wstring t = Trim(raw);
  if (t.empty() || t.size() > 2048) return {};
  if (t.find_first_of(L" \t\r\n") != std::wstring::npos) return {};                       // a search phrase, not an address
  if (StartsWithI(t, L"https://") || StartsWithI(t, L"http://")) return t;
  const size_t dot = t.find(L'.');
  const size_t slash = t.find(L'/');
  if (dot == std::wstring::npos || dot == 0 || (slash != std::wstring::npos && slash < dot)) return {};   // "localhost/x", "a/b.c": not a site address
  if (dot + 1 >= t.size() || t[dot + 1] == L'.') return {};
  return L"https://" + t;                                                                // Chrome/Edge hide the scheme in the bar
}

std::wstring BrowserPageUrl(HWND window) {
  if (!window) return {};
  ComPtr<IUIAutomation> uia;
  if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia.GetAddressOf())))) return {};
  ComPtr<IUIAutomationElement> root;
  if (FAILED(uia->ElementFromHandle(window, &root)) || !root) return {};

  ComPtr<IUIAutomationCondition> isEdit;
  VARIANT v; VariantInit(&v); v.vt = VT_I4; v.lVal = UIA_EditControlTypeId;
  if (FAILED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &isEdit))) return {};
  ComPtr<IUIAutomationElementArray> edits;
  if (FAILED(root->FindAll(TreeScope_Descendants, isEdit.Get(), &edits)) || !edits) return {};
  int n = 0; edits->get_Length(&n);

  std::wstring firstAddress, named;
  for (int i = 0; i < n && i < 40; ++i) {
    ComPtr<IUIAutomationElement> el;
    if (FAILED(edits->GetElement(i, &el)) || !el) continue;
    ComPtr<IUIAutomationValuePattern> vp;
    if (FAILED(el->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(vp.GetAddressOf()))) || !vp) continue;
    BSTR val = nullptr;
    if (FAILED(vp->get_CurrentValue(&val)) || !val) continue;
    const std::wstring url = NormalizeAddressBarText(val);
    SysFreeString(val);
    if (url.empty()) continue;
    if (firstAddress.empty()) firstAddress = url;
    BSTR name = nullptr;
    if (SUCCEEDED(el->get_CurrentName(&name)) && name) {
      const std::wstring nm = ToLower(name);
      SysFreeString(name);
      // "Address and search bar" (Chrome, Edge, Brave), "Search with ... or enter address" (Firefox)
      if (named.empty() && (nm.find(L"address") != std::wstring::npos || nm.find(L"url") != std::wstring::npos)) named = url;
    }
  }
  return !named.empty() ? named : firstAddress;
}

}  // namespace filo::shell
