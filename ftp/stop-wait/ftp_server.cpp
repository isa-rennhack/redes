/*
    FTP Server com UDP - Stop and Wait
    Suporta upload e download de arquivos com threads paralelas
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

#define BUFLEN 1024
#define PORT 9999
#define TIMEOUT_SEC 2
#define MAX_RETRIES 5

// Tipos de pacotes
#define PKT_UPLOAD_REQUEST 1
#define PKT_DOWNLOAD_REQUEST 2
#define PKT_DATA 3
#define PKT_ACK 4
#define PKT_END 5
#define PKT_ERROR 6

// Estrutura do pacote
typedef struct {
    int type;           // Tipo do pacote
    int seq_num;        // Número de sequência
    int data_len;       // Tamanho dos dados
    char filename[256]; // Nome do arquivo
    char data[BUFLEN];  // Dados
} Packet;

// Estrutura para thread
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

// Função para enviar pacote com retransmissão (Stop-and-Wait)
int send_packet_with_ack(int sockfd, Packet *pkt, struct sockaddr_in *addr, socklen_t addr_len)
{
    Packet ack;
    int tentativa = 0;
    
    // Configurar timeout
    struct timeval tv;
    tv.tv_sec = TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    while (tentativa < MAX_RETRIES) {
        // Enviar pacote
        if (sendto(sockfd, pkt, sizeof(Packet), 0, (struct sockaddr*)addr, addr_len) == -1) {
            perror("sendto");
            return -1;
        }
        
        printf("  📤 Enviado pacote seq=%d (tentativa %d/%d)\n", pkt->seq_num, tentativa + 1, MAX_RETRIES);
        
        // Aguardar ACK
        memset(&ack, 0, sizeof(Packet));
        int recv_len = recvfrom(sockfd, &ack, sizeof(Packet), 0, (struct sockaddr*)addr, &addr_len);
        
        if (recv_len > 0 && ack.type == PKT_ACK && ack.seq_num == pkt->seq_num) {
            printf("  ✓ ACK recebido para seq=%d\n", pkt->seq_num);
            return 0;
        }
        
        if (recv_len == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            printf("  ⚠️  Timeout aguardando ACK seq=%d\n", pkt->seq_num);
        }
        
        tentativa++;
    }
    
    printf("  ❌ Falha após %d tentativas para seq=%d\n", MAX_RETRIES, pkt->seq_num);
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
    printf("  📩 ACK enviado para seq=%d\n", seq_num);
}

// Thread para DOWNLOAD (servidor envia arquivo para cliente)
void* thread_download(void* arg)
{
    ThreadArgs *args = (ThreadArgs*)arg;
    pthread_detach(pthread_self());
    
    printf("\n[DOWNLOAD] Thread iniciada para arquivo: %s\n", args->request.filename);
    
    // Abrir arquivo para leitura
    int fd = open(args->request.filename, O_RDONLY);
    if (fd == -1) {
        printf("[DOWNLOAD] Erro ao abrir arquivo: %s\n", args->request.filename);
        
        // Enviar pacote de erro
        Packet error_pkt;
        memset(&error_pkt, 0, sizeof(Packet));
        error_pkt.type = PKT_ERROR;
        strcpy(error_pkt.data, "Arquivo nao encontrado");
        sendto(args->sockfd, &error_pkt, sizeof(Packet), 0, 
               (struct sockaddr*)&args->client_addr, args->addr_len);
        
        free(args);
        return NULL;
    }
    
    // Enviar arquivo em pacotes
    Packet pkt;
    int seq_num = 0;
    int bytes_read;
    
    while ((bytes_read = read(fd, pkt.data, BUFLEN)) > 0) {
        memset(&pkt, 0, sizeof(Packet));
        pkt.type = PKT_DATA;
        pkt.seq_num = seq_num;
        pkt.data_len = bytes_read;
        read(fd, pkt.data, bytes_read); // Ler novamente após memset
        lseek(fd, seq_num * BUFLEN, SEEK_SET); // Posicionar corretamente
        bytes_read = read(fd, pkt.data, BUFLEN);
        if (bytes_read <= 0) break;
        
        pkt.data_len = bytes_read;
        
        printf("[DOWNLOAD] Enviando pacote %d (%d bytes)\n", seq_num, bytes_read);
        
        if (send_packet_with_ack(args->sockfd, &pkt, &args->client_addr, args->addr_len) == -1) {
            printf("[DOWNLOAD] Falha ao enviar pacote %d\n", seq_num);
            close(fd);
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
    send_packet_with_ack(args->sockfd, &pkt, &args->client_addr, args->addr_len);
    
    printf("[DOWNLOAD] ✓ Transferência concluída: %s (%d pacotes)\n", args->request.filename, seq_num);
    
    free(args);
    return NULL;
}

// Thread para UPLOAD (servidor recebe arquivo do cliente)
void* thread_upload(void* arg)
{
    ThreadArgs *args = (ThreadArgs*)arg;
    pthread_detach(pthread_self());
    
    printf("\n[UPLOAD] Thread iniciada para arquivo: %s\n", args->request.filename);
    
    // Criar arquivo para escrita
    char upload_filename[300];
    snprintf(upload_filename, sizeof(upload_filename), "received_%s", args->request.filename);
    
    int fd = creat(upload_filename, 0666);
    if (fd == -1) {
        printf("[UPLOAD] Erro ao criar arquivo: %s\n", upload_filename);
        
        // Enviar erro
        Packet error_pkt;
        memset(&error_pkt, 0, sizeof(Packet));
        error_pkt.type = PKT_ERROR;
        strcpy(error_pkt.data, "Erro ao criar arquivo no servidor");
        sendto(args->sockfd, &error_pkt, sizeof(Packet), 0, 
               (struct sockaddr*)&args->client_addr, args->addr_len);
        
        free(args);
        return NULL;
    }
    
    // Enviar ACK para requisição inicial
    send_ack(args->sockfd, 0, &args->client_addr, args->addr_len);
    
    // Receber pacotes de dados
    Packet pkt;
    int expected_seq = 0;
    
    while (1) {
        memset(&pkt, 0, sizeof(Packet));
        
        int recv_len = recvfrom(args->sockfd, &pkt, sizeof(Packet), 0, 
                                (struct sockaddr*)&args->client_addr, &args->addr_len);
        
        if (recv_len <= 0) {
            printf("[UPLOAD] Erro ao receber pacote\n");
            break;
        }
        
        printf("[UPLOAD] Recebido pacote tipo=%d seq=%d\n", pkt.type, pkt.seq_num);
        
        if (pkt.type == PKT_END) {
            send_ack(args->sockfd, pkt.seq_num, &args->client_addr, args->addr_len);
            printf("[UPLOAD] ✓ Transferência concluída: %s\n", upload_filename);
            break;
        }
        
        if (pkt.type == PKT_DATA) {
            if (pkt.seq_num == expected_seq) {
                // Escrever dados no arquivo
                write(fd, pkt.data, pkt.data_len);
                printf("[UPLOAD] Pacote %d escrito (%d bytes)\n", pkt.seq_num, pkt.data_len);
                
                // Enviar ACK
                send_ack(args->sockfd, pkt.seq_num, &args->client_addr, args->addr_len);
                expected_seq++;
            } else {
                printf("[UPLOAD] Pacote fora de ordem: esperado=%d, recebido=%d\n", 
                       expected_seq, pkt.seq_num);
                // Reenviar último ACK
                send_ack(args->sockfd, expected_seq - 1, &args->client_addr, args->addr_len);
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
    printf("       SERVIDOR FTP UDP - STOP AND WAIT\n");
    printf("═══════════════════════════════════════════\n\n");
    
    // Criar socket UDP
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
    
    // Obter e exibir IP do servidor
    char hostname[256];
    char ip[100];
    struct hostent *host_entry;
    
    gethostname(hostname, sizeof(hostname));
    host_entry = gethostbyname(hostname);
    
    if (host_entry != NULL) {
        strcpy(ip, inet_ntoa(*((struct in_addr*)host_entry->h_addr_list[0])));
        printf("✓ Servidor rodando em %s:%d\n", ip, PORT);
    } else {
        printf("✓ Servidor rodando na porta %d\n", PORT);
    }
    
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
