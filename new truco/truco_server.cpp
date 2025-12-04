/*
 * Servidor do Jogo de Truco Espanhol
 * Protocolo: TCP/IP
 * Porta: 8080
 */

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <random>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PORT 8080
#define MAX_PLAYERS 2
#define CARDS 3
#define BUFFER_SIZE 1024
#define POINTS_TO_WIN 12

// Estruturas do jogo
enum Naipe { OURO = 0, COPAS = 1, ESPADAS = 2, PAUS = 3 };
enum Value { QUATRO = 0, CINCO = 1, SEIS = 2, SETE = 3, DEZ = 4, ONZE = 5, DOZE = 6, 
             AS = 7, DOIS = 8, TRES = 9, SETE_OUROS = 10, SETE_ESPADAS = 11, AS_PAUS = 12, AS_ESPADAS = 13 };
enum Bet { NONE = -1, TRUCO = 0, RETRUCO = 1, VALEQUATRO = 2, FLOR = 3, CONTRAFLOR = 4, ENVIDO = 5, REALENVIDO = 6};
enum Command { PLAY, QUERO, NAOQUERO, QUIT, NEWGAME };

struct Card {
    Naipe naipe;
    Value value;
    
    std::string toString() const {
        std::string valores[] = {
            "4", "5", "6", "7", "10", "11", "12", "As", "2", "3",
            "7♦", "7♠", "As♣", "As♠"
        };
        std::string naipes[] = {"♦", "♥", "♠", "♣"};
        
        if (value >= SETE_OUROS) {
            return valores[value];
        }
        
        return valores[value] + naipes[naipe];
    }
};

struct Hand {
    Card card;
    bool played = 0;
};

struct Player {
    int socket;
    std::string name;
    std::vector<Hand> hand;
    int points = 0;
    bool active = false;
};

struct Table {
    std::vector<std::vector<Card>> roundCards;
    int currentRound = 0;

    void reset() {
        roundCards.clear();
        currentRound = 0;
    }
    
    void addCard(int round, const Card& card) {
        if (round >= (int)roundCards.size()) {
            roundCards.resize(round + 1);
        }
        roundCards[round].push_back(card);
    }
};

struct Round {
    std::vector<int> winners;
    int roundValue = 1;
    int roundWinner = -1;

    void reset() {
        winners.clear();
        roundValue = 1;
        roundWinner = -1;
    }
};

struct Match {
    Player players[MAX_PLAYERS];
    Table table;
    Round round;
    Bet currentBet = NONE;
    int betPlayer = -1;
    bool emAndamento = false;
    int vez = 0;  // Índice de quem joga
    int salaId;
    pthread_mutex_t mutex;
    
    Match(int id) : salaId(id) {
        pthread_mutex_init(&mutex, NULL);
    }
    
    ~Match() {
        pthread_mutex_destroy(&mutex);
    }
};

int getBetValues(Bet bet) {
    int betValue = 0;
    switch (bet)
    {
    case TRUCO:
        betValue = 2;
        break;
    case RETRUCO:
        betValue = 3;
        break;
    case VALEQUATRO:
        betValue = 4;
        break;
    case FLOR:
        betValue = 3;
        break;
    case CONTRAFLOR:
        betValue = 6;
        break;
    case ENVIDO:
        betValue = 2;
        break;
    case REALENVIDO:
        betValue = 5;
        break;
    default:
        break;
    }
}

void game() {
    Command command;
    switch (command) {
    case PLAY:

        break;
    case QUERO:
        break;
    case NAOQUERO:
        break;
    case NEWGAME:
        break;
    case QUIT:
        break;
    }
}

void* threadJogador(void* arg) {
    int idx = *(int*)arg;
    delete (int*)arg;

    Player& jogador = partidaAtual.players[idx];

    while (jogador.active && partidaAtual.emAndamento) {
        game(jogador, partidaAtual, partidaAtual.table);
    }

    return nullptr;
}

int compararCartas(const Card& c1, const Card& c2) {
    if (c1.value > c2.value) return 0;
    if (c2.value > c1.value) return 1;
    return -1;
}

int main() {
    int serverSocket, clientSocket;
    struct sockaddr_in serverAddr, clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);
    
    // Criar socket
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        std::cerr << "Erro ao criar socket" << std::endl;
        return 1;
    }
    
    // Configurar opções do socket
    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Configurar endereço
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(PORT);
    
    // Bind
    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "Erro no bind" << std::endl;
        close(serverSocket);
        return 1;
    }
    
    // Listen
    if (listen(serverSocket, MAX_PLAYERS) < 0) {
        std::cerr << "Erro no listen" << std::endl;
        close(serverSocket);
        return 1;
    }
    
    std::cout << "Servidor de Truco Espanhol iniciado na porta " << PORT << std::endl;
    std::cout << "Aguardando jogadores..." << std::endl;
    
    // Aceitar conexões (loop infinito para permitir reconexões)
    while (true) {
        clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientAddrLen);
        
        if (clientSocket < 0) {
            std::cerr << "Erro ao aceitar conexão" << std::endl;
            continue;
        }
       
    }
    
    close(serverSocket);
    pthread_mutex_destroy(&partidaAtual.mutex);
    pthread_mutex_destroy(&mutexConexao);
    
    return 0;
}