#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_random.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>


namespace ProjetoRF {
constexpr uint8_t FLAG = 0x7E;
constexpr uint8_t DATA = 1, ACK = 2, END = 3;
constexpr uint8_t MAX_DATA = 16, DADOS_POR_QUADRO = 10;
constexpr uint8_t CABECALHO_OBJETO = 7;
constexpr uint16_t MAX_OBJETO = 2048;
constexpr uint16_t MAX_FLUXO = MAX_OBJETO + CABECALHO_OBJETO;
constexpr uint8_t MAX_QUADROS = (MAX_FLUXO + DADOS_POR_QUADRO - 1) / DADOS_POR_QUADRO;
constexpr uint8_t MAPA_BYTES = (MAX_QUADROS + 7) / 8;
constexpr uint8_t LADO_IMAGEM = 32;
constexpr uint16_t BYTES_MONO = LADO_IMAGEM * LADO_IMAGEM / 8;

struct Quadro {
  uint8_t tipo = 0, seq = 0, len = 0;
  uint8_t dados[MAX_DATA] = {};
};

inline uint8_t crc8Byte(uint8_t crc, uint8_t dado) {
  crc ^= dado;
  for (uint8_t i = 0; i < 8; ++i)
    crc = (crc & 0x80) ? uint8_t((crc << 1) ^ 0x07) : uint8_t(crc << 1);
  return crc;
}

inline uint8_t fcs(const Quadro& q) {
  uint8_t crc = crc8Byte(crc8Byte(crc8Byte(0, q.tipo), q.seq), q.len);
  for (uint8_t i = 0; i < q.len; ++i) crc = crc8Byte(crc, q.dados[i]);
  return crc;
}

inline uint32_t crc32(const uint8_t* bytes, uint16_t tamanho) {
  uint32_t crc = 0xFFFFFFFFUL;
  while (tamanho--) {
    crc ^= *bytes++;
    for (uint8_t i = 0; i < 8; ++i)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0);
  }
  return crc ^ 0xFFFFFFFFUL;
}

inline void gravar32(uint8_t* out, uint32_t numero) {
  for (uint8_t i = 0; i < 4; ++i) out[i] = uint8_t(numero >> (8 * i));
}

inline uint32_t ler32(const uint8_t* in) {
  uint32_t numero = 0;
  for (uint8_t i = 0; i < 4; ++i) numero |= uint32_t(in[i]) << (8 * i);
  return numero;
}

inline bool objetoPermitido(char tipo, uint16_t bytes) {
  if (bytes == 0 || bytes > MAX_OBJETO) return false;
  return tipo == 'T' || tipo == 'P' || tipo == 'J' || (tipo == 'M' && bytes == BYTES_MONO);
}

inline uint8_t quantidadeQuadros(uint16_t bytes) {
  return (bytes + CABECALHO_OBJETO + DADOS_POR_QUADRO - 1) / DADOS_POR_QUADRO;
}

inline bool tem(const uint8_t* mapa, uint8_t seq) {
  return mapa[seq / 8] & (1U << (seq % 8));
}

inline void marcar(uint8_t* mapa, uint8_t seq) {
  mapa[seq / 8] |= 1U << (seq % 8);
}

struct Emissor {
  uint8_t fluxo[MAX_FLUXO] = {};
  uint8_t confirmados[MAPA_BYTES] = {}, enviados[MAPA_BYTES] = {};
  uint16_t tamanho = 0;
  uint8_t total = 0, base = 0, janela = 1;
  uint32_t id = 0;

  // A entrada USB ja coloca os bytes em fluxo + CABECALHO_OBJETO.
  void iniciar(char tipo, uint16_t bytes, uint32_t sessao, bool seletivo) {
    fluxo[0] = tipo;
    fluxo[1] = uint8_t(bytes);
    fluxo[2] = uint8_t(bytes >> 8);
    gravar32(fluxo + 3, crc32(fluxo + CABECALHO_OBJETO, bytes));
    tamanho = bytes + CABECALHO_OBJETO;
    total = quantidadeQuadros(bytes);
    base = 0;
    janela = seletivo ? 4 : 1;
    id = sessao;
    memset(confirmados, 0, sizeof(confirmados));
    memset(enviados, 0, sizeof(enviados));
  }

  uint8_t montarRodada(uint8_t* rodada) const {
    uint8_t n = 0;
    for (uint16_t seq = base; seq < total && seq < base + janela; ++seq)
      if (!tem(confirmados, seq)) rodada[n++] = seq;
    return n;
  }

  Quadro dados(uint8_t seq, uint8_t seguintes) const {
    Quadro q;
    q.tipo = DATA;
    q.seq = seq;
    q.len = MAX_DATA;
    gravar32(q.dados, id);
    q.dados[4] = seguintes | (janela == 4 ? 4 : 0);
    q.dados[5] = base;
    uint16_t offset = uint16_t(seq) * DADOS_POR_QUADRO;
    uint16_t bytes = tamanho - offset;
    if (bytes > DADOS_POR_QUADRO) bytes = DADOS_POR_QUADRO;
    memcpy(q.dados + 6, fluxo + offset, bytes);
    return q;
  }

  uint8_t confirmar(uint8_t inicio, uint8_t mascara) {
    uint8_t novos = 0;
    for (uint8_t i = 0; i < janela && uint16_t(inicio) + i < total; ++i) {
      uint8_t seq = inicio + i;
      if ((mascara & (1U << i)) && tem(enviados, seq) && !tem(confirmados, seq)) {
        marcar(confirmados, seq);
        novos |= 1U << i;
      }
    }
    while (base < total && tem(confirmados, base)) ++base;
    return novos;
  }
};

struct Receptor {
  uint8_t fluxo[MAX_QUADROS * DADOS_POR_QUADRO] = {};
  uint8_t recebidos[MAPA_BYTES] = {};
  uint32_t id = 0;
  bool ativa = false, entregue = false, seletivo = false;

  bool receber(const Quadro& q, bool& novo) {
    if (q.len != MAX_DATA || q.seq >= MAX_QUADROS) return false;
    uint8_t controle = q.dados[4], base = q.dados[5];
    bool modo = controle & 4;
    if (controle > 7 || (!modo && (controle & 3))) return false;
    if (q.seq < base || q.seq - base >= (modo ? 4 : 1)) return false;
    uint32_t sessao = ler32(q.dados);
    if (!ativa || sessao != id) {
      ativa = true;
      entregue = false;
      seletivo = modo;
      id = sessao;
      memset(recebidos, 0, sizeof(recebidos));
    } else if (modo != seletivo) {
      return false;
    }
    novo = !tem(recebidos, q.seq);
    if (novo) {
      memcpy(fluxo + uint16_t(q.seq) * DADOS_POR_QUADRO, q.dados + 6, DADOS_POR_QUADRO);
      marcar(recebidos, q.seq);
    }
    return true;
  }

  uint8_t mascara(uint8_t base) const {
    uint8_t bits = 0;
    for (uint8_t i = 0; i < (seletivo ? 4 : 1) && uint16_t(base) + i < MAX_QUADROS; ++i)
      if (tem(recebidos, base + i)) bits |= 1U << i;
    return bits;
  }

  uint16_t tamanho() const { return uint16_t(fluxo[1]) | (uint16_t(fluxo[2]) << 8); }
  const uint8_t* objeto() const { return fluxo + CABECALHO_OBJETO; }

  bool finalizar(uint8_t total) const {
    if (!tem(recebidos, 0) || !objetoPermitido(char(fluxo[0]), tamanho()) ||
        quantidadeQuadros(tamanho()) != total) return false;
    for (uint8_t seq = 0; seq < total; ++seq) if (!tem(recebidos, seq)) return false;
    return ler32(fluxo + 3) == crc32(objeto(), tamanho());
  }
};
}


namespace ManchesterRF {
constexpr uint16_t SINCRONISMO = 0xD391;
constexpr uint32_t MEIO_BIT_US = 5000;

class Decodificador {
  ProjetoRF::Quadro candidato;
  uint16_t janela = 0;
  uint8_t bits = 0, byteLido = 0, campo = 0, crc = 0;
  bool sincronizado = false;

 public:
  uint32_t errosFcs = 0;

  void limpar() {
    janela = 0;
    bits = byteLido = campo = crc = 0;
    sincronizado = false;
  }

  bool receber(bool bit, ProjetoRF::Quadro& saida) {
    if (!sincronizado) {
      janela = uint16_t((janela << 1) | bit);
      sincronizado = janela == SINCRONISMO;
      return false;
    }

    byteLido = uint8_t((byteLido << 1) | bit);
    if (++bits < 8) return false;
    uint8_t dado = byteLido;
    bits = byteLido = 0;

    switch (campo++) {
      case 0:
        if (dado != ProjetoRF::FLAG) limpar();
        return false;
      case 1: candidato.tipo = dado; break;
      case 2: candidato.seq = dado; break;
      case 3:
        candidato.len = dado;
        if (dado > ProjetoRF::MAX_DATA) { limpar(); return false; }
        break;
      default:
        if (campo - 5 == candidato.len) {
          bool valido = dado == crc;
          if (valido) saida = candidato;
          else ++errosFcs;
          limpar();
          return valido;
        }
        candidato.dados[campo - 5] = dado;
        break;
    }
    crc = ProjetoRF::crc8Byte(crc, dado);
    return false;
  }
};

class Receptor {
  // Testa as duas fases Manchester, nas duas polaridades.
  Decodificador dec[4];
  bool primeiro[4] = {}, metade[4] = {};
  bool pular[4] = {false, true, false, true};
  bool pendente = false, nivelPendente = false;
  uint32_t tempoPendente = 0;

  bool meio(bool nivel, ProjetoRF::Quadro& quadro) {
    bool pronto = false;
    for (uint8_t i = 0; i < 4; ++i) {
      if (pular[i]) { pular[i] = false; continue; }
      if (!metade[i]) {
        primeiro[i] = nivel;
        metade[i] = true;
      } else {
        metade[i] = false;
        if (primeiro[i] == nivel) dec[i].limpar();
        else if (dec[i].receber((!primeiro[i]) ^ (i >= 2), quadro)) pronto = true;
      }
    }
    return pronto;
  }

  bool trecho(bool nivel, uint32_t tempo, ProjetoRF::Quadro& quadro) {
    if (tempo < 2500 || tempo > 12500) {
      limparFase();
      return false;
    }
    bool pronto = meio(nivel, quadro);
    if (tempo >= 7500 && meio(nivel, quadro)) pronto = true;
    return pronto;
  }

  void limparFase() {
    for (uint8_t i = 0; i < 4; ++i) {
      dec[i].limpar();
      metade[i] = false;
      pular[i] = i & 1;
    }
  }

  void somar(uint32_t tempo) {
    if (tempo > 100000 || tempoPendente > 100000 - tempo) tempoPendente = 100000;
    else tempoPendente += tempo;
  }

 public:
  void limpar() {
    limparFase();
    pendente = false;
  }

  uint32_t errosFcs() const {
    uint32_t soma = 0;
    for (const auto& item : dec) soma += item.errosFcs;
    return soma;
  }

  bool receber(bool nivel, uint32_t tempo, ProjetoRF::Quadro& quadro) {
    // Junta os trechos separados por pulsos espurios menores que 1 ms.
    if (tempo < 1000) {
      if (pendente) somar(tempo);
      return false;
    }
    if (pendente && nivel == nivelPendente) {
      somar(tempo);
      return false;
    }
    bool pronto = pendente && trecho(nivelPendente, tempoPendente, quadro);
    pendente = true;
    nivelPendente = nivel;
    tempoPendente = tempo;
    return pronto;
  }
};
}

using namespace ProjetoRF;

#ifndef COM_OLED
#error Defina COM_OLED no sketch
#endif
constexpr int TX_PIN = 26, RX_PIN = 27;
constexpr uint8_t PREAMBULO_DADOS = 32, PREAMBULO_ACK = 64;
constexpr uint32_t PAUSA_ACK_MS = 400, LIMITE_ACK_MS = 3500, PAUSA_APOS_ACK_MS = 250;
constexpr uint32_t INTERVALO_QUADROS_MS = 200;
// DATA tem 21 bytes de enlace, 16 bits de sincronismo e mais um byte de cauda.
constexpr uint32_t SLOT_DADOS_MS = (PREAMBULO_DADOS + 16 + (21 + 1) * 8) *
    (2 * ManchesterRF::MEIO_BIT_US) / 1000 + INTERVALO_QUADROS_MS + 30;

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
bool oledPronto = false;
Emissor emissor;
Receptor destino;
ManchesterRF::Receptor radio;

constexpr uint16_t FILA = 256;
volatile uint32_t duracoes[FILA];
volatile uint8_t niveis[FILA];
volatile uint16_t escrita = 0, leitura = 0;
volatile uint32_t ultimaBorda = 0;
volatile uint8_t nivelAnterior = LOW;
volatile bool transbordou = false, escutando = true;
portMUX_TYPE trava = portMUX_INITIALIZER_UNLOCKED;

char linha[12 + MAX_OBJETO * 2 + 1];
size_t tamanhoLinha = 0;
bool linhaLonga = false;
uint32_t proximoId = 0;
uint32_t inicioEnvio = 0, inicioEspera = 0, proximaRodada = 0;
uint32_t errosInformados = 0;
bool enviando = false, esperaAck = false, emFim = false, fimRepetido = false;
bool simularErro = false, erroInjetado = false;
uint8_t baseEnviada = 0;

bool respostaPendente = false, entregarAposAck = false;
uint8_t baseResposta = 0, mascaraResposta = 0;
uint32_t idResposta = 0, horarioResposta = 0;

void emitir(const ProjetoRF::Quadro& q, uint8_t preambulo, bool corromper);
void processar(const ProjetoRF::Quadro& q);

void IRAM_ATTR capturarBorda() {
  portENTER_CRITICAL_ISR(&trava);
  if (escutando) {
    uint32_t agora = micros();
    uint16_t proximo = (escrita + 1) & (FILA - 1);
    if (proximo == leitura) transbordou = true;
    else {
      duracoes[escrita] = agora - ultimaBorda;
      niveis[escrita] = nivelAnterior;
      escrita = proximo;
    }
    ultimaBorda = agora;
    nivelAnterior = digitalRead(RX_PIN);
  }
  portEXIT_CRITICAL_ISR(&trava);
}

void mudarEscuta(bool ativar) {
  portENTER_CRITICAL(&trava);
  escutando = ativar;
  leitura = escrita;
  transbordou = false;
  nivelAnterior = digitalRead(RX_PIN);
  ultimaBorda = micros();
  portEXIT_CRITICAL(&trava);
  radio.limpar();
}

void enviarBit(bool bit) {
  digitalWrite(TX_PIN, bit ? LOW : HIGH);
  delayMicroseconds(ManchesterRF::MEIO_BIT_US);
  digitalWrite(TX_PIN, bit ? HIGH : LOW);
  delayMicroseconds(ManchesterRF::MEIO_BIT_US);
}

void enviarByte(uint8_t dado) {
  for (int i = 7; i >= 0; --i) enviarBit((dado >> i) & 1);
}

void emitir(const Quadro& q, uint8_t preambulo, bool corromper) {
  uint8_t crc = fcs(q);
  mudarEscuta(false);
  for (uint8_t i = 0; i < preambulo; ++i) enviarBit(false);
  enviarByte(uint8_t(ManchesterRF::SINCRONISMO >> 8));
  enviarByte(uint8_t(ManchesterRF::SINCRONISMO));
  enviarByte(FLAG);
  enviarByte(q.tipo);
  enviarByte(q.seq);
  enviarByte(q.len);
  for (uint8_t i = 0; i < q.len; ++i) enviarByte(q.dados[i] ^ (corromper && i == 6 ? 1 : 0));
  enviarByte(crc);
  enviarByte(0);  // As bordas da cauda permitem fechar o FCS.
  digitalWrite(TX_PIN, LOW);
  mudarEscuta(true);
}

const char* nomeTipo(char tipo) { return tipo == 'T' ? "Texto recebido" : "Imagem recebida"; }

void imprimirOled(char tipo) {
  if (!oledPronto) return;
  oled.clearDisplay();
  oled.setCursor(0, 0);
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setTextWrap(true);
  oled.println(nomeTipo(tipo));
  oled.setCursor(0, 16);
  if (tipo == 'T') {
    // Preserva UTF-8 na serial. A fonte do OLED exibe ASCII e aproxima acentos comuns.
    static const char mapa[] = "AAAAAA?CEEEEIIII?NOOOOO?OUUUUY??aaaaaa?ceeeeiiii?nooooo?ouuuuy?y";
    const uint8_t* bytes = destino.objeto();
    uint16_t n = destino.tamanho();
    for (uint16_t i = 0; i < n && oled.getCursorY() < 64; ++i) {
      uint8_t b = bytes[i];
      if (b < 128) oled.write(b == '\t' ? ' ' : b);
      else {
        if (b == 0xC3 && i + 1 < n && bytes[i + 1] >= 0x80 && bytes[i + 1] <= 0xBF) {
          uint8_t indice = bytes[++i] - 0x80;
          oled.write(indice < sizeof(mapa) - 1 ? mapa[indice] : '?');
        } else if ((b & 0xC0) != 0x80) oled.write('?');
      }
    }
  } else {
    oled.print("Veja no computador.");
  }
  oled.display();
}

void entregarObjeto() {
  char tipo = char(destino.fluxo[0]);
  Serial.printf("@RECEBIDO %c ", tipo);
  const char* hex = "0123456789ABCDEF";
  const uint8_t* bytes = destino.objeto();
  char bloco[128];
  size_t usados = 0;
  for (uint16_t i = 0; i < destino.tamanho(); ++i) {
    bloco[usados++] = hex[bytes[i] >> 4];
    bloco[usados++] = hex[bytes[i] & 15];
    if (usados == sizeof(bloco)) {
      Serial.write((const uint8_t*)bloco, usados);
      usados = 0;
    }
  }
  if (usados) Serial.write((const uint8_t*)bloco, usados);
  Serial.println();
  imprimirOled(tipo);
}

void enviarResposta() {
  Quadro q;
  q.tipo = ACK;
  q.seq = baseResposta;
  q.len = 5;
  gravar32(q.dados, idResposta);
  q.dados[4] = mascaraResposta;
  respostaPendente = false;
  emitir(q, PREAMBULO_ACK, false);
  Serial.printf("ACK enviado | SEQ=%u | mapa=0x%X\n", unsigned(q.seq), unsigned(q.dados[4]));
  if (entregarAposAck) {
    entregarAposAck = false;
    entregarObjeto();
  }
}

void processar(const Quadro& q) {
  if (q.tipo == ACK && q.len == 5 && enviando && esperaAck && q.seq == baseEnviada && ler32(q.dados) == emissor.id) {
    if (emFim) {
      if (q.dados[4] == 0) {
        // O destino pode ter perdido a sessao ou detectado erro no objeto inteiro.
        uint16_t bytes = emissor.tamanho - CABECALHO_OBJETO;
        if (++proximoId == 0) ++proximoId;
        emissor.iniciar(char(emissor.fluxo[0]), bytes, proximoId, emissor.janela == 4);
        esperaAck = emFim = fimRepetido = false;
        proximaRodada = millis() + PAUSA_APOS_ACK_MS;
        Serial.println("END recusado pelo destino | reenviando o objeto inteiro");
        return;
      }
      if (q.dados[4] != 1) return;
      esperaAck = enviando = false;
      Serial.printf("@FIM %lu\n", (unsigned long)(millis() - inicioEnvio));
      return;
    }
    uint8_t novos = emissor.confirmar(q.seq, q.dados[4]);
    for (uint8_t i = 0; i < emissor.janela; ++i)
      if (novos & (1U << i)) Serial.printf("ACK recebido | quadro %u/%u\n", unsigned(q.seq + i + 1), unsigned(emissor.total));
    esperaAck = false;
    proximaRodada = millis() + PAUSA_APOS_ACK_MS;
    return;
  }

  if (enviando) return;
  if (q.tipo == DATA) {
    bool novo;
    if (!destino.receber(q, novo)) return;
    uint8_t seguintes = q.dados[4] & 3;
    idResposta = destino.id;
    baseResposta = q.dados[5];
    mascaraResposta = destino.mascara(baseResposta);
    horarioResposta = millis() + PAUSA_ACK_MS + seguintes * SLOT_DADOS_MS;
    respostaPendente = true;
    entregarAposAck = false;
    Serial.printf("RX DATA | SEQ=%u | %s\n", unsigned(q.seq), novo ? "novo" : "duplicado");
    return;
  }

  if (q.tipo == END && q.len == 4) {
    uint32_t sessao = ler32(q.dados);
    bool valido = destino.ativa && sessao == destino.id && destino.finalizar(q.seq);
    idResposta = sessao;
    baseResposta = q.seq;
    mascaraResposta = valido ? 1 : 0;
    horarioResposta = millis() + PAUSA_ACK_MS;
    respostaPendente = true;
    entregarAposAck = valido && (entregarAposAck || !destino.entregue);
    if (valido) {
      destino.entregue = true;
      Serial.println("RX END | CRC-32 do objeto conferido");
    } else {
      Serial.println("END recusado | sessao, comprimento, quadros ou CRC-32 do objeto invalidos");
    }
  }
}

void lerRadio() {
  for (uint16_t i = 0; i < FILA; ++i) {
    uint32_t duracao = 0;
    bool nivel = false, disponivel = false, perdeu = false;
    portENTER_CRITICAL(&trava);
    if (transbordou) {
      leitura = escrita;
      transbordou = false;
      perdeu = true;
    } else if (leitura != escrita) {
      duracao = duracoes[leitura];
      nivel = niveis[leitura];
      leitura = (leitura + 1) & (FILA - 1);
      disponivel = true;
    }
    portEXIT_CRITICAL(&trava);
    if (perdeu) radio.limpar();
    if (!disponivel) break;
    Quadro q;
    if (radio.receber(nivel, duracao, q)) processar(q);
  }
  portENTER_CRITICAL(&trava);
  uint32_t silencio = micros() - ultimaBorda;
  bool vazia = leitura == escrita;
  portEXIT_CRITICAL(&trava);
  if (vazia && silencio > 30000) radio.limpar();
  uint32_t erros = radio.errosFcs();
  if (erros != errosInformados) {
    errosInformados = erros;
    Serial.printf("FCS INVALIDO | quadros descartados=%lu\n", (unsigned long)erros);
  }
}

bool lerNibble(char c, uint8_t& n) {
  if (c >= '0' && c <= '9') n = c - '0';
  else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
  else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
  else return false;
  return true;
}

void processarLinha() {
  if (!tamanhoLinha && !linhaLonga) return;
  linha[tamanhoLinha] = '\0';
  if (strcmp(linha, "IDENTIFICAR") == 0) {
    Serial.printf("@PRONTO RF1 %c %s\n", COM_OLED ? 'B' : 'A', enviando ? "OCUPADO" : "LIVRE");
    return;
  }
  if (strcmp(linha, "CANCELAR") == 0) {
    if (enviando) {
      enviando = esperaAck = false;
      Serial.println("@CANCELADO");
    }
    return;
  }
  // A inicia DATA/END; B transmite somente ACK. Assim as duas placas nao
  // iniciam objetos simultaneamente e deixam de ignorar DATA uma da outra.
  if (COM_OLED && strncmp(linha, "ENVIA ", 6) == 0) {
    Serial.println("@ERRO Placa B apenas recebe e confirma");
    return;
  }
  if (enviando || respostaPendente) { Serial.println("@ERRO Radio ocupado"); return; }
  // ENVIA <S|R> <T|M|P|J> <0|1> <hexadecimal>
  if (linhaLonga || tamanhoLinha < 14 || strncmp(linha, "ENVIA ", 6) ||
      (linha[6] != 'S' && linha[6] != 'R') || linha[7] != ' ' || linha[9] != ' ' ||
      (linha[10] != '0' && linha[10] != '1') || linha[11] != ' ' || (tamanhoLinha - 12) % 2) {
    Serial.println("@ERRO Comando USB invalido");
    return;
  }
  uint16_t bytes = (tamanhoLinha - 12) / 2;
  if (!objetoPermitido(linha[8], bytes)) { Serial.println("@ERRO Objeto vazio, formato ou tamanho invalido"); return; }
  for (uint16_t i = 0; i < bytes; ++i) {
    uint8_t alto, baixo;
    if (!lerNibble(linha[12 + i * 2], alto) || !lerNibble(linha[13 + i * 2], baixo)) {
      Serial.println("@ERRO Dados USB invalidos");
      return;
    }
    emissor.fluxo[CABECALHO_OBJETO + i] = (alto << 4) | baixo;
  }
  if (++proximoId == 0) ++proximoId;
  emissor.iniciar(linha[8], bytes, proximoId, linha[6] == 'R');
  simularErro = linha[10] == '1';
  erroInjetado = esperaAck = emFim = fimRepetido = false;
  enviando = true;
  inicioEnvio = proximaRodada = millis();
  Serial.printf("@INICIO %u %u\n", unsigned(bytes), unsigned(emissor.total));
}

void lerSerial() {
  for (int i = 0; i < 512 && Serial.available(); ++i) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\n' || c == '\r') {
      processarLinha();
      tamanhoLinha = 0;
      linhaLonga = false;
    } else if (c > 0) {
      if (tamanhoLinha < sizeof(linha) - 1) linha[tamanhoLinha++] = char(c);
      else linhaLonga = true;
    }
  }
}

void avancarEmissor() {
  if (esperaAck) {
    if (uint32_t(millis() - inicioEspera) < LIMITE_ACK_MS) return;
    esperaAck = false;
    Serial.println("Timeout | reenviando o que ficou sem ACK");
  }
  if (int32_t(millis() - proximaRodada) < 0) return;
  if (emissor.base == emissor.total) {
    Quadro q;
    q.tipo = END;
    q.seq = emissor.total;
    q.len = 4;
    gravar32(q.dados, emissor.id);
    emFim = true;
    baseEnviada = q.seq;
    Serial.println(fimRepetido ? "RETX END" : "TX END");
    fimRepetido = true;
    emitir(q, PREAMBULO_DADOS, false);
  } else {
    uint8_t rodada[4];
    uint8_t n = emissor.montarRodada(rodada);
    baseEnviada = emissor.base;
    for (uint8_t i = 0; i < n && enviando; ++i) {
      uint8_t seq = rodada[i];
      Quadro q = emissor.dados(seq, n - i - 1);
      bool repetido = tem(emissor.enviados, seq);
      bool falha = simularErro && !erroInjetado && seq == (emissor.total > 1 ? 1 : 0);
      marcar(emissor.enviados, seq);
      if (falha) erroInjetado = true;
      Serial.printf("%s DATA | quadro %u/%u | SEQ=%u%s\n", repetido ? "RETX" : "TX",
                    unsigned(seq + 1), unsigned(emissor.total), unsigned(seq), falha ? " | ERRO INJETADO" : "");
      emitir(q, PREAMBULO_DADOS, falha);
      if (i + 1 < n) {
        lerSerial();
        delay(INTERVALO_QUADROS_MS);
      }
    }
    if (!enviando) return;
  }
  inicioEspera = millis();
  esperaAck = true;
}

void setup() {
  Serial.setRxBufferSize(8192);
  Serial.begin(115200);
  pinMode(TX_PIN, OUTPUT);
  digitalWrite(TX_PIN, LOW);
  pinMode(RX_PIN, INPUT);
  proximoId = esp_random();
  if (COM_OLED) {
    Wire.begin(21, 22);
    for (uint8_t endereco : {0x3C, 0x3D}) {
      Wire.beginTransmission(endereco);
      if (!Wire.endTransmission()) { oledPronto = oled.begin(SSD1306_SWITCHCAPVCC, endereco, false, false); break; }
    }
    if (oledPronto) {
      oled.clearDisplay();
      oled.setTextColor(SSD1306_WHITE);
      oled.setCursor(0, 0);
      oled.println("Aguardando mensagem");
      oled.display();
    } else Serial.println("OLED indisponivel. Confira SDA 21, SCL 22 e alimentacao.");
  }
  mudarEscuta(true);
  attachInterrupt(digitalPinToInterrupt(RX_PIN), capturarBorda, CHANGE);
  Serial.printf("@PRONTO RF1 %c LIVRE\n", COM_OLED ? 'B' : 'A');
}

void loop() {
  lerRadio();
  lerSerial();
  if (respostaPendente && int32_t(millis() - horarioResposta) >= 0) enviarResposta();
  if (enviando) avancarEmissor();
  delay(1);
}

