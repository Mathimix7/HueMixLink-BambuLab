from __future__ import annotations

import threading
import time
import logging
from datetime import datetime, timezone
from typing import Any, Callable

logger = logging.getLogger('plugins.bambu_printer')


class PrinterWorker:
    """Background worker that maintains a persistent MQTT connection to a Bambu Lab printer."""

    def __init__(
        self,
        printer_id: str,
        name: str,
        ip: str,
        serial: str,
        access_code: str,
        on_status_change: Callable[[str, dict[str, Any]], None] | None = None,
    ) -> None:
        self.printer_id = printer_id
        self.name = name
        self.ip = ip
        self.serial = serial
        self.access_code = access_code
        self.on_status_change = on_status_change

        self._printer: Any = None
        self._lock = threading.RLock()
        self._thread: threading.Thread | None = None
        self._running = False
        self._last_status_hash: int = 0

        self._status: dict[str, Any] = {}
        self._connected = False
        self._error: str | None = None
        self._last_update: str | None = None
        self._consecutive_failures: int = 0
        self.MAX_CONSECUTIVE_FAILURES = 2
        self._last_failure_time: float = 0.0
        self._retry_delay: float = 5.0

    def start(self) -> None:
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(
            target=self._run,
            daemon=True,
            name=f'BambuPrinter-{self.printer_id}',
        )
        self._thread.start()

    def stop(self, block: bool = True) -> None:
        self._running = False
        if self._printer:
            try:
                self._printer.disconnect()
            except Exception:
                pass
            self._printer = None
        if self._thread:
            if block:
                self._thread.join(timeout=5)

    def _run(self) -> None:
        try:
            from .bambulib import BambuPrinter
            self._printer = BambuPrinter(self.ip, self.serial, self.access_code)
        except ImportError:
            with self._lock:
                self._connected = False
                self._error = 'paho-mqtt package not installed'
            return

        with self._lock:
            self._connected = False
            self._error = None

        while self._running:
            try:
                old_connected = self._connected
                self._ensure_connected()
                with self._lock:
                    self._refresh_status(old_connected)
            except Exception as e:
                logger.warning(f'Error updating status for {self.name}: {e}')
                with self._lock:
                    self._connected = False
                    self._error = str(e)
            time.sleep(1)

        if self._printer:
            try:
                self._printer.disconnect()
            except Exception:
                pass
            self._printer = None

    def _ensure_connected(self) -> None:
        """Connect or reconnect to the printer. Does not raise on failure — marks offline."""
        if not self._printer:
            return
        if self._printer.client.is_connected():
            return
        with self._lock:
            should_retry = not (self._consecutive_failures >= self.MAX_CONSECUTIVE_FAILURES
                                and time.time() - self._last_failure_time < self._retry_delay)
            self._connected = False
            if not should_retry:
                self._error = 'Connection failed — check IP, serial, and access code'
                return
            self._error = 'Connecting...'
        try:
            self._printer.disconnect()
        except Exception:
            pass
        try:
            self._printer.connect()
            ready = self._printer.wait_until_ready(timeout=2)
            with self._lock:
                self._connected = ready
                if ready:
                    self._consecutive_failures = 0
                    self._error = None
                    logger.info(f'Connected to printer {self.name}')
                else:
                    self._consecutive_failures += 1
                    self._last_failure_time = time.time()
                    self._retry_delay = 30.0
                    self._error = 'Printer did not respond — check serial number and access code'
                    if self._consecutive_failures >= self.MAX_CONSECUTIVE_FAILURES:
                        logger.warning(f'{self.name}: too many failures, next retry in {self._retry_delay:.0f}s')
                        self._error = 'Connection failed — check IP, serial, and access code'
                        try:
                            self._printer.disconnect()
                        except Exception:
                            pass
        except Exception as e:
            with self._lock:
                self._connected = False
                self._error = str(e)
                self._consecutive_failures += 1
                self._last_failure_time = time.time()
                self._retry_delay = 5.0

    def _refresh_status(self, old_connected: bool = False) -> None:
        p = self._printer
        if not p:
            return

        s = p.get_status()

        ams_data: list[dict[str, Any]] = []
        ams_env: list[dict[str, Any]] = []
        for unit in p.get_ams_units():
            ams_env.append({
                'index': unit.unit_index,
                'humidity': round(unit.humidity),
                'temperature': round(unit.temperature),
            })
            for slot in unit.slots:
                ams_data.append({
                    'color': slot.color,
                    'type': slot.material,
                    'remaining': slot.remaining_percent,
                    'ams_id': unit.unit_index,
                    'tray_id': slot.slot_id,
                })

        self._status = {
            'state': s.state,
            'percentage': round(s.progress),
            'file_name': s.filename,
            'current_layer': round(s.current_layer),
            'total_layer': round(s.total_layers),
            'nozzle_temp': round(s.nozzle_temp),
            'nozzle_target_temp': round(s.nozzle_target_temp),
            'bed_temp': round(s.bed_temp),
            'bed_target_temp': round(s.bed_target_temp),
            'remaining_time': round(s.remaining_minutes * 60),
            'ams': ams_data,
            'ams_env': ams_env,
            'active_tray': s.active_tray,
            'target_tray': s.target_tray,
        }

        if p.client.is_connected() and s.state != 'UNKNOWN':
            self._connected = True
            self._error = None
            self._last_update = datetime.now(timezone.utc).isoformat()

        if old_connected != self._connected:
            self._last_status_hash = 0

        if self.on_status_change:
            new_hash = hash(str(self._status))
            if new_hash != self._last_status_hash:
                self._last_status_hash = new_hash
                try:
                    self.on_status_change(self.printer_id, dict(self._status))
                except Exception:
                    pass

    def get_status_snapshot(self) -> dict[str, Any]:
        with self._lock:
            return {
                'printer_id': self.printer_id,
                'name': self.name,
                'ip': self.ip,
                'serial': self.serial,
                'connected': self._connected,
                'error': self._error,
                'last_update': self._last_update,
                'status': dict(self._status),
            }
