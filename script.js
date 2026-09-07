const alertaGrande = document.getElementById("alertaGrande");
const litrosText = document.getElementById("litrosText");
const percent = document.getElementById("percent");
const statusText = document.getElementById("status");
const water = document.getElementById("water");
const MAX_QUEDA_POR_LEITURA = 60; // ajuste (ex: 50L)


// ===== NOVO: GASTO HOJE =====
let menorNivelHoje = null;
let ultimoLitros = null;
let tempoUltimaLeitura = Date.now();
let consumoHoje = 0;
const LIMIAR = 0.5;

// ===== NOVO: GRÁFICO =====
let grafico = null;

// --- CONFIG ---
const MODO_SIMULACAO = false;
const R_BASE = 58.0;
const R_TOPO = 75.5;
const H_UTIL = 75.0;

const AREA_UTIL = 49; 

let notificacao30Enviada = false;
let notificacao38Enviada = false;
let notificacao81Enviada = false;

const firebaseConfig = {
  apiKey: "AIzaSyCQipZjlc86GtZGx3_aoyCT-jDrZ1oYyYM",
  authDomain: "monitor-caixa-agua-ff63a.firebaseapp.com",
  databaseURL: "https://monitor-caixa-agua-ff63a-default-rtdb.firebaseio.com",
  projectId: "monitor-caixa-agua-ff63a",
  storageBucket: "monitor-caixa-agua-ff63a.firebasestorage.app",
  messagingSenderId: "176234978770",
  appId: "1:176234978770:web:e193d8242f4f111abd3c0b"
};

if (!firebase.apps.length) firebase.initializeApp(firebaseConfig);
const database = firebase.database();

let nivelDestino = 0;
let nivelAtualAnim = 0;

// ===== DATA HOJE =====
function getDataHoje() {
  const hoje = new Date();

  const ano = hoje.getFullYear();
  const mes = String(hoje.getMonth() + 1).padStart(2, "0");
  const dia = String(hoje.getDate()).padStart(2, "0");

  return `${ano}-${mes}-${dia}`;
}

const dataHoje = getDataHoje();
let dataAtual = dataHoje;
let consumoRef = database.ref("consumo/" + dataHoje);

// ===== CARREGAR CONSUMO SALVO =====
consumoRef.once("value").then(snapshot => {
  if (snapshot.exists()) {
    const data = snapshot.val();
    consumoHoje = data.total || 0;
    menorNivelHoje = data.menorNivel || null;
    atualizarConsumoHoje(consumoHoje);
  }
});

// ===== ANIMAÇÃO =====
function animar() {
  nivelAtualAnim += (nivelDestino - nivelAtualAnim) * 0.1;
  if(water) water.style.height = (nivelAtualAnim * AREA_UTIL / 100) + "%";
  if(percent) percent.innerText = nivelAtualAnim.toFixed(1) + "%";
  requestAnimationFrame(animar);
}
requestAnimationFrame(animar);

// ===== INTERFACE =====
function atualizarInterface(nivel, litros) {
  nivelDestino = nivel;

  if (litros !== undefined && litrosText) {
      litrosText.innerText = Math.round(litros) + " L";
  }

  // ===== PROCESSA CONSUMO AQUI =====
  processarConsumo(litros);

  if (nivel <= 30) { 
    if(water) water.style.background = "linear-gradient(to top, #ff0000, #ff4d4d)";
    statusText.innerText = "MUITO CRÍTICO";
    alertaGrande.innerText = "🚨 PERIGO: CAIXA VAZIA!";
    alertaGrande.style.display = "block";

    if (!notificacao30Enviada) {
      enviarTelegram("🚨 Atenção: Nível Muito Crítico! " + nivel.toFixed(1) + "% - Não abra os Registros de água.");
      avisarAlexa("caixamuitocritica"); 
      notificacao30Enviada = true;
      notificacao38Enviada = false;
    }
  } 
  else if (nivel <= 38) { 
    if(water) water.style.background = "linear-gradient(to top, #ff7b00, #ffc107)";
    statusText.innerText = "LIGAR BOMBA";
    alertaGrande.innerText = "⚠ ABAIXO DE 38%";
    alertaGrande.style.display = "block";

    if (!notificacao38Enviada) {
      enviarTelegram("⚠ Atenção: Nível em 38%. Ligue a bomba urgente!");
      avisarAlexa("ligarbomba"); 
      notificacao38Enviada = true;
      notificacao30Enviada = false;
      notificacao81Enviada = false;
    }
  } 
  else if (nivel >= 81) { 
    if(water) water.style.background = "linear-gradient(to top, #0077ff, #00c6ff)";
    statusText.innerText = "Caixa Cheia";
    alertaGrande.innerText = "⛔ DESLIGAR BOMBA";
    alertaGrande.style.display = "block";

    if (!notificacao81Enviada) {
      enviarTelegram("🔔 ATENÇÃO: Caixa d'Água Encheu! " + nivel.toFixed(1) + "% - Desligue a Bomba.");
      avisarAlexa("caixacheia"); 
      notificacao81Enviada = true;
      notificacao38Enviada = false;
    }
  } 
  else {
    if(water) water.style.background = "linear-gradient(to top, #0077ff, #00c6ff)";
    statusText.innerText = "Normal";
    alertaGrande.style.display = "none";

    if (nivel > 41 && nivel < 75) {
        notificacao30Enviada = false;
        notificacao38Enviada = false;
        notificacao81Enviada = false;
    }
  }
}

// ===== CONSUMO INTELIGENTE =====
function processarConsumo(litrosAtual) {
  if (litrosAtual === undefined || litrosAtual === null) return;

  if (litrosAtual > 1100 || litrosAtual < 0) return;


  
const agora = Date.now();

if (ultimoLitros !== null) {
  const diferenca = Math.abs(litrosAtual - ultimoLitros);

  // variação absurda em pouco tempo = erro
  if (diferenca > 100 && (agora - tempoUltimaLeitura < 5000)) {
    console.warn("⚠️ Leitura descartada (instável)");
    return;
  }
}

ultimoLitros = litrosAtual;
tempoUltimaLeitura = agora;


  

  

  if (menorNivelHoje === null) {
    menorNivelHoje = litrosAtual;
    return;
  }

  if (litrosAtual < menorNivelHoje) {

    const diferenca = menorNivelHoje - litrosAtual;

    // 🚨 BLOQUEIA QUEDA IRREAL
    if (diferenca > MAX_QUEDA_POR_LEITURA) {
      console.warn("⚠️ Queda ignorada (possível erro sensor):", diferenca);
      return;
    }

    // só soma se for consumo real
    if (diferenca > LIMIAR) {
      consumoHoje += diferenca;
      atualizarConsumoHoje(consumoHoje);
    }

    menorNivelHoje = litrosAtual;

    consumoRef.set({
      total: parseFloat(consumoHoje.toFixed(2)),
      menorNivel: menorNivelHoje,
      ultimaAtualizacao: Date.now()
    });
  }
}

// ===== ATUALIZA UI =====
function atualizarConsumoHoje(valor) {
  const el = document.getElementById("gastoHoje");
  if (el) el.innerText = valor.toFixed(1) + " L";
}

// ===== GRÁFICO =====
function iniciarGrafico() {
  const ctx = document.getElementById("graficoConsumo")?.getContext("2d");
  if (!ctx) return;

  grafico = new Chart(ctx, {
    type: "line",
    data: {
      labels: [],
      datasets: [{
        label: "Litros por dia",
        data: [],
        tension: 0.3
      }]
    },
    options: {
      responsive: true,
      animation: false
    }
  });
}

function escutarGraficoTempoReal() {
  database.ref("consumo").on("value", snapshot => {
    const dados = snapshot.val();
    if (!dados || !grafico) return;

    const datas = [];
    const valores = [];

    Object.keys(dados).sort().forEach(d => {
      datas.push(d.split("-").reverse().join("/"));
      valores.push(dados[d].total || 0);
    });

    grafico.data.labels = datas;
    grafico.data.datasets[0].data = valores;
    grafico.update();
  });
}

// ===== ATIVAÇÃO DE ACORDO COM O MODO (SIMULAÇÃO OU REAL) =====
if (MODO_SIMULACAO) {
  console.log("🤖 Modo Simulação Ativo!");
  let subindo = true;
  let simNivel = 50;

  setInterval(() => {
    if (subindo) simNivel += 0.5;
    else simNivel -= 0.5;

    if (simNivel >= 100) subindo = false;
    if (simNivel <= 5) subindo = true;

    // FÓRMULA TRONCO DE CONE INTEGRADA
    const h = (simNivel / 100) * H_UTIL;
    const raioAt = R_BASE + (R_TOPO - R_BASE) * (h / H_UTIL);
    const vol_cm3 = (3.14159 * h / 3.0) * (Math.pow(raioAt, 2) + (raioAt * R_BASE) + Math.pow(R_BASE, 2));
    const litrosSimulados = vol_cm3 / 1000.0;

    atualizarInterface(simNivel, litrosSimulados);
  }, 500); 
} else {
  console.log("📡 Modo Produção (Firebase Real) Ativo!");
  database.ref('/').on('value', (snapshot) => {
    const data = snapshot.val();
    if (data && data.nivel !== undefined) {
      atualizarInterface(parseFloat(data.nivel), parseFloat(data.litros));
    }
  });
}

// ===== INICIAR GRÁFICO =====
iniciarGrafico();
escutarGraficoTempoReal();

// ===== CONTROLE DA AUTOMACAO VIA FIREBASE =====
function comandoBomba(comando) {
  if (comando !== 'ON' && comando !== 'OFF') return;
  const msg = comando === 'ON' ? 'Ligar bomba' : 'Desligar bomba';
  if (!confirm(msg + '?')) return;

  database.ref('comandos/bomba').set(comando).then(() => {
    console.log('Comando enviado ao ESP32:', comando);
  }).catch(err => {
    alert('Erro ao enviar comando: ' + err.message);
  });
}

function comandoModo(modo) {
  if (modo !== 'AUTO' && modo !== 'MANUAL') return;
  database.ref('comandos/modo').set(modo).then(() => {
    console.log('Modo enviado ao ESP32:', modo);
  }).catch(err => {
    alert('Erro ao enviar modo: ' + err.message);
  });
}

function atualizarTexto(id, valor) {
  const el = document.getElementById(id);
  if (el) el.innerText = valor;
}

function atualizarBoia(id, valor) {
  atualizarTexto(id, valor ? '🟢' : '⚪');
}

function escutarAutomacao() {
  database.ref('status_bomba').on('value', s => {
    const ligada = String(s.val()).toUpperCase() === 'ON' || s.val() === true;
    atualizarTexto('statusBomba', ligada ? '🟢 LIGADA' : '🔴 DESLIGADA');
  });

  database.ref('modo_manual').on('value', s => {
    atualizarTexto('modoAutomacao', s.val() ? '🖐️ MANUAL' : '🤖 AUTOMÁTICO');
  });

  database.ref('sonoff_online').on('value', s => {
    atualizarTexto('statusSonoff', s.val() ? '🟢 ONLINE' : '🔴 OFFLINE');
  });

  database.ref('dispositivo/rssi').on('value', s => {
    const rssi = s.val();
    atualizarTexto('wifiRssi', rssi !== null && rssi !== undefined ? rssi + ' dBm' : '-');
  });

  ['20','40','60','80','95'].forEach(n => {
    database.ref('boias/' + n).on('value', s => atualizarBoia('boia' + n, !!s.val()));
  });

  database.ref('sistema_seguro').on('value', s => {
    atualizarTexto('sistemaSeguro', s.val() ? '🟢 SEGURO' : '🔴 FALHA');
  });

  database.ref('ultimo_evento').on('value', s => {
    atualizarTexto('ultimoEvento', s.val() || '-');
  });
}

escutarAutomacao();

setInterval(() => {
  const novaData = getDataHoje();

  if (novaData !== dataAtual && document.visibilityState === "visible") {
    console.log("🔄 Novo dia detectado!");

    dataAtual = novaData;

    // 🧹 ZERA VARIÁVEIS
    menorNivelHoje = null;
    consumoHoje = 0;
    atualizarConsumoHoje(0);

    // 🔄 ATUALIZA FIREBASE
    consumoRef = database.ref("consumo/" + novaData);

    // 🔄 CARREGA DADOS DO NOVO DIA (se existir)
    consumoRef.once("value").then(snapshot => {
      if (snapshot.exists()) {
        const data = snapshot.val();

        consumoHoje = data.total || 0;
        menorNivelHoje = data.menorNivel || null;

        atualizarConsumoHoje(consumoHoje);
      }
    });
  }
}, 60000); // verifica a cada 1 minuto
