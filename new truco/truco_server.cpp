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
#include <ifaddrs.h>
#include <netdb.h>

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
    bool onGoing = false;
    int turn = 0;
    int roomId;
    pthread_mutex_t mutex;
    bool waitingBetResponse = false;
    int betRespondent = -1;
    int pendingPoints = 1;
    
    Match(int id) : roomId(id) {
        pthread_mutex_init(&mutex, NULL);
    }
    
    ~Match() {
        pthread_mutex_destroy(&mutex);
    }
};

// Variáveis globais para salas
std::vector<Match*> rooms;
int nextRoomId = 1;
pthread_mutex_t mutexRooms = PTHREAD_MUTEX_INITIALIZER;

Command parseCommand(const std::string& cmd) {
    if (cmd == "quit" || cmd == "QUIT" || cmd == "sair") return QUIT;
    if (cmd == "start" || cmd == "START") return NEWGAME;
    if (cmd == "quero" || cmd == "QUERO") return QUERO;
    if (cmd == "naoquero" || cmd == "NAOQUERO") return NAOQUERO;
    if (cmd == "1" || cmd == "2" || cmd == "3") return PLAY;
    return PLAY; // default
}

int getBetValues(Bet bet) {
    switch (bet) {
        case TRUCO: return 2;
        case RETRUCO: return 3;
        case VALEQUATRO: return 4;
        case FLOR: return 3;
        case CONTRAFLOR: return 6;
        case ENVIDO: return 2;
        case REALENVIDO: return 5;
        default: return 1;
    }
}

bool hasFlor(Player& player) {
    if (player.hand.size() < 3) return false;
    
    Naipe firstNaipe = player.hand[0].card.naipe;
    for (size_t i = 1; i < player.hand.size(); i++) {
        if (player.hand[i].card.naipe != firstNaipe) {
            return false;
        }
    }
    return true;
}

std::string getAvailableCommands(Match* match, int playerIdx) {
    Player& player = match->players[playerIdx];
    std::string commands = "\n>>> Comandos disponíveis: ";
    
    // Se está aguardando resposta de aposta
    if (match->waitingBetResponse && match->betRespondent == playerIdx) {
        commands += "[quero] [naoquero]";
        return commands;
    }
    
    // Se não é a vez do jogador, não mostrar comandos
    if (match->turn != playerIdx) {
        return "";
    }
    
    // Comandos de jogo
    commands += "[1/2/3 - jogar carta]";
    
    // Primeira rodada - pode pedir envido, flor e truco
    if (match->table.currentRound == 0) {
        if (match->currentBet == NONE) {
            commands += " [envido] [truco]";
            if (hasFlor(player)) {
                commands += " [flor]";
            }
        } else if (match->currentBet == ENVIDO) {
            commands += " [realenvido] [truco]";
        } else if (match->currentBet == FLOR) {
            int opponent = (playerIdx + 1) % MAX_PLAYERS;
            if (hasFlor(match->players[opponent])) {
                commands += " [contraflor]";
            }
            commands += " [truco]";
        } else if (match->currentBet == TRUCO) {
            commands += " [retruco]";
        } else if (match->currentBet == RETRUCO) {
            commands += " [vale4]";
        }
    } else {
        // Rodadas 2 e 3 - apenas truco e derivados
        if (match->currentBet == NONE) {
            commands += " [truco]";
        } else if (match->currentBet == TRUCO) {
            commands += " [retruco]";
        } else if (match->currentBet == RETRUCO) {
            commands += " [vale4]";
        }
    }
    
    return commands;
}

void sendMessage(int socket, const std::string& msg) {
    send(socket, msg.c_str(), msg.length(), 0);
}

void sendToAll(Match* match, const std::string& msg) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (match->players[i].active) {
            sendMessage(match->players[i].socket, msg);
        }
    }
}

void distributeCards(Match* match) {
    std::vector<Card> deck;
    
    for (int n = 0; n < 4; n++) {
        for (int v = 0; v < 10; v++) {
            Card card;
            card.naipe = static_cast<Naipe>(n);
            card.value = static_cast<Value>(v);
            deck.push_back(card);
        }
    }
    
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(deck.begin(), deck.end(), g);
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        match->players[i].hand.clear();
        for (int j = 0; j < CARDS; j++) {
            Hand hand;
            hand.card = deck.back();
            hand.played = false;
            match->players[i].hand.push_back(hand);
            deck.pop_back();
        }
    }
}

int compareCards(const Card& c1, const Card& c2) {
    if (c1.value > c2.value) return 0;
    if (c2.value > c1.value) return 1;
    return -1;
}

int handWinner(Match* match) {
    if (match->round.winners.size() < 2) return -1;
    
    int winP0 = 0, winsP1 = 0;
    for (int w : match->round.winners) {
        if (w == 0) winP0++;
        else if (w == 1) winsP1++;
    }
    
    if (winP0 >= 2) return 0;
    if (winsP1 >= 2) return 1;
    if (match->round.winners.size() == 3 && match->round.winners[2] != -1) 
        return match->round.winners[2];
    
    return -1;
}

void startMatch(Match* match) {
    pthread_mutex_lock(&match->mutex);
    
    match->onGoing = true;
    match->table.reset();
    match->round.reset();
    match->turn = rand() % MAX_PLAYERS;
    match->currentBet = NONE;
    match->betPlayer = -1;
    match->waitingBetResponse = false;
    match->betRespondent = -1;
    match->pendingPoints = 1;
    match->round.roundValue = 1;
    
    distributeCards(match);
    
    pthread_mutex_unlock(&match->mutex);
    
    sendToAll(match, "\n=== NOVA MÃO ===\n");
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (match->players[i].active) {
            std::string cards = "cartas | ";
            for (size_t j = 0; j < match->players[i].hand.size(); j++) {
                cards += match->players[i].hand[j].card.toString();
                if (j < match->players[i].hand.size() - 1) cards += "; ";
            }
            cards += "\n";
            sendMessage(match->players[i].socket, cards);
        }
    }
    
    std::string turnMsg = "rodada | " + std::to_string(match->turn) + "|" + match->players[match->turn].name + "\n";
    sendToAll(match, turnMsg);
    
    // Enviar comandos disponíveis para o jogador da vez
    std::string cmds = getAvailableCommands(match, match->turn);
    if (!cmds.empty()) {
        sendMessage(match->players[match->turn].socket, cmds + "\n");
    }
}

void proccessCommand(Match* match, int playerIdx, const std::string& cmdStr) {
    pthread_mutex_lock(&match->mutex);
    
    Player& player = match->players[playerIdx];
    std::string cmd = cmdStr;
    
    // Converter para minúsculas
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);
    
    if (cmd == "quit" || cmd == "sair") {
        player.active = false;
        pthread_mutex_unlock(&match->mutex);
        sendToAll(match, "INFO | " + player.name + " saiu.\n");
        return;
    }
    
    if (cmd == "start") {
        bool bothReady = true;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!match->players[i].active) bothReady = false;
        }
        
        pthread_mutex_unlock(&match->mutex);
        if (bothReady) {
            startMatch(match);
        }
        return;
    }
    
    // Resposta a apostas
    if (cmd == "quero") {
        if (!match->waitingBetResponse || match->betRespondent != playerIdx) {
            sendMessage(player.socket, "ERRO | Não há aposta para responder!\n");
            pthread_mutex_unlock(&match->mutex);
            return;
        }
        
        sendToAll(match, "INFO | " + player.name + " aceitou a aposta!\n");
        match->round.roundValue = match->pendingPoints;
        match->waitingBetResponse = false;
        
        // Enviar comandos atualizados
        std::string cmds = getAvailableCommands(match, match->turn);
        if (!cmds.empty()) {
            sendMessage(match->players[match->turn].socket, cmds + "\n");
        }
        
        pthread_mutex_unlock(&match->mutex);
        return;
    }
    
    if (cmd == "naoquero") {
        if (!match->waitingBetResponse || match->betRespondent != playerIdx) {
            sendMessage(player.socket, "ERRO | Não há aposta para responder!\n");
            pthread_mutex_unlock(&match->mutex);
            return;
        }
        
        // Pontos vão para quem apostou (valor anterior à aposta)
        int betterIdx = match->betPlayer;
        int previousValue = match->round.roundValue;
        match->players[betterIdx].points += previousValue;
        
        std::string msg = "INFO | " + player.name + " não quis! ";
        msg += match->players[betterIdx].name + " ganha " + std::to_string(previousValue) + " pontos!\n";
        msg += "Placar: " + std::to_string(match->players[0].points) + "x" + std::to_string(match->players[1].points) + "\n";
        sendToAll(match, msg);
        
        if (match->players[betterIdx].points >= POINTS_TO_WIN) {
            sendToAll(match, "VITORIA | " + match->players[betterIdx].name + " venceu o jogo!\n");
            match->onGoing = false;
        } else {
            pthread_mutex_unlock(&match->mutex);
            sleep(2);
            startMatch(match);
            return;
        }
        
        pthread_mutex_unlock(&match->mutex);
        return;
    }
    
    // Apostas
    if (cmd == "envido" || cmd == "realenvido" || cmd == "flor" || cmd == "contraflor" ||
        cmd == "truco" || cmd == "retruco" || cmd == "vale4") {
        
        if (match->turn != playerIdx) {
            sendMessage(player.socket, "ERRO | Não é sua vez!\n");
            pthread_mutex_unlock(&match->mutex);
            return;
        }
        
        Bet newBet = NONE;
        int newPoints = 0;
        
        if (cmd == "envido") {
            if (match->table.currentRound != 0 || match->currentBet != NONE) {
                sendMessage(player.socket, "ERRO | Envido só na primeira rodada!\n");
                pthread_mutex_unlock(&match->mutex);
                return;
            }
            newBet = ENVIDO;
            newPoints = 2;
        } else if (cmd == "realenvido") {
            if (match->currentBet != ENVIDO) {
                sendMessage(player.socket, "ERRO | Real envido só após envido!\n");
                pthread_mutex_unlock(&match->mutex);
                return;
            }
            newBet = REALENVIDO;
            newPoints = 5;
        } else if (cmd == "flor") {
            if (match->table.currentRound != 0 || !hasFlor(player)) {
                sendMessage(player.socket, "ERRO | Você não tem flor!\n");
                pthread_mutex_unlock(&match->mutex);
                return;
            }
            newBet = FLOR;
            newPoints = 3;
        } else if (cmd == "contraflor") {
            if (match->currentBet != FLOR) {
                sendMessage(player.socket, "ERRO | Contraflor só após flor!\n");
                pthread_mutex_unlock(&match->mutex);
                return;
            }
            int opponent = (playerIdx + 1) % MAX_PLAYERS;
            if (!hasFlor(match->players[opponent])) {
                sendMessage(player.socket, "ERRO | Você não tem flor para contraflor!\n");
                pthread_mutex_unlock(&match->mutex);
                return;
            }
            newBet = CONTRAFLOR;
            newPoints = 6;
        } else if (cmd == "truco") {
            if (match->currentBet >= TRUCO) {
                sendMessage(player.socket, "ERRO | Truco já foi pedido!\n");
                pthread_mutex_unlock(&match->mutex);
                return;
            }
            newBet = TRUCO;
            newPoints = 2;
        } else if (cmd == "retruco") {
            if (match->currentBet != TRUCO) {
                sendMessage(player.socket, "ERRO | Retruco só após truco!\n");
                pthread_mutex_unlock(&match->mutex);
                return;
            }
            newBet = RETRUCO;
            newPoints = 3;
        } else if (cmd == "vale4") {
            if (match->currentBet != RETRUCO) {
                sendMessage(player.socket, "ERRO | Vale 4 só após retruco!\n");
                pthread_mutex_unlock(&match->mutex);
                return;
            }
            newBet = VALEQUATRO;
            newPoints = 4;
        }
        
        match->currentBet = newBet;
        match->betPlayer = playerIdx;
        match->betRespondent = (playerIdx + 1) % MAX_PLAYERS;
        match->waitingBetResponse = true;
        match->pendingPoints = newPoints;
        
        sendToAll(match, "APOSTA | " + player.name + " pediu " + cmd + "!\n");
        
        std::string askMsg = ">>> " + match->players[match->betRespondent].name + ", responda: [quero] ou [naoquero]\n";
        sendMessage(match->players[match->betRespondent].socket, askMsg);
        
        pthread_mutex_unlock(&match->mutex);
        return;
    }
    
    // Jogar carta
    if (cmd == "1" || cmd == "2" || cmd == "3") {
        if (match->turn != playerIdx) {
            sendMessage(player.socket, "ERRO | Não é sua vez!\n");
            pthread_mutex_unlock(&match->mutex);
            return;
        }
        
        if (match->waitingBetResponse) {
            sendMessage(player.socket, "ERRO | Aguarde resposta da aposta!\n");
            pthread_mutex_unlock(&match->mutex);
            return;
        }
        
        int idx = std::atoi(cmd.c_str()) - 1;
        
        if (idx < 0 || idx >= (int)player.hand.size() || player.hand[idx].played) {
            sendMessage(player.socket, "ERRO | Carta inválida!\n");
            pthread_mutex_unlock(&match->mutex);
            return;
        }
        
        Card card = player.hand[idx].card;
        player.hand[idx].played = true;
        match->table.addCard(match->table.currentRound, card);
        
        sendToAll(match, "JOGADA | " + player.name + " jogou " + card.toString() + "\n");
        
        match->turn = (match->turn + 1) % MAX_PLAYERS;
        
        if (match->table.roundCards[match->table.currentRound].size() == 2) {
            Card c1 = match->table.roundCards[match->table.currentRound][0];
            Card c2 = match->table.roundCards[match->table.currentRound][1];
            
            int vencedor = compareCards(c1, c2);
            match->round.winners.push_back(vencedor);
            
            if (vencedor != -1) {
                sendToAll(match, "RODADA | " + match->players[vencedor].name + " venceu a rodada!\n");
                match->turn = vencedor;
            }
            
            int winnerHand = handWinner(match);
            if (winnerHand != -1) {
                match->players[winnerHand].points += match->round.roundValue;
                
                std::string msg = "MAO | " + match->players[winnerHand].name + " venceu a mão! (" + std::to_string(match->round.roundValue) + " pontos)\n";
                msg += "Placar: " + std::to_string(match->players[0].points) + "x" + std::to_string(match->players[1].points) + "\n";
                sendToAll(match, msg);
                
                if (match->players[winnerHand].points >= POINTS_TO_WIN) {
                    sendToAll(match, "VITORIA | " + match->players[winnerHand].name + " venceu o jogo!\n");
                    match->onGoing = false;
                } else {
                    pthread_mutex_unlock(&match->mutex);
                    sleep(2);
                    startMatch(match);
                    return;
                }
            } else {
                match->table.currentRound++;
            }
        }
        
        if (match->onGoing) {
            std::string turnMsg = "turn | " + std::to_string(match->turn) + "|" + match->players[match->turn].name + "\n";
            sendToAll(match, turnMsg);
            
            // Enviar comandos disponíveis
            std::string cmds = getAvailableCommands(match, match->turn);
            if (!cmds.empty()) {
                sendMessage(match->players[match->turn].socket, cmds + "\n");
            }
        }
    }
    
    pthread_mutex_unlock(&match->mutex);
}

void printLocalIPs() {
    struct ifaddrs *ifaddr, *ifa;
    char host[NI_MAXHOST];
    
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return;
    }
    
    std::cout << "\n📡 IPs disponíveis para conexão:\n";
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        
        int family = ifa->ifa_addr->sa_family;
        if (family == AF_INET) {
            if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
                           host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST) == 0) {
                std::string ip(host);
                if (ip != "127.0.0.1") {
                    std::cout << "   • " << ifa->ifa_name << ": " << ip << "\n";
                }
            }
        }
    }
    std::cout << "\n";
    freeifaddrs(ifaddr);
}

void* threadPlayer(void* arg) {
    int* data = (int*)arg;
    int playerIdx = data[0];
    int roomId = data[1];
    delete[] data;
    
    Match* match = rooms[roomId - 1];
    Player& player = match->players[playerIdx];
    char buffer[BUFFER_SIZE];
    
    int bytes = recv(player.socket, buffer, BUFFER_SIZE - 1, 0);
    if (bytes > 0) {
        buffer[bytes] = '\0';
        player.name = std::string(buffer);
        player.name.erase(player.name.find_last_not_of(" \n\r\t") + 1);
        
        std::string bemVindo = "BEM_VINDO | Sala " + std::to_string(roomId) + " - " + player.name + "\n";
        sendMessage(player.socket, bemVindo);
        std::cout << "[Sala " << roomId << "] " << player.name << " conectou\n";
    }
    
    pthread_mutex_lock(&match->mutex);
    bool both = match->players[0].active && match->players[1].active;
    pthread_mutex_unlock(&match->mutex);
    
    if (both) {
        sendToAll(match, "INFO | Todos conectados! Digite 'start' para começar.\n");
    } else {
        sendMessage(player.socket, "INFO | Aguardando outro jogador...\n");
    }
    
    while (player.active) {
        bytes = recv(player.socket, buffer, BUFFER_SIZE - 1, 0);
        
        if (bytes <= 0) {
            player.active = false;
            std::cout << "[Sala " << roomId << "] " << player.name << " desconectou\n";
            sendToAll(match, "INFO | " + player.name + " desconectou.\n");
            break;
        }
        
        buffer[bytes] = '\0';
        std::string comando(buffer);
        comando.erase(comando.find_last_not_of(" \n\r\t") + 1);
        
        if (!comando.empty()) {
            proccessCommand(match, playerIdx, comando);
        }
    }
    
    close(player.socket);
    return nullptr;
}

int main() {
    int serverSocket;
    struct sockaddr_in serverAddr, clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);
    
    std::cout << "╔═══════════════════════════════════════╗\n";
    std::cout << "║   SERVIDOR DE TRUCO ESPANHOL         ║\n";
    std::cout << "║   Porta: " << PORT << "                        ║\n";
    std::cout << "╚═══════════════════════════════════════╝\n";
    
    printLocalIPs();
    
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        std::cerr << "Erro ao criar socket\n";
        return 1;
    }
    
    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;  // Aceita conexões de qualquer IP
    serverAddr.sin_port = htons(PORT);
    
    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "Erro no bind\n";
        close(serverSocket);
        return 1;
    }
    
    if (listen(serverSocket, 10) < 0) {
        std::cerr << "Erro no listen\n";
        close(serverSocket);
        return 1;
    }
    
    std::cout << "Servidor iniciado. Aguardando playeres...\n\n";
    
    std::vector<int> filaEspera;
    
    while (true) {
        int clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientAddrLen);
        
        if (clientSocket < 0) {
            std::cerr << "Erro ao aceitar conexão\n";
            continue;
        }
        
        std::cout << "Nova conexão: " << inet_ntoa(clientAddr.sin_addr) << "\n";
        
        pthread_mutex_lock(&mutexRooms);
        
        filaEspera.push_back(clientSocket);
        
        if (filaEspera.size() >= 2) {
            int roomId = nextRoomId++;
            Match* newRoom = new Match(roomId);
            rooms.push_back(newRoom);
            
            std::cout << "Criando Sala " << roomId << "\n";
            
            for (int i = 0; i < 2; i++) {
                newRoom->players[i].socket = filaEspera[0];
                newRoom->players[i].active = true;
                filaEspera.erase(filaEspera.begin());
                
                pthread_t tid;
                int* data = new int[2];
                data[0] = i;
                data[1] = roomId;
                
                pthread_create(&tid, NULL, threadPlayer, data);
                pthread_detach(tid);
            }
        }
        
        pthread_mutex_unlock(&mutexRooms);
    }
    
    close(serverSocket);
    return 0;
}