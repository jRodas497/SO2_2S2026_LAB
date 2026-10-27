/* so2monitor · dashboard.
 *
 * Todo texto que viene del sistema (nombres de archivo, lineas de log, nombres
 * de proceso) se inserta con textContent: un archivo llamado "<img onerror=...>"
 * no debe poder ejecutar codigo en el navegador del administrador.
 * Los permisos reales los aplica el daemon; aqui solo se oculta lo que el rol no usa.
 */
'use strict';

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];

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

const MAX_HIST = 120;
const MAX_ALERTAS = 300;

const S = {
  usuario: '', rol: '',
  flags: { monitoreo: true, procesos: true, archivos: false, sensor_disponible: true, ruta: '' },
  metricas: null, historial: [],
  procesos: { resumen: {}, top: [] },
  alertas: [], filtro: '',
  archivos: new Map(), amenazas: [], logwatches: [], firmas: 0,
};
let stream = null;
let charts = {};
let ultimoChequeo = 0;

/* ------------------------------------------------------------- utilidades */

function fmtKB(kb) {
  if (kb == null || isNaN(kb)) return '–';
  if (kb >= 1048576) return (kb / 1048576).toFixed(2) + ' GB';
  if (kb >= 1024) return (kb / 1024).toFixed(1) + ' MB';
  return Math.round(kb) + ' KB';
}
const mb = kb => +(kb / 1024).toFixed(1);
const horaDe = ts => (ts || '').slice(11, 19);
const esAdmin = () => S.rol === 'admin';

function toast(msg, tipo = '') {
  const t = el('div', { class: 'toast ' + tipo, text: msg });
  $('#toasts').append(t);
  setTimeout(() => t.remove(), tipo === 'error' ? 6500 : 4200);
}

async function api(path, method = 'GET', body) {
  const opt = { method, credentials: 'same-origin', headers: {} };
  if (body !== undefined) {
    opt.body = typeof body === 'string' ? body : JSON.stringify(body);
    opt.headers['Content-Type'] = 'application/json';
  }
  let res;
  try { res = await fetch(path, opt); } catch { throw new Error('Sin conexión con el daemon'); }
  let data = null;
  try { data = await res.json(); } catch { /* sin cuerpo JSON */ }
  if (res.status === 401 && path !== '/api/login') {
    mostrarLogin('La sesión expiró. Inicie sesión de nuevo.');
    throw new Error('No autenticado');
  }
  if (!res.ok || (data && data.ok === false)) throw new Error((data && data.error) || 'Error ' + res.status);
  return data;
}

function confirmar(titulo, mensaje, textoOk = 'Confirmar') {
  const dlg = $('#dlg');
  $('#dlg-titulo').textContent = titulo;
  $('#dlg-msg').textContent = mensaje;
  $('#dlg-ok').textContent = textoOk;
  return new Promise(res => {
    dlg.addEventListener('close', () => res(dlg.returnValue === 'ok'), { once: true });
    dlg.returnValue = 'cancel';
    dlg.showModal();
  });
}

/* ---------------------------------------------------------------- vistas */

function mostrarLogin(msg) {
  if (stream) { stream.close(); stream = null; }
  Object.values(charts).forEach(c => c.destroy());
  charts = {};
  S.rol = '';
  document.body.dataset.rol = '';
  $('#vista-app').hidden = true;
  $('#vista-login').hidden = false;
  const m = $('#login-msg');
  m.hidden = !msg;
  m.textContent = msg || '';
  $('#login-password').value = '';
  $('#login-usuario').focus();
}

async function iniciar() {
  const st = await api('/api/state');
  S.usuario = st.usuario;
  S.rol = st.rol;
  S.flags = st.flags;
  S.metricas = st.metricas.disponible ? st.metricas : null;
  S.historial = st.historial;
  S.procesos = st.procesos;
  S.alertas = st.alertas;
  S.archivos = new Map(st.archivos.map(f => [f.archivo, f]));
  S.amenazas = st.amenazas;
  S.logwatches = st.logwatches;
  S.firmas = st.firmas;

  document.body.dataset.rol = S.rol;
  $('#vista-login').hidden = true;
  $('#vista-app').hidden = false;
  $('#usuario-actual').textContent = S.usuario;
  const rb = $('#rol-badge');
  rb.textContent = S.rol === 'admin' ? 'administrador' : 'usuario';
  rb.className = 'badge ' + S.rol;

  crearGraficas();
  renderTodo();
  conectarStream();
}

/* -------------------------------------------------------------- gráficas */

function crearGraficas() {
  Object.values(charts).forEach(c => c.destroy());
  const css = getComputedStyle(document.documentElement);
  const col = n => css.getPropertyValue(n).trim();
  const texto = col('--texto-2'), rejilla = col('--borde');
  Chart.defaults.color = texto;
  Chart.defaults.borderColor = rejilla;
  Chart.defaults.font.family = getComputedStyle(document.body).fontFamily;

  const comun = { responsive: true, maintainAspectRatio: false, animation: false };
  const lineaX = { ticks: { maxTicksLimit: 8, maxRotation: 0 }, grid: { display: false } };
  const dona = (id, etiquetas, colores) => new Chart($(id), {
    type: 'doughnut',
    data: { labels: etiquetas, datasets: [{ data: [0, 0, 0].slice(0, etiquetas.length), backgroundColor: colores, borderColor: col('--panel-2'), borderWidth: 3 }] },
    options: { ...comun, cutout: '62%', plugins: { legend: { position: 'bottom' }, tooltip: { callbacks: { label: c => ` ${c.label}: ${fmtKB(c.parsed * 1024)}` } } } },
  });

  charts.paginas = dona('#g-paginas', ['Activas', 'Inactivas'], [col('--acento'), col('--low')]);
  charts.distrib = dona('#g-distribucion', ['Libre', 'Caché y buffers', 'Utilizada'], [col('--ok'), col('--low'), col('--medium')]);
  charts.tiempo = new Chart($('#g-tiempo'), {
    type: 'line',
    data: { labels: [], datasets: [
      { label: 'Memoria usada', data: [], borderColor: col('--medium'), backgroundColor: col('--medium') + '22', fill: true, tension: .25, pointRadius: 0, borderWidth: 2 },
      { label: 'Swap usada', data: [], borderColor: col('--high'), tension: .25, pointRadius: 0, borderWidth: 2 },
    ] },
    options: { ...comun, interaction: { mode: 'index', intersect: false }, scales: { x: lineaX, y: { beginAtZero: true, title: { display: true, text: 'MB' } } }, plugins: { legend: { position: 'bottom' } } },
  });
  charts.fallos = new Chart($('#g-fallos'), {
    type: 'line',
    data: { labels: [], datasets: [
      { label: 'Menores (pgfault)', data: [], borderColor: col('--low'), tension: .25, pointRadius: 0, borderWidth: 2, yAxisID: 'y' },
      { label: 'Mayores (pgmajfault)', data: [], borderColor: col('--high'), tension: .25, pointRadius: 0, borderWidth: 2, yAxisID: 'y1' },
    ] },
    options: { ...comun, interaction: { mode: 'index', intersect: false }, scales: {
      x: lineaX,
      y: { beginAtZero: true, position: 'left', title: { display: true, text: 'menores' } },
      y1: { beginAtZero: true, position: 'right', grid: { drawOnChartArea: false }, title: { display: true, text: 'mayores' } },
    }, plugins: { legend: { position: 'bottom' } } },
  });
}

function actualizarGraficas() {
  if (!charts.tiempo) return;
  const m = S.metricas;
  if (m) {
    charts.paginas.data.datasets[0].data = [mb(m.activas_kb), mb(m.inactivas_kb)];
    charts.distrib.data.datasets[0].data = [mb(m.mem_libre_kb), mb(m.cache_kb + m.buffers_kb), mb(m.mem_usada_kb)];
  }
  const h = S.historial;
  const etq = h.map(x => horaDe(x.timestamp));
  charts.tiempo.data.labels = etq;
  charts.tiempo.data.datasets[0].data = h.map(x => mb(x.mem_usada_kb));
  charts.tiempo.data.datasets[1].data = h.map(x => mb(x.swap_usada_kb));
  charts.fallos.data.labels = etq;
  charts.fallos.data.datasets[0].data = h.map(x => x.pgfault_delta);
  charts.fallos.data.datasets[1].data = h.map(x => x.pgmajfault_delta);
  Object.values(charts).forEach(c => c.update('none'));
}

/* ------------------------------------------------------------- renderizado */

function renderTodo() {
  renderFlags();
  renderTiles();
  actualizarGraficas();
  renderProcesos();
  renderArchivos();
  renderAmenazas();
  renderLogwatch();
  renderAlertas();
}

function renderFlags() {
  $('#sw-monitoreo').checked = S.flags.monitoreo;
  $('#sw-procesos').checked = S.flags.procesos;
  $('#sw-archivos').checked = S.flags.archivos;
  $('#aviso-monitoreo').hidden = S.flags.monitoreo;
  $('#aviso-sensor').hidden = S.flags.sensor_disponible;
  $('#sw-archivos').disabled = !S.flags.sensor_disponible;
  const ri = $('#ruta-input');
  if (document.activeElement !== ri) ri.value = S.flags.ruta || '';
}

function renderTiles() {
  const m = S.metricas;
  if (m) {
    const pm = 100 * m.mem_usada_kb / m.mem_total_kb;
    $('#t-mem').textContent = pm.toFixed(1) + ' %';
    $('#t-mem-sub').textContent = `${fmtKB(m.mem_usada_kb)} de ${fmtKB(m.mem_total_kb)}`;
    const ps = m.swap_total_kb ? 100 * m.swap_usada_kb / m.swap_total_kb : 0;
    $('#t-swap').textContent = ps.toFixed(1) + ' %';
    $('#t-swap-sub').textContent = `${fmtKB(m.swap_usada_kb)} de ${fmtKB(m.swap_total_kb)}`;
  }
  const r = S.procesos.resumen || {};
  if (S.procesos.disponible) {
    $('#t-proc').textContent = r.total;
    $('#t-proc-sub').textContent = `${r.running} en ejecución · ${r.sleeping} durmiendo · ${r.zombie} zombie`;
  }
  $('#t-amen').textContent = S.amenazas.length;
  $('#t-amen-sub').textContent = `${S.firmas} firmas cargadas`;
  $('#firmas-info').textContent = `${S.firmas} firmas en la base de datos`;
}

function filaVacia(cols, txt) {
  return el('tr', {}, el('td', { class: 'vacio', colspan: cols, text: txt }));
}

function renderProcesos() {
  const tb = $('#tb-procesos');
  tb.replaceChildren();
  const top = S.procesos.top || [];
  if (!top.length) { tb.append(filaVacia(6, S.flags.procesos ? 'Esperando datos…' : 'El escaneo de procesos está desactivado')); return; }
  for (const p of top) {
    tb.append(el('tr', {},
      el('td', { class: 'num', text: p.pid }),
      el('td', { text: p.nombre }),
      el('td', { text: p.estado_texto, title: 'Código ' + p.estado }),
      el('td', { class: 'num' }, el('span', { class: 'barra-mem' }, el('i', { style: `width:${Math.min(100, p.porcentaje * 4)}%` })), p.porcentaje.toFixed(2) + ' %'),
      el('td', { class: 'num', text: fmtKB(p.rss_kb) }),
      el('td', { class: 'solo-admin' }, el('button', { class: 'btn btn-mini btn-peligro', type: 'button', onclick: () => matarProceso(p.pid, p.nombre) }, 'Matar')),
    ));
  }
}

function badgeEstado(e) {
  const txt = { benigno: 'Benigno', infectado: 'Infectado', cuarentena: 'En cuarentena' }[e] || e;
  return el('span', { class: 'estado ' + e, text: txt });
}

function renderArchivos() {
  const tb = $('#tb-archivos');
  tb.replaceChildren();
  const lista = [...S.archivos.values()].sort((a, b) => b.analizado.localeCompare(a.analizado));
  if (!lista.length) { tb.append(filaVacia(5, 'Todavía no se ha analizado ningún archivo')); return; }
  for (const f of lista) {
    tb.append(el('tr', {},
      el('td', { class: 'ruta', text: f.archivo }),
      el('td', {}, badgeEstado(f.estado)),
      el('td', { text: f.ultima_modificacion }),
      el('td', { text: f.amenaza || '–' }),
      el('td', { class: 'solo-admin' }, f.estado === 'cuarentena' ? '–' :
        el('button', { class: 'btn btn-mini', type: 'button', onclick: () => ponerCuarentena(f.archivo) }, 'Cuarentena')),
    ));
  }
}

function renderAmenazas() {
  const tb = $('#tb-amenazas');
  tb.replaceChildren();
  if (!S.amenazas.length) { tb.append(filaVacia(4, 'No se han detectado amenazas')); return; }
  for (const t of S.amenazas) {
    tb.append(el('tr', {},
      el('td', {}, el('strong', { text: t.amenaza })),
      el('td', { class: 'ruta', text: t.archivo }),
      el('td', { text: t.descripcion }),
      el('td', { text: t.timestamp }),
    ));
  }
}

function renderLogwatch() {
  const tb = $('#tb-logwatch');
  tb.replaceChildren();
  if (!S.logwatches.length) { tb.append(filaVacia(6, 'No hay monitoreos de logs activos')); return; }
  for (const l of S.logwatches) {
    tb.append(el('tr', {},
      el('td', { class: 'num', text: l.id }),
      el('td', {}, el('code', { text: l.palabra })),
      el('td', { class: 'ruta', text: l.central }),
      el('td', { class: 'ruta' }, l.rutas.map(r => el('div', { text: r }))),
      el('td', { text: `${l.iniciado} (${l.por})` }),
      el('td', { class: 'solo-admin' }, el('button', { class: 'btn btn-mini btn-peligro', type: 'button', onclick: () => finalizarLogwatch(l.id) }, 'Finalizar')),
    ));
  }
}

const SEV_TXT = { LOW: 'Informativo', MEDIUM: 'Advertencia', HIGH: 'Crítico' };

function nodoAlerta(a, nueva = false) {
  const s = 'sev-' + a.severidad.toLowerCase();
  return el('li', { class: `alerta ${s}${nueva ? ' nueva' : ''}` },
    el('div', { class: 'alerta-cab' },
      el('span', { class: 'sev ' + s, text: a.severidad, title: SEV_TXT[a.severidad] }),
      el('span', { class: 'tipo', text: a.tipo })),
    el('div', { class: 'alerta-titulo', text: a.titulo }),
    el('div', { class: 'alerta-desc', text: a.descripcion }),
    el('time', { class: 'alerta-hora', text: a.timestamp }));
}

function renderAlertas() {
  const ul = $('#lista-alertas');
  ul.replaceChildren();
  const vis = S.alertas.filter(a => !S.filtro || a.severidad === S.filtro);
  $('#alertas-n').textContent = vis.length;
  if (!vis.length) { ul.append(el('li', { class: 'muted', text: 'Sin alertas', style: 'padding:12px' })); return; }
  for (const a of vis) ul.append(nodoAlerta(a));   // ya vienen de la mas reciente a la mas antigua
}

/* ------------------------------------------------------------ tiempo real */

function estadoConexion(on) {
  $('#conexion').className = 'conexion ' + (on ? 'on' : 'off');
  $('#conexion-txt').textContent = on ? 'En vivo' : 'Reconectando…';
}

function conectarStream() {
  if (stream) stream.close();
  stream = new EventSource('/api/stream');
  stream.onopen = () => estadoConexion(true);
  stream.onerror = async () => {
    estadoConexion(false);
    if (Date.now() - ultimoChequeo < 4000) return;
    ultimoChequeo = Date.now();
    try { await api('/api/me'); } catch { /* si fue 401, api() ya mostro el login */ }
  };
  const on = (ev, fn) => stream.addEventListener(ev, e => { try { fn(JSON.parse(e.data)); } catch (x) { console.error(ev, x); } });

  on('metrics', m => {
    S.metricas = m;
    S.historial.push(m);
    if (S.historial.length > MAX_HIST) S.historial.shift();
    renderTiles();
    actualizarGraficas();
  });
  on('procs', p => { S.procesos = p; renderTiles(); renderProcesos(); });
  on('alert', a => {
    S.alertas.unshift(a);
    if (S.alertas.length > MAX_ALERTAS) S.alertas.pop();
    if (!S.filtro || S.filtro === a.severidad) {
      const ul = $('#lista-alertas');
      if (!ul.querySelector('.alerta')) ul.replaceChildren();
      ul.prepend(nodoAlerta(a, true));
      $('#alertas-n').textContent = S.alertas.filter(x => !S.filtro || x.severidad === S.filtro).length;
    }
    if (a.severidad === 'HIGH') toast(`${a.titulo}: ${a.descripcion}`, 'alta');
  });
  on('file', f => { S.archivos.set(f.archivo, f); renderArchivos(); if (!$('#caja-cuarentena').hidden) cargarCuarentena(); });
  on('threat', t => { S.amenazas.unshift(t); renderTiles(); renderAmenazas(); });
  on('flags', f => { S.flags = f; renderFlags(); });
  on('logwatches', l => { S.logwatches = l; renderLogwatch(); });
}

/* ---------------------------------------------------------------- acciones */

async function accion(fn, okMsg) {
  try {
    const r = await fn();
    if (okMsg) toast(okMsg);
    return r;
  } catch (e) {
    if (e.message !== 'No autenticado') toast(e.message, 'error');
    return null;
  }
}

function enlazarInterruptor(id, ruta) {
  const sw = $(id);
  sw.addEventListener('change', async () => {
    const quiere = sw.checked;
    const r = await accion(() => api(ruta, 'POST', { activo: quiere }));
    if (r === null) sw.checked = !quiere;
  });
}

async function matarProceso(pid, nombre) {
  const ok = await confirmar('¿Terminar el proceso?',
    `Se enviará SIGKILL al proceso ${nombre} (PID ${pid}). Terminar procesos del sistema puede afectar su estabilidad.`, 'Matar proceso');
  if (!ok) return;
  await accion(() => api('/api/kill', 'POST', { pid }), `Proceso ${pid} terminado`);
}

async function ponerCuarentena(ruta) {
  const ok = await confirmar('¿Poner en cuarentena?',
    `El archivo ${ruta} quedará con permisos 0000: no podrá leerse, escribirse ni ejecutarse.`, 'Poner en cuarentena');
  if (!ok) return;
  await accion(() => api('/api/quarantine', 'POST', { ruta }), 'Archivo puesto en cuarentena');
}

async function finalizarLogwatch(id) {
  const ok = await confirmar('¿Finalizar el monitoreo?', `Se detendrá el monitoreo de logs #${id} (sys_stop_log_watch).`, 'Finalizar');
  if (!ok) return;
  await accion(() => api('/api/logwatch/stop', 'POST', { id }), `Monitoreo ${id} finalizado`);
}

async function cargarCuarentena() {
  const lista = await accion(() => api('/api/quarantine'));
  if (!lista) return;
  const tb = $('#tb-cuarentena');
  tb.replaceChildren();
  if (!lista.length) { tb.append(filaVacia(3, 'No hay archivos en cuarentena')); return; }
  for (const f of lista) {
    tb.append(el('tr', {}, el('td', { class: 'ruta', text: f.archivo }), el('td', { text: f.amenaza || 'Manual' }), el('td', { class: 'ruta', text: f.sha256.slice(0, 16) + '…', title: f.sha256 })));
  }
}

function enlazarFormularios() {
  $('#form-login').addEventListener('submit', async e => {
    e.preventDefault();
    const btn = $('#btn-login');
    btn.disabled = true;
    try {
      await api('/api/login', 'POST', { usuario: $('#login-usuario').value.trim(), password: $('#login-password').value });
      await iniciar();
    } catch (x) {
      mostrarLogin(x.message);
    } finally {
      btn.disabled = false;
    }
  });

  $('#btn-salir').addEventListener('click', async () => {
    try { await api('/api/logout', 'POST'); } catch { /* da igual */ }
    mostrarLogin();
  });

  enlazarInterruptor('#sw-monitoreo', '/api/monitoring');
  enlazarInterruptor('#sw-procesos', '/api/procscan');
  enlazarInterruptor('#sw-archivos', '/api/filescan');

  $('#form-ruta').addEventListener('submit', async e => {
    e.preventDefault();
    await accion(() => api('/api/path', 'POST', { ruta: $('#ruta-input').value.trim() }), 'Ruta actualizada');
  });

  $('#btn-ver-cuarentena').addEventListener('click', () => {
    const caja = $('#caja-cuarentena');
    caja.hidden = !caja.hidden;
    $('#btn-ver-cuarentena').textContent = caja.hidden ? 'Ver cuarentena' : 'Ocultar cuarentena';
    if (!caja.hidden) cargarCuarentena();
  });

  $('#form-pid').addEventListener('submit', async e => {
    e.preventDefault();
    const pid = parseInt($('#pid-input').value, 10);
    const caja = $('#info-pid');
    const info = await accion(() => api('/api/process?pid=' + pid));
    if (!info) { caja.hidden = true; return; }
    caja.replaceChildren(
      ...[['PID', info.pid], ['Nombre', info.nombre], ['Tiempo de ejecución', info.tiempo_ejecucion_s.toFixed(1) + ' s'], ['Memoria', fmtKB(info.memoria_kb)]]
        .map(([k, v]) => el('dl', {}, el('dt', { text: k }), el('dd', { text: v }))),
      el('button', { class: 'btn btn-peligro', type: 'button', onclick: () => matarProceso(info.pid, info.nombre) }, 'Matar proceso'));
    caja.hidden = false;
  });

  $('#form-blacklist').addEventListener('submit', async e => {
    e.preventDefault();
    const f = $('#bl-archivo').files[0];
    if (!f) return;
    const texto = await f.text();
    try { JSON.parse(texto); } catch { toast('El archivo no es un JSON válido', 'error'); return; }
    const r = await accion(() => api('/api/blacklist', 'POST', texto));
    if (r) {
      S.firmas = r.firmas;
      renderTiles();
      toast(`Base de datos actualizada: ${r.firmas} firmas`);
      $('#form-blacklist').reset();
    }
  });

  $('#form-logwatch').addEventListener('submit', async e => {
    e.preventDefault();
    const rutas = $('#lw-rutas').value.split('\n').map(s => s.trim()).filter(Boolean);
    const r = await accion(() => api('/api/logwatch', 'POST', {
      central: $('#lw-central').value.trim(), palabra: $('#lw-palabra').value.trim(), rutas,
    }));
    if (r) { toast(`Monitoreo #${r.id} iniciado`); $('#lw-palabra').value = ''; $('#lw-rutas').value = ''; }
  });

  $$('.chip').forEach(c => c.addEventListener('click', () => {
    S.filtro = c.dataset.sev;
    $$('.chip').forEach(x => x.classList.toggle('activo', x === c));
    renderAlertas();
  }));
}

/* ------------------------------------------------------------------ inicio */

enlazarFormularios();
(async () => {
  // Primera visita: sin sesion no es un error, simplemente se muestra el login.
  let hayCookie = false;
  try { hayCookie = (await fetch('/api/me', { credentials: 'same-origin' })).ok; } catch { /* daemon caido */ }
  if (hayCookie) {
    try { await iniciar(); } catch { if ($('#vista-app').hidden) mostrarLogin(); }
  } else {
    mostrarLogin();
  }
})();
