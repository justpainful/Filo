// Minimal tolerant XML parser (SAX core + DOM on top) for OOXML / ODF / EPUB parts, plus well-formedness verification through
// Windows XmlLite (an independent implementation, used by the verifiers and the tests).
#pragma once
#include "engines/doc/docutil.h"
#include <functional>
#include <memory>

namespace filo::doc::xml {

using AttrList = std::vector<std::pair<std::string, std::string>>;

struct Sax {
  std::function<void(std::string_view name, const AttrList& attrs)> start;
  std::function<void(std::string_view name)> end;       // also fired for self-closing elements, right after start
  std::function<void(const std::string& text)> text;    // entity-decoded; CDATA included
};

// Parses UTF-8 (BOM stripped; UTF-16 and a declared ISO-8859-1/windows-1252 encoding are transcoded). Tolerates HTML-ish entities.
// Returns false (with *err) only when no element could be found or the nesting is absurdly deep.
bool Parse(std::string_view data, const Sax& sax, std::string* err = nullptr);

inline std::string_view LocalName(std::string_view qn) { size_t c = qn.rfind(':'); return c == std::string_view::npos ? qn : qn.substr(c + 1); }

struct Node {
  std::string name;                       // qualified name as written ("w:p")
  AttrList attrs;
  std::vector<std::unique_ptr<Node>> kids;
  std::string text;                       // only for text nodes
  bool isText = false;

  std::string_view Local() const { return LocalName(name); }
  bool Is(std::string_view local) const { return !isText && Local() == local; }
  // Attribute by qualified name or, failing that, by local name ("val" matches "w:val").
  const std::string* Attr(std::string_view n) const;
  std::string AttrOr(std::string_view n, std::string_view def = {}) const { auto* a = Attr(n); return a ? *a : std::string(def); }
  const Node* Child(std::string_view local) const;
  std::vector<const Node*> Children(std::string_view local) const;
  std::string InnerText() const;          // concatenated descendant text
};

std::unique_ptr<Node> ParseDom(std::string_view data, std::string* err = nullptr, size_t maxNodes = 5000000);

// XmlLite well-formedness check (does not validate against a schema). `why` receives "line N: message" on failure.
bool CheckWellFormed(const uint8_t* data, size_t n, std::string* why = nullptr);
inline bool CheckWellFormed(const std::string& s, std::string* why = nullptr) { return CheckWellFormed((const uint8_t*)s.data(), s.size(), why); }

// Named character reference (HTML 4 subset + XML five) -> code point, 0 when unknown.
uint32_t LookupEntity(std::string_view name);
// Decodes &named; &#N; &#xH; references (unknown ones are kept verbatim) and appends the result.
void DecodeEntitiesInto(std::string_view s, std::string& out);
inline std::string DecodeEntities(std::string_view s) { std::string o; DecodeEntitiesInto(s, o); return o; }

} // namespace filo::doc::xml
