"""Find a newer Prepare P4 package on GitHub, download it, verify it and unpack it next to this one.

A package carries its own exe and its whole source/ tree, so switching to the new package
also picks up any new preparation or conversion scripts that release added.
"""
import hashlib
import json
from pathlib import Path
import re
import urllib.request
import zipfile

RELEASES = 'https://api.github.com/repos/dreikk91/persona4-psp/releases?per_page=30'
DOWNLOADS = 'https://github.com/dreikk91/persona4-psp/releases/download/'


def _get(url, timeout=15):
    request = urllib.request.Request(url, headers={'User-Agent': 'Prepare-P4-updater',
                                                   'Accept': 'application/vnd.github+json'})
    return urllib.request.urlopen(request, timeout=timeout)


def pick(releases, current):
    """The newest bNNN release above `current` with a Windows package and SHA256SUMS.txt, or None."""
    best = None
    for release in releases:
        match = re.fullmatch(r'b(\d+)', release.get('tag_name', ''))
        if not match or release.get('draft') or int(match[1]) <= current:
            continue
        assets = {a['name']: a['browser_download_url'] for a in release.get('assets', [])}
        package = next((n for n in assets if n.endswith('-windows-x64.zip')), None)
        if package and 'SHA256SUMS.txt' in assets and (best is None or int(match[1]) > best['build']):
            best = dict(build=int(match[1]), name=package, zip=assets[package], sums=assets['SHA256SUMS.txt'],
                        page=release.get('html_url', ''), notes=release.get('body') or '')
    if best and not all(best[k].startswith(DOWNLOADS) for k in ('zip', 'sums')):
        return None
    return best


def check(current):
    with _get(RELEASES) as response:
        return pick(json.load(response), current)


def install(update, bundle, progress=print):
    """Download, verify and unpack `update` beside `bundle`; returns the new Prepare P4.exe."""
    bundle = Path(bundle).resolve()
    folder = bundle.parent / update['name'][:-len('.zip')]
    exe = folder / 'Prepare P4.exe'
    if exe.is_file():
        return exe
    with _get(update['sums']) as response:
        sums = dict(reversed(line.split(None, 1)) for line in response.read().decode().splitlines() if line.strip())
    wanted = sums.get(update['name']) or sums.get('*' + update['name'])
    if not wanted:
        raise ValueError(f"SHA256SUMS.txt has no line for {update['name']}.")
    archive = bundle.parent / (update['name'] + '.part')
    digest = hashlib.sha256()
    with _get(update['zip'], timeout=60) as response, archive.open('wb') as out:
        total, done = int(response.headers.get('Content-Length') or 0), 0
        while chunk := response.read(1 << 20):
            out.write(chunk); digest.update(chunk); done += len(chunk)
            progress(f'Downloading b{update["build"]}: {done >> 20} / {total >> 20} MiB')
    try:
        if digest.hexdigest() != wanted.strip().lower():
            raise ValueError('The downloaded package does not match SHA256SUMS.txt. Try again later.')
        progress('Unpacking...')
        with zipfile.ZipFile(archive) as package:
            names = package.namelist()
            if not all(n.startswith(folder.name + '/') and '..' not in Path(n).parts for n in names):
                raise ValueError('Unexpected file layout in the downloaded package.')
            package.extractall(bundle.parent)
    finally:
        archive.unlink(missing_ok=True)
    if not exe.is_file():
        raise ValueError('The downloaded package has no Prepare P4.exe.')
    return exe


if __name__ == '__main__':
    releases = [
        dict(tag_name='toolchain-gcc15.2', assets=[]),
        dict(tag_name='b495', assets=[dict(name='P4-PSP-b495-windows-x64.zip', browser_download_url=DOWNLOADS + 'b495/a.zip'),
                                      dict(name='SHA256SUMS.txt', browser_download_url=DOWNLOADS + 'b495/S')]),
        dict(tag_name='b530', assets=[dict(name='P4-PSP-b530-windows-x64.zip', browser_download_url=DOWNLOADS + 'b530/a.zip'),
                                      dict(name='SHA256SUMS.txt', browser_download_url=DOWNLOADS + 'b530/S')]),
        dict(tag_name='b540', assets=[dict(name='P4-PSP-b540-windows-x64.zip', browser_download_url=DOWNLOADS + 'b540/a.zip')]),
        dict(tag_name='b550', draft=True, assets=[]),
    ]
    assert pick(releases, 495)['build'] == 530
    assert pick(releases, 530) is None
    releases[2]['assets'][0]['browser_download_url'] = 'https://evil.example/a.zip'
    assert pick(releases, 495) is None
    print('ok')
