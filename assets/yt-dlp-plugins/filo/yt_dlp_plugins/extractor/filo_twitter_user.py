# Filo plugin for yt-dlp: every post of an X (Twitter) account.
#
# yt-dlp has no extractor for x.com/<name> (X only serves timelines to signed-in sessions). This one reads the account's timeline
# from the public FixTweet API (api.fxtwitter.com), which needs no account. The only thing sent to that service is the account's
# handle. Each post becomes a flat playlist entry that already carries its full text and the direct addresses of its pictures and
# videos ("_filo_media"), so Filo can save the post without asking X again.
#
#   --extractor-args "filotwitteruser:reposts=1"   also list the posts the account reposted (skipped by default)
import re
import urllib.parse

from yt_dlp.extractor.common import InfoExtractor
from yt_dlp.utils import ExtractorError, int_or_none, traverse_obj

_RESERVED = (
    'i|home|explore|search|settings|intent|share|hashtag|messages|notifications|compose|login|signup|tos|privacy|about|jobs|download|'
    'oauth|account|status|app|help|support|lists|topics|communities|premium|verified|following|followers'
)


def _ext(url, default):
    m = re.search(r'\.(jpe?g|png|webp|gif|mp4)(?:[?#~]|$)', url, re.I) or re.search(r'[?&]format=(jpe?g|png|webp)\b', url, re.I)
    ext = m.group(1).lower() if m else default
    return 'jpg' if ext == 'jpeg' else ext


class FiloTwitterUserIE(InfoExtractor):
    IE_NAME = 'filo:twitter:user'
    _VALID_URL = (
        r'https?://(?:(?:www|mobile)\.)?(?:twitter|x)\.com/(?P<id>(?!(?:%s)(?:/|$|\?|#))[A-Za-z0-9_]{1,15})'
        r'(?:/(?P<tab>media|with_replies|highlights|articles))?/?(?:[?#]|$)' % _RESERVED)
    _API = 'https://api.fxtwitter.com/2/profile/%s/statuses'

    def _page(self, user, cursor, first):
        url = self._API % urllib.parse.quote(user, safe='')
        if cursor:
            url += '?cursor=' + urllib.parse.quote(cursor, safe='')
        last = None
        # the service answers 404 now and then even for accounts that exist: ask again before believing it
        for attempt in range(8):
            try:
                data = self._download_json(url, user, note=False, fatal=True, expected_status=lambda _: True,
                                           headers={'Accept': 'application/json'})
            except ExtractorError as e:
                last = str(e)
                data = None
            if isinstance(data, dict) and data.get('code') == 200 and isinstance(data.get('results'), list):
                return data
            if isinstance(data, dict):
                last = data.get('code') or data.get('message') or last
            self._sleep(min(1.5 * (attempt + 1), 8), user)
        if first:
            raise ExtractorError(f'The posts of @{user} could not be read (the account may not exist, may be protected, '
                                 f'or the timeline service did not answer: {last})', expected=True)
        self.report_warning(f'The timeline stopped early ({last}); the posts found so far are kept')
        return None

    @staticmethod
    def _media(status):
        out = []
        for m in traverse_obj(status, ('media', 'all', lambda _, v: isinstance(v, dict))) or []:
            kind = m.get('type')
            url = m.get('url')
            if not url:
                continue
            if kind == 'photo':
                out.append({'kind': 'image', 'url': url, 'ext': _ext(url.split('?')[0], 'jpg'), 'width': int_or_none(m.get('width')),
                            'height': int_or_none(m.get('height')), 'alt': m.get('altText')})
            elif kind in ('video', 'gif'):
                out.append({'kind': 'video', 'url': url, 'ext': 'mp4', 'width': int_or_none(m.get('width')),
                            'height': int_or_none(m.get('height')), 'duration': m.get('duration'), 'thumbnail': m.get('thumbnail_url')})
        return out

    def _entry(self, status):
        sid = status['id']
        author = status.get('author') or {}
        handle = author.get('screen_name') or ''
        text = (status.get('text') or '').strip()
        one_line = re.sub(r'\s+', ' ', re.sub(r'https?://\S+', '', text)).strip()
        return {
            '_type': 'url',
            'ie_key': 'Twitter',
            'id': sid,
            'url': status.get('url') or f'https://x.com/{handle}/status/{sid}',
            'title': one_line[:100] or sid,
            'description': text,
            'timestamp': int_or_none(status.get('created_timestamp')),
            'uploader': author.get('name'),
            'uploader_id': handle,
            'like_count': int_or_none(status.get('likes')),
            'repost_count': int_or_none(status.get('reposts')),
            'comment_count': int_or_none(status.get('replies')),
            'view_count': int_or_none(status.get('views')),
            '_filo_media': self._media(status),
            '_filo_repost': bool(status.get('reposted_by')),
        }

    def _entries(self, user, with_reposts, media_only):
        cursor = None
        seen = set()
        first = True
        while True:
            data = self._page(user, cursor, first)
            first = False
            if not data:
                return
            fresh = 0
            for status in data['results']:
                sid = status.get('id')
                if not sid or sid in seen:
                    continue
                seen.add(sid)
                fresh += 1
                reposted = bool(status.get('reposted_by'))
                if reposted and not with_reposts:
                    continue
                if not reposted and (traverse_obj(status, ('author', 'screen_name')) or '').lower() != user.lower():
                    continue                                   # the service sometimes pads a short timeline with unrelated posts
                entry = self._entry(status)
                if media_only and not entry['_filo_media']:
                    continue
                yield entry
            cursor = traverse_obj(data, ('cursor', 'bottom'))
            if not cursor or not fresh:
                return

    def _real_extract(self, url):
        user, tab = self._match_valid_url(url).group('id', 'tab')
        with_reposts = bool(self._configuration_arg('reposts', ie_key='FiloTwitterUser'))
        return self.playlist_result(self._entries(user, with_reposts, tab == 'media'), user, f'@{user}',
                                    uploader_id=user, webpage_url=f'https://x.com/{user}')
