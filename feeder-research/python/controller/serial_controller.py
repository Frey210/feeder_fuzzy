from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Dict, Iterator, List, Optional

import serial

from utils.parser import parse_json_line


@dataclass
class SerialConfig:
    port: str
    baudrate: int = 115200
    timeout: float = 1.0


class SerialController:
    """Research controller for UNO or ESP32 USB bridge links."""

    def __init__(self, config: SerialConfig):
        self.config = config
        self.serial_port: Optional[serial.Serial] = None

    def connect(self) -> None:
        if self.serial_port and self.serial_port.is_open:
            return
        self.serial_port = serial.Serial(
            port=self.config.port,
            baudrate=self.config.baudrate,
            timeout=self.config.timeout,
        )
        self.serial_port.reset_input_buffer()
        self.serial_port.reset_output_buffer()
        time.sleep(2.0)

    def close(self) -> None:
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()

    def __enter__(self) -> "SerialController":
        self.connect()
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.close()

    def _require_port(self) -> serial.Serial:
        if not self.serial_port or not self.serial_port.is_open:
            raise RuntimeError("Serial port is not connected.")
        return self.serial_port

    def send_command(self, command: str) -> None:
        port = self._require_port()
        wire = command.strip() + "\n"
        port.write(wire.encode("utf-8"))
        port.flush()

    def configure_simulation(
        self,
        temp_c: Optional[float] = None,
        biomass_g: Optional[float] = None,
        distance_cm: Optional[float] = None,
        enable: bool = True,
    ) -> None:
        self.send_command("SIM_MODE:ON" if enable else "SIM_MODE:OFF")
        if temp_c is not None:
            self.send_command(f"SET_TEMP:{temp_c:.2f}")
        if biomass_g is not None:
            self.send_command(f"SET_BIOMASS:{biomass_g:.2f}")
        if distance_cm is not None:
            self.send_command(f"SET_DISTANCE:{distance_cm:.2f}")

    def read_line(self) -> str:
        port = self._require_port()
        raw = port.readline()
        if not raw:
            return ""
        return raw.decode("utf-8", errors="ignore").strip()

    def iter_packets(self) -> Iterator[Dict]:
        while True:
            line = self.read_line()
            if not line:
                continue
            payload = parse_json_line(line)
            if payload is not None:
                yield payload

    def read_packet(self, timeout_s: float = 5.0) -> Optional[Dict]:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            line = self.read_line()
            if not line:
                continue
            payload = parse_json_line(line)
            if payload is not None:
                return payload
        return None

    def collect_packets(self, duration_s: float, idle_timeout_s: float = 2.0) -> List[Dict]:
        deadline = time.monotonic() + duration_s
        packets: List[Dict] = []
        while time.monotonic() < deadline:
            packet = self.read_packet(timeout_s=min(idle_timeout_s, max(0.1, deadline - time.monotonic())))
            if packet is not None:
                packets.append(packet)
        return packets
