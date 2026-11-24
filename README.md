# Projeto RF 433 MHz: texto e imagem

Esta pasta deve ser copiada para **os dois computadores**. O PC A envia por USB ao seu ESP32; os dados atravessam somente os módulos RF e chegam ao ESP32 B. O PC B recebe por USB e salva a imagem reconstruída em `recebidos`. O OLED da placa B mostra o **texto**; para imagem, mostra apenas um aviso para olhar o arquivo no computador.

## Montagem

| Ligação | Placa A (TX) | Placa B (RX com OLED) |
|---|---|---|
| DATA do transmissor RF | GPIO 26 | GPIO 26 (devolve ACK) |
| DATA do receptor RF | GPIO 27 (recebe ACK) | GPIO 27 |
| OLED I2C | — | SDA 21, SCL 22 |

Cada placa precisa de **um módulo TX e um RX de 433 MHz**. Ligue o GND de cada módulo ao GND do seu ESP32 e alimente cada módulo conforme seu modelo. A saída de DATA que entra no ESP32 deve ter nível compatível com 3,3 V. Os computadores não se comunicam por Wi-Fi ou cabo entre si; cada USB liga apenas o PC à sua placa.

## Instalação

1. Na Arduino IDE, selecione **ESP32 Dev Module**. Instale **Adafruit GFX Library** e **Adafruit SSD1306** pelo Gerenciador de Bibliotecas. Instale também a biblioteca comum do projeto: **Sketch > Incluir Biblioteca > Adicionar Biblioteca .ZIP...** e escolha `RadioProjeto.zip` desta pasta.
2. Grave `tx_final/tx_final.ino` na placa **A** e `rx_final/rx_final.ino` na placa **B**. São dois sketches separados que usam a mesma biblioteca `RadioProjeto`; isso evita manter duas cópias de mais de 700 linhas.
3. Em **cada** PC, abra PowerShell nesta pasta e execute:

   ```powershell
   py -m pip install -r .\requirements.txt
   py .\projeto_rf.py
   ```

   Se `py` não existir, use `python`. Feche o Monitor Serial da Arduino IDE antes de iniciar o Python. Escolha a porta **COM local** mostrada pelo programa e confirme que ele identificou `A` ou `B`.

   O Python precisa dos pacotes `pyserial` e `Pillow` listados em `requirements.txt`. Instale-os no mesmo interpretador usado para iniciar o programa.

## Usar

Deixe o Python aberto nos dois PCs. No **PC A**, digite `s` para **Stop-and-Wait** ou `r` para **Selective Repeat** (janela de quatro quadros). Depois use `t` para digitar texto ou `i` para informar o **caminho completo de uma imagem**. É possível alternar entre texto e imagem e mudar o ARQ a cada novo envio. `x` liga/desliga a corrupção proposital de um quadro para demonstrar descarte e retransmissão; `c` cancela; `q` encerra.

Para imagem, **Enter** envia uma foto colorida compactada como JPEG, com no máximo **32 × 32 pixels**. Ela continua reconhecível como foto, mas perde resolução para caber no rádio; o JPEG recebido contém exatamente os bytes enviados. Escolha `m` para a versão preta e branca de 128 bytes, mais rápida. Texto tem limite de 2.048 bytes em UTF-8. O programa imprime o caminho do arquivo recebido e o SHA-256 da carga em ambos os PCs.

Os arquivos e logs ficam em `recebidos`, ao lado de `projeto_rf.py`, no PC que recebeu. **Mantenha o Python da placa B aberto durante todo o envio**: o ESP32 consegue confirmar um objeto mesmo que o programa do PC B não esteja aberto, mas nesse caso o arquivo não será salvo. Envie uma mensagem por vez. Para a apresentação, comece com `s`, `t` e uma frase curta; depois teste `r` e `i`. A foto compacta leva vários minutos na taxa atual; `m` é a melhor opção para o teste rápido. Teste a comunicação e o ACK de volta nas placas reais antes de usar uma imagem maior.

## Protocolo e limites

O quadro RF tem `FLAG | TYPE | SEQ | LEN | DATA | FCS`, com até 16 bytes em `DATA` e 21 bytes no total. `TYPE` é DATA, ACK ou END. O FCS é CRC-8, polinômio `0x07`, calculado sobre `TYPE`, `SEQ`, `LEN` e `DATA`; quadros com FCS diferente são descartados. O objeto completo também tem CRC-32. Stop-and-Wait espera o ACK de cada DATA. Selective Repeat confirma separadamente até quatro DATA por rodada e retransmite apenas os que faltaram. Entre quadros da mesma rodada há uma pausa de 200 ms para dar mais tempo aos módulos RF. O enlace usa codificação Manchester e transmissão RF bruta, sem RadioHead.

**Estado dos testes:** os logs em `evidencias` comprovam entregas de texto e imagem e recuperação após retransmissões. A distância informada foi de 80 cm a 1 m; o requisito de distância superior a 1 m e o teste com ruído controlado ainda precisam de evidências. O enunciado menciona LCD, enquanto a montagem informada usa OLED; confirmem com o professor a aceitação desse display.

O relatório editável para Overleaf está em `relatorio/relatorio.tex`, com as imagens em `relatorio/figuras`. Também há `relatorio_overleaf.zip` para importar tudo de uma vez. Os logs publicados tiveram somente o caminho local do usuário reduzido para `recebidos/`.
