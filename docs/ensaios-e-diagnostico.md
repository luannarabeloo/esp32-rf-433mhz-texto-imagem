# Ensaios, falhas e diagnóstico

Este documento resume a evidência experimental usada no [relatório em LaTeX](../relatorio/relatorio.tex). A coleta local de 24/11/2026 contém 37 arquivos `log_COM*.txt`. O [índice](../evidencias/indice_logs.csv) lista cada arquivo e suas contagens sem publicar textos pessoais nem caminhos do computador. Os quatro logs pareados mais representativos estão em [`evidencias/`](../evidencias/). Os demais logs foram auditados localmente; o índice não substitui as capturas completas.

## O que os registros mostram

| Evidência | Resultado | Limite da conclusão |
|---|---|---|
| COM3/COM5, 09:01 | Texto de 20 bytes em Stop-and-Wait, `@FIM 131294`, arquivo recebido e SHA-256 igual | Prova essa entrega; não representa a taxa média do enlace |
| COM3/COM5, 08:26 | JPEG compacto de 664 bytes em Stop-and-Wait, arquivo recebido e SHA-256 igual | As duas placas estavam identificadas como A nessa sessão, portanto ela não demonstra o OLED B |
| COM3/COM5, 08:34 | Texto de 42 bytes em Selective Repeat, arquivo salvo | Modos S e R usaram cargas diferentes, então não há comparação de velocidade controlada |
| COM5→COM3, 09:23 | `ERRO INJETADO`, `FCS INVALIDO`, retransmissão e entrega com hash igual | Ensaio histórico em sentido inverso ao papel atual de A/B; corrupção de byte, não ruído externo |
| COM3→COM5, 09:25 | Segundo ensaio com `ERRO INJETADO`, `FCS INVALIDO`, retransmissão e entrega | Demonstra detecção e recuperação no sentido atual A→B |
| COM3, 19:22–19:42 | Oito `@FIM` após a restrição para só A iniciar dados | O log remoto B dessa sessão não está no acervo local; não confirma caminhos de arquivos salvos |

Na auditoria dos 37 logs há 60 comandos de envio, 29 registros `@FIM`, 18 cancelamentos e 13 envios sem desfecho no mesmo arquivo. Há 16 objetos com `RECEBIDO:`, 2 eventos de erro injetado, 2 de FCS inválido e 12 avisos de desconexão serial. Foram registradas 1160 linhas de timeout e 1672 de retransmissão DATA. Essas são **contagens de mensagens de log**, não 60 ensaios independentes nem uma taxa estatística de erro: sessões têm objetos e durações diferentes, e algumas capturas terminam no meio de um envio.

## Por que o sistema pode deixar de entregar

1. **Nenhum ACK chega ao emissor.** O sintoma é `Timeout` e `RETX DATA` repetidos para o mesmo quadro. No log COM3 iniciado às 18:33, um texto de 16 bytes ficou no primeiro quadro com 53 retransmissões e sem ACK antes do fim da captura. A partir de apenas um log não se sabe se o DATA não chegou, se o ACK voltou corrompido, se há falha de alimentação/conexão ou se a placa estava ocupada.
2. **Duas iniciações disputavam o rádio no protótipo anterior.** Enquanto uma placa enviava, o firmware atendia aos ACKs da própria sessão e descartava DATA/END de outra. A versão atual impede o Python da placa B de iniciar objetos. B continua transmitindo ACKs. Essa alteração coincide com mais conclusões posteriores, mas não houve ensaio controlado isolando sua influência de mudanças físicas.
3. **Temporização apertada entre quadros.** A pausa do Selective Repeat foi ampliada de 12 ms para 200 ms. A pausa tenta dar tempo à recuperação do receptor. Ela aumenta o tempo de envio. Não foi medida, em condições idênticas, a contribuição isolada desse ajuste.
4. **Porta serial se desconecta.** O erro `ClearCommError failed` e outros avisos de desconexão aparecem nos logs. Eles dizem respeito à ligação PC–ESP32, não provam falha do RF. Conferir cabo, COM e programas que usam a porta é parte do diagnóstico.
5. **Sketch errado na placa do OLED.** Em alguns testes, as duas placas anunciaram `@PRONTO RF1 A`. Nesse caso a placa física com OLED não estava exercitando o papel B esperado. Confirmar `A` no PC emissor e `B` no receptor antes de enviar.
6. **O terminal do PC receptor precisa ficar aberto.** `@FIM` no emissor significa ACK final recebido. Para verificar o arquivo no segundo PC, procurar também `RECEBIDO:` e o SHA-256 no log B. Se o terminal Python B não estiver escutando a serial, o arquivo pode não aparecer.

## Roteiro curto para a apresentação

1. Com USB e RF ligados, abrir o Python em cada PC e confirmar `@PRONTO RF1 A` no emissor e `@PRONTO RF1 B` no receptor. Manter ambos os terminais abertos.
2. Enviar uma frase curta com `s` e `t`. Conferir `@FIM` em A, `RECEBIDO:` em B, o texto no OLED e hashes iguais.
3. Repetir uma carga curta com `r`, observando ACKs e eventual retransmissão. Para comparar os modos, usar **a mesma carga**, posição e alimentação.
4. Usar `x` com uma frase curta para mostrar `ERRO INJETADO`, `FCS INVALIDO`, retransmissão e entrega final. O erro proposital já foi demonstrado em dois logs históricos.
5. Usar `i` e `m` para a imagem monocromática rápida; para JPEG colorido, reservar vários minutos. Conferir arquivo no PC B.
6. Se o professor solicitar o requisito de alcance, medir com trena uma separação **superior a 1 m** e registrar a distância. A foto atual mostra somente o arranjo que o grupo estimou em 0,8–1,0 m.

Ainda faltam evidências de interferência **física** externa controlada, medição acima de 1 m e fotografia do texto já exibido no OLED. A escolha de OLED no lugar de LCD também depende de confirmação do professor. Essas lacunas estão marcadas no relatório e não são apresentadas como testes concluídos.
