/*
 * PCM em loop -> Caixa Bluetooth (A2DP Source)
 * Placa: DFRobot FireBeetle 2 ESP32-E (4MB)
 *
 * Audio: PCM cru (sem cabecalho), 44100 Hz, estereo, 16-bit little-endian.
 * Gerar com:
 *   ffmpeg -i entrada.wav -ar 44100 -ac 2 -f s16le -acodec pcm_s16le -ss 00:00:00 -to 00:00:10 loop.pcm
 * Fazer o crossfade (opcional) com util/loopfix.py
 * Enviar com "Upload Filesystem Image" (pio run -t uploadfs).
 *
 */

#include <Arduino.h>
#include "AudioTools.h"
#include "AudioTools/Communication/A2DPStream.h"
#include <LittleFS.h>
#include "esp_adc_cal.h"

// ================== CONFIGURACAO ==================
const char* ARQUIVO_PCM   = "/loop.pcm";
const float VOLUME        = 0.30;         // 0.0 a 1.0
// controle de bateria
const int PIN_VBAT = 34;   // ADC1_CH6 — funciona com rádio ligado
const int N_AMOSTRAS = 10;
const float V_CORTE   = 3.30;   // desliga
const float V_RETOMA  = 3.70;   // religa (só faz sentido se carregando)
const uint64_t RECHECK_US = 30ULL * 60 * 1000000;  // 30 min
// Parâmetros para a descoberta de novas caixas de som
const uint32_t JANELA_DESCOBERTA_MS = 15000;  // coleta candidatos por 15 s
const int      RSSI_MINIMO          = -70;    // ignora sinais mais fracos que isso
// ==================================================


// ============== Variáveis globais =================
A2DPStream a2dp;                 // saida: Bluetooth A2DP (source)
A2DPNoVolumeControl semAtenuacao; // nao atenua o PCM local; volume fica 100% na caixa
File arquivo;
StreamCopy copier(a2dp, arquivo); // PCM direto: arquivo -> A2DP

// estado do volume da caixa
int volumeAnterior = -1;
bool volumeAplicado = false;
uint32_t conectadoDesde = 0;

// controle de bateria
static volatile float vbatFiltrada = 4.2;   // otimista até encher o buffer
static volatile bool  vbatPronta   = false;

// estado da seleção de caixa
int          melhorRssi = -128;
esp_bd_addr_t melhorEndereco;
char         melhorNome[64] = "";
uint32_t     inicioDescoberta = 0;

// botao de reeleicao (botao "27" da placa: GPIO27 -> GND quando pressionado)
const uint8_t PINO_BOTAO = 27;
uint32_t botaoDesde = 0;      // debounce/hold
bool reeleicaoDisparada = false;

void taskVbat(void *pv) {
  float buf[N_AMOSTRAS];
  int idx = 0, n = 0;

  for (;;) {
    buf[idx] = analogReadMilliVolts(PIN_VBAT) * 2.0f / 1000.0f;
    idx = (idx + 1) % N_AMOSTRAS;
    if (n < N_AMOSTRAS) n++;

    // mediana
    float tmp[N_AMOSTRAS];
    memcpy(tmp, buf, n * sizeof(float));
    for (int i = 1; i < n; i++)
      for (int j = i; j > 0 && tmp[j] < tmp[j-1]; j--)
        { float t = tmp[j]; tmp[j] = tmp[j-1]; tmp[j-1] = t; }
    vbatFiltrada = tmp[n/2];
    vbatPronta   = (n == N_AMOSTRAS);

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void dormirSeBateriaBaixa() {
  if (!vbatPronta || vbatFiltrada > V_CORTE) return;

  Serial.println("Bateria baixa — desligando");
  Serial.flush();

  // desliga periféricos antes de dormir
  btStop();

  esp_sleep_enable_timer_wakeup(RECHECK_US);
  esp_deep_sleep_start();
}

bool selecionarMaisForte(const char* nome, esp_bd_addr_t endereco, int rssi) {
  uint32_t agora = millis();
  if (inicioDescoberta == 0) inicioDescoberta = agora;

  if (rssi < RSSI_MINIMO) {
    Serial.printf("Descartando: %s (RSSI %d < %d)\n", nome, rssi, RSSI_MINIMO);
    return false;
  }

  // fase 1: coleta — anota o melhor, não conecta ainda
  if (rssi > melhorRssi) {
    melhorRssi = rssi;
    memcpy(melhorEndereco, endereco, ESP_BD_ADDR_LEN);
    strncpy(melhorNome, nome, sizeof(melhorNome) - 1);
    Serial.printf("Candidata: %s (RSSI %d) — melhor até agora\n", nome, rssi);
  }
  if (agora - inicioDescoberta < JANELA_DESCOBERTA_MS) return false;  // Não acabou o tempo ainda

  // fase 2: captura — janela encerrada, aceita o campeão quando reaparecer
  if (memcmp(endereco, melhorEndereco, ESP_BD_ADDR_LEN) == 0) {
    Serial.printf("Conectando na mais forte: %s (RSSI %d)\n", melhorNome, melhorRssi);
    return true;
  }
  // rede de segurança: campeão sumiu por 15 s extras? aceita qualquer uma acima do corte
  if (agora - inicioDescoberta > 2 * JANELA_DESCOBERTA_MS) {
    Serial.printf("Campeã indisponível; conectando em %s (RSSI %d)\n", nome, rssi);
    return true;
  }
  return false;
}

void setup() {
  Serial.begin(115200);

  // controle de bateria
  xTaskCreatePinnedToCore(taskVbat, "vbat", 2048, NULL, 1, NULL, 0);

  // botao de reeleicao
  pinMode(PINO_BOTAO, INPUT_PULLUP);

  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  if (!LittleFS.begin()) {
    Serial.println("ERRO: falha ao montar LittleFS. Rodou uploadfs?");
    while (true) delay(1000);
  }

  arquivo = LittleFS.open(ARQUIVO_PCM, "r");
  if (!arquivo || arquivo.size() == 0) {
    Serial.println("ERRO: /loop.pcm nao encontrado ou vazio!");
    while (true) delay(1000);
  }
  // PCM precisa ter numero inteiro de frames (multiplo de 4 bytes: 2ch x 16bit)
  if (arquivo.size() % 4 != 0) {
    Serial.println("AVISO: tamanho nao multiplo de 4 bytes; ultimo frame sera parcial");
  }

  auto cfg = a2dp.defaultConfig(TX_MODE);
  cfg.auto_reconnect = true;
  cfg.silence_on_nodata = true;
  cfg.wait_for_connection = false;
  a2dp.source().set_volume_control(&semAtenuacao);
  a2dp.source().set_ssid_callback(selecionarMaisForte);
  a2dp.begin(cfg);

  copier.begin(a2dp, arquivo);
}

void loop() {
  // --- botao de reeleicao: segurar 1 s ---
  if (digitalRead(PINO_BOTAO) == LOW) {
    if (botaoDesde == 0) botaoDesde = millis();
    if (!reeleicaoDisparada && millis() - botaoDesde > 1000) {
      reeleicaoDisparada = true;
      Serial.println(">>> Reeleicao: esquecendo caixa preferencial e reiniciando...");
      a2dp.source().clean_last_connection();
      delay(100);
      ESP.restart();
    }
  } else {
    botaoDesde = 0;
    reeleicaoDisparada = false;
  }

  // --- bateria ---
  dormirSeBateriaBaixa();

  // --- audio + estado de conexao ---
  if (a2dp.source().is_connected()) {
    if (conectadoDesde == 0) {          // transicao: acabou de conectar
      conectadoDesde = millis();
      melhorRssi = -128;                // rearma a eleicao p/ proxima queda
      inicioDescoberta = 0;
      melhorNome[0] = '\0';
    }
    if (!volumeAplicado && millis() - conectadoDesde > 1500) {
      a2dp.setVolume(VOLUME);
      volumeAplicado = true;
      Serial.printf("Volume imposto: %.0f%%\n", VOLUME * 100);
    }
    if (!copier.copy()) arquivo.seek(0);

    int v = a2dp.source().get_volume();  // so faz sentido conectado
    if ((v != volumeAnterior) && volumeAplicado) {
      Serial.printf("Volume da caixa: %d/127 (%.0f%%)\n", v, v * 100.0 / 127);
      volumeAnterior = v;
    }
  } else {
    conectadoDesde = 0;                  // rearma p/ proxima conexao
    volumeAplicado = false;
    delay(20);
  }
}
