from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

ROOT = Path(__file__).resolve().parents[2]
PYTHON_ROOT = ROOT / "python"

if str(PYTHON_ROOT) not in sys.path:
    sys.path.insert(0, str(PYTHON_ROOT))

from controller.serial_controller import SerialConfig, SerialController
from logger.data_logger import DataLogger


class ExperimentRunner:
    def __init__(
        self,
        port: str,
        output_dir: Path,
        settle_s: float,
        capture_s: float,
        pause_s: float,
        prompt_manual_weight: bool,
    ):
        self.controller = SerialController(SerialConfig(port=port))
        self.logger = DataLogger(output_dir)
        self.settle_s = settle_s
        self.capture_s = capture_s
        self.pause_s = pause_s
        self.prompt_manual_weight = prompt_manual_weight

    def __enter__(self) -> "ExperimentRunner":
        self.controller.connect()
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.controller.close()

    def run(self, test_name: str) -> Dict[str, Path]:
        dispatch = {
            "A": self.run_test_a,
            "B": self.run_test_b,
            "C": self.run_test_c,
            "D": self.run_test_d,
        }
        return dispatch[test_name]()

    def run_test_a(self) -> Dict[str, Path]:
        records = self._run_cases(
            test_label="test_a",
            cases=[
                {"case_id": f"T{temp:02d}", "temp": float(temp), "biomass": 2000.0}
                for temp in range(22, 37)
            ],
        )
        return self.logger.write_records("test_a", records, metadata={"description": "Temperature sweep"})

    def run_test_b(self) -> Dict[str, Path]:
        biomass_values = [500.0, 1000.0, 1500.0, 2000.0, 2500.0, 3500.0, 4500.0, 5000.0]
        records = self._run_cases(
            test_label="test_b",
            cases=[
                {"case_id": f"B{int(biomass)}", "temp": 30.0, "biomass": biomass}
                for biomass in biomass_values
            ],
        )
        return self.logger.write_records("test_b", records, metadata={"description": "Biomass sweep"})

    def run_test_c(self) -> Dict[str, Path]:
        temps = [22.0, 26.0, 30.0, 34.0, 36.0]
        biomass_values = [500.0, 2000.0, 3500.0, 5000.0]
        cases = []
        for temp in temps:
            for biomass in biomass_values:
                cases.append(
                    {
                        "case_id": f"T{int(temp)}_B{int(biomass)}",
                        "temp": temp,
                        "biomass": biomass,
                    }
                )
        records = self._run_cases("test_c", cases)
        return self.logger.write_records("test_c", records, metadata={"description": "Full grid sweep"})

    def run_test_d(self) -> Dict[str, Path]:
        cases = [{"case_id": f"R{index+1:02d}", "temp": 30.0, "biomass": 2000.0} for index in range(10)]
        records = self._run_cases("test_d", cases)
        return self.logger.write_records("test_d", records, metadata={"description": "Repeatability run"})

    def _run_cases(self, test_label: str, cases: Iterable[Dict]) -> List[Dict]:
        cases = list(cases)
        records: List[Dict] = []
        self.controller.send_command("SIM_MODE:ON")

        for index, case in enumerate(cases, start=1):
            print(f"[{test_label}] case {index}/{len(cases)} -> temp={case['temp']:.2f}C biomass={case['biomass']:.2f}g")
            self.controller.send_command(f"SET_TEMP:{case['temp']:.2f}")
            self.controller.send_command(f"SET_BIOMASS:{case['biomass']:.2f}")
            time.sleep(self.settle_s)
            self.controller.send_command("REQUEST_STATUS")
            self.controller.send_command("RUN_FEED")

            case_packets = self.controller.collect_packets(duration_s=self.capture_s)
            manual_measurement = self._capture_manual_measurement(index, len(cases), case)
            if manual_measurement["real_output_g_mean"] is not None:
                self.controller.send_command(f"SET_REAL_WEIGHT:{manual_measurement['real_output_g_mean']:.2f}")
            host_time = time.time()
            for packet in case_packets:
                enriched = dict(packet)
                enriched["host_timestamp"] = host_time
                enriched["test_name"] = test_label
                enriched["case_index"] = index
                enriched["case_id"] = case["case_id"]
                enriched["set_temp"] = case["temp"]
                enriched["set_biomass"] = case["biomass"]
                enriched.update(manual_measurement)
                fuzzy_output = enriched.get("fuzzy_output_g")
                real_mean = manual_measurement["real_output_g_mean"]
                if fuzzy_output is not None and real_mean is not None:
                    abs_error = abs(real_mean - fuzzy_output)
                    enriched["abs_error_g"] = abs_error
                    enriched["pct_error"] = (abs_error / fuzzy_output * 100.0) if fuzzy_output else None
                    enriched["signed_error_g"] = real_mean - fuzzy_output
                else:
                    enriched["abs_error_g"] = None
                    enriched["pct_error"] = None
                    enriched["signed_error_g"] = None
                records.append(enriched)

        self.controller.send_command("SIM_MODE:OFF")
        return records

    def _capture_manual_measurement(self, case_index: int, total_cases: int, case: Dict) -> Dict[str, Optional[float] | str | int]:
        if self.pause_s > 0:
            print(
                f"Pause {self.pause_s:.0f}s before next case for manual weighing "
                f"(case {case_index}/{total_cases}, temp={case['temp']:.2f}, biomass={case['biomass']:.2f})."
            )
            time.sleep(self.pause_s)

        if not self.prompt_manual_weight:
            return self._empty_measurement()

        print("Masukkan gramasi real keluaran. Bisa satu nilai atau beberapa nilai dipisah koma, contoh: 28.4,28.9,28.7")
        weight_text = input("Gramasi real (kosong untuk skip): ").strip()
        note_text = input("Catatan pengukuran (opsional): ").strip()
        if not weight_text:
            measurement = self._empty_measurement()
            measurement["measurement_note"] = note_text
            return measurement

        try:
            values = [float(item.strip()) for item in weight_text.split(",") if item.strip()]
            if not values:
                measurement = self._empty_measurement()
                measurement["measurement_note"] = note_text
                return measurement
            std_value = statistics.stdev(values) if len(values) > 1 else 0.0
            return {
                "real_output_g_values": ",".join(f"{value:.3f}" for value in values),
                "real_output_g_count": len(values),
                "real_output_g_mean": statistics.mean(values),
                "real_output_g_std": std_value,
                "real_output_g_min": min(values),
                "real_output_g_max": max(values),
                "measurement_note": note_text,
            }
        except ValueError:
            print("Input gramasi tidak valid, nilai real_output_g disimpan kosong.")
            measurement = self._empty_measurement()
            measurement["measurement_note"] = note_text
            return measurement

    @staticmethod
    def _empty_measurement() -> Dict[str, Optional[float] | str | int]:
        return {
            "real_output_g_values": "",
            "real_output_g_count": 0,
            "real_output_g_mean": None,
            "real_output_g_std": None,
            "real_output_g_min": None,
            "real_output_g_max": None,
            "measurement_note": "",
        }


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Automated fish feeder experiment runner")
    parser.add_argument("--port", required=True, help="Serial port of UNO or ESP32 USB bridge")
    parser.add_argument("--test", choices=["A", "B", "C", "D"], required=True, help="Test scenario")
    parser.add_argument("--output-dir", default=str(ROOT / "data" / "raw"))
    parser.add_argument("--settle-s", type=float, default=1.0, help="Wait time after parameter update")
    parser.add_argument("--capture-s", type=float, default=6.0, help="Capture window after RUN_FEED")
    parser.add_argument("--pause-s", type=float, default=15.0, help="Pause between cases for manual weighing")
    parser.add_argument(
        "--no-manual-weight",
        action="store_true",
        help="Disable manual gram input prompts between test cases",
    )
    return parser


def main() -> None:
    args = build_argument_parser().parse_args()
    with ExperimentRunner(
        port=args.port,
        output_dir=Path(args.output_dir),
        settle_s=args.settle_s,
        capture_s=args.capture_s,
        pause_s=args.pause_s,
        prompt_manual_weight=not args.no_manual_weight,
    ) as runner:
        outputs = runner.run(args.test)

    serializable = {key: str(path) for key, path in outputs.items()}
    print(json.dumps(serializable, indent=2))


if __name__ == "__main__":
    main()
