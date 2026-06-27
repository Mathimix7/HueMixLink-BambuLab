(function () {
  'use strict';

  const STATUS_COLORS = {
    Printing: '#1BFF04',
    Done: '#00E5FF',
    Error: '#FF3B30',
    Idle: '#707070',
    Paused: '#FFCC4D',
    Preparing: '#FF9F0A',
    Cancelling: '#FF3B30',
    Offline: '#FFFFFF',
    Fallback: '#707070',
  };

  const STATUS_ICONS = {
    Printing: 'fa-spinner fa-pulse',
    Done: 'fa-check-circle',
    Error: 'fa-exclamation-triangle',
    Idle: 'fa-pause-circle',
    Paused: 'fa-pause-circle',
    Preparing: 'fa-fire',
    Cancelling: 'fa-times-circle',
    Offline: 'fa-plug',
    Fallback: 'fa-question-circle',
  };

  function getStatusLabel(state) {
    const s = (state || '').toUpperCase();
    if (s === 'IDLE') return 'Idle';
    if (s === 'RUNNING') return 'Printing';
    if (s === 'FINISH' || s === 'SUCCESS') return 'Done';
    if (s === 'FAILED') return 'Error';
    if (s === 'PAUSE') return 'Paused';
    if (s === 'PREPARE') return 'Preparing';
    if (s === 'CANCEL') return 'Cancelling';
    return state || 'Fallback';
  }

  let pollInterval = null;

  function getPrinterIds() {
    return Array.from(document.querySelectorAll('.printer-card')).map((c) => c.dataset.printerId);
  }

  async function refreshAll() {
    try {
      const res = await fetch('/plugins/bambu/api/status');
      const data = await res.json();
      if (data.success && data.printers) data.printers.forEach(updatePrinterCard);
    } catch (err) {
      console.error(err);
    }
  }

  async function manualRefresh() {
    const icon = document.getElementById('refresh-icon');
    if (icon) {
      icon.classList.add('rotate-360');
      setTimeout(() => icon.classList.remove('rotate-360'), 500);
    }
    const cards = document.querySelectorAll('.printer-card[data-printer-id]');
    await Promise.all(
      Array.from(cards).map(card =>
        fetch('/plugins/bambu/api/printers/' + card.dataset.printerId + '/reconnect', { method: 'POST' })
          .then(r => r.json())
          .then(d => { if (d.success && d.snapshot) updatePrinterCard(d.snapshot); })
          .catch(() => {})
      )
    );
    refreshAll();
  }

  function updatePrinterCard(snapshot) {
    const card = document.querySelector(`.printer-card[data-printer-id="${snapshot.printer_id}"]`);
    if (!card) return;

    const st = snapshot.status || {};
    const connected = snapshot.connected;
    const error = snapshot.error;

    // ── Connection dot ──────────────────────────────────────
    const dot = card.querySelector('.conn-dot');
    if (dot) {
      if (connected) {
        dot.className = 'conn-dot w-2 h-2 rounded-full bg-emerald-500';
        dot.title = 'Connected';
      } else {
        dot.className = 'conn-dot w-2 h-2 rounded-full bg-red-400';
        dot.title = error && error !== 'None' ? error : 'Disconnected';
      }
    }

    // ── State badge ─────────────────────────────────────────
    const badge = card.querySelector('.state-badge');
    const icon = card.querySelector('.status-icon');
    const text = card.querySelector('.status-text');
    if (badge && icon && text) {
      let label, color;
      if (!connected) {
        label = 'Offline';
        color = STATUS_COLORS.Offline;
      } else {
        label = getStatusLabel(st.state);
        color = STATUS_COLORS[label] || STATUS_COLORS.Fallback;
      }
      badge.style.backgroundColor = color;
      badge.style.color = textColorForBg(color);
      text.textContent = label;
      icon.className = 'status-icon mr-2 fas ' + (STATUS_ICONS[label] || STATUS_ICONS.Fallback);
    }

    // If offline, clear all values and stop
    if (!connected) {
      const nozzleCard = card.querySelector('.metric-card:nth-child(1)');
      if (nozzleCard) {
        nozzleCard.querySelector('.temp-value').textContent = '--°C';
        nozzleCard.querySelector('.temp-target').textContent = '';
        fillThermometer(nozzleCard, 0, 0);
      }
      const bedCard = card.querySelector('.metric-card:nth-child(2)');
      if (bedCard) {
        bedCard.querySelector('.temp-value').textContent = '--°C';
        bedCard.querySelector('.temp-target').textContent = '';
        fillThermometer(bedCard, 0, 0);
      }
      const envCard = card.querySelector('.metric-card:nth-child(3)');
      if (envCard) {
        envCard.querySelector('.ams-env-temp').textContent = '--';
        envCard.querySelector('.ams-env-humidity').textContent = '--';
      }
      const prog = card.querySelector('.print-progress');
      if (prog) prog.classList.add('hidden');
      const amsSec = card.querySelector('.ams-section');
      if (amsSec) amsSec.classList.add('hidden');
      const errSec = card.querySelector('.error-section');
      if (errSec) {
        // errSec.classList.remove('hidden');
        errSec.querySelector('.error-message').textContent = error || 'Printer is offline';
        errSec.querySelector('.reconnect-btn').dataset.printerId = snapshot.printer_id;
      }
      return;
    }

    // ── Nozzle ──────────────────────────────────────────────
    const nozzleCard = card.querySelector('.metric-card:nth-child(1)');
    if (nozzleCard) {
      nozzleCard.querySelector('.temp-value').textContent = formatTemp(st.nozzle_temp);
      const nt = st.nozzle_target_temp;
      nozzleCard.querySelector('.temp-target').textContent = nt ? 'target ' + Math.round(nt) + '°C' : '';
      fillThermometer(nozzleCard, st.nozzle_temp, nt);
    }

    // ── Bed ─────────────────────────────────────────────────
    const bedCard = card.querySelector('.metric-card:nth-child(2)');
    if (bedCard) {
      bedCard.querySelector('.temp-value').textContent = formatTemp(st.bed_temp);
      const bt = st.bed_target_temp;
      bedCard.querySelector('.temp-target').textContent = bt ? 'target ' + Math.round(bt) + '°C' : '';
      fillThermometer(bedCard, st.bed_temp, bt);
    }


    // ── AMS Environment ─────────────────────────────────────
    const envCard = card.querySelector('.metric-card:nth-child(3)');
    if (envCard) {
      const env = st.ams_env && st.ams_env[0];
      envCard.querySelector('.ams-env-temp').textContent = env ? env.temperature + '°C' : '--';
      envCard.querySelector('.ams-env-humidity').textContent = env ? humidityLabel(env.humidity) : '--';
    }

    // ── Print Progress ─────────────────────────────────────
    const prog = card.querySelector('.print-progress');
    if (prog) {
      const isPrinting = st.state === 'RUNNING';
      if (isPrinting && st.percentage !== null && st.percentage !== undefined && st.percentage !== 'Unknown') {
        prog.classList.remove('hidden');
        const pct = typeof st.percentage === 'number' ? st.percentage : parseInt(st.percentage) || 0;
        prog.querySelector('.file-name').textContent = escapeHtml(st.file_name || '-');
        prog.querySelector('.progress-pct').textContent = pct + '%';
        prog.querySelector('.progress-bar').style.width = pct + '%';
        const hasLayers = st.current_layer != null && st.total_layer != null;
        prog.querySelector('.layers').innerHTML =
          '<i class="fas fa-layer-group mr-1"></i>Layer ' +
          (hasLayers ? st.current_layer + '/' + st.total_layer : '-/-');
        const rem = st.remaining_time;
        prog.querySelector('.remaining-time').innerHTML =
          '<i class="far fa-clock mr-1"></i>' +
          (rem != null && rem !== 'Unknown' ? formatTime(rem) + ' left' : 'Remaining: -');
      } else {
        prog.classList.add('hidden');
      }
    }

    // ── AMS ─────────────────────────────────────────────────
    const amsSec = card.querySelector('.ams-section');
    const amsGrid = card.querySelector('.ams-trays');
    if (amsSec && amsGrid) {
      const ams = st.ams;
      const activeSlot = st.active_tray;
      if (ams && ams.length > 0) {
        amsSec.classList.remove('hidden');
        amsGrid.innerHTML = ams
          .map((t) => {
            const color = normalizeColor(t.color);
            const isActive = t.tray_id === activeSlot;
            return `
              <div class="filament-card rounded-xl border transition-all duration-200 p-3 ${isActive ? 'ring-2 ring-emerald-400 border-emerald-300 bg-emerald-50/30' : 'border-gray-200 bg-white hover:shadow-md'}">
                <div class="flex items-center space-x-2 mb-2">
                  <span class="color-swatch w-5 h-5 rounded-full border border-gray-300 flex-shrink-0" style="background-color:${color}"></span>
                  <span class="text-sm font-semibold text-gray-800 truncate">${escapeHtml(t.type || 'Unknown')}</span>
                  ${isActive ? '<span class="text-xs font-bold text-emerald-600 uppercase ml-auto">Active</span>' : ''}
                </div>
                <div class="remaining-bar-track w-full h-1.5 rounded-full bg-gray-200 border border-gray-300">
                  <div class="remaining-bar-fill h-1.5 rounded-full transition-all duration-300" style="width:${t.remaining || 0}%;background-color:${color}"></div>
                </div>
                <div class="flex justify-between text-xs text-gray-500 mt-1">
                  <span>AMS ${t.ams_id + 1} · Slot ${t.tray_id + 1}</span>
                  <span>${t.remaining || 0}%</span>
                </div>
              </div>`;
          })
          .join('');
      } else {
        amsSec.classList.add('hidden');
      }
    }
  }

  async function reconnectPrinter(printerId) {
    try {
      const res = await fetch('/plugins/bambu/api/printers/' + printerId + '/reconnect', { method: 'POST' });
      const data = await res.json();
      if (data.success && data.snapshot) {
        updatePrinterCard(data.snapshot);
      }
    } catch (err) {
      console.error(err);
    }
  }

  function getActiveTrayInfo(st) {
    const activeIdx = st.active_tray;
    if (activeIdx == null) return null;
    const ams = st.ams || [];
    for (const t of ams) {
      if (t.tray_id === activeIdx) {
        return { color: normalizeColor(t.color), type: t.type || 'Filament', slot: t.tray_id + 1, remaining: t.remaining };
      }
    }
    return { color: '#d1d5db', type: 'Slot #' + (activeIdx + 1), slot: activeIdx + 1, remaining: null };
  }

  function fillThermometer(card, current, target) {
    const fill = card.querySelector('.thermometer-fill');
    if (!fill) return;
    if (target && target > 0 && current != null) {
      const pct = Math.min(100, Math.round((current / target) * 100));
      fill.style.width = pct + '%';
    } else {
      fill.style.width = '0%';
    }
  }

  function normalizeColor(c) {
    if (!c || c === '') return '#808080';
    if (c.startsWith('#')) return c;
    return '#' + c;
  }

  function textColorForBg(hex) {
    if (!hex || hex === '#00000000') return '#374151';
    const c = hex.charAt(0) === '#' ? hex.substring(1) : hex;
    const r = parseInt(c.substring(0, 2), 16);
    const g = parseInt(c.substring(2, 4), 16);
    const b = parseInt(c.substring(4, 6), 16);
    if (isNaN(r) || isNaN(g) || isNaN(b)) return '#000';
    return (0.299 * r + 0.587 * g + 0.114 * b) / 255 > 0.5 ? '#1a1a1a' : '#fff';
  }

  function formatTemp(t) {
    if (t == null) return '--°C';
    return Math.round(t) + '°C';
  }

  function formatTime(seconds) {
    if (!seconds || seconds === 'Unknown' || seconds === 0) return '-';
    const s = typeof seconds === 'string' ? parseInt(seconds) : seconds;
    if (isNaN(s)) return '-';
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    return h > 0 ? h + 'h ' + m + 'm' : m + 'm';
  }

  function humidityLabel(val) {
    const map = { 5: 'A', 4: 'B', 3: 'C', 2: 'D', 1: 'E' };
    return val != null ? map[val] || val + '?' : '--';
  }

  function escapeHtml(str) {
    if (!str) return '';
    const d = document.createElement('div');
    d.textContent = str;
    return d.innerHTML;
  }

  const ids = getPrinterIds();
  if (ids.length > 0) {
    refreshAll();
    pollInterval = setInterval(refreshAll, 5000);
  }

  window.manualRefresh = manualRefresh;
  window.reconnectPrinter = reconnectPrinter;
})();
