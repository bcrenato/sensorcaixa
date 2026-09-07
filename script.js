const alertaGrande=document.getElementById('alertaGrande');
const litrosText=document.getElementById('litrosText');
const percent=document.getElementById('percent');
const statusText=document.getElementById('status');
const water=document.getElementById('water');
const AREA_UTIL=49;
const MODO_SIMULACAO=false;

const firebaseConfig={
  apiKey:'SUA_API_KEY_FIREBASE',
  authDomain:'monitor-caixa-agua-ff63a.firebaseapp.com',
  databaseURL:'https://monitor-caixa-agua-ff63a-default-rtdb.firebaseio.com',
  projectId:'monitor-caixa-agua-ff63a',
  storageBucket:'monitor-caixa-agua-ff63a.firebasestorage.app',
  messagingSenderId:'176234978770',
  appId:'1:176234978770:web:e193d8242f4f111abd3c0b'
};
if(!firebase.apps.length)firebase.initializeApp(firebaseConfig);
const database=firebase.database();

let nivelDestino=0,nivelAtualAnim=0;
let config={nivel_ligar:40,nivel_desligar:95,timeout_bomba:90,debounce:5};
let consumoHoje=0,menorNivelHoje=null,ultimoLitros=null,tempoUltimaLeitura=Date.now();
const MAX_QUEDA_POR_LEITURA=60,LIMIAR=.5;
let grafico=null;

function getDataHoje(){const d=new Date();return `${d.getFullYear()}-${String(d.getMonth()+1).padStart(2,'0')}-${String(d.getDate()).padStart(2,'0')}`}
let dataAtual=getDataHoje();
let consumoRef=database.ref('consumo/'+dataAtual);

consumoRef.once('value').then(s=>{if(s.exists()){const d=s.val();consumoHoje=d.total||0;menorNivelHoje=d.menorNivel??null;atualizarConsumoHoje(consumoHoje)}});

function animar(){nivelAtualAnim+=(nivelDestino-nivelAtualAnim)*.1;if(water)water.style.height=(nivelAtualAnim*AREA_UTIL/100)+'%';if(percent)percent.innerText=nivelAtualAnim.toFixed(1)+'%';requestAnimationFrame(animar)}
requestAnimationFrame(animar);

function atualizarInterface(nivel,litros){
  nivelDestino=nivel;
  if(litros!=null&&litrosText)litrosText.innerText=Math.round(litros)+' L';
  processarConsumo(litros);
  if(nivel<=30){water.style.background='linear-gradient(to top,#ff0000,#ff4d4d)';statusText.innerText='MUITO CRÍTICO';alertaGrande.innerText='🚨 PERIGO: CAIXA MUITO BAIXA!';alertaGrande.style.display='block'}
  else if(nivel<=config.nivel_ligar){water.style.background='linear-gradient(to top,#ff7b00,#ffc107)';statusText.innerText='NÍVEL BAIXO';alertaGrande.innerText='⚠️ BOMBA: NÍVEL PARA LIGAR';alertaGrande.style.display='block'}
  else if(nivel>=config.nivel_desligar){water.style.background='linear-gradient(to top,#0077ff,#00c6ff)';statusText.innerText='CAIXA CHEIA';alertaGrande.innerText='⛔ BOMBA: NÍVEL PARA DESLIGAR';alertaGrande.style.display='block'}
  else{water.style.background='linear-gradient(to top,#0077ff,#00c6ff)';statusText.innerText='Normal';alertaGrande.style.display='none'}
}

function processarConsumo(litrosAtual){
  if(litrosAtual==null||litrosAtual<0||litrosAtual>1100)return;
  const agora=Date.now();
  if(ultimoLitros!==null){const dif=Math.abs(litrosAtual-ultimoLitros);if(dif>100&&agora-tempoUltimaLeitura<5000)return}
  ultimoLitros=litrosAtual;tempoUltimaLeitura=agora;
  if(menorNivelHoje===null){menorNivelHoje=litrosAtual;return}
  if(litrosAtual<menorNivelHoje){const dif=menorNivelHoje-litrosAtual;if(dif>MAX_QUEDA_POR_LEITURA)return;if(dif>LIMIAR){consumoHoje+=dif;atualizarConsumoHoje(consumoHoje)}menorNivelHoje=litrosAtual;consumoRef.set({total:+consumoHoje.toFixed(2),menorNivel:menorNivelHoje,ultimaAtualizacao:Date.now()})}
}
function atualizarConsumoHoje(v){const e=document.getElementById('gastoHoje');if(e)e.innerText=v.toFixed(1)+' L'}

function atualizarBoia(id,v){const e=document.getElementById(id);if(e)e.innerText=v?'🟢':'⚪'}
function atualizarAutomacao(data){
  const b=document.getElementById('statusBomba');if(b)b.innerText=data.status_bomba?'🟢 LIGADA':'🔴 DESLIGADA';
  const m=document.getElementById('modoAutomacao');if(m)m.innerText=data.modo_manual?'MANUAL':'AUTOMÁTICO';
  const s=document.getElementById('statusSonoff');if(s)s.innerText=data.sonoff_online?'🟢 ONLINE':'🔴 OFFLINE';
  const r=document.getElementById('wifiRssi');if(r)r.innerText=data.dispositivo?.rssi!=null?data.dispositivo.rssi+' dBm':'-';
  const seg=document.getElementById('sistemaSeguro');if(seg)seg.innerText=data.sistema_seguro?'🟢 NORMAL':'🚨 FALHA';
  const ev=document.getElementById('ultimoEvento');if(ev)ev.innerText=data.ultimo_evento||'-';
  atualizarBoia('boia20',data.boias?.['20']);atualizarBoia('boia40',data.boias?.['40']);atualizarBoia('boia60',data.boias?.['60']);atualizarBoia('boia80',data.boias?.['80']);atualizarBoia('boia95',data.boias?.['95']);
}

database.ref('/').on('value',s=>{const d=s.val();if(!d)return;if(d.nivel!==undefined)atualizarInterface(+d.nivel,+d.litros||0);atualizarAutomacao(d)});
database.ref('configuracao').on('value',s=>{if(s.exists())config=Object.assign(config,s.val())});

function comandoBomba(cmd){if(confirm(cmd==='ON'?'Ligar a bomba?':'Desligar a bomba?'))database.ref('comandos/bomba').set(cmd)}
function comandoModo(modo){database.ref('comandos/modo').set(modo)}

function iniciarGrafico(){const ctx=document.getElementById('graficoConsumo')?.getContext('2d');if(!ctx)return;grafico=new Chart(ctx,{type:'line',data:{labels:[],datasets:[{label:'Litros por dia',data:[],tension:.3}]},options:{responsive:true,animation:false}})}
function escutarGrafico(){database.ref('consumo').on('value',s=>{const d=s.val();if(!d||!grafico)return;const labels=[],valores=[];Object.keys(d).sort().forEach(k=>{labels.push(k.split('-').reverse().join('/'));valores.push(d[k].total||0)});grafico.data.labels=labels;grafico.data.datasets[0].data=valores;grafico.update()})}
iniciarGrafico();escutarGrafico();

setInterval(()=>{const nova=getDataHoje();if(nova!==dataAtual){dataAtual=nova;consumoHoje=0;menorNivelHoje=null;ultimoLitros=null;atualizarConsumoHoje(0);consumoRef=database.ref('consumo/'+nova);consumoRef.once('value').then(s=>{if(s.exists()){const d=s.val();consumoHoje=d.total||0;menorNivelHoje=d.menorNivel??null;atualizarConsumoHoje(consumoHoje)}})}},60000);
