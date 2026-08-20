# Chuvisco

Tocador de áudio em loop via Bluetooth A2DP para ESP32. O dispositivo transmite um
arquivo PCM em loop contínuo (ruído branco/rosa/marrom, som ambiente, etc.) para
qualquer caixa de som Bluetooth, funcionando como *A2DP source*. Alimentado por
bateria 18650 com recarga USB-C, seleciona automaticamente a caixa de sinal mais
forte e memoriza a preferência entre reinícios.

## Hardware

| Componente | Especificação |
|---|---|
| Placa | DFRobot FireBeetle 2 ESP32-E (DFR0654) — ESP32 clássico, obrigatório para A2DP |
| Alimentação | Célula 18650 protegida via conector JST PH2.0; recarga USB-C integrada (500 mA) |
| Medição de bateria | Divisor de tensão no GPIO34 (ADC1_CH6) |
| Botão de reeleição | Botão "27" onboard (GPIO27 → GND) |
| LED de estado | WS2812 RGB onboard (GPIO5); LED azul de status (GPIO2) |

Apenas o **ESP32 clássico** suporta Bluetooth Classic (BR/EDR) e, portanto, A2DP.
Variantes S3/C3/C6 têm apenas BLE e **não** servem para este projeto.

## Funcionalidades

- **Loop sample-perfect** — reproduz PCM, sem decoder nem
  reabertura de arquivo.
- **Seleção da caixa mais forte** — janela de descoberta configurável (15 s) que
  coleta candidatas e conecta na de melhor sinal. Ignorando sinais muito fracos.
- **Preferência persistente** — uma vez conectada, a caixa é memorizada no NVS e
  reconectada automaticamente nos boots seguintes (`auto_reconnect`).
- **Reeleição sob demanda** — segurar o botão 27 por 1s esquece a caixa atual e
  reinicia a busca.
- **Controle de volume** — impõe um volume-alvo na caixa após a conexão e monitora
  mudanças feitas nos botões físicos dela (via AVRCP).
- **Proteção de bateria** — monitoramento contínuo com mediana móvel e deep sleep
  ao atingir tensão de corte, com re-checagem periódica.

## Estrutura do projeto

```
chuvisco/
├── src/main.cpp          Firmware principal
├── platformio.ini        Configuração de build (PlatformIO)
├── partitions.csv        Tabela de partições (app 1,875 MB + LittleFS ~2 MB)
├── data/                 Conteúdo do filesystem — apenas loop.pcm vai para a flash
│   └── loop.pcm
└── util/                 Área de trabalho (fontes de áudio, scripts) — não versionada
```

## Preparação do áudio

O firmware reproduz **PCM raw**: 44100 Hz, estéreo, 16-bit little-endian, sem
cabeçalho. Converta a partir de qualquer fonte com ffmpeg:

```bash
ffmpeg -i entrada.wav -ar 44100 -ac 2 -f s16le -acodec pcm_s16le fs/loop.pcm
```

A taxa é 176.400 bytes/segundo. Com a partição de ~2 MB, o limite é **~11 segundos**
de áudio — adequado para loops de ruído/ambiente. O firmware informa a duração real
no boot pelo monitor serial.

### Loop sem cliques

Um clique na emenda do loop indica descontinuidade da forma de onda entre o fim e o
início do arquivo. Duas soluções, da mais simples à definitiva:

1. **Corte em zero-crossing** (Audacity, tecla Z) — suficiente para ruído/ambiente.
2. **Crossfade de costura** — funde o final no início com curvas equal-power,
   eliminando o clique por construção, independente do conteúdo. Ver `loopfix.py`.

## Build e gravação

O projeto usa **PlatformIO**. Bibliotecas (todas do autor pschatzmann, com versões
pinadas no `platformio.ini`): `arduino-audio-tools`, `ESP32-A2DP`, `arduino-libhelix`.

```bash
pio pkg install          # resolve e instala dependências
pio run                  # compila
pio run -t upload        # grava o firmware
pio run -t uploadfs      # grava o LittleFS (conteúdo de fs/)
pio device monitor       # monitor serial a 115200
```

O `uploadfs` grava uma imagem completa da partição — todo o conteúdo de `data/` é
enviado e o conteúdo anterior na flash é substituído.

### Configuração

Parâmetros no topo de `src/main.cpp`:

| Constante | Função |
|---|---|
| `VOLUME` | Volume-alvo imposto à caixa (0.0–1.0) |
| `JANELA_DESCOBERTA_MS` | Duração da janela de eleição da caixa (padrão 15s) |
| `RSSI_MINIMO` | Corte de sinal; caixas mais fracas são ignoradas (padrão −70db) |
| `V_CORTE` / `V_RETOMA` | Tensões de desligamento e religamento da bateria |
| `RECHECK_US` | Intervalo de re-checagem em deep sleep (padrão 30 min) |

## Ambiente de desenvolvimento

O driver CH340 da placa não enumera de forma confiável no macOS
recente, enquanto o driver `ch341` do kernel Linux suporta o chip nativamente.
A solução é usar uma VM Linux rodando o PlatformIO.

O fluxo usa um remote git na VM, com push do host antes de cada build:

```bash
# no host, após editar:
git push vm main
ssh vm "cd ~/chuvisco && pio run -t upload && pio run -t uploadfs"
```

## Indicação de estado (LED RGB)

| Cor | Estado |
|---|---|
| Âmbar (pulsante) | Em descoberta/eleição de caixa |
| Verde | Conectado, transmitindo |
| Azul | Reeleição disparada |
| Vermelho (piscante) | Bateria baixa, prestes a dormir |

## Notas de implementação

Três comportamentos da biblioteca ESP32-A2DP exigiram contorno e valem registro para
quem for manter o código:

- **`begin()` bloqueia até conectar** por padrão (`wait_for_connection`). É preciso
  desabilitar para que o `loop()` — e o botão — rodem antes de haver conexão.
- **Áudio só deve ser copiado quando conectado**: sem consumidor do buffer A2DP, o
  `StreamCopy::copy()` bloqueia. O loop condiciona a cópia ao estado de conexão.
- **Reeleição via reset**: o caminho de desconexão voluntária da biblioteca não
  reinicia o discovery de forma confiável; a reeleição limpa o NVS e reinicia a placa,
  caindo no fluxo de descoberta limpo do boot.

## Licença

Distribuído sob a **GNU General Public License v3.0** (GPLv3), em conformidade com
as bibliotecas ESP32-A2DP e arduino-audio-tools (Phil Schatzmann), também GPLv3, das
quais o firmware depende. Veja o arquivo [LICENSE](LICENSE) para o texto completo.