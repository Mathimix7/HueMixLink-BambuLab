from __future__ import annotations

import json
import ssl
import threading
from dataclasses import dataclass, field

import paho.mqtt.client as mqtt
import logging

logger = logging.getLogger(__name__)


@dataclass
class AmsSlot:
    slot_id: int
    material: str
    color: str
    remaining_percent: int


@dataclass
class AmsUnit:
    ams_id: str
    unit_index: int

    humidity: int
    temperature: float

    slots: list[AmsSlot] = field(default_factory=list)


@dataclass
class PrinterStatus:
    nozzle_temp: float = 0.0
    nozzle_target_temp: float = 0.0

    bed_temp: float = 0.0
    bed_target_temp: float = 0.0

    state: str = "UNKNOWN"

    progress: int = 0

    current_layer: int = 0
    total_layers: int = 0

    remaining_minutes: int = 0

    filename: str = ""

    active_tray: int | None = None
    target_tray: int | None = None

    ams_units: list[AmsUnit] = field(default_factory=list)


class BambuPrinter:

    def __init__(
        self,
        host: str,
        serial: str,
        access_code: str,
    ):
        self.host = host
        self.serial = serial
        self.access_code = access_code

        self.status = PrinterStatus()

        self._raw: dict = {}
        self._ready = threading.Event()

        self.report_topic = f"device/{serial}/report"
        self.command_topic = f"device/{serial}/request"

        client_id = f"huemixlink-bambu-{serial}"
        self.client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=client_id,
        )

        self.client.username_pw_set("bblp", access_code)

        self.client.tls_set(
            cert_reqs=ssl.CERT_NONE,
        )

        self.client.tls_insecure_set(True)

        self.client.reconnect_delay_set(min_delay=2, max_delay=30)

        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message


    def connect(self):
        self._ready.clear()

        self.client.connect(
            self.host,
            8883,
            keepalive=3,
        )

        self.client.loop_start()

    def disconnect(self):
        self.client.loop_stop()
        self.client.disconnect()

    def wait_until_ready(
        self,
        timeout: float = 10.0,
    ) -> bool:

        return self._ready.wait(timeout)

    def _on_connect(
        self,
        client,
        userdata,
        flags,
        reason_code,
        properties,
    ):

        if logger:
            logger.info(f"[BAMBU-MQTT] Connected: {reason_code}")

        client.subscribe(self.report_topic)

        if logger:
            logger.debug(f"[BAMBU-MQTT] Subscribed: {self.report_topic}")

        self.refresh()

    def _on_message(
        self,
        client,
        userdata,
        msg,
    ):

        try:
            payload = json.loads(msg.payload.decode())
        except Exception:
            return

        if "print" not in payload:
            return

        p = payload["print"]

        self._raw.update(p)

        self._rebuild_status()

        if "gcode_state" in self._raw:
            self._ready.set()


    def refresh(self):
        payload = {
            "pushing": {
                "sequence_id": "1",
                "command": "pushall",
            }
        }

        self.client.publish(
            self.command_topic,
            json.dumps(payload),
        )


    def _rebuild_status(self):
        p = self._raw

        self.status.nozzle_temp = p.get(
            "nozzle_temper",
            self.status.nozzle_temp,
        )

        self.status.nozzle_target_temp = p.get(
            "nozzle_target_temper",
            self.status.nozzle_target_temp,
        )

        self.status.bed_temp = p.get(
            "bed_temper",
            self.status.bed_temp,
        )

        self.status.bed_target_temp = p.get(
            "bed_target_temper",
            self.status.bed_target_temp,
        )

        self.status.state = p.get(
            "gcode_state",
            self.status.state,
        )

        self.status.progress = p.get(
            "mc_percent",
            self.status.progress,
        )

        self.status.current_layer = p.get(
            "layer_num",
            self.status.current_layer,
        )

        self.status.total_layers = p.get(
            "total_layer_num",
            self.status.total_layers,
        )

        self.status.remaining_minutes = p.get(
            "mc_remaining_time",
            self.status.remaining_minutes,
        )

        self.status.filename = p.get(
            "gcode_file",
            self.status.filename,
        )

        ams_root = p.get("ams")

        if ams_root:

            self.status.active_tray = self._safe_int(
                ams_root.get("tray_now")
            )

            self.status.target_tray = self._safe_int(
                ams_root.get("tray_tar")
            )

            self.status.ams_units = self._parse_ams(
                ams_root
            )

    @staticmethod
    def _parse_ams(
        ams_root: dict,
    ) -> list[AmsUnit]:

        result = []

        for unit_index, ams in enumerate(
            ams_root.get("ams", [])
        ):

            unit = AmsUnit(
                ams_id=ams.get("ams_id", ""),
                unit_index=unit_index,
                humidity=int(
                    ams.get("humidity", 0)
                ),
                temperature=float(
                    ams.get("temp", 0)
                ),
            )

            for tray in ams.get("tray", []):

                unit.slots.append(
                    AmsSlot(
                        slot_id=int(
                            tray.get("id", 0)
                        ),
                        material=tray.get(
                            "tray_sub_brands",
                            "",
                        ),
                        color=tray.get(
                            "tray_color",
                            "",
                        ),
                        remaining_percent=int(
                            tray.get(
                                "remain",
                                0,
                            )
                        ),
                    )
                )

            result.append(unit)

        return result

    @staticmethod
    def _safe_int(value):

        try:
            value = int(value)

            if value == 255:
                return None

            return value

        except Exception:
            return None


    def summary(self) -> str:
        s = self.status

        return (
            f"{s.state} | "
            f"{s.progress}% | "
            f"{s.nozzle_temp:.1f}°C nozzle | "
            f"{s.bed_temp:.1f}°C bed | "
            f"layer {s.current_layer}/{s.total_layers} | "
            f"{s.remaining_minutes} min left | "
            f"{s.filename}"
        )
    
    def get_status(self) -> PrinterStatus:
        return self.status
    
    def get_ams_units(self) -> list[AmsUnit]:
        return self.status.ams_units