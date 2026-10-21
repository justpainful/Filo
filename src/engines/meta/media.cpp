// Tag editing for audio containers (MP3 / FLAC / OGG / Opus / WAV) and MKV / WebM through ffmpeg -c copy:
// the packets are copied untouched, only the metadata (and optionally the cover picture) changes.
#include "engines/common/ffmpeg.h"
#include "engines/meta/internal.h"
#include <algorithm>
#include <map>

namespace filo::meta::detail {

namespace {

std::string FfmpegKey(const std::string& tagIn) {
  std::string tag = tagIn;
  size_t c = tag.rfind(':');
  if (c != std::string::npos) tag = tag.substr(c + 1);
  static const std::map<std::string, std::string> m = {
      {"Title", "title"}, {"Artist", "artist"}, {"Album", "album"}, {"AlbumArtist", "album_artist"}, {"Band", "album_artist"}, {"Track", "track"},
      {"TrackNumber", "track"}, {"Year", "date"}, {"Date", "date"}, {"RecordingTime", "date"}, {"Genre", "genre"}, {"Comment", "comment"}, {"Comments", "comment"},
      {"Composer", "composer"}, {"Copyright", "copyright"}, {"DiscNumber", "disc"}, {"Part", "disc"}, {"Lyrics", "lyrics"}, {"Publisher", "publisher"},
      {"Description", "description"}, {"Language", "language"}, {"Conductor", "conductor"}, {"Subtitle", "subtitle"}, {"Performer", "performer"}, {"Encoder", "encoder"}};
  auto it = m.find(tag);
  return it == m.end() ? std::string() : it->second;
}

std::wstring SafeValue(const std::wstring& v) {
  std::wstring s;
  for (wchar_t c : v) s.push_back(c == L'\r' ? L' ' : c);          // ffmpeg metadata values are single strings; newlines are kept for lyrics
  return s;
}

} // namespace

WriteResult WriteViaFfmpeg(const fs::path& input, const std::wstring& format, const Edits& e, const fs::path& tempOut, JobContext* ctx) {
  WriteResult r;
  if (!ff::Available()) { r.error = L"ffmpeg is not installed, so tags of this file type cannot be edited."; return r; }
  bool isMatroska = format == L"mkv" || format == L"webm";
  bool coverCapable = format == L"mp3" || format == L"flac";
  if ((e.coverArt || e.removeCoverArt) && !coverCapable) {
    if (e.coverArt) { r.error = L"Cover art can only be embedded in MP3, FLAC and M4A files."; return r; }
  }
  if (e.coverArt) {
    std::error_code ec;
    if (!fs::exists(*e.coverArt, ec)) { r.error = L"The cover picture file was not found."; return r; }
    std::wstring ext = ExtOf(*e.coverArt);
    if (ext != L"jpg" && ext != L"jpeg" && ext != L"png") { r.error = L"The cover picture must be a JPG or PNG file."; return r; }
  }

  std::vector<std::wstring> a = {L"-i", input.wstring()};
  if (e.coverArt) { a.push_back(L"-i"); a.push_back(e.coverArt->wstring()); }

  // stream selection
  if (isMatroska) {
    a.insert(a.end(), {L"-map", L"0"});
  } else if (e.coverArt) {
    a.insert(a.end(), {L"-map", L"0:a", L"-map", L"1:v"});
  } else if (e.removeCoverArt || e.removeAll) {
    a.insert(a.end(), {L"-map", L"0:a"});
  } else {
    a.insert(a.end(), {L"-map", L"0"});
  }
  a.insert(a.end(), {L"-c", L"copy"});
  // Ogg / Opus keep their comments per stream: the muxer reads stream metadata, so every change is applied there too
  const bool perStream = format == L"ogg" || format == L"opus";
  if (e.removeAll) {
    a.insert(a.end(), {L"-map_metadata", L"-1", L"-map_chapters", L"-1"});
    if (perStream) a.insert(a.end(), {L"-map_metadata:s:a:0", L"-1"});
  } else a.insert(a.end(), {L"-map_metadata", L"0"});

  // removals first, then values (ffmpeg applies -metadata in command line order)
  auto addKey = [&](const std::string& key, const std::wstring* value) -> bool {
    std::string fk = FfmpegKey(key);
    if (fk.empty()) { r.error = L"The field '" + Widen(key).substr(0, 60) + L"' cannot be edited for " + format + L" files."; return false; }
    std::wstring kv = Widen(fk) + L"=" + (value ? SafeValue(*value) : std::wstring());
    a.push_back(L"-metadata");
    a.push_back(kv);
    if (perStream) { a.push_back(L"-metadata:s:a:0"); a.push_back(kv); }
    return true;
  };
  for (auto& k : e.remove) if (!addKey(k, nullptr)) return r;
  for (Group g : e.removeGroups) {
    if (g == Group::Gps) { a.insert(a.end(), {L"-metadata", L"location=", L"-metadata", L"location-eng="}); }
    else if (g == Group::Author || g == Group::Audio || g == Group::Document) {
      for (const char* k : {"title", "artist", "album", "album_artist", "composer", "comment", "genre", "date", "track", "disc", "copyright", "description", "lyrics", "publisher"}) {
        a.push_back(L"-metadata"); a.push_back(Widen(k) + L"=");
      }
    }
  }
  for (auto& [k, v] : e.set) if (!addKey(k, &v)) return r;

  if (coverCapable && e.coverArt) {
    a.insert(a.end(), {L"-disposition:v:0", L"attached_pic", L"-metadata:s:v:0", L"title=Album cover", L"-metadata:s:v:0", L"comment=Cover (front)"});
  }
  if (format == L"mp3") a.insert(a.end(), {L"-id3v2_version", L"3", L"-write_id3v1", L"0"});
  a.insert(a.end(), {L"-fflags", L"+bitexact", L"-flags:a", L"+bitexact"});
  a.push_back(tempOut.wstring());

  double dur = 0;
  if (auto mi = ff::Probe(input, 30000)) dur = mi->duration;
  JobContext local;
  JobContext& use = ctx ? *ctx : local;
  EngineResult er = ff::Run(a, dur, use, 0.15, 0.7);
  if (!er.ok) { r.error = er.error.empty() ? L"ffmpeg could not write the tags." : (L"The tags could not be written: " + er.error); return r; }
  if (format == L"wav") r.warnings.push_back(L"Only the audio and the standard tag chunks of the WAV file were carried over (extra chunks such as cue points are not kept).");
  r.ok = true;
  return r;
}

} // namespace filo::meta::detail
