/*
    FTP Client com UDP - Stop and Wait
    Suporta upload e download de arquivos
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

#define BUFLEN 1024
#define PORT 9999
#define INITIAL_TIMEOUT_SEC 5
#define MAX_RETRIES 5
#define ALPHA 0.125
#define BETA 0.25

// Tipos de pacotes
#define PKT_UPLOAD_REQUEST 1
#define PKT_DOWNLOAD_REQUEST 2
#define PKT_DATA 3
#define PKT_ACK 4
#define PKT_END 5
#define PKT_ERROR 6

// Estrutura do pacote
typedef struct {
    int type;
    int seq_num;
    int data_len;
    char filename[256];
    char data[BUFLEN];
    unsigned int checksum;
} Packet;

void die(const char *s)
{
    perror(s);
    exit(1);
}

// Calcular CRC32
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

// Obter timestamp
long long get_timestamp_ms()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)(tv.tv_sec) * 1000 + (tv.tv_usec) / 1000;
}

// Função para enviar pacote com retransmissão e timeout adaptativo
int send_packet_with_ack(int sockfd, Packet *pkt, struct sockaddr_in *addr, 
                         socklen_t addr_len, double *estimated_rtt, double *dev_rtt)
{
    Packet ack;
    int tentativa = 0;
    
    // Calcular checksum
    pkt->checksum = calculate_checksum(pkt->data, pkt->data_len);
    
    // Timeout adaptativo
    int timeout_ms = (int)((*estimated_rtt + 4 * (*dev_rtt)) * 1000);
    if (timeout_ms < 1000) timeout_ms = 1000;
    if (timeout_ms > 10000) timeout_ms = 10000;
    
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    while (tentativa < MAX_RETRIES) {
        long long send_time = get_timestamp_ms();
        
        if (sendto(sockfd, pkt, sizeof(Packet), 0, (struct sockaddr*)addr, addr_len) == -1) {
            perror("sendto");
            return -1;
        }
        
        printf("  Enviado seq=%d (tent. %d/%d, timeout=%dms)\n", 
               pkt->seq_num, tentativa + 1, MAX_RETRIES, timeout_ms);
        
        memset(&ack, 0, sizeof(Packet));
        int recv_len = recvfrom(sockfd, &ack, sizeof(Packet), 0, 
                               (struct sockaddr*)addr, &addr_len);
        
        if (recv_len > 0 && ack.type == PKT_ACK && ack.seq_num == pkt->seq_num) {
            long long recv_time = get_timestamp_ms();
            double sample_rtt = (recv_time - send_time) / 1000.0;
            
            // Atualizar RTT
            *dev_rtt = (1 - BETA) * (*dev_rtt) + BETA * fabs(sample_rtt - *estimated_rtt);
            *estimated_rtt = (1 - ALPHA) * (*estimated_rtt) + ALPHA * sample_rtt;
            
            printf("  ✓ ACK recebido seq=%d (RTT=%.3fs, Est=%.3fs)\n", 
                   pkt->seq_num, sample_rtt, *estimated_rtt);
            return 0;
        }
        
        if (recv_len == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            printf("   Timeout aguardando ACK seq=%d\n", pkt->seq_num);
            timeout_ms = (int)(timeout_ms * 1.5);
            if (timeout_ms > 10000) timeout_ms = 10000;
        }
        
        tentativa++;
    }
    
    printf("  Falha após %d tentativas para seq=%d\n", MAX_RETRIES, pkt->seq_num);
    return -1;
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

// Upload sem bug de leitura
void upload_file(int sockfd, const char *filename, struct sockaddr_in *server_addr, socklen_t addr_len)
{
    printf("\n═══════════════════════════════════════════\n");
    printf("UPLOAD: %s\n", filename);
    printf("═══════════════════════════════════════════\n");
    
    int fd = open(filename, O_RDONLY);
    if (fd == -1) {
        printf(" Erro ao abrir arquivo: %s\n", filename);
        return;
    }
    
    // Enviar requisição
    Packet pkt;
    memset(&pkt, 0, sizeof(Packet));
    pkt.type = PKT_UPLOAD_REQUEST;
    pkt.seq_num = 0;
    strncpy(pkt.filename, filename, sizeof(pkt.filename) - 1);
    
    double estimated_rtt = 1.0;
    double dev_rtt = 0.5;
    
    printf("Enviando requisição de upload...\n");
    if (send_packet_with_ack(sockfd, &pkt, server_addr, addr_len, 
                            &estimated_rtt, &dev_rtt) == -1) {
        printf(" Falha ao enviar requisição\n");
        close(fd);
        return;
    }
    
    // Enviar arquivo sem bug
    int seq_num = 0;
    int bytes_read;
    
    while (1) {
        memset(&pkt, 0, sizeof(Packet));
        
        bytes_read = read(fd, pkt.data, BUFLEN);
        
        if (bytes_read <= 0) break;
        
        pkt.type = PKT_DATA;
        pkt.seq_num = seq_num;
        pkt.data_len = bytes_read;
        
        printf("Enviando pacote %d (%d bytes)\n", seq_num, bytes_read);
        
        if (send_packet_with_ack(sockfd, &pkt, server_addr, addr_len,
                                &estimated_rtt, &dev_rtt) == -1) {
            printf(" Falha ao enviar pacote %d\n", seq_num);
            close(fd);
            return;
        }
        
        seq_num++;
    }
    
    close(fd);
    
    // Envia END
    memset(&pkt, 0, sizeof(Packet));
    pkt.type = PKT_END;
    pkt.seq_num = seq_num;
    
    printf("Enviando pacote END...\n");
    send_packet_with_ack(sockfd, &pkt, server_addr, addr_len, &estimated_rtt, &dev_rtt);
    
    printf("\n✓ Upload concluído! (%d pacotes enviados)\n", seq_num);
    printf("═══════════════════════════════════════════\n\n");
}

// Download com verificação de checksum
void download_file(int sockfd, const char *filename, struct sockaddr_in *server_addr, socklen_t addr_len)
{
    printf("\n═══════════════════════════════════════════\n");
    printf("DOWNLOAD: %s\n", filename);
    printf("═══════════════════════════════════════════\n");
    
    Packet pkt;
    memset(&pkt, 0, sizeof(Packet));
    pkt.type = PKT_DOWNLOAD_REQUEST;
    pkt.seq_num = 0;
    strncpy(pkt.filename, filename, sizeof(pkt.filename) - 1);
    
    printf("Enviando requisição de download...\n");
    if (sendto(sockfd, &pkt, sizeof(Packet), 0, (struct sockaddr*)server_addr, addr_len) == -1) {
        printf(" Erro ao enviar requisição\n");
        return;
    }
    
    char download_filename[300];
    snprintf(download_filename, sizeof(download_filename), "downloaded_%s", filename);
    
    int fd = creat(download_filename, 0666);
    if (fd == -1) {
        printf(" Erro ao criar arquivo: %s\n", download_filename);
        return;
    }
    
    int expected_seq = 0;
    
    struct timeval tv;
    tv.tv_sec = INITIAL_TIMEOUT_SEC * 2;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    // Variável para aceitar resposta de qualquer porta do servidor
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    
    while (1) {
        memset(&pkt, 0, sizeof(Packet));
        
        // Recebe de qualquer porta (não apenas 9999)
        int recv_len = recvfrom(sockfd, &pkt, sizeof(Packet), 0, 
                                (struct sockaddr*)&from_addr, &from_len);
        
        if (recv_len <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf(" Timeout aguardando pacote\n");
            }
            break;
        }
        
        printf("Recebido pacote tipo=%d seq=%d\n", pkt.type, pkt.seq_num);
        
        if (pkt.type == PKT_ERROR) {
            printf(" Erro do servidor: %s\n", pkt.data);
            close(fd);
            unlink(download_filename);
            return;
        }
        
        if (pkt.type == PKT_END) {
            // Envia ACK para o endereço que enviou (pode ser porta diferente)
            send_ack(sockfd, pkt.seq_num, &from_addr, from_len);
            printf("\n✓ Download concluído: %s\n", download_filename);
            break;
        }
        
        if (pkt.type == PKT_DATA) {
            // Verificar checksum
            unsigned int received_checksum = pkt.checksum;
            unsigned int calculated_checksum = calculate_checksum(pkt.data, pkt.data_len);
            
            if (received_checksum != calculated_checksum) {
                printf(" Checksum inválido para seq=%d! Descartando.\n", pkt.seq_num);
                continue;
            }
            
            if (pkt.seq_num == expected_seq) {
                write(fd, pkt.data, pkt.data_len);
                printf("Pacote %d escrito (%d bytes) ✓ Checksum OK\n", 
                       pkt.seq_num, pkt.data_len);
                
                // Envia ACK para o endereço que enviou (pode ser porta diferente)
                send_ack(sockfd, pkt.seq_num, &from_addr, from_len);
                expected_seq++;
            } else {
                printf("Pacote fora de ordem: esperado=%d, recebido=%d\n", 
                       expected_seq, pkt.seq_num);
                if (expected_seq > 0) {
                    send_ack(sockfd, expected_seq - 1, &from_addr, from_len);
                }
            }
        }
    }
    
    close(fd);
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
    printf("   CLIENTE FTP UDP - STOP AND WAIT\n");
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
    printf("  upload <arquivo>   - Enviar arquivo para o servidor\n");
    printf("  download <arquivo> - Baixar arquivo do servidor\n");
    printf("  sair               - Encerrar cliente\n\n");
    
    while (1) {
        printf("> ");
        fgets(command, sizeof(command), stdin);
        command[strcspn(command, "\n")] = 0;
        
        if (strcmp(command, "sair") == 0) {
            break;
        }
        else if (strcmp(command, "upload") == 0 || strcmp(command, "UPLOAD") == 0) {
            printf("Nome do arquivo: ");
            fgets(filename, sizeof(filename), stdin);
            filename[strcspn(filename, "\n")] = 0;
            
            upload_file(s, filename, &si_other, slen);
        }
        else if (strcmp(command, "download") == 0 || strcmp(command, "DOWNLOAD") == 0) {
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
