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
            std::cout << "\n" << msg.substr(10) << std::endl;
        }
        else if (msg.find("CARTAS | ") == 0) {
            std::cout << "\n=== SUAS CARTAS ===\n";
            std::string cartas = msg.substr(9);
            size_t pos = 0;
            while ((pos = cartas.find(",")) != std::string::npos) {
                std::cout << cartas.substr(0, pos) << "\n";
                cartas.erase(0, pos + 1);
            }
            if (!cartas.empty() && cartas != "\n") {
                std::cout << cartas << std::endl;
            }
        }
        else if (msg.find("VEZ | ") == 0) {
            std::string resto = msg.substr(6);
            size_t pipe = resto.find("|");
            if (pipe != std::string::npos) {
                int vezIdx = std::stoi(resto.substr(0, pipe));
                std::string nome = resto.substr(pipe + 1);
                
                std::cout << "\n>> Vez de: " << nome;
                
                // Verificar se é minha vez comparando nome
                if (nome.find(playerName) != std::string::npos) {
                    myTurn = true;
                    std::cout << " (VOCÊ!)\n";
                    std::cout << "Digite 1, 2 ou 3 para jogar uma carta: ";
                    std::cout.flush();
                } else {
                    myTurn = false;
                    std::cout << "\n";
                }
            }
        }
        else if (msg.find("JOGADA |") == 0) {
            std::cout << msg.substr(8);
        }
        else if (msg.find("RODADA |") == 0) {
            std::cout << ">> " << msg.substr(8);
        }
        else if (msg.find("MAO |") == 0) {
            std::cout << "\n" << msg.substr(5) << std::endl;
        }
        else if (msg.find("VITORIA |") == 0) {
            std::cout << "\n*** " << msg.substr(9) << " ***\n";
        }
        else if (msg.find("INFO |") == 0) {
            std::cout << msg.substr(6);
        }
        else if (msg.find("ERRO  |") == 0) {
            std::cout << "ERRO: " << msg.substr(6);
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
    
    if (argc >= 2) serverIP = argv[1];
    if (argc >= 3) port = std::atoi(argv[2]);
    
    std::cout << "=== CLIENTE TRUCO ESPANHOL ===\n\n";
    
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
    
    std::cout << "\nComandos: 1/2/3 (jogar carta), nova (iniciar), quit (sair)\n\n";
    
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
