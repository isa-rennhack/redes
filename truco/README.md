# 🎴 Jogo de Truco Espanhol - TCP/IP

Implementação do jogo de **Truco Espanhol** no modelo **cliente-servidor** utilizando o protocolo **TCP/IP**.

## 📋 Descrição

Este projeto implementa um jogo de Truco Espanhol multiplayer para 4 jogadores (2 duplas) com comunicação via sockets TCP/IP.

### Características:
- ✅ Protocolo TCP/IP
- ✅ Servidor multi-threaded (suporta 4 jogadores simultâneos)
- ✅ Baralho espanhol de 40 cartas
- ✅ Sistema de equipes (duplas)
- ✅ Distribuição de 3 cartas por jogador
- ✅ Comandos de truco/aceitar/rejeitar
- ✅ Interface de texto interativa

## 🃏 Regras do Truco Espanhol

### Baralho
- 40 cartas divididas em 4 naipes: **Ouro**, **Copas**, **Espadas**, **Paus**
- Valores (do menor ao maior): 4, 5, 6, 7, Sota, Cavalo, Rei, Ás, 2, 3

### Equipes
- 4 jogadores divididos em 2 duplas
- Jogadores 0 e 2 formam a Equipe 0
- Jogadores 1 e 3 formam a Equipe 1

### Rodada
- Cada jogador recebe 3 cartas
- Jogadores jogam uma carta por vez
- O jogador com a carta mais forte vence a rodada

### Truco
- Jogadores podem pedir "truco" para aumentar o valor da rodada
- Adversários podem aceitar ou rejeitar
- Se rejeitarem, a equipe que pediu truco ganha a rodada

## 🛠️ Compilação

### Requisitos
- Compilador C++ com suporte a C++11 (clang++ ou g++)
- Sistema Unix-like (Linux/macOS)
- Biblioteca pthread

### Compilar tudo:
```bash
make
```

### Compilar apenas o servidor:
```bash
make server
```

### Compilar apenas o cliente:
```bash
make client
```

### Limpar binários:
```bash
make clean
```

## 🚀 Execução

### 1. Iniciar o servidor

Em um terminal:
```bash
./truco_server
```

O servidor irá escutar na porta **8080** e aguardar 4 jogadores se conectarem.

### 2. Conectar clientes

Em outros 4 terminais diferentes (podem ser em máquinas diferentes na mesma rede):

**Conexão local:**
```bash
./truco_client
```

**Conexão remota:**
```bash
./truco_client <IP_DO_SERVIDOR> <PORTA>
```

Exemplo:
```bash
./truco_client 192.168.1.100 8080
```

### 3. Jogar

Após todos os 4 jogadores se conectarem, o jogo inicia automaticamente!

## 🎮 Comandos do Cliente

| Comando | Descrição |
|---------|-----------|
| `1`, `2`, `3` | Jogar a carta 1, 2 ou 3 da sua mão |
| `truco` | Pedir truco |
| `aceitar` | Aceitar o truco |
| `rejeitar` | Rejeitar o truco |
| `menu` | Mostrar comandos disponíveis |
| `sair` | Sair do jogo |

## 📡 Protocolo de Comunicação

### Mensagens Cliente → Servidor
- `JOGAR_CARTA|<numero>` - Jogar uma carta
- `TRUCO` - Pedir truco
- `ACEITAR` - Aceitar truco
- `REJEITAR` - Rejeitar truco

### Mensagens Servidor → Cliente
- `BEM_VINDO|<mensagem>` - Mensagem de boas-vindas
- `SUAS_CARTAS|<carta1>;<carta2>;<carta3>` - Cartas na mão
- `NOVA_RODADA|<mensagem>` - Nova rodada iniciada
- `TRUCO|<mensagem>` - Truco foi pedido
- `ACEITO|<mensagem>` - Truco foi aceito
- `REJEITADO|<mensagem>` - Truco foi rejeitado
- `DESCONEXAO|<mensagem>` - Jogador desconectou
- `ERRO|<mensagem>` - Mensagem de erro

## 🏗️ Estrutura do Projeto

```
truco/
├── truco_server.cpp    # Código do servidor
├── truco_client.cpp    # Código do cliente
├── Makefile            # Script de compilação
└── README.md           # Este arquivo
```

## 🔧 Arquitetura

### Servidor (`truco_server.cpp`)
- Aceita até 4 conexões TCP simultâneas
- Cada cliente é gerenciado por uma thread separada
- Controla o estado do jogo (rodadas, pontuação, cartas)
- Distribui cartas e processa comandos dos jogadores

### Cliente (`truco_client.cpp`)
- Conecta ao servidor via TCP
- Thread separada para receber mensagens do servidor
- Interface de linha de comando para interação
- Exibe as cartas e estado do jogo

## 📝 Melhorias Futuras

- [ ] Implementar hierarquia completa de cartas do truco espanhol
- [ ] Sistema de pontuação e vitória
- [ ] Envido e outros comandos especiais
- [ ] Interface gráfica (GUI)
- [ ] Logs detalhados das jogadas
- [ ] Modo espectador
- [ ] Reconexão automática
- [ ] Chat entre jogadores

## 👥 Autor

Desenvolvido como projeto de Redes de Computadores.

## 📄 Licença

Este projeto é de código aberto para fins educacionais.
