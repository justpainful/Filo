#include "test.h"
#include "engines/web/web.h"
#include "engines/web/web_internal.h"

using namespace filo;
using namespace filo::web;

TEST(web_detects_platforms) {
  CHECK(DetectPlatform(L"https://www.tiktok.com/@dram_i/video/7699093820633255174?is_from_webapp=1").id == "tiktok");
  CHECK(DetectPlatform(L"https://vm.tiktok.com/ZMabc/").name == L"TikTok");
  CHECK(DetectPlatform(L"https://youtu.be/6Eu2wkEc5xM").id == "youtube");
  CHECK(DetectPlatform(L"https://m.youtube.com/watch?v=1").name == L"YouTube");
  CHECK(DetectPlatform(L"https://x.com/th2049/status/2093312888031367387?s=20").name == L"X");
  CHECK(DetectPlatform(L"https://twitter.com/a/status/1").id == "x");
  CHECK(DetectPlatform(L"https://notx.com/a").id == "web");              // "x.com" must match a whole domain label
  Platform other = DetectPlatform(L"https://cdn.example.org/clip.mp4");
  CHECK(other.id == "web");
  CHECK(other.name == L"example");
  CHECK(!other.known);
}

TEST(web_extracts_urls_from_dragged_text) {
  CHECK(ExtractUrl(L"https://youtu.be/6Eu2wkEc5xM") == L"https://youtu.be/6Eu2wkEc5xM");
  CHECK(ExtractUrl(L"  look at this: https://x.com/a/status/1?s=20, wow") == L"https://x.com/a/status/1?s=20");
  CHECK(ExtractUrl(L"(see https://www.tiktok.com/@a/video/1)") == L"https://www.tiktok.com/@a/video/1");
  CHECK(ExtractUrl(L"www.tiktok.com/@a/video/1") == L"https://www.tiktok.com/@a/video/1");
  CHECK(ExtractUrl(L"just words").empty());
  CHECK(ExtractUrl(L"https://").empty());
  CHECK(ExtractUrl(L"first https://a.example/1 then http://b.example/2") == L"https://a.example/1");
}

TEST(web_safe_names) {
  CHECK(SafePart(L"dram_i", 18) == L"dram_i");
  CHECK(SafePart(L"  Some User!! ", 18) == L"Some_User");
  CHECK(SafePart(L"a/b\\c:d*e?f\"g<h>i|j", 30) == L"a_b_c_d_e_f_g_h_i_j");
  CHECK(SafePart(L"abcdefghijklmnopqrstuvwxyz", 8) == L"abcdefgh");
  CHECK(SafePart(L"\x0645\x0631\x062D\x0628\x0627", 18) == L"\x0645\x0631\x062D\x0628\x0627");   // Arabic names survive
  CHECK(SafePart(L"...", 8).empty());
}

TEST(web_base_name_is_unique_and_medium) {
  LinkInfo li;
  li.platform = DetectPlatform(L"https://www.tiktok.com/@dram_i/video/7699093820633255174");
  li.uploader = L"dram_i";
  li.id = "7699093820633255174";
  CHECK(BaseName(li) == L"Filo_TikTok_dram_i_0633255174");
  li.id = "6Eu2wkEc5xM";
  li.platform = DetectPlatform(L"https://youtu.be/6Eu2wkEc5xM");
  li.uploader = L"A Very Long Channel Name That Goes On";
  const std::wstring n = BaseName(li);
  CHECK(n.rfind(L"Filo_YouTube_", 0) == 0);
  CHECK(n.size() < 50);
  CHECK(n.find(L"6Eu2wkEc5xM") != std::wstring::npos);
  li.uploader.clear();
  CHECK(BaseName(li) == L"Filo_YouTube_6Eu2wkEc5xM");
}

static const char* kVideoJson = R"({
  "id":"2112843069964042240","title":"Nitish - a demo","uploader":"Nitish","uploader_id":"nitishkmrk","duration":10.0,
  "webpage_url":"https://x.com/nitishkmrk/status/2112843168576418030","thumbnail":"https://pbs.twimg.com/t.jpg",
  "formats":[
    {"format_id":"a","vcodec":"none","acodec":"mp4a","filesize":150000},
    {"format_id":"v1","vcodec":"avc1","acodec":"none","height":540,"width":540,"tbr":832,"filesize":1000000},
    {"format_id":"v2","vcodec":"avc1","acodec":"none","height":960,"width":960,"tbr":8768,"filesize":10000000}]})";

TEST(web_parses_a_single_video) {
  auto j = Json::Parse(kVideoJson);
  CHECK(j.has_value());
  LinkInfo li = ParseProbe(*j, L"https://x.com/nitishkmrk/status/2112843168576418030?s=20");
  CHECK(li.ok);
  CHECK(!li.isPlaylist);
  CHECK_EQ(li.items.size(), (size_t)1);
  CHECK(li.items[0].kind == ItemKind::Video);
  CHECK_EQ(li.items[0].height, 960);
  CHECK_EQ(li.items[0].width, 960);
  CHECK_EQ(li.items[0].size, (int64_t)10150000);
  CHECK(li.uploader == L"nitishkmrk");
  CHECK(li.HasAudioChoice());
  CHECK(li.platform.name == L"X");
  CHECK(li.items[0].detail.find(L"960") != std::wstring::npos);
  CHECK(li.items[0].detail.find(L"0:10") != std::wstring::npos);
}

static const char* kSlideshowJson = R"({
  "_type":"playlist","id":"7700000000000000001","title":"my slideshow","uploader":"someone","webpage_url":"https://www.tiktok.com/@someone/video/7700000000000000001",
  "entries":[
    {"_type":"url","id":"7700000000000000001_01","title":"s #1","url":"https://img.example/1.jpeg","ext":"jpg","width":1080,"height":1920,"_filo_kind":"image","http_headers":{"Referer":"https://www.tiktok.com/"}},
    {"id":"7700000000000000001_02","title":"s #2","url":"https://img.example/2.jpeg","ext":"jpg","width":1080,"height":1920,"_filo_kind":"image"},
    {"id":"7700000000000000001_audio","title":"Original sound","url":"https://snd.example/a.m4a","ext":"m4a","vcodec":"none","_filo_kind":"audio"}]})";

TEST(web_parses_a_slideshow) {
  auto j = Json::Parse(kSlideshowJson);
  CHECK(j.has_value());
  LinkInfo li = ParseProbe(*j, L"https://www.tiktok.com/@someone/video/7700000000000000001");
  CHECK(li.ok);
  CHECK(li.isPlaylist);
  CHECK_EQ(li.items.size(), (size_t)3);
  CHECK_EQ(li.CountOf(ItemKind::Image), 2);
  CHECK_EQ(li.CountOf(ItemKind::Audio), 1);
  CHECK(!li.HasAudioChoice());                     // sound is its own selectable item here
  CHECK_EQ(li.items[0].index, 1);
  CHECK_EQ(li.items[1].index, 2);
  CHECK(li.items[0].url == "https://img.example/1.jpeg");
  CHECK_EQ(li.items[0].headers.size(), (size_t)1);
  CHECK(li.items[0].headers[0].second == "https://www.tiktok.com/");
  CHECK(li.thumbUrl == "https://img.example/1.jpeg" || !li.thumbUrl.empty());
  CHECK(BaseName(li) == L"Filo_TikTok_someone_0000000001");
}

TEST(web_author_prefers_a_handle_over_a_numeric_id) {
  auto j = Json::Parse(R"({"id":"1","title":"t","uploader":"cin0matic","uploader_id":"689062755229882880","formats":[{"vcodec":"h264","height":1080}]})");
  CHECK(ParseProbe(*j, L"https://www.tiktok.com/@cin0matic/video/1").uploader == L"cin0matic");
  auto k = Json::Parse(R"({"id":"2","title":"t","uploader":"Display Name","uploader_id":"@handle","formats":[{"vcodec":"h264","height":720}]})");
  CHECK(ParseProbe(*k, L"https://www.youtube.com/watch?v=2").uploader == L"handle");
}

TEST(web_rejects_playlists_and_empty_results) {
  auto j = Json::Parse(R"({"_type":"playlist","id":"PL1","title":"p","entries":[{"_type":"url","id":"a","url":"https://youtu.be/a"}]})");
  LinkInfo li = ParseProbe(*j, L"https://www.youtube.com/playlist?list=PL1");
  CHECK(!li.ok);
  CHECK(!li.error.empty());
  auto e = Json::Parse(R"({"_type":"playlist","id":"x","entries":[]})");
  CHECK(!ParseProbe(*e, L"https://x.com/a/status/1").ok);
}

TEST(web_a_lone_photo_is_an_image) {
  auto j = Json::Parse(R"({"id":"p1","title":"pic","url":"https://img.example/p.png","ext":"png","width":800,"height":600,"formats":[]})");
  LinkInfo li = ParseProbe(*j, L"https://img.example/p");
  CHECK(li.ok);
  CHECK(li.items[0].kind == ItemKind::Image);
  CHECK(li.items[0].url == "https://img.example/p.png");
}

TEST(web_audio_stream_is_downloaded_by_ytdlp) {
  auto j = Json::Parse(R"({"id":"track","title":"track","formats":[{"vcodec":"none","acodec":"aac","url":"https://example.org/stream.m3u8"}]})");
  LinkInfo li = ParseProbe(*j, L"https://soundcloud.com/example/track");
  CHECK(li.ok);
  CHECK(li.items[0].kind == ItemKind::Audio);
  CHECK(li.items[0].useYtDlp);
}

TEST(web_single_playlist_entry_keeps_its_index) {
  auto j = Json::Parse(R"({"_type":"playlist","entries":[{"id":"v","formats":[{"vcodec":"h264"}]}]})");
  LinkInfo li = ParseProbe(*j, L"https://example.org/list");
  CHECK(li.ok);
  CHECK(li.isPlaylist);
  CHECK_EQ(li.items[0].index, 1);
}

TEST(web_accepts_domains_without_a_scheme) {
  CHECK(ExtractUrl(L"tiktok.com/@someone/photo/123") == L"https://tiktok.com/@someone/photo/123");
  CHECK(ExtractUrl(L"user@example.com").empty());
  CHECK(ExtractUrl(L"hello world").empty());
  CHECK(DetectPlatform(L"https://holiznacc0.bandcamp.com/album/reflections").id == "bandcamp");
}

// ---- accounts -----------------------------------------------------------------------------------------------------------------------
TEST(web_recognises_account_addresses) {
  AccountRef t = ClassifyAccount(L"https://www.tiktok.com/@nasa?lang=en");
  CHECK(t.is); CHECK(t.platform.id == "tiktok"); CHECK(t.handle == L"nasa");
  CHECK_EQ(t.listUrls.size(), (size_t)1); CHECK(t.listUrls[0] == "https://www.tiktok.com/@nasa");
  CHECK(!IsAccountUrl(L"https://www.tiktok.com/@nasa/video/7706264639297850637"));
  CHECK(!IsAccountUrl(L"https://vm.tiktok.com/ZMabc/"));

  AccountRef y = ClassifyAccount(L"https://www.youtube.com/@NASA");
  CHECK(y.is); CHECK(y.handle == L"NASA"); CHECK_EQ(y.listUrls.size(), (size_t)3);
  CHECK(y.listUrls[0] == "https://www.youtube.com/@NASA/videos");
  CHECK(y.listUrls[1] == "https://www.youtube.com/@NASA/shorts");
  CHECK(y.listUrls[2] == "https://www.youtube.com/@NASA/streams");
  AccountRef ys = ClassifyAccount(L"https://www.youtube.com/@NASA/shorts");
  CHECK_EQ(ys.listUrls.size(), (size_t)1); CHECK(ys.listUrls[0] == "https://www.youtube.com/@NASA/shorts");
  CHECK(IsAccountUrl(L"https://www.youtube.com/channel/UCLA_DiR1FfKNvjuUpBHmylQ"));
  CHECK(IsAccountUrl(L"https://m.youtube.com/c/NASA/videos"));
  AccountRef pl = ClassifyAccount(L"https://www.youtube.com/playlist?list=PL123abc&si=x");
  CHECK(pl.is); CHECK(pl.listUrls[0] == "https://www.youtube.com/playlist?list=PL123abc&si=x" || pl.listUrls[0] == "https://www.youtube.com/playlist?list=PL123abc");
  CHECK(!IsAccountUrl(L"https://www.youtube.com/watch?v=6Eu2wkEc5xM"));
  CHECK(!IsAccountUrl(L"https://youtu.be/6Eu2wkEc5xM"));
  CHECK(!IsAccountUrl(L"https://www.youtube.com/playlist"));

  AccountRef x = ClassifyAccount(L"https://x.com/NASA");
  CHECK(x.is); CHECK(x.platform.id == "x"); CHECK(x.handle == L"NASA"); CHECK(x.listUrls[0] == "https://x.com/NASA");
  CHECK(ClassifyAccount(L"https://twitter.com/NASA/media").listUrls[0] == "https://x.com/NASA/media");
  CHECK(!IsAccountUrl(L"https://x.com/NASA/status/2120974110335312297"));
  CHECK(!IsAccountUrl(L"https://x.com/home"));
  CHECK(!IsAccountUrl(L"https://x.com/i/status/1"));
  CHECK(!IsAccountUrl(L"https://x.com/search?q=nasa"));
  CHECK(!IsAccountUrl(L"https://x.com/this_name_is_far_too_long_for_x"));

  CHECK(IsAccountUrl(L"https://www.instagram.com/nasa/"));
  CHECK(!IsAccountUrl(L"https://www.instagram.com/p/Cabc123/"));
  CHECK(!IsAccountUrl(L"https://www.instagram.com/reel/Cabc123/"));
  CHECK(IsAccountUrl(L"https://www.twitch.tv/shroud"));
  CHECK(!IsAccountUrl(L"https://www.twitch.tv/videos/123456"));
  CHECK(!IsAccountUrl(L"https://clips.twitch.tv/Abc"));
  CHECK(ClassifyAccount(L"https://soundcloud.com/artist").listUrls[0] == "https://soundcloud.com/artist/tracks");
  CHECK(!IsAccountUrl(L"https://soundcloud.com/artist/some-track"));
  CHECK(IsAccountUrl(L"https://someband.bandcamp.com/"));
  CHECK(!IsAccountUrl(L"https://someband.bandcamp.com/track/song"));
  CHECK(!IsAccountUrl(L"https://vimeo.com/123456789"));
  CHECK(IsAccountUrl(L"https://vimeo.com/staffpicksfan"));
  CHECK(!IsAccountUrl(L"https://example.org/@someone"));
}

TEST(web_account_folder_name) {
  Platform p = DetectPlatform(L"https://www.tiktok.com/@nasa");
  CHECK(AccountFolderName(p, L"nasa") == L"Filo_TikTok_nasa");
  CHECK(AccountFolderName(p, L"") == L"Filo_TikTok");
  CHECK(AccountFolderName(p, L"a/b:c") == L"Filo_TikTok_a_b_c");
}

static const char* kTikTokFlat = R"({"id":"7706264639297850637","channel":"NASA","uploader":"nasa","uploader_id":"7664638705177150477",
  "duration":86,"title":"This is what American ambition looks like.","description":"This is what American ambition looks like.\nSecond line",
  "timestamp":1794254587,"view_count":93100,"like_count":10200,"repost_count":433,"comment_count":252,"ie_key":"TikTok","_type":"url",
  "url":"https://www.tiktok.com/@nasa/video/7706264639297850637"})";

TEST(web_parses_a_flat_account_entry) {
  auto j = Json::Parse(kTikTokFlat);
  CHECK(j.has_value());
  AccountEntry e;
  CHECK(ParseAccountEntry(*j, e));
  CHECK(e.id == "7706264639297850637");
  CHECK(e.url == "https://www.tiktok.com/@nasa/video/7706264639297850637");
  CHECK(e.authorHandle == "nasa");                                   // the numeric uploader_id is not a handle
  CHECK(e.authorName == L"NASA");
  CHECK_EQ(e.timestamp, (int64_t)1794254587);
  CHECK_EQ(e.likes, (int64_t)10200);
  CHECK(!e.hasMedia);
  CHECK(!e.repost);
  auto bad = Json::Parse(R"({"_type":"playlist","id":"x","entries":[]})");
  CHECK(!ParseAccountEntry(*bad, e));
  auto nourl = Json::Parse(R"({"id":"1","_type":"url"})");
  CHECK(!ParseAccountEntry(*nourl, e));
}

static const char* kXEntry = R"({"_type":"url","ie_key":"Twitter","id":"2119846862286930036","url":"https://x.com/NASA/status/2119846862286930036",
  "title":"We had a great time at MAX POWER","description":"We had a great time at MAX POWER\n\nand made a lot of memories","timestamp":1794245851,
  "uploader":"NASA","uploader_id":"NASA","like_count":5566,"repost_count":814,"comment_count":196,"view_count":null,"_filo_repost":false,
  "_filo_media":[
    {"kind":"image","url":"https://pbs.twimg.com/media/A.jpg?name=orig","ext":"jpg","width":4480,"height":3000},
    {"kind":"image","url":"https://pbs.twimg.com/media/B.jpg?name=orig","ext":"jpg","width":1366,"height":900},
    {"kind":"video","url":"https://video.twimg.com/amplify_video/1/vid/avc1/1080x1920/x.mp4","ext":"mp4","width":1080,"height":1920,"duration":40.6}]})";

TEST(web_x_entries_carry_their_media) {
  auto j = Json::Parse(kXEntry);
  CHECK(j.has_value());
  AccountEntry e;
  CHECK(ParseAccountEntry(*j, e));
  CHECK(e.hasMedia);
  CHECK_EQ(e.media.size(), (size_t)3);
  CHECK(e.media[0].kind == ItemKind::Image);
  CHECK(e.media[2].kind == ItemKind::Video);
  CHECK(e.media[2].direct);                                          // fetched from its address, no yt-dlp run
  CHECK_EQ(e.media[2].height, 1920);
  CHECK_EQ(e.views, (int64_t)-1);                                    // null stays "unknown"
  LinkInfo li = LinkInfoFromEntry(e, DetectPlatform(L"https://x.com/NASA"));
  CHECK(li.ok); CHECK(!li.textOnly);
  CHECK(li.uploader == L"NASA");
  CHECK_EQ(li.items.size(), (size_t)3);
  CHECK(BaseName(li) == L"Filo_X_NASA_9846862286930036" || BaseName(li).rfind(L"Filo_X_NASA_", 0) == 0);

  e.media.clear();
  CHECK(LinkInfoFromEntry(e, DetectPlatform(L"https://x.com/NASA")).textOnly);
  auto rt = Json::Parse(R"({"id":"5","url":"https://x.com/a/status/5","_filo_repost":true,"_filo_media":[]})");
  AccountEntry r;
  CHECK(ParseAccountEntry(*rt, r));
  CHECK(r.repost);
}

TEST(web_a_tweet_without_media_is_a_text_post) {
  auto j = Json::Parse(R"({"_type":"playlist","id":"20","title":"jack - just setting up my twttr","description":"just setting up my twttr",
    "uploader":"jack","uploader_id":"jack","timestamp":1142974214,"like_count":325372,"repost_count":126444,"comment_count":19252,"entries":[]})");
  CHECK(j.has_value());
  LinkInfo li = ParseProbe(*j, L"https://x.com/jack/status/20");
  CHECK(li.ok);
  CHECK(li.textOnly);
  CHECK(li.items.empty());
  CHECK(li.description == L"just setting up my twttr");
  CHECK_EQ(li.likes, (int64_t)325372);
  auto empty = Json::Parse(R"({"_type":"playlist","id":"1","entries":[]})");
  CHECK(!ParseProbe(*empty, L"https://x.com/a/status/1").ok);
}

TEST(web_post_text_file_has_everything_in_order) {
  LinkInfo li;
  li.platform = DetectPlatform(L"https://x.com/NASA/status/1");
  li.pageUrl = "https://x.com/NASA/status/2119846862286930036";
  li.uploader = L"NASA"; li.authorName = L"NASA";
  li.description = L"First line\n\nSecond line with \x0645\x0631\x062D\x0628\x0627";
  li.timestamp = 1794245851;
  li.likes = 5566; li.reposts = 814; li.comments = -1; li.views = 1234567;
  const std::wstring t = FormatPostText(li);
  CHECK(t.find(L"X  -  NASA\r\n") == 0);
  CHECK(t.find(L"https://x.com/NASA/status/2119846862286930036\r\n") != std::wstring::npos);
  CHECK(t.find(L"2026-11-") != std::wstring::npos);
  CHECK(t.find(L"UTC") != std::wstring::npos);
  CHECK(t.find(L"Likes 5,566") != std::wstring::npos);
  CHECK(t.find(L"Views 1,234,567") != std::wstring::npos);
  CHECK(t.find(L"Comments") == std::wstring::npos);                  // unknown counts are left out
  CHECK(t.find(L"First line\r\n\r\nSecond line with \x0645") != std::wstring::npos);   // line breaks kept, as CRLF
  CHECK(FormatUtcTime(0).empty());
  CHECK(FormatUtcTime(1794245851).size() == 20);

  LinkInfo yt;                                                         // a video: the title is a line of its own
  yt.platform = DetectPlatform(L"https://youtu.be/6Eu2wkEc5xM");
  yt.title = L"A talk"; yt.description = L"About the talk"; yt.pageUrl = "https://youtu.be/6Eu2wkEc5xM"; yt.uploader = L"chan";
  CHECK(FormatPostText(yt).find(L"A talk\r\n") == 0);
}

TEST(web_recognises_playlists) {
  AccountRef y = ClassifyAccount(L"https://www.youtube.com/playlist?list=PLF98rxtslw6sABC");
  CHECK(y.is); CHECK(y.isPlaylist);
  CHECK(!ClassifyAccount(L"https://www.youtube.com/@NASA").isPlaylist);
  AccountRef v = ClassifyAccount(L"https://vimeo.com/album/1234567");
  CHECK(v.is); CHECK(v.isPlaylist); CHECK(v.listUrls[0] == "https://vimeo.com/album/1234567");
  CHECK(ClassifyAccount(L"https://vimeo.com/showcase/777").isPlaylist);
  AccountRef s = ClassifyAccount(L"https://soundcloud.com/artist/sets/my-album");
  CHECK(s.is); CHECK(s.isPlaylist); CHECK(s.listUrls[0] == "https://soundcloud.com/artist/sets/my-album");
  CHECK(!ClassifyAccount(L"https://soundcloud.com/artist/tracks").isPlaylist);
  CHECK(ClassifyAccount(L"https://www.dailymotion.com/playlist/x7abc").isPlaylist);
}


// ---- Instagram through gallery-dl ---------------------------------------------------------------------------------------------------
static const char* kGdl1 = R"G(["Cabc123","3412345678901234567",1,3,"2026-11-12T20:16:05","nasa","NASA مرحبا",5566,"First line\nSecond \"line\"",1080,1350,"https://scontent.cdninstagram.com/v/a.jpg?stp=1&oe=ABC","jpg","post"])G";
static const char* kGdl2 = R"G(["Cabc123","3412345678901234567",2,3,"2026-11-12T20:16:05","nasa","NASA",5566,"First line\nSecond \"line\"",1080,1920,"https://scontent.cdninstagram.com/v/b.mp4?x=1","mp4","post"])G";
static const char* kGdl3 = R"G(["Cabc123","3412345678901234567",3,3,"None","nasa","NASA",null,"",null,null,"https://scontent.cdninstagram.com/v/c.webp","webp","post"])G";

TEST(web_reads_gallery_dl_lines) {
  internal::GalleryFile f;
  CHECK(internal::ParseGalleryLine(kGdl1, f));
  CHECK(f.shortcode == "Cabc123");
  CHECK_EQ(f.num, 1); CHECK_EQ(f.count, 3);
  CHECK(f.user == "nasa");
  CHECK(f.fullName == L"NASA \x0645\x0631\x062D\x0628\x0627");
  CHECK(f.text == L"First line\nSecond \"line\"");
  CHECK_EQ(f.likes, (int64_t)5566);
  CHECK_EQ(f.width, 1080); CHECK_EQ(f.height, 1350);
  CHECK(f.ext == "jpg");
  CHECK_EQ(f.timestamp, (int64_t)1794514565);                         // 2026-11-12 20:16:05 UTC
  CHECK(internal::ParseGalleryLine(kGdl3, f));
  CHECK_EQ(f.timestamp, (int64_t)0);                                  // "None" is an unknown date
  CHECK_EQ(f.likes, (int64_t)-1);
  CHECK(!internal::ParseGalleryLine("", f));
  CHECK(!internal::ParseGalleryLine("[instagram][info] Requesting https://www.instagram.com/nasa/", f));
  CHECK(!internal::ParseGalleryLine(R"G([null,null,1,1,"None",null,null,null,null,1,1,"https://x/y.jpg","jpg",null])G", f));   // no post id
}

TEST(web_gallery_files_become_one_post) {
  AccountEntry e;
  internal::GalleryFile f;
  for (const char* line : {kGdl1, kGdl2, kGdl3}) { CHECK(internal::ParseGalleryLine(line, f)); internal::AddGalleryFile(e, f); }
  CHECK(e.id == "Cabc123");
  CHECK(e.url == "https://www.instagram.com/p/Cabc123/");
  CHECK(e.hasMedia); CHECK(e.expires);
  CHECK(e.authorHandle == "nasa");
  CHECK(e.authorName == L"NASA \x0645\x0631\x062D\x0628\x0627");     // the name of the first file
  CHECK(e.title == L"First line");
  CHECK_EQ(e.media.size(), (size_t)3);
  CHECK(e.media[0].kind == ItemKind::Image);
  CHECK(e.media[1].kind == ItemKind::Video);
  CHECK(e.media[1].direct);                                           // videos are fetched from their address too
  CHECK(e.media[2].kind == ItemKind::Image);                          // .webp is a picture
  CHECK_EQ(e.media[2].index, 3);
  CHECK(e.media[1].url == "https://scontent.cdninstagram.com/v/b.mp4?x=1");

  LinkInfo li = LinkInfoFromEntry(e, DetectPlatform(L"https://www.instagram.com/nasa/"));
  CHECK(li.ok); CHECK(!li.textOnly);
  CHECK_EQ(li.items.size(), (size_t)3);
  CHECK(li.platform.id == "instagram");
  CHECK(li.description.find(L"Second") != std::wstring::npos);

  AccountEntry reel;
  const char* r = R"G(["Reel99","1",1,1,"2026-01-02T03:04:05","u","U",3,"",720,1280,"https://v/r.mp4","mp4","reel"])G";
  CHECK(internal::ParseGalleryLine(r, f));
  internal::AddGalleryFile(reel, f);
  CHECK(reel.url == "https://www.instagram.com/reel/Reel99/");
}

TEST(web_gallery_errors_are_sentences) {
  bool login = false;
  std::wstring t = internal::GalleryErrorText("[instagram][error] NotFoundError: Requested user could not be found\r\n", L"", login);
  CHECK(login);                                                       // without a sign-in this means "sign in", not "no such user"
  CHECK(t.find(L"Sign in to Instagram") != std::wstring::npos);
  login = false;
  t = internal::GalleryErrorText("[instagram][error] HttpError: '429 Too Many Requests' for 'https://www.instagram.com/api/v1/x'\n", L"", login);
  CHECK(!login);
  CHECK(t.find(L"limiting") != std::wstring::npos);
  CHECK(internal::GalleryErrorText("", L"", login).empty());
  CHECK(internal::GalleryErrorText("", L"access denied", login).find(L"access denied") != std::wstring::npos);
  CHECK(internal::UsesGalleryDl(DetectPlatform(L"https://www.instagram.com/nasa/")));
  CHECK(!internal::UsesGalleryDl(DetectPlatform(L"https://www.tiktok.com/@nasa")));
}
