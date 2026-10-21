#include "core/json.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace filo {

double Json::AsNumber(double def) const {
  if (type_ == Type::Number) return n_;
  if (type_ == Type::String && !s_.empty()) {
    char* end = nullptr;
    double v = std::strtod(s_.c_str(), &end);
    if (end != s_.c_str()) return v;
  }
  if (type_ == Type::Bool) return b_ ? 1 : 0;
  return def;
}

Json& Json::operator[](const std::string& key) {
  if (type_ != Type::Object) { *this = MakeObject(); }
  return o_[key];
}
const Json& Json::Get(const std::string& key) const {
  static const Json null;
  if (type_ != Type::Object) return null;
  auto it = o_.find(key);
  return it == o_.end() ? null : it->second;
}
void Json::Push(Json v) {
  if (type_ != Type::Array) *this = MakeArray();
  a_.push_back(std::move(v));
}
const Json& Json::At(size_t i) const {
  static const Json null;
  return (type_ == Type::Array && i < a_.size()) ? a_[i] : null;
}

static void EscapeTo(std::string& out, const std::string& s) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out.push_back((char)c);
    }
  }
  out.push_back('"');
}

void Json::DumpTo(std::string& out, bool pretty, int indent) const {
  auto nl = [&](int n) { if (pretty) { out.push_back('\n'); out.append((size_t)n * 2, ' '); } };
  switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += b_ ? "true" : "false"; break;
    case Type::Number: {
      char b[48];
      if (std::floor(n_) == n_ && std::fabs(n_) < 9e15) snprintf(b, sizeof b, "%lld", (long long)n_);
      else snprintf(b, sizeof b, "%.17g", n_);
      out += b; break; }
    case Type::String: EscapeTo(out, s_); break;
    case Type::Array:
      out.push_back('[');
      for (size_t i = 0; i < a_.size(); ++i) { if (i) out.push_back(','); nl(indent + 1); a_[i].DumpTo(out, pretty, indent + 1); }
      if (!a_.empty()) nl(indent);
      out.push_back(']'); break;
    case Type::Object: {
      out.push_back('{');
      bool first = true;
      for (auto& [k, v] : o_) {
        if (!first) out.push_back(',');
        first = false; nl(indent + 1);
        EscapeTo(out, k); out += pretty ? ": " : ":";
        v.DumpTo(out, pretty, indent + 1);
      }
      if (!o_.empty()) nl(indent);
      out.push_back('}'); break; }
  }
}
std::string Json::Dump(bool pretty) const { std::string s; DumpTo(s, pretty, 0); return s; }

namespace {
struct Parser {
  std::string_view s; size_t i = 0; std::string err; int depth = 0;
  void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
  bool fail(const char* m) { if (err.empty()) err = std::string(m) + " at offset " + std::to_string(i); return false; }
  static void utf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) o.push_back((char)cp);
    else if (cp < 0x800) { o.push_back((char)(0xC0 | (cp >> 6))); o.push_back((char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { o.push_back((char)(0xE0 | (cp >> 12))); o.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); o.push_back((char)(0x80 | (cp & 0x3F))); }
    else { o.push_back((char)(0xF0 | (cp >> 18))); o.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); o.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); o.push_back((char)(0x80 | (cp & 0x3F))); }
  }
  bool hex4(uint32_t& v) {
    if (i + 4 > s.size()) return false;
    v = 0;
    for (int k = 0; k < 4; ++k) {
      char c = s[i++]; v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10; else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return false;
    }
    return true;
  }
  bool str(std::string& out) {
    if (s[i] != '"') return fail("expected string");
    ++i;
    while (i < s.size()) {
      char c = s[i++];
      if (c == '"') return true;
      if (c == '\\') {
        if (i >= s.size()) break;
        char e = s[i++];
        switch (e) {
          case '"': out.push_back('"'); break; case '\\': out.push_back('\\'); break; case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break; case 'f': out.push_back('\f'); break; case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break; case 't': out.push_back('\t'); break;
          case 'u': {
            uint32_t cp; if (!hex4(cp)) return fail("bad \\u escape");
            if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
              i += 2; uint32_t lo; if (!hex4(lo)) return fail("bad surrogate");
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            utf8(out, cp); break; }
          default: return fail("bad escape");
        }
      } else out.push_back(c);
    }
    return fail("unterminated string");
  }
  bool value(Json& out) {
    ws();
    if (i >= s.size()) return fail("unexpected end");
    if (++depth > 200) return fail("nesting too deep");
    struct D { int& d; ~D() { --d; } } dg{depth};
    char c = s[i];
    if (c == '{') {
      ++i; out = Json::MakeObject(); ws();
      if (i < s.size() && s[i] == '}') { ++i; return true; }
      for (;;) {
        ws(); std::string k; if (i >= s.size() || !str(k)) return false;
        ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':'"); ++i;
        Json v; if (!value(v)) return false;
        out[k] = std::move(v);
        ws(); if (i >= s.size()) return fail("unexpected end");
        if (s[i] == ',') { ++i; continue; }
        if (s[i] == '}') { ++i; return true; }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      ++i; out = Json::MakeArray(); ws();
      if (i < s.size() && s[i] == ']') { ++i; return true; }
      for (;;) {
        Json v; if (!value(v)) return false;
        out.Push(std::move(v));
        ws(); if (i >= s.size()) return fail("unexpected end");
        if (s[i] == ',') { ++i; continue; }
        if (s[i] == ']') { ++i; return true; }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') { std::string v; if (!str(v)) return false; out = Json(std::move(v)); return true; }
    if (s.compare(i, 4, "true") == 0) { i += 4; out = Json(true); return true; }
    if (s.compare(i, 5, "false") == 0) { i += 5; out = Json(false); return true; }
    if (s.compare(i, 4, "null") == 0) { i += 4; out = Json(); return true; }
    size_t st = i;
    if (s[i] == '-' || s[i] == '+') ++i;
    while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '-' || s[i] == '+')) ++i;
    if (i == st) return fail("unexpected character");
    out = Json(std::strtod(std::string(s.substr(st, i - st)).c_str(), nullptr));
    return true;
  }
};
} // namespace

std::optional<Json> Json::Parse(std::string_view text, std::string* error) {
  Parser p{text};
  // tolerate a UTF-8 BOM
  if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) p.i = 3;
  Json v;
  if (!p.value(v)) { if (error) *error = p.err; return std::nullopt; }
  p.ws();
  if (p.i != p.s.size()) { if (error) *error = "trailing characters"; return std::nullopt; }
  return v;
}

} // namespace filo
