from __future__ import annotations

import csv
import json
from datetime import datetime
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence

import pandas as pd


class DataLogger:
    """Write experiment packets to CSV and JSONL for reproducible analysis."""

    def __init__(self, base_dir: Path):
        self.base_dir = Path(base_dir)
        self.base_dir.mkdir(parents=True, exist_ok=True)

    def build_stem(self, experiment_name: str) -> str:
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        return f"{experiment_name}_{stamp}"

    def write_records(
        self,
        experiment_name: str,
        records: Sequence[Dict],
        metadata: Optional[Dict] = None,
    ) -> Dict[str, Path]:
        stem = self.build_stem(experiment_name)
        csv_path = self.base_dir / f"{stem}.csv"
        jsonl_path = self.base_dir / f"{stem}.jsonl"
        xlsx_path = self.base_dir / f"{stem}.xlsx"
        meta_path = self.base_dir / f"{stem}_meta.json"

        fields = self._ordered_fields(records)
        self._write_csv(csv_path, fields, records)
        self._write_jsonl(jsonl_path, records)
        self._write_xlsx(xlsx_path, fields, records, metadata)

        metadata_payload = {
            "experiment_name": experiment_name,
            "record_count": len(records),
            "fields": fields,
            "created_at": datetime.now().isoformat(),
        }
        if metadata:
            metadata_payload.update(metadata)
        meta_path.write_text(json.dumps(metadata_payload, indent=2), encoding="utf-8")

        return {"csv": csv_path, "jsonl": jsonl_path, "xlsx": xlsx_path, "meta": meta_path}

    @staticmethod
    def _ordered_fields(records: Sequence[Dict]) -> List[str]:
        preferred = [
            "timestamp",
            "temp",
            "distance_mm",
            "feed_estimate_g",
            "biomass",
            "fuzzy_output_g",
            "real_output_g_values",
            "real_output_g_count",
            "real_output_g_mean",
            "real_output_g_std",
            "real_output_g_min",
            "real_output_g_max",
            "abs_error_g",
            "pct_error",
            "signed_error_g",
            "pwm",
            "duration_s",
            "mode",
            "state",
            "event",
            "sim_mode",
        ]
        discovered = []
        for record in records:
            for key in record.keys():
                if key not in discovered and key not in preferred:
                    discovered.append(key)
        return [field for field in preferred if any(field in record for record in records)] + discovered

    @staticmethod
    def _write_csv(path: Path, fields: Sequence[str], records: Sequence[Dict]) -> None:
        with path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(fields), extrasaction="ignore")
            writer.writeheader()
            for record in records:
                writer.writerow(record)

    @staticmethod
    def _write_jsonl(path: Path, records: Iterable[Dict]) -> None:
        with path.open("w", encoding="utf-8") as handle:
            for record in records:
                handle.write(json.dumps(record) + "\n")

    @staticmethod
    def _write_xlsx(path: Path, fields: Sequence[str], records: Sequence[Dict], metadata: Optional[Dict]) -> None:
        records_df = pd.DataFrame(records, columns=list(fields))
        meta_df = pd.DataFrame([metadata or {}])
        with pd.ExcelWriter(path, engine="openpyxl") as writer:
            records_df.to_excel(writer, sheet_name="records", index=False)
            meta_df.to_excel(writer, sheet_name="metadata", index=False)
