# Nome do binário
TARGET = server

# Arquivo fonte
SRC = server.cpp

# Compilador
CXX = g++

# Flags de compilação
CXXFLAGS = -O2 -Wall -pthread

# Bibliotecas
LIBS = -lpthread

# Pastas
LOGDIR = logs

.PHONY: all install-deps build run clean

# Alvo padrão
all: install-deps build

# Instalar dependências da Raspberry Pi
install-deps:
	@echo "==> Instalando dependências na Raspberry..."
	sudo apt update
	sudo apt install -y g++ nlohmann-json3-dev
	@echo "==> Dependências instaladas!"

# Compilar servidor
build: $(SRC)
	@echo "==> Compilando servidor..."
	$(CXX) $(SRC) -o $(TARGET) $(CXXFLAGS) $(LIBS)
	@echo "==> Compilação concluída: ./$(TARGET)"
	mkdir -p $(LOGDIR)

# Rodar servidor
run:
	@echo "==> Executando servidor..."
	./$(TARGET)

# Limpar binário e logs
clean:
	@echo "==> Limpando..."
	rm -f $(TARGET)
	rm -rf $(LOGDIR)
