/*
 * Cliente do Jogo de Truco Espanhol
 * Protocolo: TCP/IP
 */

#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define BUFFER_SIZE 1024

// Variáveis globais
int clientSocket = -1;
bool conectado = false;
pthread_t threadReceber;
bool minhaVez = false;
std::string nomeJogador = "";

// Declaração de funções
void mostrarOpcoesDisponiveis();
void mostrarOpcoesResposta();
void mostrarMenuCompleto();

// Função para receber mensagens do servidor
void* receberMensagens(void* arg) {
    char buffer[BUFFER_SIZE];
    
    while (conectado) {
        int bytesRecebidos = recv(clientSocket, buffer, BUFFER_SIZE - 1, 0);
        
        if (bytesRecebidos <= 0) {
            std::cout << "\n[DESCONECTADO] Conexão com servidor perdida." << std::endl;
            conectado = false;
            break;
        }
        
        buffer[bytesRecebidos] = '\0';
        std::string mensagem(buffer);
        
        // Processar mensagens do servidor
        if (mensagem.find("BEM_VINDO|") == 0) {
            std::string msg = mensagem.substr(10);
            std::cout << "\n" << msg << std::endl;
            
        } else if (mensagem.find("SUAS_CARTAS|") == 0) {
            std::string cartas = mensagem.substr(12);
            std::cout << "       SUAS CARTAS NA MÃO" << std::endl;
            std::cout << "═══════════════════════════════════════" << std::endl;
            
            size_t pos = 0;
            int indice = 1;
            while ((pos = cartas.find(";")) != std::string::npos) {
                std::cout << "[" << indice << "] " << cartas.substr(0, pos) << std::endl;
                cartas.erase(0, pos + 1);
                indice++;
            }
            if (!cartas.empty() && cartas != "\n") {
                std::cout << "[" << indice << "] " << cartas << std::endl;
            }
            std::cout << "\n" << std::endl;
            
        } else if (mensagem.find("NOVA_RODADA|") == 0) {
            std::string msg = mensagem.substr(12);
            std::cout << "\n  " << msg;
            std::cout << "═══════════════════════════════════════\n" << std::endl;
            
        } else if (mensagem.find("CARTA_JOGADA|") == 0) {
            std::string msg = mensagem.substr(13);
            std::cout << "\n🃏 " << msg << std::endl;
            
        } else if (mensagem.find("TRUCO|") == 0) {
            std::string msg = mensagem.substr(6);
            std::cout << "\n🎲 TRUCO! " << msg << std::endl;
            
            // Se não foi você que pediu, mostrar opções de resposta
            if (msg.find(nomeJogador) == std::string::npos) {
                mostrarOpcoesResposta();
            }
            
        } else if (mensagem.find("RETRUCO|") == 0) {
            std::string msg = mensagem.substr(8);
            std::cout << "\n🎲🎲 RETRUCO! " << msg << std::endl;
            
        } else if (mensagem.find("VALE4|") == 0) {
            std::string msg = mensagem.substr(6);
            std::cout << "\n🎲🎲🎲 VALE QUATRO! " << msg << std::endl;
            
        } else if (mensagem.find("ENVIDO|") == 0) {
            std::string msg = mensagem.substr(7);
            std::cout << "\n💎 ENVIDO! " << msg << std::endl;
            
            if (msg.find(nomeJogador) == std::string::npos) {
                std::cout << "\n💡 Opções: quero | naoquero | realenvido\n" << std::endl;
            }
            
        } else if (mensagem.find("REAL_ENVIDO|") == 0) {
            std::string msg = mensagem.substr(12);
            std::cout << "\n💎💎 REAL ENVIDO! " << msg << std::endl;
            
        } else if (mensagem.find("FLOR|") == 0) {
            std::string msg = mensagem.substr(5);
            std::cout << "\n🌸 FLOR! " << msg << std::endl;
            
            if (msg.find(nomeJogador) == std::string::npos) {
                std::cout << "\n💡 Opções: quero | naoquero | contraflor\n" << std::endl;
            }
            
        } else if (mensagem.find("CONTRA_FLOR|") == 0) {
            std::string msg = mensagem.substr(12);
            std::cout << "\n🌸🌸 CONTRA-FLOR! " << msg << std::endl;
            
        } else if (mensagem.find("QUERO|") == 0) {
            std::string msg = mensagem.substr(6);
            std::cout << "\n✓ QUERO! " << msg << std::endl;
            
        } else if (mensagem.find("NAO_QUERO|") == 0) {
            std::string msg = mensagem.substr(10);
            std::cout << "\n✗ NÃO QUERO! " << msg << std::endl;

        } else if (mensagem.find("DESCONEXAO|") == 0) {
            std::string msg = mensagem.substr(11);
            std::cout << "\n⚠️  " << msg << std::endl;
            
        } else if (mensagem.find("RODADA_RESULTADO|") == 0) {
            std::string msg = mensagem.substr(17);
            std::cout << msg;
            
        } else if (mensagem.find("PARTIDA_VENCIDA|") == 0) {
            std::string msg = mensagem.substr(16);
            std::cout << msg;
            
        } else if (mensagem.find("JOGO_FINALIZADO|") == 0) {
            std::string msg = mensagem.substr(16);
            std::cout << msg;
            conectado = false;
            
        } else if (mensagem.find("PROXIMA_RODADA|") == 0) {
            std::string msg = mensagem.substr(15);
            std::cout << "\n📢 " << msg << std::endl;
            
        } else if (mensagem.find("SUA_VEZ|") == 0) {
            std::string msg = mensagem.substr(8);

            if (msg.find(nomeJogador) != std::string::npos) {
                minhaVez = true;
                std::cout << "\n➡️  " << msg << std::endl;
                mostrarOpcoesDisponiveis();
            } else {
                minhaVez = false;
                std::cout << "\n⏳ Aguarde... É a vez de jogar do(a) adversário(a).\n" << std::endl;
            }
            
        } else if (mensagem.find("ERRO|") == 0) {
            std::string msg = mensagem.substr(5);
            std::cerr << "\n❌ ERRO: " << msg << std::endl;
            
        } else {
            // Mensagem genérica
            std::cout << mensagem;
        }
    }
    
    return nullptr;
}

void mostrarMenuCompleto() {
    std::cout << "\n┌────────────────────────────────────────┐" << std::endl;
    std::cout << "│         COMANDOS DISPONÍVEIS           │" << std::endl;
    std::cout << "├────────────────────────────────────────┤" << std::endl;
    std::cout << "│ JOGAR CARTAS:                          │" << std::endl;
    std::cout << "│   1, 2, 3 - Jogar carta 1, 2 ou 3      │" << std::endl;
    std::cout << "├────────────────────────────────────────┤" << std::endl;
    std::cout << "│ APOSTAS DE PONTOS:                     │" << std::endl;
    std::cout << "│   truco       - Pedir truco            │" << std::endl;
    std::cout << "│   retruco     - Pedir retruco          │" << std::endl;
    std::cout << "│   vale4       - Pedir vale quatro      │" << std::endl;
    std::cout << "├────────────────────────────────────────┤" << std::endl;
    std::cout << "│ APOSTAS DE ENVIDO:                     │" << std::endl;
    std::cout << "│   envido      - Pedir envido           │" << std::endl;
    std::cout << "│   realenvido  - Pedir real envido      │" << std::endl;
    std::cout << "├────────────────────────────────────────┤" << std::endl;
    std::cout << "│ APOSTAS DE FLOR:                       │" << std::endl;
    std::cout << "│   flor        - Pedir flor             │" << std::endl;
    std::cout << "│   contraflor  - Pedir contra-flor      │" << std::endl;
    std::cout << "├────────────────────────────────────────┤" << std::endl;
    std::cout << "│ RESPOSTAS:                             │" << std::endl;
    std::cout << "│   quero       - Aceitar aposta         │" << std::endl;
    std::cout << "│   naoquero    - Rejeitar aposta        │" << std::endl;
    std::cout << "├────────────────────────────────────────┤" << std::endl;
    std::cout << "│   menu/ajuda  - Mostrar este menu      │" << std::endl;
    std::cout << "└────────────────────────────────────────┘" << std::endl;
}

void mostrarOpcoesDisponiveis() {
    std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
    std::cout << "║        🎮 SUA VEZ DE JOGAR! 🎮        ║" << std::endl;
    std::cout << "╠════════════════════════════════════════╣" << std::endl;
    std::cout << "║ Opções:                                ║" << std::endl;
    std::cout << "║ • 1/2/3    - Jogar carta               ║" << std::endl;
    std::cout << "║ • truco    - Pedir truco (vale 2)      ║" << std::endl;
    std::cout << "║ • envido   - Pedir envido              ║" << std::endl;
    std::cout << "║ • flor     - Pedir flor                ║" << std::endl;
    std::cout << "║ • menu     - Ver todos comandos        ║" << std::endl;
    std::cout << "╚════════════════════════════════════════╝" << std::endl;
}

void mostrarOpcoesResposta() {
    std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
    std::cout << "║       ⚠️  RESPONDA À APOSTA! ⚠️        ║" << std::endl;
    std::cout << "╠════════════════════════════════════════╣" << std::endl;
    std::cout << "║ Opções:                                ║" << std::endl;
    std::cout << "║ • quero      - Aceitar aposta          ║" << std::endl;
    std::cout << "║ • naoquero   - Rejeitar aposta         ║" << std::endl;
    std::cout << "║ • retruco    - Aumentar aposta         ║" << std::endl;
    std::cout << "║ • menu       - Ver todos comandos      ║" << std::endl;
    std::cout << "╚════════════════════════════════════════╝" << std::endl;
}

void conectarServidor(const std::string& ip, int porta) {
    struct sockaddr_in serverAddr;
    
    // Criar socket
    clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket < 0) {
        std::cerr << "Erro ao criar socket" << std::endl;
        return;
    }
    
    // Configurar endereço do servidor
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(porta);
    
    if (inet_pton(AF_INET, ip.c_str(), &serverAddr.sin_addr) <= 0) {
        std::cerr << "Endereço IP inválido" << std::endl;
        close(clientSocket);
        return;
    }
    
    // Conectar ao servidor
    std::cout << "Conectando ao servidor " << ip << ":" << porta << "..." << std::endl;
    
    if (connect(clientSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "Erro ao conectar ao servidor" << std::endl;
        close(clientSocket);
        return;
    }
    
    conectado = true;
    std::cout << "✓ Conectado ao servidor!" << std::endl;
    
    // Solicitar nome do jogador
    std::cout << "\nDigite seu nome: ";
    std::getline(std::cin, nomeJogador);
    
    // Enviar nome ao servidor
    send(clientSocket, nomeJogador.c_str(), nomeJogador.length(), 0);
    
    // Criar thread para receber mensagens
    pthread_create(&threadReceber, NULL, receberMensagens, NULL);
}

void enviarComando(const std::string& comando) {
    if (!conectado) {
        std::cout << "Você não está conectado ao servidor!" << std::endl;
        return;
    }
    
    send(clientSocket, comando.c_str(), comando.length(), 0);
}

int main(int argc, char* argv[]) {
    std::string ip = "127.0.0.1";
    int porta = 8080;
    
    // Permitir especificar IP e porta via argumentos
    if (argc >= 2) {
        ip = argv[1];
    }
    if (argc >= 3) {
        porta = std::atoi(argv[2]);
    }
    
    std::cout << "╔═══════════════════════════════════════════╗" << std::endl;
    std::cout << "║      CLIENTE TRUCO ESPANHOL TCP/IP        ║" << std::endl;
    std::cout << "╚═══════════════════════════════════════════╝" << std::endl;
    
    // Conectar ao servidor
    conectarServidor(ip, porta);
    
    if (!conectado) {
        return 1;
    }
    
    // Mostrar menu completo inicial
    std::cout << "\n💡 Digite 'menu' a qualquer momento para ver todos os comandos\n" << std::endl;
    
    // Loop principal - ler comandos do usuário
    std::string comando;
    while (conectado) {
        std::cout << "\n> ";
        std::getline(std::cin, comando);
        
        if (comando.empty()) {
            continue;
        }
        // Processar comandos de jogo
        if (comando == "menu" || comando == "ajuda") {
            mostrarMenuCompleto();
            
        } else if (comando == "1" || comando == "2" || comando == "3") {
            enviarComando("JOGAR_CARTA|" + comando);
            
        } else if (comando == "truco") {
            enviarComando("TRUCO");
            
        } else if (comando == "retruco") {
            enviarComando("RETRUCO");
            
        } else if (comando == "vale4" || comando == "vale 4") {
            enviarComando("VALE4");
            
        } else if (comando == "envido") {
            enviarComando("ENVIDO");
            
        } else if (comando == "realenvido" || comando == "real envido") {
            enviarComando("REAL_ENVIDO");
            
        } else if (comando == "faltaenvido" || comando == "falta envido") {
            enviarComando("FALTA_ENVIDO");
            
        } else if (comando == "flor") {
            enviarComando("FLOR");
            
        } else if (comando == "contraflor" || comando == "contra-flor" || comando == "contra flor") {
            enviarComando("CONTRA_FLOR");
            
        } else if (comando == "quero" || comando == "aceitar" || comando == "aceito") {
            enviarComando("QUERO");
            
        } else if (comando == "naoquero" || comando == "nao quero" || comando == "não quero" || comando == "rejeitar" || comando == "recusar") {
            enviarComando("NAO_QUERO");
            
        } else if (comando == "sair" || comando == "exit") {
            std::cout << "Saindo do jogo..." << std::endl;
            conectado = false;
            close(clientSocket);
            
        } else {
            std::cout << "\n❌ Comando desconhecido. Digite 'menu' para ver todos os comandos." << std::endl;
        }
    }
    
    // Aguardar thread de recepção terminar
    if (conectado) {
        pthread_join(threadReceber, NULL);
    }
    
    // Fechar conexão
    if (clientSocket >= 0) {
        close(clientSocket);
    }
    
    std::cout << "Cliente encerrado." << std::endl;
    
    return 0;
}
