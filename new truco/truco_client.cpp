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

int clientSocket = -1;
bool connected = false;
bool myTurn = false;
std::string playerName = "";

void* receiveMessage(void* arg) {
    char buffer[BUFFER_SIZE];
    
    while (connected) {
        int bytes = recv(clientSocket, buffer, BUFFER_SIZE - 1, 0);
        
        if (bytes <= 0) {
            std::cout << "\nDesconnected do servidor.\n";
            connected = false;
            break;
        }
        
        buffer[bytes] = '\0';
        std::string msg(buffer);
        
        if (msg.find("BEM_VINDO | ") == 0) {
            std::cout << "\n✓ " << msg.substr(12) << std::endl;
        }
        else if (msg.find("cards | ") == 0) {
            std::cout << "\n=== SUAS CARTAS ===\n";
            std::string cartas = msg.substr(8);
            size_t pos = 0;
            while ((pos = cartas.find(",")) != std::string::npos) {
                std::cout << "   " << cartas.substr(0, pos) << "\n";
                cartas.erase(0, pos + 1);
            }
            if (!cartas.empty() && cartas != "\n") {
                std::cout << "   " << cartas;
            }
        }
        else if (msg.find("turn | ") == 0) {
            std::string resto = msg.substr(7);
            size_t pipe = resto.find("|");
            if (pipe != std::string::npos) {
                std::string nome = resto.substr(pipe + 1);
                // Remove quebra de linha
                if (!nome.empty() && nome.back() == '\n') {
                    nome.pop_back();
                }
                
                std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━\n";
                std::cout << "⏵ Vez de: " << nome;
                
                // Verificar se é minha vez comparando nome
                if (nome.find(playerName) != std::string::npos) {
                    myTurn = true;
                    std::cout << " (VOCÊ!)\n";
                } else {
                    myTurn = false;
                    std::cout << " (aguarde...)\n";
                }
            }
        }
        else if (msg.find("JOGADA | ") == 0) {
            std::cout << "  🃏 " << msg.substr(9);
        }
        else if (msg.find("RODADA | ") == 0) {
            std::cout << "\n🏆 " << msg.substr(9);
        }
        else if (msg.find("MAO | ") == 0) {
            std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━\n";
            std::cout << "🎯 " << msg.substr(6);
        }
        else if (msg.find("VITORIA | ") == 0) {
            std::cout << "\n╔═══════════════════════════════════════╗\n";
            std::cout << "║  " << msg.substr(10);
            std::cout << "╚═══════════════════════════════════════╝\n";
        }
        else if (msg.find("APOSTA | ") == 0) {
            std::cout << "\n💰 " << msg.substr(9);
        }
        else if (msg.find("INFO | ") == 0) {
            std::cout << "ℹ️  " << msg.substr(7);
        }
        else if (msg.find("ERRO | ") == 0) {
            std::cout << "\n❌ ERRO: " << msg.substr(7);
        }
        else if (msg.find(">>> ") == 0) {
            // Comandos disponíveis ou perguntas
            std::cout << msg;
        }
        else {
            std::cout << msg;
        }
    }
    
    return nullptr;
}

int main(int argc, char* argv[]) {
    std::string serverIP = "127.0.0.1";
    int port = 8080;
    
    std::cout << "╔═══════════════════════════════════════╗\n";
    std::cout << "║   CLIENTE DE TRUCO ESPANHOL          ║\n";
    std::cout << "╚═══════════════════════════════════════╝\n\n";
    
    if (argc >= 2) {
        serverIP = argv[1];
    } else {
        std::cout << "Digite o IP do servidor (ou Enter para localhost): ";
        std::string input;
        std::getline(std::cin, input);
        if (!input.empty()) {
            serverIP = input;
        }
    }
    
    if (argc >= 3) port = std::atoi(argv[2]);
    
    clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket < 0) {
        std::cerr << "Erro ao criar socket\n";
        return 1;
    }
    
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, serverIP.c_str(), &serverAddr.sin_addr) <= 0) {
        std::cerr << "Endereço inválido\n";
        close(clientSocket);
        return 1;
    }
    
    std::cout << "Conectando ao servidor " << serverIP << ":" << port << "...\n";
    
    if (connect(clientSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "Erro ao conectar\n";
        close(clientSocket);
        return 1;
    }
    
    connected = true;
    std::cout << "connected!\n\n";
    
    std::cout << "Digite seu nome: ";
    std::getline(std::cin, playerName);
    
    send(clientSocket, playerName.c_str(), playerName.length(), 0);
    
    pthread_t tid;
    pthread_create(&tid, NULL, receiveMessage, NULL);
    pthread_detach(tid);
    
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "  Comandos básicos: start (iniciar), quit (sair)\n";
    std::cout << "   Durante o jogo, o servidor mostrará suas opções disponíveis.\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
    
    std::string comando;
    while (connected) {
        std::cout << "> ";
        std::getline(std::cin, comando);
        
        if (comando.empty()) continue;
        
        if (comando == "quit" || comando == "sair") {
            connected = false;
            break;
        }
        
        send(clientSocket, comando.c_str(), comando.length(), 0);
    }
    
    close(clientSocket);
    std::cout << "Cliente encerrado.\n";
    
    return 0;
}
