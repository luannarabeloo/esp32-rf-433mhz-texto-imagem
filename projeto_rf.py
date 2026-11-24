"""Terminal USB para os dois ESP32 do projeto RF. Requer pyserial e Pillow."""

import hashlib
from io import BytesIO
import sys
import threading
import time
from datetime import datetime
from pathlib import Path


PASTA = Path(__file__).resolve().parent / "recebidos"
LIMITE = 2048
for raiz in (Path(__file__).resolve().parent, Path(__file__).resolve().parent.parent):
    dependencias = raiz / ".deps"
    if dependencias.is_dir():
        sys.path.insert(0, str(dependencias))


def imagem_mono(caminho):
    from PIL import Image, ImageOps

    with Image.open(caminho) as origem:
        imagem = ImageOps.exif_transpose(origem).convert("RGB")
    imagem.thumbnail((32, 32), Image.Resampling.BICUBIC)
    tela = Image.new("RGB", (32, 32), "white")
    tela.paste(imagem, ((32 - imagem.width) // 2, (32 - imagem.height) // 2))
    pixels = tela.load()
    return bytes(
        sum(
            (128 >> bit) if sum(a * b for a, b in zip(pixels[x + bit, y], (299, 587, 114))) < 128000 else 0
            for bit in range(8)
        )
        for y in range(32) for x in range(0, 32, 8)
    )


def imagem_foto(caminho):
    from PIL import Image, ImageOps

    with Image.open(caminho) as origem:
        imagem = ImageOps.exif_transpose(origem).convert("RGB")
    imagem.thumbnail((32, 32), Image.Resampling.LANCZOS)
    saida = BytesIO()
    imagem.save(saida, format="JPEG", quality=55, optimize=True)
    return saida.getvalue()


def salvar(tipo, dados):
    if not 0 < len(dados) <= LIMITE:
        raise ValueError("tamanho recebido invalido")
    extensoes = {"T": ".txt", "M": ".png", "P": ".png", "J": ".jpg"}
    if tipo not in extensoes:
        raise ValueError("tipo recebido invalido")
    if tipo == "T":
        dados.decode("utf-8")
    elif tipo == "M":
        if len(dados) != 128:
            raise ValueError("imagem monocromatica invalida")
    elif not dados.startswith(b"\x89PNG\r\n\x1a\n" if tipo == "P" else b"\xff\xd8\xff"):
        raise ValueError("assinatura da imagem invalida")
    PASTA.mkdir(exist_ok=True)
    nome = "recebido_" + datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    destino = PASTA / (nome + extensoes[tipo])
    if tipo == "M":
        from PIL import Image

        Image.frombytes("1", (32, 32), bytes(b ^ 255 for b in dados)).save(destino)
    else:
        destino.write_bytes(dados)
    return destino


class Radio:
    def __init__(self, porta):
        import serial

        PASTA.mkdir(exist_ok=True)
        self.serial = serial.Serial(porta, 115200, timeout=0.2, write_timeout=2)
        self.registro = (PASTA / ("log_" + porta + "_" + datetime.now().strftime("%Y%m%d_%H%M%S") + ".txt")).open("w", encoding="utf-8")
        self.pronto = threading.Event()
        self.ocupado = threading.Event()
        self.parar = threading.Event()
        self.trava = threading.Lock()
        self.thread = threading.Thread(target=self.ler, daemon=True)
        self.thread.start()

    def log(self, mensagem):
        with self.trava:
            linha = datetime.now().strftime("%H:%M:%S") + " | " + mensagem
            print("\n" + linha, flush=True)
            self.registro.write(linha + "\n")
            self.registro.flush()

    def comando(self, linha):
        self.serial.write((linha + "\n").encode("ascii"))

    def ler(self):
        buffer = bytearray()
        try:
            while not self.parar.is_set():
                buffer.extend(self.serial.read(self.serial.in_waiting or 1))
                if len(buffer) > 5000:
                    buffer.clear()
                    self.log("Linha USB longa demais; descartada")
                while b"\n" in buffer:
                    bruto, _, resto = buffer.partition(b"\n")
                    buffer = bytearray(resto)
                    self.receber(bruto.decode("utf-8", errors="replace").strip())
        except Exception as erro:
            if not self.parar.is_set():
                self.log("Porta serial desconectada: " + str(erro))
                self.parar.set()

    def receber(self, linha):
        if linha.startswith("@PRONTO RF1 "):
            self.pronto.set()
            if linha.endswith("OCUPADO"):
                self.ocupado.set()
            self.log("ESP32 conectado: " + linha)
        elif linha.startswith("@RECEBIDO "):
            try:
                _, tipo, hexadecimal = linha.split(" ", 2)
                dados = bytes.fromhex(hexadecimal)
                caminho = salvar(tipo, dados)
                self.log("RECEBIDO: " + str(caminho))
                self.log("SHA-256 recebido: " + hashlib.sha256(dados).hexdigest())
                if tipo == "T":
                    self.log("Texto: " + dados.decode("utf-8"))
            except Exception as erro:
                self.log("Falha ao salvar recepcao: " + str(erro))
        elif linha:
            if linha.startswith(("@FIM ", "@ERRO ")) or linha == "@CANCELADO":
                self.ocupado.clear()
            self.log(linha)

    def enviar(self, modo, tipo, dados, erro):
        if self.ocupado.is_set():
            print("ESP32 ocupado; aguarde @FIM ou use c para cancelar.")
            return
        if not 0 < len(dados) <= LIMITE:
            print("Envio permitido: 1 a 2048 bytes.")
            return
        self.ocupado.set()
        try:
            self.comando(f"ENVIA {modo} {tipo} {int(erro)} {dados.hex().upper()}")
            self.log(f"ENVIO {tipo}, {len(dados)} bytes, modo {modo}; SHA-256: {hashlib.sha256(dados).hexdigest()}")
        except Exception:
            self.ocupado.clear()
            raise

    def fechar(self):
        if self.ocupado.is_set():
            try:
                self.comando("CANCELAR")
            except Exception:
                pass
        self.parar.set()
        self.thread.join(timeout=1)
        self.serial.close()
        self.registro.close()


def iniciar():
    from serial.tools import list_ports

    portas = list(list_ports.comports())
    if not portas:
        print("Nenhuma porta COM encontrada. Conecte o ESP32 por USB.")
        return
    for i, p in enumerate(portas, 1):
        print(f"{i}: {p.device} | {p.description}")
    escolha = input("Numero ou nome da COM local: ").strip()
    porta = portas[int(escolha) - 1].device if escolha.isdigit() else escolha.upper()
    radio = Radio(porta)
    try:
        print("Identificando ESP32...")
        for _ in range(8):
            if radio.pronto.wait(0.8):
                break
            radio.comando("IDENTIFICAR")
        if not radio.pronto.is_set():
            print("Sem resposta RF1. Confira a COM, o sketch e feche o Monitor Serial da IDE.")
            return
        modo, erro = "S", False
        print("Comandos: s=Stop-and-Wait  r=Selective Repeat  t=texto  i=imagem  x=erro  c=cancelar  q=sair")
        print("Arquivos recebidos:", PASTA)
        while not radio.parar.is_set():
            opcao = input(f"\n[{modo}] > ").strip().lower()
            if opcao in ("s", "r"):
                modo = opcao.upper()
                print("Modo:", "Stop-and-Wait" if modo == "S" else "Selective Repeat")
            elif opcao == "x":
                erro = not erro
                print("Corromper um quadro na primeira tentativa:", "sim" if erro else "nao")
            elif opcao == "t":
                texto = input("Texto: ").encode("utf-8")
                radio.enviar(modo, "T", texto, erro)
            elif opcao == "i":
                caminho = Path(input("Caminho da imagem (pode arrastar o arquivo aqui): ").strip().strip('"\'' )).expanduser()
                if not caminho.is_file():
                    print("Arquivo nao encontrado:", caminho)
                    continue
                formato = input("Enter=foto colorida compacta, m=preto e branco rapido: ").strip().lower()
                if formato == "m":
                    dados, tipo = imagem_mono(caminho), "M"
                elif not formato:
                    dados, tipo = imagem_foto(caminho), "J"
                    print("Foto compactada para", len(dados), "bytes (32x32 no maximo).")
                else:
                    print("Escolha Enter ou m.")
                    continue
                radio.enviar(modo, tipo, dados, erro)
            elif opcao == "c":
                radio.comando("CANCELAR")
            elif opcao == "q":
                break
            else:
                print("Use s, r, t, i, x, c ou q.")
    finally:
        radio.fechar()


if __name__ == "__main__":
    try:
        iniciar()
    except (KeyboardInterrupt, EOFError):
        print("\nEncerrado.")
    except Exception as erro:
        sys.exit("Erro: " + str(erro) + "\nConfira a COM, feche o Monitor Serial e instale pyserial e Pillow.")
