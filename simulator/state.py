import json
import os
import tempfile
from pathlib import Path
from typing import Dict, Optional

from .protocol import DEVICE_ID_PATTERN


class StateStore:
    def __init__(self, state_dir: Path):
        self.state_dir = Path(state_dir)
        self.state_dir.mkdir(parents=True, exist_ok=True)

    def _path_for(self, device_id: str) -> Path:
        if not DEVICE_ID_PATTERN.fullmatch(device_id):
            raise ValueError("invalid device_id")
        return self.state_dir / f"{device_id}.json"

    def load(self, device_id: str) -> Optional[Dict[str, object]]:
        path = self._path_for(device_id)
        if not path.exists():
            return None
        with path.open(encoding="utf-8") as state_file:
            state = json.load(state_file)
        return state if isinstance(state, dict) else None

    def save(self, device_id: str, state: Dict[str, object]) -> None:
        path = self._path_for(device_id)
        temporary_path = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8",
                dir=self.state_dir,
                prefix=f".{device_id}.",
                suffix=".tmp",
                delete=False,
            ) as temporary_file:
                temporary_path = Path(temporary_file.name)
                json.dump(state, temporary_file, ensure_ascii=False, sort_keys=True)
                temporary_file.write("\n")
            os.replace(temporary_path, path)
        finally:
            if temporary_path is not None and temporary_path.exists():
                temporary_path.unlink()
