# STM32F103 + DX-LR30 (SX1262) — Repeater LoRa compatível com Meshtastic

Firmware para transformar um combo **STM32F103C8T6 + DX-LR30 (SX1262)** — vendido em
kits chineses de AliExpress/Amazon como "STM32 + LoRa SX1262 development board" — em
um **repetidor de camada de rádio** compatível com o protocolo Meshtastic.

> **Este NÃO é o firmware oficial do Meshtastic.** O Meshtastic oficial não roda em
> STM32F103 — o chip só tem 64KB de flash e 20KB de RAM, insuficiente para o stack
> completo (protobuf, criptografia, BLE/serial API, roteamento, NodeDB). Não existe
> firmware oficial para baixar e gravar nessa placa, apesar do que alguns anúncios
> de venda dessas placas dizem.

## O que este firmware faz

- Fala o **mesmo protocolo de rádio** que o Meshtastic usa: mesma frequência (calculada
  pelo mesmo algoritmo de hash do canal), mesma modulação LoRa (SF, BW, CR), mesmo sync
  word e preâmbulo.
- Lê o cabeçalho de 16 bytes do pacote Meshtastic — que **não é criptografado** —
  contendo `to`, `from`, `id`, `hop_limit`, `hop_start`, `channel_hash`, `relay_node`.
- Se `hop_limit > 0` e o pacote não foi visto recentemente (dedup por `from`+`id`),
  decrementa o `hop_limit`, marca `relay_node` com o ID do repeater, espera um atraso
  baseado no SNR (mesma fórmula do firmware oficial, para reduzir colisões) e
  retransmite o pacote.
- **Não decifra o payload.** Não precisa — a retransmissão acontece no nível de rádio,
  o conteúdo cifrado é copiado como está, exatamente como o roteamento real do
  Meshtastic funciona (nós intermediários não precisam da chave do canal para rotear).

## O que este firmware NÃO faz (limitações honestas)

- Não é um nó completo: não aparece no app Meshtastic, não manda posição/telemetria,
  não participa do NodeDB.
- Sem CAD (Channel Activity Detection) real antes de transmitir — mais chance de
  colisão numa rede movimentada do que um nó Meshtastic de verdade.
- Sem "zero-cost hops" nem next-hop routing — sempre flooding simples.
- O pino DIO1 do módulo não está conectado a nenhum GPIO do STM32 nesse hardware
  (confirmado via schematic), então o firmware usa **polling via SPI**
  (`checkIrq()`/`clearIrq()`) em vez de interrupção por pino, tanto para RX quanto TX.

## Hardware

- **MCU**: STM32F103C8T6 ("Blue Pill"), 64KB flash / 20KB RAM (chip genuíno — alguns
  clones têm 128KB, confira com uma ferramenta como o MCUISP antes de gravar).
- **Rádio**: DX-LR30-900M22S (chip **SX1262** genuíno da Semtech). *Atenção*: o
  DX-LR20 (produto diferente, mesma família de placas) usa o chip **LLCC68**, não o
  SX1262 — confira qual módulo você tem antes de assumir o pinout.
- **Sem pino BOOT0 acessível?** Ferramentas como o MCUISP conseguem entrar no modo
  bootloader via sequência automática de DTR/RTS pelo próprio adaptador serial, sem
  precisar tocar em jumper nenhum.

### Pinout (confirmado via schematic real, não é chute)

Fonte: schematic PCBA anexado em
[stm32duino/Arduino_Core_STM32#2777](https://github.com/stm32duino/Arduino_Core_STM32/issues/2777).

| Sinal do módulo | Pino STM32 |
|---|---|
| NSS   | PA4 |
| SCK   | PA5 *(SPI1 hardware)* |
| MISO  | PA6 *(SPI1 hardware)* |
| MOSI  | PA7 *(SPI1 hardware)* |
| RESET | PA3 |
| BUSY  | PA2 |
| RXEN  | PA1 |
| TXEN  | PA0 |
| DIO1  | não conectado a nenhum GPIO neste design |

Se o seu combo STM32+LR30/LR20 for de outro fabricante, **confirme o pinout antes de
gravar** — esse mapeamento é específico desse design de placa. Uma forma rápida de
verificar: com a placa desligada, teste continuidade (multímetro) entre os pinos do
STM32 e os pinos do módulo (numeração no datasheet do LR20/LR30-900M22S).

## Frequência do canal

O Meshtastic escolhe a frequência de cada canal com um hash (djb2) do nome do canal,
módulo o número de slots disponíveis na banda da região. Não dá pra usar só a
frequência central da região — cada nome de canal cai num slot diferente. O valor
padrão neste firmware (`FREQ_MHZ` no topo do `main.cpp`) foi calculado para o canal
público **"LongFast"** na região configurada — **ajuste para a sua região e nome de
canal reais**, ou o repeater nunca vai ouvir nada.

Regiões e frequências calculadas durante o desenvolvimento (canal "LongFast", preset
LongFast = BW 250kHz, SF11, CR 4/5):

| Região | Faixa | Frequência do canal "LongFast" |
|---|---|---|
| BR_902 (Brasil) | 902.0–907.5 MHz | 903.875 MHz |
| ANZ (Austrália/NZ) | 915.0–928.0 MHz | 919.875 MHz Default |

Se o seu canal tiver nome customizado (não o padrão "LongFast"), a frequência muda.

## Como compilar e gravar

Requer [PlatformIO](https://platformio.org/) (extensão do VS Code ou `pip install platformio`).

```bash
git clone <este repo>
cd <este repo>
pio run              # compila e gera firmware.hex em .pio/build/bluepill_f103c8/
pio run -t upload    # grava via ST-Link (padrao) -- veja platformio.ini para serial
```

### Gravando sem ST-Link (via adaptador USB-serial)

1. Ligue TX do adaptador → PA10, RX do adaptador → PA9, GND → GND.
2. Use uma ferramenta como o [MCUISP](http://www.mcuisp.com/) (Windows) apontando pro
   `firmware.hex` gerado em `.pio/build/bluepill_f103c8/firmware.hex`. Ele consegue
   entrar em modo bootloader sozinho via DTR/RTS, sem precisar de jumper BOOT0 físico.
3. Se preferir gravar puro PlatformIO por serial: descomente `upload_protocol = serial`
   e `upload_port` no `platformio.ini`, comente `upload_protocol = stlink`.

O projeto já inclui um script (`scripts/create_hex.py`) que gera automaticamente o
`.hex` a partir do `.elf` em todo build — necessário porque ferramentas tipo MCUISP
pedem `.hex`, e o PlatformIO por padrão só gera `.bin`.

[Video demonstração usando MCUISP](https://www.youtube.com/watch?v=L_HUamG1tiY)

Arquivos (MCUISP e Firmware.hex):
[Arquivos GDRIVE](https://drive.google.com/drive/folders/11J58yIezrtQlW_SfMyrRccWgHRKd8J7R?usp=sharing)

## Configuração

Edite as constantes no topo de `src/main.cpp`:

```cpp
static float FREQ_MHZ       = 919.875f; // ajuste pra sua regiao/canal
static float BW_KHZ         = 250.0f;
static uint8_t SF           = 11;
static uint8_t CR           = 5;
static uint8_t SYNC_WORD    = 0x2B;     // sync word padrao do Meshtastic
static uint16_t PREAMBLE    = 16;
static int8_t TX_POWER_DBM  = 20;
```

## Diagnóstico

Abra o monitor serial em 115200 baud (`pio device monitor -p COMx -b 115200`). Saída
esperada:

```
=== Repeater LoRa (STM32F103 + SX1262) compativel com Meshtastic ===
Node ID (ultimo byte, usado em relay_node): 0x24
Slot time calculado: 28 ms
Ouvindo em 919.875 MHz (LongFast / BR_902)...
RX from=0xF993F6C4 to=0xFFFFFFFF id=0x5DBD48E6 hop=3/3 ch=0x8 snr=5.50 rssi=-24.00 len=101
  -> retransmitindo em 28 ms
  -> retransmitido OK
```
      
**Erros comuns:**

| Sintoma | Causa provável |
|---|---|
| `Falha ao iniciar SX1262, codigo: -2` | Pinout errado (NSS/RESET/BUSY) — confira a tabela acima ou teste continuidade |
| Silêncio total no monitor serial | `Serial` apontando pra porta errada — este firmware usa `Serial1` (USART1, PA9/PA10) de propósito |
| Nunca recebe nada, mas rádio inicializa OK | Frequência/região errada — confira a tabela de frequências acima |
| `ERRO ao transmitir, codigo: -5` (TX_TIMEOUT) | Corrigido nesta versão: o firmware usa `startTransmit()` + polling em vez do `transmit()` bloqueante, que dependia do pino DIO1 (não conectado nesse hardware) |

## Créditos e fontes

- Formato de pacote e parâmetros de rádio: código-fonte oficial do
  [meshtastic/firmware](https://github.com/meshtastic/firmware) (`RadioInterface.cpp`,
  `MeshRadio.h`).
- Pinout do combo STM32+LR20/LR30: schematic PCBA anexado em
  [stm32duino/Arduino_Core_STM32#2777](https://github.com/stm32duino/Arduino_Core_STM32/issues/2777).
- Biblioteca de rádio: [RadioLib](https://github.com/jgromes/RadioLib) (jgromes).
- Manual do módulo: DX-LR20/LR30-900M22S Module Technical Manual, SHEN ZHEN DX-SMART
  TECHNOLOGY CO., LTD.

## Aviso legal

Transmissão em banda ISM está sujeita a regulamentação local (ANATEL no Brasil, ACMA
na Austrália, etc.). Confirme a frequência, potência e duty cycle permitidos na sua
região antes de operar continuamente.
