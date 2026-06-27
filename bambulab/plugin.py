"""Bambu Lab Printer monitoring plugin."""
from __future__ import annotations

import binascii
import json
import struct
import threading
import time
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any

from flask import Blueprint, jsonify, render_template, request

from .printer_worker import PrinterWorker
from services.battery import calculate_battery_percent
import logging

DATA_FILE = 'bambu_printers.json'
DISPLAYS_FILE = 'bambu_display_devices.json'

PKT_BAMBU_DISPLAY = 0x52
BAMBU_DISPLAY_TYPE = 1


def _rgb_to_565(hex_color: str) -> int:
    h = hex_color.lstrip('#')
    if not h:
        return 0
    if len(h) == 6:
        r, g, b = int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)
    elif len(h) == 3:
        r, g, b = int(h[0]*2, 16), int(h[1]*2, 16), int(h[2]*2, 16)
    else:
        return 0
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def _pack_rgb565(color_565: int) -> bytes:
    return struct.pack("<H", color_565 & 0xFFFF)


def _get_mode_color(state_label: str) -> int:
    palette = {
        'Printing': '#1BFF04', 'Done': '#00E5FF', 'Error': '#FF3B30',
        'Idle': '#707070', 'Paused': '#FFCC4D',
        'Preparing': '#FF9F0A', 'Cancelling': '#FF3B30',
        'Offline': "#F8F8F8",
    }
    return _rgb_to_565(palette.get(state_label, palette['Offline']))


@dataclass
class BambuPrinterPlugin:
    """Monitors Bambu Lab 3D printers via MQTT."""

    options: dict[str, Any] = field(default_factory=dict)
    context: Any = None
    manifest: Any = None

    def __post_init__(self) -> None:
        self.blueprint = Blueprint(
            'bambu_printer',
            __name__,
            url_prefix='/plugins/bambu',
            template_folder='templates',
            static_folder='static',
        )
        self.blueprint.add_url_rule('/', 'index', self.index)
        self.blueprint.add_url_rule('/settings', 'settings_page', self.settings_page)
        self.blueprint.add_url_rule('/devices', 'devices_page', self.devices_page)
        self.blueprint.add_url_rule('/api/printers', 'api_printers', self.api_printers)
        self.blueprint.add_url_rule('/api/printers', 'api_add_printer', self.api_add_printer, methods=['POST'])
        self.blueprint.add_url_rule('/api/printers/<printer_id>', 'api_remove_printer', self.api_remove_printer, methods=['DELETE'])
        self.blueprint.add_url_rule('/api/printers/<printer_id>', 'api_update_printer', self.api_update_printer, methods=['PUT'])
        self.blueprint.add_url_rule('/api/printers/<printer_id>/status', 'api_printer_status', self.api_printer_status)
        self.blueprint.add_url_rule('/api/printers/<printer_id>/reconnect', 'api_reconnect_printer', self.api_reconnect_printer, methods=['POST'])
        self.blueprint.add_url_rule('/api/status', 'api_status', self.api_status)
        self.blueprint.add_url_rule('/api/displays', 'api_displays', self.api_displays)
        self.blueprint.add_url_rule('/api/displays/<mac>', 'api_remove_display', self.api_remove_display, methods=['DELETE'])
        self.blueprint.add_url_rule('/api/displays/<mac>/bind', 'api_bind_display', self.api_bind_display, methods=['POST'])
        self.blueprint.add_url_rule('/api/displays/<mac>/rename', 'api_rename_display', self.api_rename_display, methods=['POST'])
        self.blueprint.add_url_rule('/api/displays/<mac>/settings', 'api_display_settings', self.api_display_settings, methods=['POST'])

        self._workers: dict[str, PrinterWorker] = {}
        self._last_display_data: dict[str, tuple[int, float]] = {}
        self._plugin_uuid: str | None = None

    def register(self, app, context=None):
        self.context = context or self.context
        self._init_from_context()
        app.register_blueprint(self.blueprint)

    def start(self, context=None):
        self.context = context or self.context
        self._load_and_start_printers()

    def stop(self, context=None):
        self._stop_all_workers()

    def _init_from_context(self):
        if self.manifest is not None:
            if isinstance(self.manifest, dict):
                self._plugin_uuid = self.manifest.get('plugin_id') or self.manifest.get('id')
            else:
                self._plugin_uuid = getattr(self.manifest, 'plugin_id', None) or getattr(self.manifest, 'id', None)
        if isinstance(self._plugin_uuid, str):
            self._plugin_uuid = self._plugin_uuid.strip()

    # ── Helpers ─────────────────────────────────────────────────

    def _get_data_manager(self):
        if self.context and hasattr(self.context, 'require'):
            try:
                return self.context.require('data_manager')
            except Exception:
                pass
        return None

    def _get_network_server(self):
        if self.context and hasattr(self.context, 'require'):
            try:
                return self.context.require('network_server')
            except Exception:
                pass
        return None

    def _get_plugin_manager(self):
        if self.context and hasattr(self.context, 'require'):
            try:
                return self.context.require('plugin_manager')
            except Exception:
                pass
        return None

    def _log(self, message: str) -> None:
        logger = getattr(self.context, 'logger', None) if self.context is not None else None
        if logger is None:
            logger = logging.getLogger('plugins.bambu_printer')
        try:
            logger.info(message)
        except Exception:
            pass

    # ── Printer Config Persistence ───────────────────────────────

    def _load_printers_config(self) -> list[dict[str, Any]]:
        dm = self._get_data_manager()
        if dm is None:
            return []
        try:
            data = dm.read_json(DATA_FILE)
            if isinstance(data, dict):
                return data.get('printers', [])
            return data if isinstance(data, list) else []
        except Exception:
            return []

    def _save_printers_config(self, printers: list[dict[str, Any]]) -> None:
        dm = self._get_data_manager()
        if dm is None:
            return
        try:
            dm.write_json(DATA_FILE, {'printers': printers})
        except Exception as e:
            self._log(f'Failed to save printer config: {e}')

    # ── Display Config Persistence ───────────────────────────────

    def _load_displays(self) -> list[dict[str, Any]]:
        dm = self._get_data_manager()
        if dm is None:
            return []
        try:
            data = dm.read_json(DISPLAYS_FILE)
            if isinstance(data, dict):
                return data.get('displays', [])
            return data if isinstance(data, list) else []
        except Exception:
            return []

    def _save_displays(self, displays: list[dict[str, Any]]) -> None:
        dm = self._get_data_manager()
        if dm is None:
            return
        try:
            dm.write_json(DISPLAYS_FILE, {'displays': displays})
        except Exception as e:
            self._log(f'Failed to save displays config: {e}')

    def _upsert_display(self, mac: str, **kwargs) -> dict[str, Any]:
        displays = self._load_displays()
        for d in displays:
            if d['mac'] == mac:
                d.update(kwargs)
                d['last_seen'] = datetime.now(timezone.utc).isoformat()
                self._save_displays(displays)
                return d
        entry = {'mac': mac, 'name': f'Display {mac[-8:]}', 'printer_id': None, 'last_seen': datetime.now(timezone.utc).isoformat()}
        entry.update(kwargs)
        displays.append(entry)
        self._save_displays(displays)
        return entry

    def get_ota_devices(self) -> list[dict[str, Any]]:
        """Return OTA-capable devices for the OTA page."""
        result: list[dict[str, Any]] = []
        for d in self._load_displays():
            result.append({
                'mac_address': d['mac'],
                'name': d.get('name', f'Display {d["mac"][-8:]}'),
                'version': d.get('version', '0.0.0'),
                'platform': d.get('platform', 'esp32'),
                'device_type': BAMBU_DISPLAY_TYPE,
                'last_gateway_mac': d.get('last_seen_gateway'),
            })
        return result

    def _find_sensor_by_id(self, device_id: str) -> dict[str, Any] | None:
        """Called by the pairing API to resolve a device MAC → {id, name}."""
        for d in self._load_displays():
            if d['mac'] == device_id:
                return {'id': d['mac'], 'name': d.get('name', '')}
        return None

    # ── Worker Management ────────────────────────────────────────

    def _load_and_start_printers(self):
        configs = self._load_printers_config()
        for cfg in configs:
            if cfg.get('enabled', True):
                self._start_worker(cfg)

    def _start_worker(self, cfg: dict[str, Any]) -> PrinterWorker | None:
        printer_id = cfg.get('id') or str(uuid.uuid4())
        old = self._workers.pop(printer_id, None)
        if old:
            old.stop(block=False)

        worker = PrinterWorker(
            printer_id=printer_id,
            name=cfg.get('name', 'Bambu Printer'),
            ip=cfg.get('ip', ''),
            serial=cfg.get('serial', ''),
            access_code=cfg.get('access_code', ''),
            on_status_change=self._push_to_displays,
        )
        worker.start()
        self._workers[printer_id] = worker
        self._log(f'Started worker for printer {cfg.get("name")} ({printer_id})')
        return worker

    def _stop_worker(self, printer_id: str, block: bool = False) -> None:
        worker = self._workers.pop(printer_id, None)
        if worker:
            worker.stop(block=block)
            self._log(f'Stopped worker for printer {printer_id}')

    def _stop_all_workers(self) -> None:
        for pid in list(self._workers.keys()):
            self._stop_worker(pid)

    def _restart_worker(self, cfg: dict[str, Any]) -> None:
        self._start_worker(cfg)

    def _build_packet(self, ns, target_mac: str, payload: bytes, msg_id: int) -> bytes:
        src_mac = b'\x00' * 6
        tgt_mac = binascii.unhexlify(target_mac.replace(':', ''))
        signature = ns.encoder._calculate_hash(payload)
        packet = struct.pack("<BI", PKT_BAMBU_DISPLAY, signature) + src_mac + tgt_mac + struct.pack("<B", msg_id) + payload
        return packet

    def _encode_display_data(self, status: dict[str, Any], connected: bool = True) -> bytes:
        NA_VALUE = 0xFFFF

        if not connected:
            state_label = 'Offline'
            mode_color = _get_mode_color('Offline')
            nozzle_temp = NA_VALUE
            bed_temp = NA_VALUE
            nozzle_target = NA_VALUE
            bed_target = NA_VALUE
            progress_pct = 0xFF
            cl = NA_VALUE
            tl = NA_VALUE
            rem_min = NA_VALUE
            fn = b'\x00' * 40
            tray_type = ''
            tray_color = 0
        else:
            state_label = self._get_status_label(status.get('state'))
            mode_color = _get_mode_color(state_label)

            nozzle_temp = round(max(0, min(65535, status.get('nozzle_temp', 0) or NA_VALUE)))
            bed_temp = round(max(0, min(65535, status.get('bed_temp', 0) or NA_VALUE)))
            nozzle_target = round(max(0, min(65535, status.get('nozzle_target_temp', 0) or NA_VALUE)))
            bed_target = round(max(0, min(65535, status.get('bed_target_temp', 0) or NA_VALUE)))

            pct = status.get('percentage')
            if pct is None or pct == 'Unknown':
                progress_pct = 0xFF
            else:
                progress_pct = round(max(0, min(100, int(pct))))

            cl = status.get('current_layer')
            tl = status.get('total_layer')
            if cl is None or tl == 0:
                cl = NA_VALUE
            if tl is None or tl == 0:
                tl = NA_VALUE
            rem_sec = status.get('remaining_time', 0) or 0
            rem_min = round(max(0, min(65535, int(rem_sec / 60))))
            if rem_sec == 0:
                rem_min = 0xFFFF

            fn = (status.get('file_name') or '')[:40].encode('ascii', errors='replace')
            fn = fn.ljust(40, b'\x00')[:40]

            tray_type = ''
            tray_color = 0
            active_tray = status.get('active_tray')
            ams_list = status.get('ams', [])
            if active_tray is not None:
                for t in ams_list:
                    if t.get('tray_id') == active_tray:
                        tray_type = (t.get('type') or '')[:15]
                        tray_color = _rgb_to_565(t.get('color', '#808080'))
                        break

        status_text = (state_label or '')[:20].encode('ascii', errors='replace')
        status_text = status_text.ljust(20, b'\x00')[:20]

        tray_type_bytes = tray_type.encode('ascii', errors='replace')
        tray_type_bytes = tray_type_bytes.ljust(15, b'\x00')[:15]
        
        payload = struct.pack(
            "<BHHHHBHHH",
            0x01,
            int(nozzle_temp),
            int(bed_temp),
            int(nozzle_target),
            int(bed_target),
            progress_pct,
            int(cl), 
            int(tl),
            int(rem_min),
        )
        payload += _pack_rgb565(mode_color)
        payload += bytes([len(status_text.rstrip(b'\x00'))])
        payload += status_text
        payload += bytes([len(fn.rstrip(b'\x00'))])
        payload += fn
        payload += bytes([len(tray_type_bytes.rstrip(b'\x00'))])
        payload += tray_type_bytes
        payload += _pack_rgb565(tray_color)
        now = datetime.now()
        clock_flag = 0x01 if not connected else 0x00
        payload += struct.pack("<BBB", now.hour, now.minute, now.second)
        payload += struct.pack("<B", clock_flag)
        payload += b'\x00' * (185 - len(payload))
        return payload[:185]

    @staticmethod
    def _get_status_label(state):
        s = (state or '').upper()
        if s == 'IDLE':
            return 'Idle'
        if s == 'RUNNING':
            return 'Printing'
        if s == 'FINISH' or s == 'SUCCESS':
            return 'Done'
        if s == 'FAILED':
            return 'Error'
        if s == 'PAUSE':
            return 'Paused'
        if s == 'PREPARE':
            return 'Preparing'
        if s == 'CANCEL':
            return 'Cancelling'
        return state or 'Offline'

    # ── Event Handlers ───────────────────────────────────────────

    def on_plugin_hello(self, event_name: str, payload: dict[str, Any], host=None):
        if not self._plugin_uuid:
            return
        device_mac = payload.get('device_mac')
        plugin_uuid = payload.get('plugin_uuid', '')
        plugin_device_type = payload.get('plugin_device_type', 0)
        is_paired = payload.get('is_paired', False)

        if plugin_uuid.replace('-', '').lower() != self._plugin_uuid.replace('-', '').lower():
            return
        if plugin_device_type != BAMBU_DISPLAY_TYPE:
            return

        rssi = payload.get('rssi', -100)
        version = payload.get('version', '0.0.0')
        platform = payload.get('platform', 'esp32')
        gateway_radio_mac = payload.get('gateway_radio_mac')

        if not is_paired:
            self._log(f'Display {device_mac} is unpaired — ignoring')
            return

        norm_gw = gateway_radio_mac.upper() if isinstance(gateway_radio_mac, str) else None
        self._upsert_display(device_mac, rssi=rssi, version=version, platform=platform, last_seen_gateway=norm_gw, battery_mv=0, battery_percent=None)
        self._last_display_data.pop(device_mac, None)
        self._log(f'Display device registered/updated: {device_mac} rssi={rssi}')

    def on_packet_received(self, event_name: str, payload: dict[str, Any], host=None):
        packet_type = payload.get('type')
        packet_payload = payload.get('payload') or b''
        source_mac = payload.get('source_mac')

        if not self._plugin_uuid:
            return
        if packet_type != PKT_BAMBU_DISPLAY:
            return

        pm = self._get_plugin_manager()
        if pm:
            plugin_for_mac = pm.get_plugin_for_mac(source_mac)
            if not plugin_for_mac or plugin_for_mac.replace('-', '').lower() != self._plugin_uuid.replace('-', '').lower():
                return

        if len(packet_payload) < 1:
            return
        subtype = packet_payload[0]

        if subtype == 0x02:
            gateway_radio_mac = payload.get('gateway_radio_mac')
            self._handle_display_request(source_mac, gateway_radio_mac, payload)

    def _handle_display_request(self, display_mac: str, gateway_radio_mac: str | None, pkt_payload: dict[str, Any]):
        kwargs: dict[str, Any] = {'last_seen_gateway': gateway_radio_mac}
        raw = pkt_payload.get('payload', b'')
        if isinstance(raw, (bytes, bytearray)) and len(raw) >= 3:
            battery_mv = struct.unpack('<H', raw[1:3])[0]
            kwargs['battery_mv'] = battery_mv
            kwargs['battery_percent'] = calculate_battery_percent(battery_mv)
        self._upsert_display(display_mac, **kwargs)

        displays = self._load_displays()
        disp = next((d for d in displays if d['mac'] == display_mac), None)
        if not disp:
            return
        printer_id = disp.get('printer_id')
        if not printer_id:
            return
        worker = self._workers.get(printer_id)
        if not worker:
            return
        snapshot = worker.get_status_snapshot()
        st = snapshot.get('status', {})
        connected = snapshot.get('connected', False)

        ns = self._get_network_server()
        if ns is None:
            return
        try:
            data_packet = self._encode_display_data(st, connected=connected)
            msg_id = ns.get_message_id()
            full_packet = self._build_packet(ns, display_mac, data_packet, msg_id)
            threading.Thread(
                target=ns.send_raw_packet_to_device,
                args=(display_mac, full_packet),
                kwargs={'wait_for_delivery': True, 'msg_id': msg_id, 'gateway_preference': gateway_radio_mac},
                daemon=True,
            ).start()
            self._log(f'Sent status to display {display_mac} on request')
        except Exception as e:
            self._log(f'Failed to respond to display request from {display_mac}: {e}')

    def _send_to_display(self, display: dict[str, Any], data_packet: bytes) -> None:
        """Send a data packet to a display device (async, non-blocking)."""
        ns = self._get_network_server()
        if ns is None:
            return
        try:
            mac = display['mac']
            gateway = display.get('last_seen_gateway')
            msg_id = ns.get_message_id()
            full_packet = self._build_packet(ns, mac, data_packet, msg_id)
            threading.Thread(
                target=ns.send_raw_packet_to_device,
                args=(mac, full_packet),
                kwargs={'wait_for_delivery': True, 'msg_id': msg_id, 'gateway_preference': gateway},
                daemon=True,
            ).start()
        except Exception as e:
            self._log(f'Failed to send to display {mac}: {e}')

    def _push_to_displays(self, printer_id: str, status: dict[str, Any]) -> None:
        """Push updated status to all displays bound to this printer."""
        displays = self._load_displays()
        for disp in displays:
            if disp.get('printer_id') != printer_id:
                continue
            worker = self._workers.get(printer_id)
            if not worker:
                continue
            snapshot = worker.get_status_snapshot()
            st = snapshot.get('status', {})
            connected = snapshot.get('connected', False)

            mac = disp['mac']
            data_packet = self._encode_display_data(st, connected=connected)

            now = time.time()
            packet_hash = hash(data_packet)
            last_hash = self._last_display_data.get(mac, (0, 0.0))[0]
            if packet_hash != last_hash:
                self._last_display_data[mac] = (packet_hash, now)
                self._send_to_display(disp, data_packet)
                self._log(f'Pushed status to display {mac} for printer {printer_id}')

    # ── Routes ───────────────────────────────────────────────────

    def index(self):
        printers = self._load_printers_config()
        return render_template('bambu.html', printers=printers)

    def settings_page(self):
        printers = self._load_printers_config()
        return render_template('bambu_settings.html', printers=printers)

    def api_status(self):
        snapshots = []
        for pid, worker in list(self._workers.items()):
            snapshots.append(worker.get_status_snapshot())
        return jsonify({'success': True, 'printers': snapshots})

    def api_printers(self):
        configs = self._load_printers_config()
        return jsonify({'success': True, 'printers': configs})

    def api_add_printer(self):
        data = request.get_json(silent=True) or {}
        ip = str(data.get('ip', '')).strip()
        serial = str(data.get('serial', '')).strip()
        access_code = str(data.get('access_code', '')).strip()
        name = str(data.get('name', '')).strip() or f'Printer {serial[:8] or ip}'

        if not ip or not serial or not access_code:
            return jsonify({'success': False, 'error': 'IP, serial, and access code are required'}), 400

        configs = self._load_printers_config()
        new_id = str(uuid.uuid4())
        entry = {
            'id': new_id,
            'name': name,
            'ip': ip,
            'serial': serial,
            'access_code': access_code,
            'enabled': True,
        }
        configs.append(entry)
        self._save_printers_config(configs)
        self._start_worker(entry)
        return jsonify({'success': True, 'printer': entry})

    def api_remove_printer(self, printer_id: str):
        configs = self._load_printers_config()
        configs = [c for c in configs if c.get('id') != printer_id]
        self._save_printers_config(configs)
        self._stop_worker(printer_id)
        return jsonify({'success': True})

    def api_update_printer(self, printer_id: str):
        data = request.get_json(silent=True) or {}
        configs = self._load_printers_config()
        for cfg in configs:
            if cfg.get('id') == printer_id:
                if 'name' in data:
                    cfg['name'] = str(data['name']).strip()
                if 'ip' in data:
                    cfg['ip'] = str(data['ip']).strip()
                if 'serial' in data:
                    cfg['serial'] = str(data['serial']).strip()
                if 'access_code' in data:
                    cfg['access_code'] = str(data['access_code']).strip()
                self._save_printers_config(configs)
                worker = self._start_worker(cfg)
                snapshot = worker.get_status_snapshot() if worker else None
                return jsonify({'success': True, 'printer': cfg, 'snapshot': snapshot})
        return jsonify({'success': False, 'error': 'Printer not found'}), 404

    def api_printer_status(self, printer_id: str):
        worker = self._workers.get(printer_id)
        if not worker:
            return jsonify({'success': False, 'error': 'Printer not found'}), 404
        return jsonify({'success': True, 'printer': worker.get_status_snapshot()})

    def api_reconnect_printer(self, printer_id: str):
        configs = self._load_printers_config()
        for cfg in configs:
            if cfg.get('id') == printer_id:
                worker = self._start_worker(cfg)
                snapshot = worker.get_status_snapshot() if worker else None
                return jsonify({'success': True, 'printer': cfg, 'snapshot': snapshot})
        return jsonify({'success': False, 'error': 'Printer not found'}), 404

    def api_displays(self):
        displays = self._load_displays()
        printers = self._load_printers_config()
        printer_map = {p['id']: p['name'] for p in printers}
        for d in displays:
            d['printer_name'] = printer_map.get(d.get('printer_id'), '')
        return jsonify({'success': True, 'displays': displays})

    def api_remove_display(self, mac: str):
        displays = self._load_displays()
        displays = [d for d in displays if d['mac'] != mac]
        self._save_displays(displays)
        return jsonify({'success': True})

    def api_bind_display(self, mac: str):
        data = request.get_json(silent=True) or {}
        printer_id = data.get('printer_id')
        displays = self._load_displays()
        for d in displays:
            if d['mac'] == mac:
                d['printer_id'] = printer_id
                self._save_displays(displays)
                return jsonify({'success': True})
        return jsonify({'success': False, 'error': 'Display not found'}), 404

    def api_rename_display(self, mac: str):
        data = request.get_json(silent=True) or {}
        name = str(data.get('name', '')).strip()
        if not name:
            return jsonify({'success': False, 'error': 'Name is required'}), 400
        displays = self._load_displays()
        for d in displays:
            if d['mac'] == mac:
                d['name'] = name
                self._save_displays(displays)
                return jsonify({'success': True})
        return jsonify({'success': False, 'error': 'Display not found'}), 404

    def api_display_settings(self, mac: str):
        data = request.get_json(silent=True) or {}
        printer_id = data.get('printer_id')
        displays = self._load_displays()
        for d in displays:
            if d['mac'] == mac:
                d['printer_id'] = printer_id
                self._save_displays(displays)
                return jsonify({'success': True})
        return jsonify({'success': False, 'error': 'Display not found'}), 404

    def devices_page(self):
        printers = self._load_printers_config()
        displays = self._load_displays()
        printer_map = {p['id']: p['name'] for p in printers}
        for d in displays:
            d['printer_name'] = printer_map.get(d.get('printer_id'), '')
        return render_template('bambu_devices.html', printers=printers, displays=displays)


def create_plugin(options=None, context=None, manifest=None, host=None, **_kwargs):
    return BambuPrinterPlugin(options=options or {}, context=context or host, manifest=manifest)
