import json
from typing import Any, Dict, Optional


def parse_json_line(line: str) -> Optional[Dict[str, Any]]:
    """Parse one newline-delimited JSON record and ignore corrupted lines."""
    if not line:
        return None

    candidate = line.strip()
    if not candidate or not candidate.startswith("{"):
        return None

    try:
        payload = json.loads(candidate)
    except json.JSONDecodeError:
        return None

    if not isinstance(payload, dict):
        return None

    return payload
