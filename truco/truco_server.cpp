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
#define BUFFER_SIZE 1024

// Estruturas do jogo
enum Naipe { OURO = 0, COPAS = 1, ESPADAS = 2, PAUS = 3 };
enum Valor { QUATRO = 0, CINCO = 1, SEIS = 2, SETE = 3, DEZ = 4, ONZE = 5, DOZE = 6, 
             AS = 7, DOIS = 8, TRES = 9, SETE_OUROS = 10, SETE_ESPADAS = 11, AS_PAUS = 12, AS_ESPADAS = 13 };

struct Carta {
    Naipe naipe;
    Valor valor;
    
    std::string toString() const {
        std::string valores[] = {"4", "5", "6", "7", "10", "11", "12", "As", "2", "3", "7 de Ouros", "7 de Espadas", "As de Paus", "As de Espadas"};
        std::string naipes[] = {"Ouro", "Copas", "Espadas", "Paus"};
        
        // Manilhas já têm o naipe no nome, não concatenar
        if (valor >= SETE_OUROS && valor <= AS_ESPADAS) {
            return valores[valor];
        }
        
        return valores[valor] + " de " + naipes[naipe];
    }
};

struct Jogador {
    int socket;
    std::string nome;
    std::vector<Carta> mao;
    int equipe; // 0 ou 1
    bool ativo;
    
    Jogador() : socket(-1), equipe(-1), ativo(false) {}
};

struct Mesa {
    std::vector<Carta> cartasJogadas;
    std::vector<int> jogadorQuemJogou;
};

struct Partida {
    Jogador jogadores[MAX_PLAYERS];
    Mesa mesa;
    std::vector<Carta> baralho;
    int pontosEquipe[2];
    int rodada;
    int vez; // índice do jogador da vez
    bool truco;
    bool retruco;
    bool valeQuatro;
    bool envido;
    bool realEnvido;
    bool flor;
    bool contraFlor;
    int valorTruco;
    bool emAndamento;
    pthread_mutex_t mutex;
    
    Partida() {
        pontosEquipe[0] = 0;
        pontosEquipe[1] = 0;
        rodada = 0;
        vez = 0;
        truco = false;
        retruco = false;
        valeQuatro = false;
        envido = false;
        realEnvido = false;
        flor = false;
        contraFlor = false;
        valorTruco = 1;
        emAndamento = false;
        pthread_mutex_init(&mutex, NULL);
    }
};

// Variáveis globais
Partida partidaAtual;
int numJogadoresConectados = 0;
pthread_mutex_t mutexConexao = PTHREAD_MUTEX_INITIALIZER;

// Funções auxiliares
void criarBaralho(std::vector<Carta>& baralho) {
    baralho.clear();
    // Truco espanhol: 40 cartas (4 naipes x 10 valores)
    for (int n = 0; n < 4; n++) {
        for (int v = 0; v < 10; v++) {
            Carta carta;
            carta.naipe = static_cast<Naipe>(n);
            carta.valor = static_cast<Valor>(v);
            baralho.push_back(carta);
        }
    }
}

void embaralhar(std::vector<Carta>& baralho) {
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(baralho.begin(), baralho.end(), g);
}

void distribuirCartas(Partida& partida) {
    criarBaralho(partida.baralho);
    embaralhar(partida.baralho);
    
    // Distribuir 3 cartas para cada jogador
    for (int i = 0; i < MAX_PLAYERS; i++) {
        partida.jogadores[i].mao.clear();
        for (int j = 0; j < 3; j++) {
            partida.jogadores[i].mao.push_back(partida.baralho.back());
            partida.baralho.pop_back();
        }
    }
}

void enviarMensagem(int socket, const std::string& mensagem) {
    send(socket, mensagem.c_str(), mensagem.length(), 0);
}

void enviarParaTodos(const std::string& mensagem) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (partidaAtual.jogadores[i].ativo) {
            enviarMensagem(partidaAtual.jogadores[i].socket, mensagem);
        }
    }
}

void iniciarRodada(Partida& partida) {
    partida.rodada++;
    partida.mesa.cartasJogadas.clear();
    partida.mesa.jogadorQuemJogou.clear();
    partida.valorTruco = 1;
    partida.truco = false;
    
    distribuirCartas(partida);
    
    enviarParaTodos("NOVA_RODADA|Rodada " + std::to_string(partida.rodada) + " iniciada!\n");
    
    // Pequeno delay para garantir ordem das mensagens
    usleep(50000); // 50ms
    
    // Enviar cartas para cada jogador
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (partida.jogadores[i].ativo) {
            std::string msg = "SUAS_CARTAS|";
            for (size_t j = 0; j < partida.jogadores[i].mao.size(); j++) {
                msg += partida.jogadores[i].mao[j].toString();
                if (j < partida.jogadores[i].mao.size() - 1) msg += ";";
            }
            msg += "\n";
            enviarMensagem(partida.jogadores[i].socket, msg);
        }
    }
}

void* threadJogador(void* arg) {
    int jogadorIdx = *(int*)arg;
    delete (int*)arg;
    
    Jogador& jogador = partidaAtual.jogadores[jogadorIdx];
    char buffer[BUFFER_SIZE];
    
    // Receber nome do jogador
    int bytesRecebidos = recv(jogador.socket, buffer, BUFFER_SIZE - 1, 0);
    if (bytesRecebidos > 0) {
        buffer[bytesRecebidos] = '\0';
        jogador.nome = std::string(buffer);
        std::cout << "Jogador " << jogador.nome << " conectado (Equipe " << jogador.equipe << ")" << std::endl;
        
        enviarMensagem(jogador.socket, "BEM_VINDO|Bem-vindo ao Truco Espanhol, " + jogador.nome + "!\n");
    }
    
    // Loop principal do jogador
    while (jogador.ativo) {
        bytesRecebidos = recv(jogador.socket, buffer, BUFFER_SIZE - 1, 0);
        
        if (bytesRecebidos <= 0) {
            // Desconexão
            pthread_mutex_lock(&mutexConexao);
            jogador.ativo = false;
            numJogadoresConectados--;
            pthread_mutex_unlock(&mutexConexao);
            
            std::cout << "Jogador " << jogador.nome << " desconectou" << std::endl;
            enviarParaTodos("DESCONEXAO|" + jogador.nome + " saiu do jogo\n");
            break;
        }
        
        buffer[bytesRecebidos] = '\0';
        std::string comando(buffer);
        
        pthread_mutex_lock(&partidaAtual.mutex);
        
        // Processar comandos do jogador
        if (comando.find("JOGAR_CARTA|") == 0) {
            // Extrair índice da carta (1, 2 ou 3)
            std::string indiceStr = comando.substr(12); // Pula "JOGAR_CARTA|"
            int indice = std::atoi(indiceStr.c_str()) - 1; // Converter para índice 0-based
            
            if (indice >= 0 && indice < (int)jogador.mao.size()) {
                Carta carta = jogador.mao[indice];
                partidaAtual.mesa.cartasJogadas.push_back(carta);
                partidaAtual.mesa.jogadorQuemJogou.push_back(jogadorIdx);
                jogador.mao.erase(jogador.mao.begin() + indice);
                
                std::string msg = "CARTA_JOGADA|" + jogador.nome + " jogou: " + carta.toString() + "\n";
                enviarParaTodos(msg);
                std::cout << jogador.nome << " jogou: " << carta.toString() << std::endl;
            } else {
                enviarMensagem(jogador.socket, "ERRO|Índice de carta inválido!\n");
            }
            
        } else if (comando.find("TRUCO") == 0) {
            partidaAtual.truco = true;
            enviarParaTodos("TRUCO|" + jogador.nome + " pediu TRUCO!\n");
            
        } else if (comando.find("RETRUCO") == 0) {
            partidaAtual.retruco = true;
            enviarParaTodos("RETRUCO|" + jogador.nome + " pediu RETRUCO!\n");
            
        } else if (comando.find("VALE4") == 0) {
            partidaAtual.valeQuatro = true;
            enviarParaTodos("VALE4|" + jogador.nome + " pediu VALE QUATRO!\n");
            
        } else if (comando.find("ENVIDO") == 0) {
            partidaAtual.envido = true;
            enviarParaTodos("ENVIDO|" + jogador.nome + " pediu ENVIDO!\n");
            
        } else if (comando.find("REAL_ENVIDO") == 0) {
            partidaAtual.realEnvido = true;
            enviarParaTodos("REAL_ENVIDO|" + jogador.nome + " pediu REAL ENVIDO!\n");
            
        } else if (comando.find("FLOR") == 0) {
            partidaAtual.flor = true;
            enviarParaTodos("FLOR|" + jogador.nome + " pediu FLOR!\n");
            
        } else if (comando.find("CONTRA_FLOR") == 0) {
            partidaAtual.contraFlor = true;
            enviarParaTodos("CONTRA_FLOR|" + jogador.nome + " pediu CONTRA-FLOR!\n");
            
        } else if (comando.find("QUERO") == 0) {
            enviarParaTodos("QUERO|" + jogador.nome + " disse QUERO!\n");
            
        } else if (comando.find("NAO_QUERO") == 0) {
            enviarParaTodos("NAO_QUERO|" + jogador.nome + " disse NÃO QUERO!\n");
        }
        
        pthread_mutex_unlock(&partidaAtual.mutex);
    }
    
    close(jogador.socket);
    return nullptr;
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
    std::cout << "Aguardando " << MAX_PLAYERS << " jogadores..." << std::endl;
    
    // Aceitar conexões
    while (numJogadoresConectados < MAX_PLAYERS) {
        clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientAddrLen);
        
        if (clientSocket < 0) {
            std::cerr << "Erro ao aceitar conexão" << std::endl;
            continue;
        }
        
        pthread_mutex_lock(&mutexConexao);
        
        // Encontrar slot livre
        int jogadorIdx = -1;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!partidaAtual.jogadores[i].ativo) {
                jogadorIdx = i;
                break;
            }
        }
        
        if (jogadorIdx >= 0) {
            partidaAtual.jogadores[jogadorIdx].socket = clientSocket;
            partidaAtual.jogadores[jogadorIdx].ativo = true;
            partidaAtual.jogadores[jogadorIdx].equipe = jogadorIdx % 2; // Equipes alternadas
            numJogadoresConectados++;
            
            std::cout << "Jogador " << (jogadorIdx + 1) << " conectado (Equipe " 
                     << partidaAtual.jogadores[jogadorIdx].equipe << ")" << std::endl;
            
            // Criar thread para o jogador
            pthread_t thread;
            int* idx = new int(jogadorIdx);
            pthread_create(&thread, NULL, threadJogador, idx);
            pthread_detach(thread);
        } else {
            std::string msg = "ERRO|Servidor cheio\n";
            send(clientSocket, msg.c_str(), msg.length(), 0);
            close(clientSocket);
        }
        
        pthread_mutex_unlock(&mutexConexao);
    }
    
    // Todos os jogadores conectados - iniciar jogo
    std::cout << "Todos os jogadores conectados! Iniciando jogo..." << std::endl;
    partidaAtual.emAndamento = true;
    
    iniciarRodada(partidaAtual);
    
    // Manter servidor rodando
    while (partidaAtual.emAndamento) {
        sleep(1);
    }
    
    close(serverSocket);
    pthread_mutex_destroy(&partidaAtual.mutex);
    pthread_mutex_destroy(&mutexConexao);
    
    return 0;
}
