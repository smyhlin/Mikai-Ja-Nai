from __future__ import annotations


class MikaiApiError(Exception):
    def __init__(self, code: str, message: str, status: int) -> None:
        super().__init__(f"{code} ({status}): {message}")
        self.code = code
        self.message = message
        self.status = status
