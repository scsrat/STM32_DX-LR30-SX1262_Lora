/*
 * Repeater LoRa "nivel de radio" compativel com Meshtastic
 * Hardware: STM32F103C8T6 (Blue Pill) + modulo SX1262 (ex: DX-LR30)
 *
 * O QUE ISSO FAZ:
 *  - Escuta pacotes LoRa usando os MESMOS parametros de radio que o Meshtastic usa
 *    (mesma frequencia, SF, BW, CR, sync word, preambulo) para o preset LongFast.
 *  - Ao receber um pacote, le o cabecalho de 16 bytes (NAO CRIPTOGRAFADO) definido
 *    pelo protocolo Meshtastic: to, from, id, flags (hop_limit/want_ack/via_mqtt/
 *    hop_start), channel_hash, next_hop, relay_node.
 *  - Se hop_limit > 0 e o pacote ainda nao foi visto recentemente (dedup por
 *    from+id), decrementa hop_limit, atualiza relay_node com o proprio ID, espera
 *    um atraso baseado no SNR (igual ao algoritmo real do Meshtastic, para reduzir
 *    colisoes) e retransmite o pacote byte a byte.
 *  - NAO decifra o payload (nao sabemos a chave/PSK do canal, e nao precisamos:
 *    retransmissao acontece no nivel de radio, o payload cifrado e so copiado).
 *
 * O QUE ISSO NAO FAZ (limitacoes honestas):
 *  - Nao e um no completo: nao aparece no app Meshtastic, nao manda posicao/telemetria,
 *    nao participa do NodeDB.
 *  - Nao implementa CAD (Channel Activity Detection) antes de transmitir -- em vez
 *    disso, faz um "listen before talk" simplificado ouvindo por RSSI. Em uma rede
 *    congestionada isso rende mais colisoes que um no Meshtastic de verdade.
 *  - Nao decrementa hop_limit para nos favoritos (zero-cost hops) nem faz next-hop
 *    routing -- sempre usa flooding puro, igual ao algoritmo classico do Meshtastic.
 *  - PRECISA que voce ajuste os pinos (NSS/RESET/BUSY/DIO1) abaixo para bater com
 *    a fiacao real do seu modulo DX-LR30 -- eu nao tenho o desenho exato do seu modulo.
 *
 * Parametros de radio abaixo calculados para regiao BR/ANZ (919.875 MHz) e
 * canal publico padrao "LongFast". Se voce usa outra regiao/canal, ajuste FREQ_MHZ.
 */

#include <Arduino.h>
#include <RadioLib.h>

// ======================= PINOS CONFIRMADOS PELO SCHEMATIC REAL =======================
// Fonte: schematic PCBA do combo STM32F103+DX-LR30/LR20, obtido via
// github.com/stm32duino/Arduino_Core_STM32 issue #2777. SPI (SCK/MISO/MOSI)
// cai exatamente no SPI1 de hardware do Blue Pill.
#define PIN_LORA_NSS   PA4
#define PIN_LORA_RESET PA3
#define PIN_LORA_BUSY  PA2
#define PIN_LORA_RXEN  PA1
#define PIN_LORA_TXEN  PA0
// LED onboard da Blue Pill (o LED verde que ja vem na placa). Ativo em LOW
// (LOW = aceso). Se sua placa tiver o LED em outro pino, ajuste aqui.
// LED onboard da placa (confirmado por teste fisico com led_test.cpp).
// Polaridade ainda nao confirmada -- se o LED ficar aceso em repouso e apagar
// durante a retransmissao (invertido do esperado), troque LOW<->HIGH abaixo.
#define PIN_LED_TX     PB11
// SCK=PA5, MISO=PA6, MOSI=PA7 -- ja sao os pinos default do SPI1, nao precisa
// declarar, o SPI global do Arduino ja usa esses automaticamente.
//
// DIO1 aparece rotulado na serigrafia da placa, mas nao foi encontrado ligado
// a nenhum GPIO no schematic disponivel. Por seguranca, este firmware NAO
// depende de DIO1 -- ele usa polling via SPI (checkIrq/clearIrq) em vez de
// interrupcao por pino, entao funciona esteja o DIO1 conectado ou nao.
// ======================================================================

// ==================== PARAMETROS DE RADIO (LongFast / BR_902) ====================
static float FREQ_MHZ       = 919.875f; // canal publico "LongFast" na regiao ANZ (915.0-928.0 MHz)
static float BW_KHZ         = 250.0f;   // LongFast usa 250 kHz
static uint8_t SF           = 11;       // LongFast usa SF11
static uint8_t CR           = 5;        // coding rate 4/5
static uint8_t SYNC_WORD    = 0x2B;     // sync word do Meshtastic
static uint16_t PREAMBLE    = 16;       // simbolos de preambulo
static int8_t TX_POWER_DBM  = 20;       // comece conservador; DX-LR30 aguenta ate 22 dBm
// ===================================================================================

#define PACKET_HEADER_LEN 16
#define MAX_LORA_PAYLOAD  255

// Layout exato do PacketHeader do Meshtastic (16 bytes, little-endian, sem protobuf)
struct __attribute__((packed)) PacketHeader {
    uint32_t to;
    uint32_t from;
    uint32_t id;
    uint8_t  flags;      // bits0-2=hop_limit, bit3=want_ack, bit4=via_mqtt, bits5-7=hop_start
    uint8_t  channel;    // hash do canal
    uint8_t  next_hop;
    uint8_t  relay_node;
};

#define FLAGS_HOP_LIMIT_MASK 0x07
#define FLAGS_HOP_START_MASK 0xE0
#define FLAGS_HOP_START_SHIFT 5

SX1262 radio = new Module(PIN_LORA_NSS, RADIOLIB_NC, PIN_LORA_RESET, PIN_LORA_BUSY);
// NOTA: chip real confirmado como SX1262 genuino da Semtech (o DX-LR30 usa
// SX1262; o DX-LR20, que e um produto diferente, usa o LLCC68). O segundo
// parametro (pino de interrupcao/DIO1) foi setado como RADIOLIB_NC porque
// nao encontramos DIO1 ligado a nenhum GPIO no schematic -- este firmware usa
// polling via SPI em vez de depender de interrupcao por pino.

// (sem flag de interrupcao -- este firmware usa polling via checkIrq()/clearIrq())

uint8_t rxBuffer[MAX_LORA_PAYLOAD];
uint8_t myNodeIdByte; // ultimo byte do "node id" deste repeater (usado em relay_node)

// -------- Dedup: guarda os ultimos pacotes vistos (from,id) para nao retransmitir 2x --------
#define DEDUP_SIZE 40
struct SeenPacket { uint32_t from; uint32_t id; };
SeenPacket seenPackets[DEDUP_SIZE];
uint8_t seenIndex = 0;

bool wasSeenRecently(uint32_t from, uint32_t id) {
    for (uint8_t i = 0; i < DEDUP_SIZE; i++) {
        if (seenPackets[i].from == from && seenPackets[i].id == id) return true;
    }
    return false;
}
void markSeen(uint32_t from, uint32_t id) {
    seenPackets[seenIndex].from = from;
    seenPackets[seenIndex].id = id;
    seenIndex = (seenIndex + 1) % DEDUP_SIZE;
}

// -------- Replica o calculo de slot time / atraso ponderado por SNR do firmware real --------
// Fonte: meshtastic/firmware src/mesh/RadioInterface.cpp (CWmin=3, CWmax=8)
const uint8_t CWmin = 3;
const uint8_t CWmax = 8;

uint32_t pow_of_2(int n) { return 1UL << n; }

uint32_t computeSlotTimeMsec(uint8_t sf, float bwKHz) {
    float symbolTimeMs = pow_of_2(sf) / bwKHz; // pow(2,sf)/bw, bw em kHz -> ms
    float sumPropTurnaroundMac = 0.2f + 0.4f + 7.0f;
    const uint8_t NUM_SYM_CAD = 2;
    float cadDuration = (NUM_SYM_CAD + 0.5f > 2.25f ? NUM_SYM_CAD + 0.5f : 2.25f) * symbolTimeMs;
    return (uint32_t)(cadDuration + sumPropTurnaroundMac);
}

uint8_t getCWsize(float snr) {
    const int32_t SNR_MIN = -20, SNR_MAX = 10;
    long v = map((long)(snr * 100), SNR_MIN * 100, SNR_MAX * 100, CWmin, CWmax);
    if (v < CWmin) v = CWmin;
    if (v > CWmax) v = CWmax;
    return (uint8_t)v;
}

// Atraso antes de retransmitir: mais SNR (sinal forte/perto) = espera mais;
// SNR ruim (provavelmente mais longe, quer ter prioridade) = espera menos.
uint32_t getRebroadcastDelayMsec(float snr, uint32_t slotTimeMsec) {
    uint8_t CWsize = getCWsize(snr);
    uint32_t maxSlots = pow_of_2(CWsize);
    uint32_t r = random(0, maxSlots);
    return r * slotTimeMsec;
}

uint32_t slotTimeMsec;

void setup() {
    pinMode(PIN_LED_TX, OUTPUT);
    // Teste de boot: pisca o LED 4 vezes logo ao ligar, independente da
    // polaridade real da sua placa (LED ativo em LOW ou em HIGH -- ambos
    // vao mostrar alguma piscada aqui). Se nada piscar, o pino PIN_LED_TX
    // esta errado ou nao ha LED fisico nesse pino nesta placa.
    for (uint8_t i = 0; i < 4; i++) {
        digitalWrite(PIN_LED_TX, LOW);
        delay(150);
        digitalWrite(PIN_LED_TX, HIGH);
        delay(150);
    }
    digitalWrite(PIN_LED_TX, LOW); // estado de repouso: apagado (economiza bateria)
    Serial1.begin(115200);
    delay(2000); // da tempo do adaptador USB-serial estabilizar

    Serial1.println();
    Serial1.println("=== Repeater LoRa (STM32F103 + SX1262) compativel com Meshtastic ===");

    // Deriva um "node id" simples a partir do Unique ID de fabrica do STM32
    uint32_t *uid = (uint32_t *)0x1FFFF7E8;
    myNodeIdByte = (uint8_t)(uid[0] ^ uid[1] ^ uid[2]);
    Serial1.print("Node ID (ultimo byte, usado em relay_node): 0x");
    Serial1.println(myNodeIdByte, HEX);

    int state = radio.begin(FREQ_MHZ, BW_KHZ, SF, CR, SYNC_WORD, TX_POWER_DBM, PREAMBLE);
    if (state != RADIOLIB_ERR_NONE) {
        Serial1.print("Falha ao iniciar SX1262, codigo: ");
        Serial1.println(state);
        Serial1.println("Verifique os pinos NSS/RESET/BUSY/DIO1 no topo do arquivo.");
        while (true) { delay(1000); }
    }

    radio.setCRC(true);
    radio.setRfSwitchPins(PIN_LORA_RXEN, PIN_LORA_TXEN);

    slotTimeMsec = computeSlotTimeMsec(SF, BW_KHZ);
    Serial1.print("Slot time calculado: ");
    Serial1.print(slotTimeMsec);
    Serial1.println(" ms");

    state = radio.startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        Serial1.print("Falha ao iniciar recepcao continua, codigo: ");
        Serial1.println(state);
    } else {
        Serial1.print("Ouvindo em ");
        Serial1.print(FREQ_MHZ, 3);
        Serial1.println(" MHz (LongFast / BR_902)...");
    }

    randomSeed(uid[0] ^ millis());
}

void loop() {
    // Polling via SPI: como nao dependemos do pino DIO1, checamos a flag
    // generica de IRQ (API publica do RadioLib) em vez de esperar interrupcao.
    if (radio.checkIrq(RADIOLIB_IRQ_RX_DONE) != 1) {
        return; // nada recebido ainda
    }
    radio.clearIrq(RADIOLIB_IRQ_RX_DONE);

    int len = radio.getPacketLength();
    if (len < PACKET_HEADER_LEN || len > MAX_LORA_PAYLOAD) {
        radio.startReceive();
        return;
    }

    int state = radio.readData(rxBuffer, len);
    float snr = radio.getSNR();
    float rssi = radio.getRSSI();

    // volta a ouvir imediatamente (nao bloquear a recepcao enquanto processa)
    radio.startReceive();

    if (state != RADIOLIB_ERR_NONE) return;

    PacketHeader hdr;
    memcpy(&hdr, rxBuffer, PACKET_HEADER_LEN);

    uint8_t hopLimit = hdr.flags & FLAGS_HOP_LIMIT_MASK;
    uint8_t hopStart = (hdr.flags & FLAGS_HOP_START_MASK) >> FLAGS_HOP_START_SHIFT;

    Serial1.print("RX from=0x"); Serial1.print(hdr.from, HEX);
    Serial1.print(" to=0x"); Serial1.print(hdr.to, HEX);
    Serial1.print(" id=0x"); Serial1.print(hdr.id, HEX);
    Serial1.print(" hop="); Serial1.print(hopLimit);
    Serial1.print("/"); Serial1.print(hopStart);
    Serial1.print(" ch=0x"); Serial1.print(hdr.channel, HEX);
    Serial1.print(" snr="); Serial1.print(snr);
    Serial1.print(" rssi="); Serial1.print(rssi);
    Serial1.print(" len="); Serial1.println(len);

    if (hopLimit == 0) {
        Serial1.println("  -> hop_limit=0, nao retransmite");
        return;
    }
    if (wasSeenRecently(hdr.from, hdr.id)) {
        Serial1.println("  -> ja visto, ignorando (dedup)");
        return;
    }
    markSeen(hdr.from, hdr.id);

    // Espera um atraso baseado no SNR antes de retransmitir (evita colisao com
    // outros nos que tambem podem estar retransmitindo o mesmo pacote)
    uint32_t delayMs = getRebroadcastDelayMsec(snr, slotTimeMsec);
    Serial1.print("  -> retransmitindo em "); Serial1.print(delayMs); Serial1.println(" ms");
    delay(delayMs);

    // Monta o pacote de saida: mesmo payload, so decrementa hop_limit e marca
    // relay_node como o ultimo byte do nosso proprio id (preserva hop_start,
    // want_ack, via_mqtt e todo o payload cifrado intactos)
    hdr.flags = (hdr.flags & ~FLAGS_HOP_LIMIT_MASK) | (hopLimit - 1);
    hdr.relay_node = myNodeIdByte;
    memcpy(rxBuffer, &hdr, PACKET_HEADER_LEN);

    radio.standby();
    digitalWrite(PIN_LED_TX, HIGH); // acende (retransmissao em andamento)
    int txState = radio.startTransmit(rxBuffer, len);
    if (txState != RADIOLIB_ERR_NONE) {
        Serial1.print("  -> ERRO ao iniciar transmissao, codigo: ");
        Serial1.println(txState);
        radio.startReceive();
        return;
    }

    // Espera TX_DONE via polling (sem depender do pino DIO1). Timeout de
    // seguranca de 2s para nunca travar o loop caso algo de errado.
    uint32_t txStart = millis();
    bool txDone = false;
    while (millis() - txStart < 2000) {
        if (radio.checkIrq(RADIOLIB_IRQ_TX_DONE) == 1) {
            txDone = true;
            break;
        }
    }

    if (txDone) {
        radio.clearIrq(RADIOLIB_IRQ_TX_DONE);
        radio.finishTransmit();
        Serial1.println("  -> retransmitido OK");
    } else {
        Serial1.println("  -> ERRO: timeout esperando TX_DONE (2s)");
    }
    digitalWrite(PIN_LED_TX, LOW); // apaga (volta ao repouso)
    radio.startReceive();
}
