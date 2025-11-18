import json
import logging
import socket
import sys
import time
from json.decoder import JSONDecodeError
from typing import Generator, Tuple

RASPBERRY_DEFAULT_HOST = "192.168.100.167"   # Endereço IP da Raspberry Pi na rede local
PFD_PORT = 12345
MFD_PORT = 12346

LOG_TO_FILE = False
LOG_FILE = "simulink_stream_log.jsonl"

LOGGER = logging.getLogger("simulink_monitor")


def configure_logging(verbose: bool = False) -> None:
    level = logging.DEBUG if verbose else logging.INFO
    logging.basicConfig(
        level=level,
        format="[%(asctime)s] [%(levelname)s] %(name)s: %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )


def append_log(line: str) -> None:
    if not LOG_TO_FILE:
        return

    try:
        with open(LOG_FILE, "a", encoding="utf-8") as file:
            file.write(line + "\n")
    except OSError as exc:
        LOGGER.error("Falha ao gravar log em '%s': %s", LOG_FILE, exc)


def iter_json_from_stream(sock: socket.socket) -> Generator[dict, None, None]:
    buffer = ""
    decoder = json.JSONDecoder()

    while True:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        except OSError as exc:
            LOGGER.error("Erro de recv no socket: %s", exc)
            break

        if not chunk:
            LOGGER.info("Conexão encerrada pelo servidor (EOF).")
            break

        piece = chunk.replace(b"\x00", b"").decode("utf-8", errors="ignore")
        buffer += piece

        while buffer:
            stripped = buffer.lstrip()
            offset = len(buffer) - len(stripped)
            if offset:
                buffer = stripped

            if not buffer:
                break

            try:
                obj, idx = decoder.raw_decode(buffer)
            except JSONDecodeError:
                break

            yield obj
            buffer = buffer[idx:]


def choose_host_port_from_args() -> Tuple[str, int, bool]:
    argv = sys.argv[1:]
    verbose = False

    if argv and argv[0] in ("-v", "--verbose"):
        verbose = True
        argv = argv[1:]

    if not argv:
        return RASPBERRY_DEFAULT_HOST, PFD_PORT, verbose

    if len(argv) == 1 and argv[0].lower() in {"pfd", "mfd"}:
        role = argv[0].lower()
        port = PFD_PORT if role == "pfd" else MFD_PORT
        return RASPBERRY_DEFAULT_HOST, port, verbose

    if len(argv) == 2:
        host = argv[0]
        try:
            port = int(argv[1])
        except ValueError:
            print(f"Porta inválida: {argv[1]}")
            sys.exit(1)
        return host, port, verbose

    print("Uso:")
    print("  python tablets.py")
    print("  python tablets.py pfd")
    print("  python tablets.py mfd")
    print("  python tablets.py <HOST> <PORTA>")
    print("  (pode usar -v ou --verbose antes dos argumentos acima)")
    sys.exit(1)


def main() -> None:
    host, port, verbose = choose_host_port_from_args()
    configure_logging(verbose)

    LOGGER.info("Conectando em %s:%d ...", host, port)

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(2.0)

    try:
        sock.connect((host, port))
    except OSError as exc:
        LOGGER.critical("Não foi possível conectar em %s:%d -> %s", host, port, exc)
        sys.exit(1)

    LOGGER.info("Conectado. Aguardando dados JSON do servidor...")
    LOGGER.info("Pressione Ctrl+C para encerrar.\n")

    try:
        for obj in iter_json_from_stream(sock):
            timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
            pretty_json = json.dumps(obj, ensure_ascii=False, indent=2)

            print(f"[{timestamp}] JSON recebido:")
            print(pretty_json)
            print()

            append_log(json.dumps(obj, ensure_ascii=False))

    except KeyboardInterrupt:
        LOGGER.info("Interrompido pelo usuário (Ctrl+C).")

    finally:
        try:
            sock.close()
        except OSError:
            pass
        LOGGER.info("Conexão encerrada.")


if __name__ == "__main__":
    main()
