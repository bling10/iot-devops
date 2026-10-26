import hashlib
import socket
import time
from dataclasses import dataclass
from typing import Optional
from urllib.error import HTTPError, URLError
from urllib.request import ProxyHandler, Request, build_opener

from .protocol import OtaCommand


@dataclass(frozen=True)
class DownloadResult:
    artifact_size: int
    sha256: str


class OtaDownloadError(RuntimeError):
    def __init__(self, reason: str):
        super().__init__(reason)
        self.reason = reason


def download_and_verify(
    command: OtaCommand,
    timeout_sec: float,
    fault_mode: str = "none",
    chunk_size: int = 64 * 1024,
    opener=None,
) -> DownloadResult:
    if fault_mode == "download_failed":
        raise OtaDownloadError("http_open_failed")

    http_opener = opener or build_opener(ProxyHandler({}))
    request = Request(command.url, headers={"User-Agent": "iot-ota-simulator/1.0"})

    try:
        response = http_opener.open(request, timeout=timeout_sec)
    except HTTPError:
        raise OtaDownloadError("http_status_invalid")
    except (URLError, TimeoutError, socket.timeout, OSError):
        raise OtaDownloadError("http_open_failed")

    digest = hashlib.sha256()
    downloaded_size = 0
    try:
        status = getattr(response, "status", 200)
        if status != 200:
            raise OtaDownloadError("http_status_invalid")

        content_length = response.headers.get("Content-Length")
        if content_length is not None:
            try:
                if int(content_length) != command.artifact_size:
                    raise OtaDownloadError("content_length_mismatch")
            except ValueError:
                raise OtaDownloadError("content_length_mismatch")

        while True:
            try:
                chunk = response.read(chunk_size)
            except (TimeoutError, socket.timeout, OSError):
                raise OtaDownloadError("http_read_failed")
            if not chunk:
                break
            downloaded_size += len(chunk)
            if downloaded_size > command.artifact_size:
                raise OtaDownloadError("artifact_size_exceeded")
            digest.update(chunk)
            if fault_mode == "slow_download":
                time.sleep(0.05)
    finally:
        response.close()

    if downloaded_size != command.artifact_size:
        raise OtaDownloadError("artifact_size_mismatch")

    actual_sha256 = digest.hexdigest()
    if fault_mode == "sha256_mismatch" or actual_sha256 != command.sha256:
        raise OtaDownloadError("sha256_mismatch")

    return DownloadResult(downloaded_size, actual_sha256)
