from __future__ import annotations

import base64
import re
import time
from urllib.parse import urljoin

import requests

UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/142.0.0.0 Safari/537.36"
PAGE_H = {
    "User-Agent": UA,
    "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
    "Accept-Language": "uk-UA,uk;q=0.9,en;q=0.8",
}
MEDIA_H = {
    "User-Agent": UA,
    "Accept": "*/*",
    "Accept-Language": "uk-UA,uk;q=0.9,en;q=0.8",
}
ASHDI_H = {"User-Agent": UA}
TORTUGA_H = {"User-Agent": UA}
TIMEOUT = 20
FETCH_RETRIES = 2
RETRYABLE_STATUS_CODES = {429, 500, 502, 503, 504}


class StreamError(Exception):
    pass


class NoSourceError(StreamError):
    pass


def detect_site(page_url: str) -> str:
    if "ashdi.vip/vod/" in page_url:
        return "ashdi"
    if "moonanime.art/iframe/" in page_url:
        return "moon"
    if "tortuga.tw/vod/" in page_url:
        return "tortuga"
    raise ValueError(f"unsupported episode url: {page_url}")


def fetch_text(session: requests.Session, url: str, headers: dict[str, str] | None = None) -> str:
    for attempt in range(FETCH_RETRIES + 1):
        try:
            response = session.get(url, headers=headers, timeout=TIMEOUT)
            status_code = getattr(response, "status_code", 200)
            if status_code in RETRYABLE_STATUS_CODES and attempt < FETCH_RETRIES:
                retry_after = getattr(response, "headers", {}).get("Retry-After")
                try:
                    delay = (
                        min(2.0, max(0.25, float(retry_after)))
                        if retry_after
                        else 0.4 * (attempt + 1)
                    )
                except ValueError:
                    delay = 0.4 * (attempt + 1)
                time.sleep(delay)
                continue
            response.raise_for_status()
            return response.text
        except requests.RequestException as exc:
            raise StreamError(f"GET failed for {url}: {exc}") from exc
    raise StreamError(f"GET failed for {url}: exhausted retries")


def ashdi_master(session: requests.Session, page_url: str) -> str:
    html = fetch_text(session, page_url, ASHDI_H)
    match = re.search(r"""file\s*:\s*['"](https?://[^'"]+?\.m3u8[^'"]*)['"]""", html)
    if not match:
        raise StreamError(f"ashdi master playlist not found in {page_url}")
    return match.group(1)


def moon_master(session: requests.Session, page_url: str) -> str:
    html = fetch_text(session, page_url, PAGE_H)
    blob = re.search(r'atob\("([A-Za-z0-9+/=]{100,})"\)', html)
    if not blob:
        raise StreamError(f"moonanime payload not found in {page_url}")
    buf = base64.b64decode(blob.group(1))
    if len(buf) < 34:
        raise StreamError(f"moonanime payload too short in {page_url}")
    key, enc = buf[1:33], buf[33:]
    out, k = bytearray(len(enc)), buf[0]
    for i in range(len(enc)):
        out[i] = enc[i] ^ key[i % 32] ^ k
        k = (enc[i] + key[i % 32]) & 255
    try:
        js = bytes(out).decode("utf-8")
    except UnicodeDecodeError as exc:
        raise StreamError(f"moonanime outer decode failed for {page_url}") from exc

    if re.search(r'var rawVideo\s*=\s*["\']\s*["\']|file:\s*["\']\s*["\']', js):
        raise NoSourceError(f"moonanime has no video source for {page_url} (no_source)")

    xk_match = re.search(r'function _0xd\(e\)\{var k="([^"]+)"', js)
    raw_match = re.search(r'var rawVideo = _0xd\("([^"]+)"\)', js)
    if not xk_match or not raw_match:
        raise StreamError(f"moonanime keys not found in {page_url}")
    xk = xk_match.group(1).encode()
    raw = base64.b64decode(raw_match.group(1))
    try:
        decoded = bytes(b ^ xk[i % len(xk)] for i, b in enumerate(raw)).decode("utf-8")
    except UnicodeDecodeError as exc:
        raise StreamError(f"moonanime inner decode failed for {page_url}") from exc
    pairs = re.findall(r"\[([^\]]+)\]((?:https?://)[^,\[\s]+)", decoded)
    if pairs:
        return pairs[0][1]
    master = decoded.strip('" ')
    if not master.startswith("http"):
        raise StreamError(f"moonanime master not found in {page_url}")
    return master


def tortuga_decode(token: str) -> str:
    cipher = base64.b64decode(token)
    seed = cipher[0]
    return bytes(byte ^ ((seed + 13 + 7 * i) % 256) for i, byte in enumerate(cipher[1:])).decode(
        "utf-8"
    )


def tortuga_master(session: requests.Session, page_url: str) -> str:
    html = fetch_text(session, page_url, TORTUGA_H)
    match = re.search(r'file:\s*"([^"]+)"', html)
    if not match:
        raise StreamError(f"tortuga file token not found in {page_url}")
    try:
        return tortuga_decode(match.group(1))
    except Exception as exc:
        raise StreamError(f"tortuga token decode failed for {page_url}: {exc}") from exc


def extract_master(session: requests.Session, page_url: str) -> str:
    site = detect_site(page_url)
    if site == "ashdi":
        return ashdi_master(session, page_url)
    if site == "tortuga":
        return tortuga_master(session, page_url)
    return moon_master(session, page_url)


def parse_variants(master_text: str, base_url: str = "") -> list[dict]:
    variants: list[dict] = []
    pending: dict | None = None
    for raw_line in master_text.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("#EXT-X-STREAM-INF"):
            resolution = re.search(r"RESOLUTION=(\d+)x(\d+)", line)
            bandwidth = re.search(r"BANDWIDTH=(\d+)", line)
            pending = {
                "width": int(resolution.group(1)) if resolution else None,
                "height": int(resolution.group(2)) if resolution else None,
                "bandwidth": int(bandwidth.group(1)) if bandwidth else None,
                "url": "",
            }
        elif pending is not None and not line.startswith("#"):
            pending["url"] = line if re.match(r"https?://", line) else urljoin(base_url, line)
            variants.append(pending)
            pending = None
    return variants


def get_quality_url(variants: list[dict], height: int) -> str:
    if not variants:
        raise StreamError("master playlist has no variants")
    exact = [variant for variant in variants if variant["height"] == height]
    if exact:
        return exact[0]["url"]
    lower = [
        variant
        for variant in variants
        if variant["height"] is not None and variant["height"] < height
    ]
    if lower:
        return max(lower, key=lambda variant: variant["height"])["url"]
    known = [variant for variant in variants if variant["height"] is not None]
    if known:
        return max(known, key=lambda variant: variant["height"])["url"]
    return variants[0]["url"]


def list_segments(
    session: requests.Session,
    quality_url: str,
    headers: dict[str, str] | None = None,
) -> list[str]:
    urls = []
    for raw_line in fetch_text(session, quality_url, headers).splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        urls.append(line if re.match(r"https?://", line) else urljoin(quality_url, line))
    return urls


def get_quality_streams(page_url: str) -> dict[int, str]:
    site = detect_site(page_url)
    session = requests.Session()
    master = extract_master(session, page_url)
    headers = ASHDI_H if site in ("ashdi", "tortuga") else MEDIA_H
    variants = parse_variants(fetch_text(session, master, headers), master)
    return {
        variant["height"]: variant["url"]
        for variant in variants
        if variant.get("height") is not None and variant.get("url")
    }
