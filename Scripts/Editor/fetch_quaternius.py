"""Fetch a Quaternius free asset pack from the author's official itch.io page.

itch.io resolves to poisoned DNS records on this network (the connection
times out without a proxy), so the script optionally routes every request
through a local HTTP proxy given by --proxy (default: none). The download
still comes from the author's official page; the proxy only carries the
bytes.

Official free-download flow for a "name your own price" pack (all requests
share one cookie jar, tokens expire within ~80 seconds so the flow must run
in one pass):
  1. GET  https://quaternius.itch.io/<game>            -> csrf_token #1
  2. POST https://quaternius.itch.io/<game>/download_url (csrf_token #1)
                                                          -> {url}
  3. GET  {url}                                        -> download page,
                                                          csrf_token #2,
                                                          data-upload_id
  4. POST {url} (csrf_token #2, upload_id, action=accept_nda)
                                                          -> {url} direct file
  5. GET  {url}                                        -> the archive bytes

Outputs (ASCII names only):
  SourceAssets/Downloads/<slug>.zip
  SourceAssets/Downloads/<slug>.sha256
  SourceAssets/Licenses/Quaternius-itch-page-<slug>.html  (page snapshot
  proving the CC0 statement at fetch time)

This script never writes UE assets; importing into the engine is a separate
editor-Python step. ASCII only; safe to re-run.
"""
import argparse
import hashlib
import http.cookiejar
import json
import re
import sys
import urllib.parse
import urllib.request
from pathlib import Path

USER_AGENT = (
    'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 '
    '(KHTML, like Gecko) Chrome/126.0 Safari/537.36'
)


class ItchSession:
    def __init__(self, proxy):
        self.jar = http.cookiejar.CookieJar()
        handlers = [urllib.request.HTTPCookieProcessor(self.jar)]
        if proxy:
            handlers.append(urllib.request.ProxyHandler({
                'http': proxy,
                'https': proxy,
            }))
        else:
            handlers.append(urllib.request.ProxyHandler({}))
        self.opener = urllib.request.build_opener(*handlers)

    def get(self, url, referer=None):
        request = urllib.request.Request(url, headers={
            'User-Agent': USER_AGENT,
            'Accept': 'text/html,application/json;q=0.9,*/*;q=0.8',
            **({'Referer': referer} if referer else {}),
        })
        with self.opener.open(request, timeout=60) as response:
            return response.geturl(), response.read()

    def post(self, url, fields, referer):
        data = urllib.parse.urlencode(fields).encode('ascii')
        request = urllib.request.Request(url, data=data, headers={
            'User-Agent': USER_AGENT,
            'Content-Type': 'application/x-www-form-urlencoded',
            'X-Requested-With': 'XMLHttpRequest',
            'Referer': referer,
            'Origin': 'https://quaternius.itch.io',
        })
        with self.opener.open(request, timeout=60) as response:
            return response.geturl(), response.read()


def find_csrf(text):
    matches = re.findall(r'csrf_token" value="([^"]+)"', text)
    if not matches:
        raise RuntimeError('csrf_token not found on the page')
    return matches[0]


def find_upload(text):
    matches = re.findall(r'data-upload_id="(\d+)"', text)
    names = re.findall(r'<strong class="name" title="([^"]+\.zip)"', text)
    if not matches:
        raise RuntimeError('no upload button (data-upload_id) on the download page')
    return matches[0], (names[0] if names else 'unknown.zip')


def fetch_pack(game, out_dir, proxy):
    session = ItchSession(proxy)
    base = 'https://quaternius.itch.io/' + game

    page_url, page = session.get(base)
    page_text = page.decode('utf-8', 'replace')
    csrf1 = find_csrf(page_text)
    print('step1 game page ok, csrf1 len=%d' % len(csrf1))

    _, payload = session.post(
        base + '/download_url', {'csrf_token': csrf1}, referer=base)
    ticket_url = json.loads(payload.decode('utf-8'))['url']
    print('step2 download_url ok')

    dl_page_url, dl_page = session.get(ticket_url, referer=base)
    dl_text = dl_page.decode('utf-8', 'replace')
    if 'Download %s' not in dl_text and 'download_btn' not in dl_text:
        raise RuntimeError('download page did not load: ' + dl_page_url)
    csrf2 = find_csrf(dl_text)
    upload_id, file_name = find_upload(dl_text)
    print('step3 download page ok, upload_id=%s file=%s' % (upload_id, file_name))

    # The download page's button POSTs to /<game>/file/<upload_id> with the
    # page's own csrf token (see itch.io bundle: sr = "/" + slug + "/file/" +
    # upload_id + params). That endpoint returns the direct file URL.
    _, payload = session.post(
        base + '/file/' + upload_id,
        {'csrf_token': csrf2, 'source': 'game_download'},
        referer=ticket_url)
    step4 = json.loads(payload.decode('utf-8'))
    if 'url' not in step4:
        raise RuntimeError('step4 response missing url: %r' % payload[:400])
    file_url = step4['url']
    print('step4 direct file url ok')

    final_url, blob = session.get(file_url, referer=ticket_url)
    if len(blob) < 1024 or blob[:2] != b'PK':
        raise RuntimeError(
            'downloaded file is not a zip (first bytes: %r, from %s)'
            % (blob[:16], final_url))

    out_dir.mkdir(parents=True, exist_ok=True)
    zip_path = out_dir / (game + '.zip')
    zip_path.write_bytes(blob)
    digest = hashlib.sha256(blob).hexdigest()
    (out_dir / (game + '.sha256')).write_text(
        digest + '  ' + zip_path.name + '\n', encoding='ascii')

    lic_dir = out_dir.parent / 'Licenses'
    lic_dir.mkdir(parents=True, exist_ok=True)
    (lic_dir / ('Quaternius-itch-page-' + game + '.html')).write_bytes(page)

    print('saved %s (%d bytes) sha256=%s' % (zip_path, len(blob), digest))
    print('page snapshot: %s' % (lic_dir / ('Quaternius-itch-page-' + game + '.html')))
    return zip_path


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--game', required=True,
                        help='itch.io game slug, e.g. universal-animation-library-2')
    parser.add_argument('--out', default='SourceAssets/Downloads',
                        help='output directory for the archive')
    parser.add_argument('--proxy', default='',
                        help='local proxy URL, e.g. http://127.0.0.1:7890 '
                             '(required on networks where itch.io is blocked)')
    args = parser.parse_args()
    try:
        fetch_pack(args.game, Path(args.out), args.proxy)
    except Exception as error:  # noqa: BLE001 - report and fail loudly
        print('FETCH FAILED: %s' % error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
