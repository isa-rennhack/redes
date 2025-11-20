/*
    FTP Client com UDP - Sliding Window
    Suporta upload e download de arquivos
    - Janela deslizante com reenvio seletivo
    - Checksum CRC32 para integridade
    - Timeout adaptativo
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <math.h>
#include <pthread.h>

#define BUFLEN 1024
#define PORT 9999
#define INITIAL_TIMEOUT_MS 2000
#define MAX_RETRIES 5
#define ALPHA 0.125
#define BETA 0.25
#define WINDOW_SIZE 5

//packet types
#define PKT_UPLOAD_REQUEST 1
#define PKT_DOWNLOAD_REQUEST 2
#define PKT_DATA 3
#define PKT_ACK 4
#define PKT_END 5
#define PKT_ERROR 6

typedef struct {
    int type;
    int seq_num;
    int data_len;
    char filename[256];
    char data[BUFLEN];
    unsigned int checksum;
} Packet;

typedef struct {
    Packet packets[WINDOW_SIZE];
    long long send_times[WINDOW_SIZE];
    int acked[WINDOW_SIZE];
    int base;
    int next_seq_num;
    int total_packets;
    pthread_mutex_t lock;
    int sockfd;
    struct sockaddr_in *server_addr;
    socklen_t addr_len;
    int finished;
    double estimated_rtt;
    double dev_rtt;
} SlidingWindow;

void die(const char *s)
{
    perror(s);
    exit(1);
}

//Checksum CRC32
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

long long get_timestamp_ms()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)(tv.tv_sec) * 1000 + (tv.tv_usec) / 1000;
}

void* thread_receive_acks(void *arg) {
    SlidingWindow *window = (SlidingWindow*)arg;
    Packet ack;

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    setsockopt(window->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (!window->finished) {
        memset(&ack, 0, sizeof(Packet));

        int recv_len = recvfrom(window->sockfd, &ack, sizeof(Packet), 0, (struct sockaddr*)window->server_addr, &window->addr_len);
        if (recv_len > 0 && ack.type == PKT_ACK) {
            pthread_mutex_lock(&window->lock);
            int seq = ack.seq_num;
            int idx = seq % WINDOW_SIZE;

            if(seq >= window->base && seq < window->next_seq_num) {
                if(!window->acked[idx]){
                    window->acked[idx] = 1;

                    long long now = get_timestamp_ms();
                    double sample_rtt = (now - window->send_times[idx]) / 1000.0;

                    window->dev_rtt = (1 -BETA) * window->dev_rtt + BETA * fabs(sample_rtt - window->estimated_rtt);
                    window->estimated_rtt = (1 - ALPHA) * window->estimated_rtt + ALPHA * sample_rtt;

                    printf("  ACK recebido para seq=%d | RTT: %.3f s \n", seq, sample_rtt);
                }

                while (window->acked[window->base % WINDOW_SIZE] && window->base < window->total_packets) {
                    window->acked[window->base % WINDOW_SIZE] = 0;
                    window->base++;
                    printf("  Janela movida. Nova base=%d\n", window->base);
                }
            }
            pthread_mutex_unlock(&window->lock);
        }
    }
    return NULL;
};

void* thread_check_timeouts(void* arg)
{
    SlidingWindow *window = (SlidingWindow*)arg;
    
    while (!window->finished) {
        usleep(100000); // Verifica a cada 100ms
        
        pthread_mutex_lock(&window->lock);
        
        long long now = get_timestamp_ms();
        int timeout_ms = (int)((window->estimated_rtt + 4 * window->dev_rtt) * 1000);
        if (timeout_ms < 500) timeout_ms = 500;
        if (timeout_ms > 5000) timeout_ms = 5000;
        
        // ✅ Verifica cada pacote na janela
        for (int seq = window->base; seq < window->next_seq_num; seq++) {
            int idx = seq % WINDOW_SIZE;
            
            if (!window->acked[idx] && 
                (now - window->send_times[idx]) > timeout_ms) {
                
                // ✅ RETRANSMITIR apenas este pacote (Selective Repeat)
                Packet *pkt = &window->packets[idx];
                sendto(window->sockfd, pkt, sizeof(Packet), 0, 
                       (struct sockaddr*)window->server_addr, window->addr_len);
                
                window->send_times[idx] = now;
                
                printf("  🔄 Retransmitindo seq=%d (timeout=%dms)\n", seq, timeout_ms);
            }
        }
        
        pthread_mutex_unlock(&window->lock);
    }
    
    return NULL;
}

void send_ack(int sockfd, int seq_num, struct sockaddr_in *addr, socklen_t addr_len)
{
    Packet ack;
    memset(&ack, 0, sizeof(Packet));
    ack.type = PKT_ACK;
    ack.seq_num = seq_num;
    
    sendto(sockfd, &ack, sizeof(Packet), 0, (struct sockaddr*)addr, addr_len);
    printf("  ACK enviado para seq=%d\n", seq_num);
}

//Upload
void upload_file(int sockfd, const char *filename, struct sockaddr_in *server_addr, socklen_t addr_len)
{
    printf("\n═══════════════════════════════════════════\n");
    printf("UPLOAD: %s (Selective Repeat)\n", filename);
    printf("═══════════════════════════════════════════\n");
    
    int fd = open(filename, O_RDONLY);
    if (fd == -1) {
        printf("❌ Erro ao abrir arquivo: %s\n", filename);
        return;
    }
    
    // Enviar requisição inicial (Stop-and-Wait)
    Packet req;
    memset(&req, 0, sizeof(Packet));
    req.type = PKT_UPLOAD_REQUEST;
    req.seq_num = 0;
    strncpy(req.filename, filename, sizeof(req.filename) - 1);
    
    printf("Enviando requisição de upload...\n");
    sendto(sockfd, &req, sizeof(Packet), 0, (struct sockaddr*)server_addr, addr_len);
    
    // Aguardar ACK da requisição
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    Packet ack;
    struct sockaddr_in server_thread_addr;
    socklen_t server_thread_len = sizeof(server_thread_addr);
    
    // ✅ CRÍTICO: Receber ACK e descobrir porta da thread do servidor
    if (recvfrom(sockfd, &ack, sizeof(Packet), 0, 
                (struct sockaddr*)&server_thread_addr, &server_thread_len) <= 0 || 
        ack.type != PKT_ACK) {
        printf("❌ Servidor não respondeu à requisição\n");
        close(fd);
        return;
    }
    
    printf("✓ Servidor pronto para receber\n");
    printf("✓ Thread do servidor: %s:%d\n\n", 
           inet_ntoa(server_thread_addr.sin_addr), 
           ntohs(server_thread_addr.sin_port));
    
    // ✅ Atualizar endereço do servidor para a porta da thread
    *server_addr = server_thread_addr;
    
    // ✅ Inicializar janela deslizante
    SlidingWindow window;
    memset(&window, 0, sizeof(SlidingWindow));
    window.base = 0;
    window.next_seq_num = 0;
    window.sockfd = sockfd;
    window.server_addr = server_addr;
    window.addr_len = addr_len;
    window.finished = 0;
    window.estimated_rtt = 1.0;
    window.dev_rtt = 0.5;
    pthread_mutex_init(&window.lock, NULL);
    
    // ✅ Ler arquivo e preparar pacotes (buffer dinâmico)
    Packet *all_packets = (Packet*)malloc(1000 * sizeof(Packet));
    if (!all_packets) {
        printf("❌ Erro ao alocar memória\n");
        close(fd);
        return;
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
    
    window.total_packets = total_packets;
    printf("📦 Total de pacotes: %d\n", total_packets);
    printf("📊 Tamanho da janela: %d\n\n", WINDOW_SIZE);
    
    // ✅ Criar threads
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
            
            // ✅ Enviar para a porta da thread (não para porta 9999)
            sendto(sockfd, &window.packets[idx], sizeof(Packet), 0, 
                   (struct sockaddr*)server_addr, addr_len);
            
            printf("📤 Enviado seq=%d [base=%d, janela=%d-%d]\n", 
                   window.next_seq_num, window.base, 
                   window.base, window.base + WINDOW_SIZE - 1);
            
            window.next_seq_num++;
        }
        
        pthread_mutex_unlock(&window.lock);
        usleep(10000); // 10ms
    }
    
    printf("\n⏳ Aguardando ACKs finais...\n");
    sleep(2); // Aguarda ACKs finais
    
    // ✅ Enviar pacote END
    Packet end_pkt;
    memset(&end_pkt, 0, sizeof(Packet));
    end_pkt.type = PKT_END;
    end_pkt.seq_num = total_packets;
    
    printf("Enviando pacote END...\n");
    for (int i = 0; i < 3; i++) {
        // ✅ END também vai para porta da thread
        sendto(sockfd, &end_pkt, sizeof(Packet), 0, 
               (struct sockaddr*)server_addr, addr_len);
        usleep(100000);
    }
    
    // ✅ Encerrar threads
    window.finished = 1;
    pthread_join(tid_ack, NULL);
    pthread_join(tid_timeout, NULL);
    pthread_mutex_destroy(&window.lock);
    
    printf("\n✓ Upload concluído! (%d pacotes)\n", total_packets);
    printf("═══════════════════════════════════════════\n\n");
    
    free(all_packets);
}

//Download
void download_file(int sockfd, const char *filename, struct sockaddr_in *server_addr, socklen_t addr_len)
{
    printf("\n═══════════════════════════════════════════\n");
    printf("DOWNLOAD: %s (Selective Repeat)\n", filename);
    printf("═══════════════════════════════════════════\n");
    
    Packet req;
    memset(&req, 0, sizeof(Packet));
    req.type = PKT_DOWNLOAD_REQUEST;
    req.seq_num = 0;
    strncpy(req.filename, filename, sizeof(req.filename) - 1);
    
    printf("Enviando requisição de download...\n");
    sendto(sockfd, &req, sizeof(Packet), 0, (struct sockaddr*)server_addr, addr_len);
    
    char download_filename[300];
    snprintf(download_filename, sizeof(download_filename), "downloaded_%s", filename);
    
    int fd = creat(download_filename, 0666);
    if (fd == -1) {
        printf("❌ Erro ao criar arquivo\n");
        return;
    }
    
    // ✅ Buffer dinâmico para recebimento fora de ordem
    Packet *buffer = (Packet*)malloc(1000 * sizeof(Packet));
    int *received = (int*)calloc(1000, sizeof(int));
    if (!buffer || !received) {
        printf("❌ Erro ao alocar memória\n");
        close(fd);
        if (buffer) free(buffer);
        if (received) free(received);
        return;
    }
    int base = 0;
    
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    int first_packet = 1;
    
    while (1) {
        Packet pkt;
        memset(&pkt, 0, sizeof(Packet));
        
        int recv_len = recvfrom(sockfd, &pkt, sizeof(Packet), 0, 
                                (struct sockaddr*)&from_addr, &from_len);
        
        if (recv_len <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("⏰ Timeout\n");
            }
            break;
        }
        
        // ✅ CRÍTICO: Capturar porta da thread no primeiro pacote DATA
        if (first_packet && pkt.type == PKT_DATA) {
            printf("✓ Thread do servidor: %s:%d\n", 
                   inet_ntoa(from_addr.sin_addr), 
                   ntohs(from_addr.sin_port));
            first_packet = 0;
        }
        
        if (pkt.type == PKT_ERROR) {
            printf("❌ Erro: %s\n", pkt.data);
            close(fd);
            unlink(download_filename);
            free(buffer);
            free(received);
            return;
        }
        
        if (pkt.type == PKT_END) {
            Packet ack;
            memset(&ack, 0, sizeof(Packet));
            ack.type = PKT_ACK;
            ack.seq_num = pkt.seq_num;
            // ✅ ACK vai para porta da thread
            sendto(sockfd, &ack, sizeof(Packet), 0, 
                   (struct sockaddr*)&from_addr, from_len);
            printf("\n✓ Download concluído\n");
            break;
        }
        
        if (pkt.type == PKT_DATA) {
            // Verificar checksum
            unsigned int calc_checksum = calculate_checksum(pkt.data, pkt.data_len);
            if (pkt.checksum != calc_checksum) {
                printf("❌ Checksum inválido seq=%d\n", pkt.seq_num);
                continue;
            }
            
            // ✅ Armazenar pacote (mesmo fora de ordem)
            if (!received[pkt.seq_num]) {
                buffer[pkt.seq_num] = pkt;
                received[pkt.seq_num] = 1;
                printf("📥 Recebido seq=%d ✓ Checksum OK\n", pkt.seq_num);
            }
            
            // ✅ Enviar ACK seletivo para porta da thread (from_addr já está correto)
            Packet ack;
            memset(&ack, 0, sizeof(Packet));
            ack.type = PKT_ACK;
            ack.seq_num = pkt.seq_num;
            sendto(sockfd, &ack, sizeof(Packet), 0, 
                   (struct sockaddr*)&from_addr, from_len);
            
            // ✅ Escrever pacotes em ordem no arquivo
            while (received[base]) {
                write(fd, buffer[base].data, buffer[base].data_len);
                printf("💾 Escrito seq=%d no arquivo\n", base);
                base++;
            }
        }
    }
    
    close(fd);
    free(buffer);
    free(received);
    printf("═══════════════════════════════════════════\n\n");
}

int main(void)
{
    struct sockaddr_in si_other;
    int s;
    socklen_t slen = sizeof(si_other);
    char server_ip[16];
    char command[10];
    char filename[256];
    
    printf("═══════════════════════════════════════════\n");
    printf("   CLIENTE FTP UDP - SLIDING WINDOW\n");
    printf("═══════════════════════════════════════════\n\n");
    
    if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
        die("socket");
    }
    
    printf("Digite o IP do servidor: ");
    fgets(server_ip, sizeof(server_ip), stdin);
    server_ip[strcspn(server_ip, "\n")] = 0;
    
    memset((char *)&si_other, 0, sizeof(si_other));
    si_other.sin_family = AF_INET;
    si_other.sin_port = htons(PORT);
    
    if (inet_aton(server_ip, &si_other.sin_addr) == 0) {
        fprintf(stderr, "Endereço inválido\n");
        exit(1);
    }
    
    printf("✓ Conectado ao servidor %s:%d\n\n", server_ip, PORT);
    
    printf("Comandos disponíveis:\n");
    printf("  upload <arquivo>   - Enviar arquivo\n");
    printf("  download <arquivo> - Baixar arquivo\n");
    printf("  sair               - Encerrar cliente\n\n");
    
    while (1) {
        printf("> ");
        fgets(command, sizeof(command), stdin);
        command[strcspn(command, "\n")] = 0;
        
        if (strcmp(command, "sair") == 0) {
            break;
        }
        else if (strcmp(command, "upload") == 0) {
            printf("Nome do arquivo: ");
            fgets(filename, sizeof(filename), stdin);
            filename[strcspn(filename, "\n")] = 0;
            
            upload_file(s, filename, &si_other, slen);
        }
        else if (strcmp(command, "download") == 0) {
            printf("Nome do arquivo: ");
            fgets(filename, sizeof(filename), stdin);
            filename[strcspn(filename, "\n")] = 0;
            
            download_file(s, filename, &si_other, slen);
        }
        else {
            printf("Comando não reconhecido. Use: upload, download ou sair\n");
        }
    }
    
    close(s);
    printf("\nEncerrando cliente...\n");
    return 0;
}
