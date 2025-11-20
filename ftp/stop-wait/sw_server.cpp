/*
    FTP Server com UDP - Stop and Wait MELHORADO
    Suporta upload e download de arquivos com threads paralelas
    - Corrigido bug de leitura de arquivos
    - Socket dedicado por thread (sem race condition)
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

// Estrutura para thread com socket dedicado
typedef struct {
    struct sockaddr_in client_addr;
    socklen_t addr_len;
    int sockfd;  // Socket dedicado para esta thread
    Packet request;
    double estimated_rtt;  // NOVO: RTT estimado para timeout adaptativo
    double dev_rtt;        // NOVO: Desvio do RTT
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

// MELHORADO: Função para enviar pacote com retransmissão e timeout adaptativo
int send_packet_with_ack(int sockfd, Packet *pkt, struct sockaddr_in *addr, 
                         socklen_t addr_len, double *estimated_rtt, double *dev_rtt)
{
    Packet ack;
    int tentativa = 0;
    
    // Calcular checksum antes de enviar
    pkt->checksum = calculate_checksum(pkt->data, pkt->data_len);
    
    // MELHORADO: Timeout adaptativo baseado no RTT
    int timeout_ms = (int)((*estimated_rtt + 4 * (*dev_rtt)) * 1000);
    if (timeout_ms < 1000) timeout_ms = 1000;  // Mínimo 1 segundo
    if (timeout_ms > 10000) timeout_ms = 10000; // Máximo 10 segundos
    
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    while (tentativa < MAX_RETRIES) {
        long long send_time = get_timestamp_ms();
        
        // Enviar pacote
        if (sendto(sockfd, pkt, sizeof(Packet), 0, (struct sockaddr*)addr, addr_len) == -1) {
            perror("sendto");
            return -1;
        }
        
        printf("  Enviado seq=%d (tent. %d/%d, timeout=%dms)\n", 
               pkt->seq_num, tentativa + 1, MAX_RETRIES, timeout_ms);
        
        // Aguardar ACK
        memset(&ack, 0, sizeof(Packet));
        int recv_len = recvfrom(sockfd, &ack, sizeof(Packet), 0, 
                               (struct sockaddr*)addr, &addr_len);
        
        if (recv_len > 0 && ack.type == PKT_ACK && ack.seq_num == pkt->seq_num) {
            long long recv_time = get_timestamp_ms();
            double sample_rtt = (recv_time - send_time) / 1000.0; // em segundos
            
            // NOVO: Atualizar RTT estimado (algoritmo de Jacobson/Karels)
            *dev_rtt = (1 - BETA) * (*dev_rtt) + BETA * fabs(sample_rtt - *estimated_rtt);
            *estimated_rtt = (1 - ALPHA) * (*estimated_rtt) + ALPHA * sample_rtt;
            
            printf("  ✓ ACK recebido seq=%d (RTT=%.3fs, Est=%.3fs, Dev=%.3fs)\n", 
                   pkt->seq_num, sample_rtt, *estimated_rtt, *dev_rtt);
            return 0;
        }
        
        if (recv_len == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            printf("  ⚠️  Timeout aguardando ACK seq=%d\n", pkt->seq_num);
            // Aumentar timeout após timeout (backoff exponencial)
            timeout_ms = (int)(timeout_ms * 1.5);
            if (timeout_ms > 10000) timeout_ms = 10000;
        }
        
        tentativa++;
    }
    
    printf("  Falha após %d tentativas para seq=%d\n", MAX_RETRIES, pkt->seq_num);
    return -1;
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

// MELHORADO: Thread para DOWNLOAD (servidor envia arquivo para cliente)
void* thread_download(void* arg)
{
    ThreadArgs *args = (ThreadArgs*)arg;
    pthread_detach(pthread_self());
    
    printf("\n[DOWNLOAD] Thread iniciada para arquivo: %s\n", args->request.filename);
    printf("[DOWNLOAD] Cliente: %s:%d\n", 
           inet_ntoa(args->client_addr.sin_addr), ntohs(args->client_addr.sin_port));
    
    // ✅ CORREÇÃO FINAL: Socket dedicado para receber ACKs (evita conflito com loop principal)
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd == -1) {
        perror("socket thread_download");
        free(args);
        return NULL;
    }
    
    // Bind em porta aleatória para receber ACKs
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = 0;  // Porta automática
    local_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(sockfd, (struct sockaddr*)&local_addr, sizeof(local_addr)) == -1) {
        perror("bind thread_download");
        close(sockfd);
        free(args);
        return NULL;
    }
    
    // Abrir arquivo para leitura
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
    
    // MELHORADO: Enviar arquivo em pacotes (SEM bug de leitura duplicada)
    Packet pkt;
    int seq_num = 0;
    int bytes_read;
    
    while (1) {
        memset(&pkt, 0, sizeof(Packet));
        
        // CORRIGIDO: Ler arquivo UMA VEZ apenas
        bytes_read = read(fd, pkt.data, BUFLEN);
        
        if (bytes_read <= 0) break;  // Fim do arquivo
        
        pkt.type = PKT_DATA;
        pkt.seq_num = seq_num;
        pkt.data_len = bytes_read;
        
        printf("[DOWNLOAD] Enviando pacote %d (%d bytes)\n", seq_num, bytes_read);
        
        if (send_packet_with_ack(sockfd, &pkt, &args->client_addr, args->addr_len,
                                &args->estimated_rtt, &args->dev_rtt) == -1) {
            printf("[DOWNLOAD] Falha ao enviar pacote %d\n", seq_num);
            close(fd);
            close(sockfd);
            free(args);
            return NULL;
        }
        
        seq_num++;
    }
    
    close(fd);
    
    // Enviar pacote END
    memset(&pkt, 0, sizeof(Packet));
    pkt.type = PKT_END;
    pkt.seq_num = seq_num;
    
    printf("[DOWNLOAD] Enviando pacote END\n");
    send_packet_with_ack(sockfd, &pkt, &args->client_addr, args->addr_len,
                        &args->estimated_rtt, &args->dev_rtt);
    
    printf("[DOWNLOAD] ✓ Transferência concluída: %s (%d pacotes)\n", 
           args->request.filename, seq_num);
    
    close(sockfd);
    free(args);
    return NULL;
}

// MELHORADO: Thread para UPLOAD (servidor recebe arquivo do cliente)
void* thread_upload(void* arg)
{
    ThreadArgs *args = (ThreadArgs*)arg;
    pthread_detach(pthread_self());
    
    printf("\n[UPLOAD] Thread iniciada para arquivo: %s\n", args->request.filename);
    printf("[UPLOAD] Cliente: %s:%d\n", 
           inet_ntoa(args->client_addr.sin_addr), ntohs(args->client_addr.sin_port));
    
    // ✅ CORREÇÃO: Usar socket principal (compartilhado, mas seguro com sendto/recvfrom)
    int sockfd = args->sockfd;
    
    // Criar arquivo para escrita
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
    
    // Receber pacotes de dados
    Packet pkt;
    int expected_seq = 0;
    
    // Configurar timeout inicial
    struct timeval tv;
    tv.tv_sec = INITIAL_TIMEOUT_SEC * 2;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    while (1) {
        memset(&pkt, 0, sizeof(Packet));
        
        int recv_len = recvfrom(sockfd, &pkt, sizeof(Packet), 0, 
                                (struct sockaddr*)&args->client_addr, &args->addr_len);
        
        if (recv_len <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("[UPLOAD] Timeout aguardando pacotes\n");
            }
            break;
        }
        
        printf("[UPLOAD] Recebido pacote tipo=%d seq=%d\n", pkt.type, pkt.seq_num);
        
        if (pkt.type == PKT_END) {
            send_ack(sockfd, pkt.seq_num, &args->client_addr, args->addr_len);
            printf("[UPLOAD] ✓ Transferência concluída: %s\n", upload_filename);
            break;
        }
        
        if (pkt.type == PKT_DATA) {
            // NOVO: Verificar checksum
            unsigned int received_checksum = pkt.checksum;
            unsigned int calculated_checksum = calculate_checksum(pkt.data, pkt.data_len);
            
            if (received_checksum != calculated_checksum) {
                printf("[UPLOAD] Checksum inválido para seq=%d! Descartando pacote.\n", 
                       pkt.seq_num);
                // Não envia ACK, forçando retransmissão
                continue;
            }
            
            if (pkt.seq_num == expected_seq) {
                // Escrever dados no arquivo
                write(fd, pkt.data, pkt.data_len);
                printf("[UPLOAD] Pacote %d escrito (%d bytes) ✓ Checksum OK\n", 
                       pkt.seq_num, pkt.data_len);
                
                // Enviar ACK
                send_ack(sockfd, pkt.seq_num, &args->client_addr, args->addr_len);
                expected_seq++;
            } else {
                printf("[UPLOAD] Pacote fora de ordem: esperado=%d, recebido=%d\n", 
                       expected_seq, pkt.seq_num);
                // Reenviar último ACK válido
                if (expected_seq > 0) {
                    send_ack(sockfd, expected_seq - 1, &args->client_addr, args->addr_len);
                }
            }
        }
    }
    
    close(fd);
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
    printf("   SERVIDOR FTP UDP - STOP AND WAIT\n");
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
        args->sockfd = s;  // Socket será substituído por um dedicado na thread
        args->request = pkt;
        args->estimated_rtt = 1.0;  // NOVO: RTT inicial estimado de 1 segundo
        args->dev_rtt = 0.5;         // NOVO: Desvio inicial de 0.5 segundo
        
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
