#!/usr/bin/env python3
"""External production smoke check for the Watering System."""

from __future__ import annotations

import argparse
import json
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


DEFAULT_BASE_URL = "https://watering.sibex.zip"
EXPECTED_GOOGLE_HOST = "accounts.google.com"


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: ANN001
        return None


def fetch(url: str, *, follow_redirects: bool = True) -> tuple[int, bytes, str]:
    request = urllib.request.Request(
        url,
        headers={"User-Agent": "smart-watering-uptime-monitor/1.0"},
    )
    handlers: list[urllib.request.BaseHandler] = [
        urllib.request.HTTPSHandler(context=ssl.create_default_context())
    ]
    if not follow_redirects:
        handlers.append(NoRedirect())
    opener = urllib.request.build_opener(*handlers)

    try:
        with opener.open(request, timeout=15) as response:
            return response.status, response.read(), response.headers.get("Location", "")
    except urllib.error.HTTPError as error:
        if not follow_redirects:
            return error.code, error.read(), error.headers.get("Location", "")
        raise


def check_once(base_url: str) -> None:
    base_url = base_url.rstrip("/")

    status, body, _ = fetch(f"{base_url}/")
    if status != 200 or not body:
        raise RuntimeError(f"homepage check failed with HTTP {status}")

    status, body, _ = fetch(f"{base_url}/health")
    if status != 200:
        raise RuntimeError(f"health check failed with HTTP {status}")
    try:
        health = json.loads(body)
    except json.JSONDecodeError as error:
        raise RuntimeError("health endpoint did not return valid JSON") from error
    if health.get("status") != "ok":
        raise RuntimeError("application health status is not ok")
    if health.get("database") != "ok":
        raise RuntimeError("database health status is not ok")
    if health.get("authMode") != "google":
        raise RuntimeError("production authentication mode is not google")

    status, _, location = fetch(f"{base_url}/auth/google/login", follow_redirects=False)
    if status != 302:
        raise RuntimeError(f"Google login check failed with HTTP {status}")
    redirect = urllib.parse.urlparse(location)
    if redirect.hostname != EXPECTED_GOOGLE_HOST:
        raise RuntimeError("Google login did not redirect to the expected authorization host")
    query = urllib.parse.parse_qs(redirect.query)
    expected_callback = f"{base_url}/auth/google/callback"
    if query.get("redirect_uri", [""])[0] != expected_callback:
        raise RuntimeError("Google login generated an unexpected callback URL")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL)
    parser.add_argument("--attempts", type=int, default=3)
    parser.add_argument("--retry-delay", type=float, default=5.0)
    args = parser.parse_args()
    if args.attempts < 1:
        parser.error("--attempts must be at least 1")

    for attempt in range(1, args.attempts + 1):
        try:
            check_once(args.base_url)
        except Exception as error:  # Convert every probe failure to a concise monitor result.
            print(f"attempt {attempt}/{args.attempts} failed: {error}", file=sys.stderr)
            if attempt < args.attempts:
                time.sleep(args.retry_delay)
        else:
            print("production_monitor=healthy")
            return 0

    print("production_monitor=unhealthy", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
