# Filo plugin for yt-dlp: photo posts.
#
# yt-dlp itself only downloads video and audio. Filo wants every picture of a TikTok slideshow and every photo of an X (Twitter)
# post, so these two extractors extend the built-in ones. Anything that is not a photo post behaves exactly like upstream.
# Each photo is returned as a playlist entry carrying the marker "_filo_kind": "image" (the audio of a slideshow is marked "audio").
import html
import re

from yt_dlp.extractor.tiktok import TikTokIE as _TikTokIE
from yt_dlp.extractor.twitter import TwitterIE as _TwitterIE
from yt_dlp.utils import ExtractorError, int_or_none, traverse_obj, unified_timestamp, url_or_none


def _image_ext(url, default='jpg'):
    m = re.search(r'\.(jpe?g|png|webp|heic|avif|gif)(?:[?#~]|$)', url, re.I) or re.search(r'[?&]format=(jpe?g|png|webp)\b', url, re.I)
    ext = (m.group(1).lower() if m else default)
    return 'jpg' if ext == 'jpeg' else ext


def _full_text(status):
    """The post as written: line breaks kept, t.co links expanded, the trailing media links removed."""
    text = status.get('full_text') or status.get('text') or ''
    for u in traverse_obj(status, ('entities', 'urls', lambda _, v: isinstance(v, dict) and v.get('url'))) or []:
        text = text.replace(u['url'], u.get('expanded_url') or u['url'])
    for m in traverse_obj(status, ('extended_entities', 'media', lambda _, v: isinstance(v, dict) and v.get('url'))) or []:
        text = text.replace(m['url'], '')
    return html.unescape(text).strip()


def _post_fields(status):
    user = status.get('user') or {}
    return {
        'uploader': user.get('name'),
        'uploader_id': user.get('screen_name'),
        'timestamp': unified_timestamp(status.get('created_at')),
        'like_count': int_or_none(status.get('favorite_count')),
        'repost_count': int_or_none(status.get('retweet_count')),
        'comment_count': int_or_none(status.get('reply_count')),
        'view_count': int_or_none(traverse_obj(status, ('ext_views', 'count'))),
    }


class TikTokIE(_TikTokIE, plugin_name='filo'):
    def _real_extract(self, url):
        video_id, user_id = self._match_valid_url(url).group('id', 'user_id')
        page_url = self._create_url(user_id, video_id)
        video_data, status = self._extract_web_data_and_status(page_url, video_id)
        if not (video_data and status == 0):
            return _TikTokIE._real_extract(self, page_url)  # upstream produces the proper error messages

        images = traverse_obj(video_data, ('imagePost', 'images', lambda _, v: v.get('imageURL'), {dict})) or []
        info = self._parse_aweme_video_web(video_data, page_url, video_id)
        if not images:
            return info

        entries = []
        for index, image in enumerate(images, 1):
            urls = [u for u in traverse_obj(image, ('imageURL', 'urlList', ..., {url_or_none})) or []]
            if not urls:
                continue
            # the list holds the same picture in several encodings; prefer JPEG (widest support), then whatever is first
            url_best = next((u for u in urls if _image_ext(u, '') == 'jpg'), urls[0])
            entries.append({
                '_type': 'url',
                'id': f'{video_id}_{index:02d}',
                'title': f'{info.get("title") or video_id} #{index}',
                'url': url_best,
                'ext': _image_ext(url_best),
                'width': int_or_none(image.get('imageWidth')),
                'height': int_or_none(image.get('imageHeight')),
                'thumbnail': url_best,
                'http_headers': {'Referer': page_url},
                'vcodec': 'none', 'acodec': 'none',
                '_filo_kind': 'image',
            })

        audio_url = traverse_obj(video_data, ('music', 'playUrl', {url_or_none}))
        if audio_url:
            entries.append({
                'id': f'{video_id}_audio',
                'title': traverse_obj(video_data, ('music', 'title', {str})) or 'Sound',
                'url': audio_url,
                'ext': 'mp3' if '.mp3' in audio_url else 'm4a',
                'vcodec': 'none',
                'http_headers': {'Referer': page_url},
                '_filo_kind': 'audio',
            })

        info.pop('formats', None)
        info.pop('subtitles', None)
        return self.playlist_result(entries, video_id, info.get('title'), info.get('description'),
                                    **{k: v for k, v in info.items() if k in (
                                        'uploader', 'uploader_id', 'channel', 'channel_id', 'timestamp', 'thumbnails', 'like_count',
                                        'view_count', 'comment_count', 'repost_count')},
                                    webpage_url=page_url, _filo_slideshow=True)


class FiloTikTokPhotoIE(_TikTokIE):
    _VALID_URL = _TikTokIE._VALID_URL.replace('/video)', '/photo)')
    _real_extract = TikTokIE._real_extract


class TwitterIE(_TwitterIE, plugin_name='filo'):
    def _extract_status(self, twid):
        cached = getattr(self, '_filo_status_cache', None)
        if cached and cached[0] == twid:
            return cached[1]
        status = super()._extract_status(twid)
        self._filo_status_cache = (twid, status)
        return status

    def _real_extract(self, url):
        twid, selected_index = self._match_valid_url(url).group('id', 'index')
        try:
            status = self._extract_status(twid)
        except ExtractorError:
            return super()._real_extract(url)

        media = traverse_obj(status, ('extended_entities', 'media', lambda _, m: isinstance(m, dict))) or []
        photos = [(i, m) for i, m in enumerate(media, 1) if m.get('type') == 'photo' and (m.get('media_url_https') or m.get('media_url'))]
        if selected_index:
            photos = [(i, m) for i, m in photos if i == int(selected_index)]
        if not photos:
            try:
                return super()._real_extract(url)
            except ExtractorError as e:
                if selected_index or 'No video could be found' not in str(e):
                    raise
                # a post with neither pictures nor video: the text is the post
                user = status.get('user') or {}
                text = _full_text(status)
                one_line = re.sub(r'\s+', ' ', re.sub(r'https?://\S+', '', text)).strip()
                title = f'{user["name"]} - {one_line}' if user.get('name') else one_line
                uploader_id = user.get('screen_name')
                return self.playlist_result([], twid, title[:72], text,
                                            uploader_url=f'https://twitter.com/{uploader_id}' if uploader_id else None,
                                            webpage_url=url, _filo_kind='text', **_post_fields(status))

        user = status.get('user') or {}
        full_text = _full_text(status)
        text = full_text.replace('\n', ' ')
        text = re.sub(r'\s+(https?://\S+)', '', text).strip()
        title = f'{user["name"]} - {text}' if user.get('name') else text
        uploader_id = user.get('screen_name')

        entries = []
        for position, (index, m) in enumerate(photos, 1):
            base = (m.get('media_url_https') or m.get('media_url')).split('?')[0]
            ext = _image_ext(base)
            original = m.get('original_info') or {}
            entries.append({
                '_type': 'url',
                'id': f'{twid}_{index:02d}',
                'title': f'{title[:72]} #{index}',
                'url': f'{base.rsplit(".", 1)[0]}?format={ext}&name=orig',
                'ext': ext,
                'width': int_or_none(original.get('width')),
                'height': int_or_none(original.get('height')),
                'thumbnail': f'{base}?name=small',
                '_filo_kind': 'image',
                'vcodec': 'none', 'acodec': 'none',
            })

        if not selected_index:
            try:
                upstream = super()._real_extract(url)
            except ExtractorError:
                upstream = None                       # a post with pictures only: upstream says "no video in this tweet"
            if upstream:
                videos = upstream.get('entries') if upstream.get('_type') == 'playlist' else [upstream]
                entries.extend(v for v in (videos or []) if v)

        return self.playlist_result(entries, twid, title[:72], full_text, uploader_url=f'https://twitter.com/{uploader_id}' if uploader_id else None,
                                    webpage_url=url, **_post_fields(status))
