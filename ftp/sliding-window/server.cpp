/*
    FTP Server com UDP - Slinding Window
    Suporta upload e download de arquivos com threads paralelas
    - Socket dedicado por thread
    - Checksum CRC32 para integridade
    - Timeout adaptativo
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <sys/time.h>
#include <cmath>
#include <ifaddrs.h>

#define BUFLEN 1024
#define PORT 9999
#define INITIAL_TIMEOUT_SEC 5
#define MAX_RETRIES 5
#define ALPHA 0.125  // Fator para RTT médio (usado em timeout adaptativo)
#define BETA 0.25    // Fator para variação de RTT
#define WINDOW_SIZE 5  // Tamanho da janela deslizante

// Tipos de pacotes
#define PKT_UPLOAD_REQUEST 1
#define PKT_DOWNLOAD_REQUEST 2
#define PKT_DATA 3
#define PKT_ACK 4
#define PKT_END 5
#define PKT_ERROR 6

// Estrutura do pacote com checksum
typedef struct {
    int type;
    int seq_num;
    int data_len;
    char filename[256];
    char data[BUFLEN];
    unsigned int checksum;  // NOVO: CRC32 para integridade
} Packet;

// Estrutura de janela deslizante para Selective Repeat
typedef struct {
    Packet packets[WINDOW_SIZE];       // Buffer de pacotes
    long long send_times[WINDOW_SIZE]; // Timestamps de envio
    int acked[WINDOW_SIZE];             // Bitmap de ACKs recebidos
    int base;                           // Início da janela
    int next_seq_num;                   // Próximo a enviar
    int total_packets;                  // Total de pacotes
    pthread_mutex_t lock;               // Mutex para sincronização
    int sockfd;
    struct sockaddr_in client_addr;
    socklen_t addr_len;
    int finished;                       // Flag para encerrar threads
    double estimated_rtt;
    double dev_rtt;
} SlidingWindow;

// Estrutura para thread de upload com buffer
typedef struct {
    struct sockaddr_in client_addr;
    socklen_t addr_len;
    int sockfd;
    Packet request;
} ThreadArgs;

void die(const char *s)
{
    perror(s);
    exit(1);
}

// NOVO: Calcular CRC32 simples (checksum)
unsigned int calculate_checksum(const char *data, int len)
{
    unsigned int crc = 0xFFFFFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (unsigned char)data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return ~crc;
}

// NOVO: Obter timestamp em milissegundos
long long get_timestamp_ms()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)(tv.tv_sec) * 1000 + (tv.tv_usec) / 1000;
}

// ✅ Thread para RECEBER ACKs (download com Selective Repeat)
void* thread_receive_acks(void* arg)
{
    SlidingWindow *window = (SlidingWindow*)arg;
    Packet ack;
    
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms timeout
    setsockopt(window->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    while (!window->finished) {
        memset(&ack, 0, sizeof(Packet));
        
        int recv_len = recvfrom(window->sockfd, &ack, sizeof(Packet), 0, 
                                (struct sockaddr*)&window->client_addr, &window->addr_len);
        
        if (recv_len > 0 && ack.type == PKT_ACK) {
            pthread_mutex_lock(&window->lock);
            
            int seq = ack.seq_num;
            int idx = seq % WINDOW_SIZE;
            
            // Marcar pacote como confirmado
            if (seq >= window->base && seq < window->next_seq_num) {
                if (!window->acked[idx]) {
                    window->acked[idx] = 1;
                    
                    // Calcular RTT
                    long long now = get_timestamp_ms();
                    double sample_rtt = (now - window->send_times[idx]) / 1000.0;
                    
                    window->dev_rtt = (1 - BETA) * window->dev_rtt + 
                                      BETA * fabs(sample_rtt - window->estimated_rtt);
                    window->estimated_rtt = (1 - ALPHA) * window->estimated_rtt + 
                                           ALPHA * sample_rtt;
                    
                    printf("  ✓ ACK recebido seq=%d (RTT=%.3fs)\n", seq, sample_rtt);
                }
                
                // Deslizar janela se o base foi confirmado
                while (window->acked[window->base % WINDOW_SIZE] && 
                       window->base < window->total_packets) {
                    window->acked[window->base % WINDOW_SIZE] = 0;
                    window->base++;
                    printf("  🔄 Janela deslizada → base=%d\n", window->base);
                }
            }
            
            pthread_mutex_unlock(&window->lock);
        }
    }
    
    return NULL;
}

// ✅ Thread para VERIFICAR TIMEOUTS e RETRANSMITIR
void* thread_check_timeouts(void* arg)
{
    SlidingWindow *window = (SlidingWindow*)arg;
    
    while (!window->finished) {
        usleep(100000); // 100ms
        
        pthread_mutex_lock(&window->lock);
        
        long long now = get_timestamp_ms();
        int timeout_ms = (int)((window->estimated_rtt + 4 * window->dev_rtt) * 1000);
        if (timeout_ms < 500) timeout_ms = 500;
        if (timeout_ms > 5000) timeout_ms = 5000;
        
        // Verificar cada pacote na janela
        for (int seq = window->base; seq < window->next_seq_num; seq++) {
            int idx = seq % WINDOW_SIZE;
            
            if (!window->acked[idx] && 
                (now - window->send_times[idx]) > timeout_ms) {
                
                // RETRANSMITIR apenas este pacote (Selective Repeat)
                Packet *pkt = &window->packets[idx];
                sendto(window->sockfd, pkt, sizeof(Packet), 0, 
                       (struct sockaddr*)&window->client_addr, window->addr_len);
                
                window->send_times[idx] = now;
                
                printf("  🔄 Retransmitindo seq=%d (timeout=%dms)\n", seq, timeout_ms);
            }
        }
        
        pthread_mutex_unlock(&window->lock);
    }
    
    return NULL;
}

// Função para enviar ACK
void send_ack(int sockfd, int seq_num, struct sockaddr_in *addr, socklen_t addr_len)
{
    Packet ack;
    memset(&ack, 0, sizeof(Packet));
    ack.type = PKT_ACK;
    ack.seq_num = seq_num;
    
    sendto(sockfd, &ack, sizeof(Packet), 0, (struct sockaddr*)addr, addr_len);
    printf("  ACK enviado para seq=%d\n", seq_num);
}

// ✅ DOWNLOAD com Sliding Window (Selective Repeat)
void* thread_download(void* arg)
{
    ThreadArgs *args = (ThreadArgs*)arg;
    pthread_detach(pthread_self());
    
    printf("\n[DOWNLOAD] Thread iniciada para arquivo: %s\n", args->request.filename);
    printf("[DOWNLOAD] Cliente: %s:%d (Selective Repeat)\n", 
           inet_ntoa(args->client_addr.sin_addr), ntohs(args->client_addr.sin_port));
    
    // Socket dedicado
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd == -1) {
        perror("socket thread_download");
        free(args);
        return NULL;
    }
    
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = 0;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(sockfd, (struct sockaddr*)&local_addr, sizeof(local_addr)) == -1) {
        perror("bind thread_download");
        close(sockfd);
        free(args);
        return NULL;
    }
    
    // Abrir arquivo
    int fd = open(args->request.filename, O_RDONLY);
    if (fd == -1) {
        printf("[DOWNLOAD] Erro ao abrir arquivo: %s\n", args->request.filename);
        Packet error_pkt;
        memset(&error_pkt, 0, sizeof(Packet));
        error_pkt.type = PKT_ERROR;
        strcpy(error_pkt.data, "Arquivo nao encontrado");
        sendto(sockfd, &error_pkt, sizeof(Packet), 0, 
               (struct sockaddr*)&args->client_addr, args->addr_len);
        close(sockfd);
        free(args);
        return NULL;
    }
    
    // ✅ Ler todos os pacotes do arquivo (buffer dinâmico)
    Packet *all_packets = (Packet*)malloc(1000 * sizeof(Packet));
    if (!all_packets) {
        printf("[DOWNLOAD] Erro ao alocar memória\n");
        close(fd);
        close(sockfd);
        free(args);
        return NULL;
    }
    
    int total_packets = 0;
    int bytes_read;
    
    while ((bytes_read = read(fd, all_packets[total_packets].data, BUFLEN)) > 0) {
        all_packets[total_packets].type = PKT_DATA;
        all_packets[total_packets].seq_num = total_packets;
        all_packets[total_packets].data_len = bytes_read;
        all_packets[total_packets].checksum = 
            calculate_checksum(all_packets[total_packets].data, bytes_read);
        total_packets++;
        if (total_packets >= 1000) break;
    }
    close(fd);
    
    printf("[DOWNLOAD] 📦 Total: %d pacotes | 📊 Janela: %d\n\n", 
           total_packets, WINDOW_SIZE);
    
    // ✅ Inicializar janela deslizante
    SlidingWindow window;
    memset(&window, 0, sizeof(SlidingWindow));
    window.base = 0;
    window.next_seq_num = 0;
    window.total_packets = total_packets;
    window.sockfd = sockfd;
    window.client_addr = args->client_addr;
    window.addr_len = args->addr_len;
    window.finished = 0;
    window.estimated_rtt = 1.0;
    window.dev_rtt = 0.5;
    pthread_mutex_init(&window.lock, NULL);
    
    // ✅ Criar threads para ACKs e timeouts
    pthread_t tid_ack, tid_timeout;
    pthread_create(&tid_ack, NULL, thread_receive_acks, &window);
    pthread_create(&tid_timeout, NULL, thread_check_timeouts, &window);
    
    // ✅ LOOP PRINCIPAL: Enviar pacotes conforme janela permite
    while (window.base < total_packets) {
        pthread_mutex_lock(&window.lock);
        
        // Enviar novos pacotes se houver espaço na janela
        while (window.next_seq_num < window.base + WINDOW_SIZE && 
               window.next_seq_num < total_packets) {
            
            int idx = window.next_seq_num % WINDOW_SIZE;
            window.packets[idx] = all_packets[window.next_seq_num];
            window.acked[idx] = 0;
            window.send_times[idx] = get_timestamp_ms();
            
            sendto(sockfd, &window.packets[idx], sizeof(Packet), 0, 
                   (struct sockaddr*)&window.client_addr, window.addr_len);
            
            printf("📤 Enviado seq=%d [base=%d, janela=%d-%d]\n", 
                   window.next_seq_num, window.base, 
                   window.base, window.base + WINDOW_SIZE - 1);
            
            window.next_seq_num++;
        }
        
        pthread_mutex_unlock(&window.lock);
        usleep(10000); // 10ms
    }
    
    printf("\n⏳ Aguardando ACKs finais...\n");
    sleep(2);
    
    // ✅ Enviar pacote END
    Packet end_pkt;
    memset(&end_pkt, 0, sizeof(Packet));
    end_pkt.type = PKT_END;
    end_pkt.seq_num = total_packets;
    
    printf("Enviando pacote END...\n");
    for (int i = 0; i < 3; i++) {
        sendto(sockfd, &end_pkt, sizeof(Packet), 0, 
               (struct sockaddr*)&window.client_addr, window.addr_len);
        usleep(100000);
    }
    
    // ✅ Encerrar threads
    window.finished = 1;
    pthread_join(tid_ack, NULL);
    pthread_join(tid_timeout, NULL);
    pthread_mutex_destroy(&window.lock);
    
    printf("\n[DOWNLOAD] ✓ Transferência concluída: %s (%d pacotes)\n", 
           args->request.filename, total_packets);
    
    close(sockfd);
    free(all_packets);
    free(args);
    return NULL;
}

// ✅ UPLOAD com Buffer para Recepção Fora de Ordem (Selective Repeat)
void* thread_upload(void* arg)
{
    ThreadArgs *args = (ThreadArgs*)arg;
    pthread_detach(pthread_self());
    
    printf("\n[UPLOAD] Thread iniciada para arquivo: %s\n", args->request.filename);
    printf("[UPLOAD] Cliente: %s:%d (Selective Repeat)\n", 
           inet_ntoa(args->client_addr.sin_addr), ntohs(args->client_addr.sin_port));
    
    // ✅ CORREÇÃO CRÍTICA: Socket dedicado para upload (evita conflito com main loop)
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd == -1) {
        perror("socket thread_upload");
        free(args);
        return NULL;
    }
    
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = 0; // Porta automática
    local_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(sockfd, (struct sockaddr*)&local_addr, sizeof(local_addr)) == -1) {
        perror("bind thread_upload");
        close(sockfd);
        free(args);
        return NULL;
    }
    
    char upload_filename[300];
    snprintf(upload_filename, sizeof(upload_filename), "received_%s", args->request.filename);
    
    int fd = creat(upload_filename, 0666);
    if (fd == -1) {
        printf("[UPLOAD] Erro ao criar arquivo: %s\n", upload_filename);
        Packet error_pkt;
        memset(&error_pkt, 0, sizeof(Packet));
        error_pkt.type = PKT_ERROR;
        strcpy(error_pkt.data, "Erro ao criar arquivo no servidor");
        sendto(sockfd, &error_pkt, sizeof(Packet), 0, 
               (struct sockaddr*)&args->client_addr, args->addr_len);
        free(args);
        return NULL;
    }
    
    // Enviar ACK para requisição inicial
    send_ack(sockfd, 0, &args->client_addr, args->addr_len);
    
    // ✅ Buffer dinâmico para recebimento fora de ordem (evita stack overflow)
    Packet *buffer = (Packet*)malloc(1000 * sizeof(Packet));
    int *received = (int*)calloc(1000, sizeof(int));
    if (!buffer || !received) {
        printf("[UPLOAD] Erro ao alocar memória\n");
        close(fd);
        close(sockfd);
        if (buffer) free(buffer);
        if (received) free(received);
        free(args);
        return NULL;
    }
    int base = 0;
    
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    while (1) {
        Packet pkt;
        memset(&pkt, 0, sizeof(Packet));
        
        int recv_len = recvfrom(sockfd, &pkt, sizeof(Packet), 0, 
                                (struct sockaddr*)&args->client_addr, &args->addr_len);
        
        if (recv_len <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("[UPLOAD] ⏰ Timeout\n");
            }
            break;
        }
        
        printf("[UPLOAD] Recebido tipo=%d seq=%d\n", pkt.type, pkt.seq_num);
        
        if (pkt.type == PKT_END) {
            send_ack(sockfd, pkt.seq_num, &args->client_addr, args->addr_len);
            printf("[UPLOAD] ✓ Transferência concluída: %s\n", upload_filename);
            break;
        }
        
        if (pkt.type == PKT_DATA) {
            // Verificar checksum
            unsigned int calc_checksum = calculate_checksum(pkt.data, pkt.data_len);
            if (pkt.checksum != calc_checksum) {
                printf("[UPLOAD] ❌ Checksum inválido seq=%d\n", pkt.seq_num);
                continue;
            }
            
            // ✅ Armazenar pacote (mesmo fora de ordem)
            if (!received[pkt.seq_num]) {
                buffer[pkt.seq_num] = pkt;
                received[pkt.seq_num] = 1;
                printf("[UPLOAD] 📥 Recebido seq=%d ✓ Checksum OK\n", pkt.seq_num);
            }
            
            // ✅ Enviar ACK seletivo (sempre ACK do que recebeu)
            send_ack(sockfd, pkt.seq_num, &args->client_addr, args->addr_len);
            
            // ✅ Escrever pacotes em ordem no arquivo
            while (received[base]) {
                write(fd, buffer[base].data, buffer[base].data_len);
                printf("[UPLOAD] 💾 Escrito seq=%d no arquivo\n", base);
                base++;
            }
        }
    }
    
    close(fd);
    close(sockfd);
    free(buffer);
    free(received);
    free(args);
    return NULL;
}

int main(void)
{
    struct sockaddr_in si_me, si_other;
    int s;
    socklen_t slen = sizeof(si_other);
    Packet pkt;
    
    printf("═══════════════════════════════════════════\n");
    printf("   SERVIDOR FTP UDP - SELECTIVE REPEAT\n");
    printf("   🚀 Janela deslizante: %d pacotes\n", WINDOW_SIZE);
    printf("   🔄 Reenvio seletivo de pacotes perdidos\n");
    printf("═══════════════════════════════════════════\n\n");
    
    // Criar socket UDP principal (apenas para receber requisições)
    if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
        die("socket");
    }
    
    memset((char *)&si_me, 0, sizeof(si_me));
    
    si_me.sin_family = AF_INET;
    si_me.sin_port = htons(PORT);
    si_me.sin_addr.s_addr = htonl(INADDR_ANY);
    
    // Bind
    if (bind(s, (struct sockaddr*)&si_me, sizeof(si_me)) == -1) {
        die("bind");
    }
    
    // Obter e exibir TODOS os IPs disponíveis (incluindo IP da rede local)
    printf("✓ Servidor rodando na porta %d\n\n", PORT);
    printf("IPs disponíveis para conexão:\n");
    printf("─────────────────────────────────────────\n");
    
    struct ifaddrs *ifaddr, *ifa;
    int found_network_ip = 0;
    
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
    } else {
        for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL) continue;
            
            // Apenas IPv4
            if (ifa->ifa_addr->sa_family == AF_INET) {
                struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
                
                // Identificar tipo de interface
                if (strcmp(ip, "127.0.0.1") == 0) {
                    printf("  Localhost:  %s:%d\n", ip, PORT);
                } else if (strncmp(ip, "192.168.", 8) == 0 || 
                          strncmp(ip, "10.", 3) == 0 || 
                          strncmp(ip, "172.", 4) == 0) {
                    printf("  Rede Local: %s:%d\n", ip, PORT);
                    found_network_ip = 1;
                } else {
                    printf("  %s: %s:%d\n", ifa->ifa_name, ip, PORT);
                }
            }
        }
        freeifaddrs(ifaddr);
    }
    
    printf("─────────────────────────────────────────\n");
    if (!found_network_ip) {
        printf(" Nenhum IP de rede local encontrado.\n");
        printf(" Certifique-se de estar conectado ao WiFi/Ethernet.\n");
    }
    printf("\n");
    
    printf("Aguardando requisições...\n\n");
    
    // Loop principal
    while (1) {
        memset(&pkt, 0, sizeof(Packet));
        
        // Receber requisição
        int recv_len = recvfrom(s, &pkt, sizeof(Packet), 0, 
                                (struct sockaddr*)&si_other, &slen);
        
        if (recv_len <= 0) continue;
        
        printf("═══════════════════════════════════════════\n");
        printf("Requisição de %s:%d\n", 
               inet_ntoa(si_other.sin_addr), ntohs(si_other.sin_port));
        
        // Criar thread conforme o tipo de requisição
        ThreadArgs *args = (ThreadArgs*)malloc(sizeof(ThreadArgs));
        args->client_addr = si_other;
        args->addr_len = slen;
        args->sockfd = s;
        args->request = pkt;
        
        pthread_t thread_id;
        
        if (pkt.type == PKT_DOWNLOAD_REQUEST) {
            printf("Tipo: DOWNLOAD arquivo '%s'\n", pkt.filename);
            pthread_create(&thread_id, NULL, thread_download, args);
        } 
        else if (pkt.type == PKT_UPLOAD_REQUEST) {
            printf("Tipo: UPLOAD arquivo '%s'\n", pkt.filename);
            pthread_create(&thread_id, NULL, thread_upload, args);
        }
        else {
            printf("Tipo desconhecido: %d\n", pkt.type);
            free(args);
        }
    }
    
    close(s);
    return 0;
}
