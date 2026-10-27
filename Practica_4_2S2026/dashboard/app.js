/* so2monitor · dashboard de la Practica 4.
 *
 * Cinco visualizaciones: pie de memoria, lineas memoria fisica vs. swap, tabla de
 * procesos, panel de eventos de seguridad y consulta por PID.
 *
 * Los datos de memoria y procesos llegan por SSE cada intervalo del daemon. Los eventos
 * de seguridad llegan por SSE en el INSTANTE en que log_watch los notifica por IPC.
 * Todo texto que viene del sistema se inserta con textContent (nunca innerHTML).
 */
'use strict';

const $ = (s, r = document) => r.querySelector(s);

function el(tag, props = {}, ...kids) {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (k === 'class') n.className = v;
    else if (k === 'text') n.textContent = v;
    else if (k.startsWith('on')) n.addEventListener(k.slice(2), v);
    else if (v !== false && v != null) n.setAttribute(k, v === true ? '' : v);
  }
  for (const k of kids.flat()) if (k != null) n.append(k.nodeType ? k : document.createTextNode(String(k)));
  return n;
}

const MAX_EVENTOS = 200;
const S = { datos: null, eventos: [], filtro: '' };
let charts = {};
let stream = null;

function fmtKB(kb) {
  if (kb == null || isNaN(kb)) return '–';
  if (kb >= 1048576) return (kb / 1048576).toFixed(2) + ' GB';
  if (kb >= 1024) return (kb / 1024).toFixed(1) + ' MB';
  return Math.round(kb) + ' KB';
}
const mb = kb => +(kb / 1024).toFixed(1);

function toast(msg, tipo = '') {
  const t = el('div', { class: 'toast ' + tipo, text: msg });
  $('#toasts').append(t);
  setTimeout(() => t.remove(), tipo === 'error' ? 6000 : 4500);
}

/* ---------------------------------------------------------------- gráficas */

function crearGraficas() {
  const css = getComputedStyle(document.documentElement);
  const col = n => css.getPropertyValue(n).trim();
  Chart.defaults.color = col('--texto-2');
  Chart.defaults.borderColor = col('--borde');
  Chart.defaults.font.family = getComputedStyle(document.body).fontFamily;
  const comun = { responsive: true, maintainAspectRatio: false, animation: false };

  charts.pie = new Chart($('#g-pie'), {
    type: 'pie',
    data: { labels: ['Usada', 'Libre', 'Caché'], datasets: [{ data: [0, 0, 0], backgroundColor: [col('--medium'), col('--ok'), col('--low')], borderColor: col('--panel-2'), borderWidth: 3 }] },
    options: { ...comun, plugins: { legend: { position: 'bottom' }, tooltip: { callbacks: { label: c => ` ${c.label}: ${fmtKB(c.parsed * 1024)}` } } } },
  });
  charts.lineas = new Chart($('#g-lineas'), {
    type: 'line',
    data: { labels: [], datasets: [
      { label: 'Memoria física usada', data: [], borderColor: col('--medium'), backgroundColor: col('--medium') + '22', fill: true, tension: .25, pointRadius: 0, borderWidth: 2 },
      { label: 'Swap usada', data: [], borderColor: col('--high'), tension: .25, pointRadius: 0, borderWidth: 2 },
    ] },
    options: { ...comun, interaction: { mode: 'index', intersect: false },
      scales: { x: { ticks: { maxTicksLimit: 8, maxRotation: 0 }, grid: { display: false } }, y: { beginAtZero: true, title: { display: true, text: 'MB' } } },
      plugins: { legend: { position: 'bottom' } } },
  });
}

function renderGraficas(d) {
  charts.pie.data.datasets[0].data = [mb(d.memoria_usada), mb(d.memoria_libre), mb(d.memoria_cache)];
  charts.lineas.data.labels = d.historial.map(h => h.timestamp.slice(11, 19));
  charts.lineas.data.datasets[0].data = d.historial.map(h => mb(h.memoria_usada));
  charts.lineas.data.datasets[1].data = d.historial.map(h => mb(h.swap_usada));
  Object.values(charts).forEach(c => c.update('none'));
}

/* ------------------------------------------------------------- renderizado */

function renderTiles(d) {
  const pm = d.memoria_total ? 100 * d.memoria_usada / d.memoria_total : 0;
  const ps = d.swap_total ? 100 * d.swap_usada / d.swap_total : 0;
  $('#t-mem').textContent = pm.toFixed(1) + ' %';
  $('#t-mem-sub').textContent = `${fmtKB(d.memoria_usada)} de ${fmtKB(d.memoria_total)}`;
  $('#t-libre').textContent = fmtKB(d.memoria_libre);
  $('#t-cache-sub').textContent = `Caché: ${fmtKB(d.memoria_cache)}`;
  $('#t-swap').textContent = ps.toFixed(1) + ' %';
  $('#t-swap-sub').textContent = `${fmtKB(d.swap_usada)} de ${fmtKB(d.swap_total)}`;
  const r = d.resumen_procesos;
  $('#t-proc').textContent = r.total;
  $('#t-proc-sub').textContent = `${r.running} en ejecución · ${r.sleeping} durmiendo · ${r.zombie} zombie`;
  $('#actualizado').textContent = 'Actualizado: ' + d.timestamp;
}

function renderProcesos(d) {
  const tb = $('#tb-procesos');
  const f = S.filtro.trim().toLowerCase();
  const lista = f ? d.procesos.filter(p => p.nombre.toLowerCase().includes(f) || String(p.pid).includes(f)) : d.procesos;
  tb.replaceChildren();
  if (!lista.length) {
    tb.append(el('tr', {}, el('td', { class: 'vacio', colspan: 4, text: d.procesos.length ? 'Ningún proceso coincide con el filtro' : 'Esperando datos…' })));
  }
  for (const p of lista) {
    tb.append(el('tr', {},
      el('td', { class: 'num', text: p.pid }),
      el('td', { text: p.nombre }),
      el('td', { text: p.estado, title: 'Código ' + p.estado_codigo }),
      el('td', { class: 'num', text: fmtKB(p.rss_kb) })));
  }
  $('#proc-cuenta').textContent = `Mostrando ${lista.length} de ${d.procesos.length}`;
  $('#fuente').textContent = 'Origen de la tabla: ' + d.fuente_procesos;
}

function filaEvento(e, nuevo) {
  return el('tr', { class: nuevo ? 'nuevo' : '' },
    el('td', { text: e.timestamp }),
    el('td', { class: 'ruta', text: e.archivo_origen }),
    el('td', {}, el('code', { text: e.palabra_detectada })),
    el('td', { class: 'contenido', text: e.contenido }));
}

function renderEventos() {
  const tb = $('#tb-eventos');
  tb.replaceChildren();
  $('#eventos-n').textContent = S.eventos.length;
  if (!S.eventos.length) {
    tb.append(el('tr', {}, el('td', { class: 'vacio', colspan: 4, text: 'Sin eventos. El daemon está dormido en sys_ipc_channel_receive esperando a log_watch.' })));
    return;
  }
  for (const e of S.eventos) tb.append(filaEvento(e, false));
}

function aplicarDatos(d) {
  S.datos = d;
  renderTiles(d);
  renderGraficas(d);
  renderProcesos(d);
}

/* ------------------------------------------------------------ tiempo real */

function estadoConexion(on) {
  $('#conexion').className = 'conexion ' + (on ? 'on' : 'off');
  $('#conexion-txt').textContent = on ? 'En vivo' : 'Reconectando…';
}

function conectar() {
  if (stream) stream.close();
  stream = new EventSource('/api/stream');
  stream.onopen = () => estadoConexion(true);
  stream.onerror = () => estadoConexion(false);

  stream.addEventListener('datos', ev => {
    try {
      const d = JSON.parse(ev.data);
      // los eventos solo se toman de 'evento' (tiempo real) o de la carga inicial
      aplicarDatos(d);
    } catch (x) { console.error('datos', x); }
  });

  stream.addEventListener('evento', ev => {
    try {
      const e = JSON.parse(ev.data);
      S.eventos.unshift(e);
      if (S.eventos.length > MAX_EVENTOS) S.eventos.pop();
      const tb = $('#tb-eventos');
      if (!tb.querySelector('td:not(.vacio)')) tb.replaceChildren();
      tb.prepend(filaEvento(e, true));
      $('#eventos-n').textContent = S.eventos.length;
      toast(`Evento de seguridad: «${e.palabra_detectada}» en ${e.archivo_origen}`, 'alta');
    } catch (x) { console.error('evento', x); }
  });
}

/* ---------------------------------------------------------------- acciones */

async function consultarPid(pid) {
  const caja = $('#info-pid');
  let res, data = null;
  try {
    res = await fetch('/api/proceso?pid=' + encodeURIComponent(pid));
    data = await res.json();
  } catch {
    toast('Sin conexión con el daemon', 'error');
    return;
  }
  if (!res.ok || data.ok === false) {
    caja.hidden = true;
    toast(data && data.error ? data.error : 'Error ' + res.status, 'error');
    return;
  }
  caja.replaceChildren(...[
    ['PID', data.pid], ['Nombre', data.nombre],
    ['Tiempo de ejecución', data.tiempo_ejecucion_s.toFixed(1) + ' s'], ['Memoria', fmtKB(data.memoria_kb)],
  ].map(([k, v]) => el('dl', {}, el('dt', { text: k }), el('dd', { text: v }))));
  caja.hidden = false;
}

/* ------------------------------------------------------------------ inicio */

async function iniciar() {
  crearGraficas();
  $('#form-pid').addEventListener('submit', e => {
    e.preventDefault();
    consultarPid(parseInt($('#pid-input').value, 10));
  });
  $('#filtro').addEventListener('input', e => {
    S.filtro = e.target.value;
    if (S.datos) renderProcesos(S.datos);
  });

  try {
    const res = await fetch('/api/datos');
    const d = await res.json();
    S.eventos = d.eventos_seguridad || [];
    aplicarDatos(d);
  } catch {
    toast('No se pudo cargar el estado inicial; esperando al daemon…', 'error');
  }
  renderEventos();
  conectar();
}

iniciar();
