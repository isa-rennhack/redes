/*
    FTP Client com UDP - Stop and Wait
    Suporta upload e download de arquivos
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

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
    int type;
    int seq_num;
    int data_len;
    char filename[256];
    char data[BUFLEN];
} Packet;

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

// Função para fazer UPLOAD (cliente envia arquivo para servidor)
void upload_file(int sockfd, const char *filename, struct sockaddr_in *server_addr, socklen_t addr_len)
{
    printf("\n═══════════════════════════════════════════\n");
    printf("UPLOAD: %s\n", filename);
    printf("═══════════════════════════════════════════\n");
    
    // Abrir arquivo para leitura
    int fd = open(filename, O_RDONLY);
    if (fd == -1) {
        printf("❌ Erro ao abrir arquivo: %s\n", filename);
        return;
    }
    
    // Enviar requisição de upload
    Packet pkt;
    memset(&pkt, 0, sizeof(Packet));
    pkt.type = PKT_UPLOAD_REQUEST;
    pkt.seq_num = 0;
    strncpy(pkt.filename, filename, sizeof(pkt.filename) - 1);
    
    printf("Enviando requisição de upload...\n");
    if (send_packet_with_ack(sockfd, &pkt, server_addr, addr_len) == -1) {
        printf("❌ Falha ao enviar requisição\n");
        close(fd);
        return;
    }
    
    // Enviar arquivo em pacotes
    int seq_num = 0;
    int bytes_read;
    
    while ((bytes_read = read(fd, pkt.data, BUFLEN)) > 0) {
        memset(&pkt, 0, sizeof(Packet));
        pkt.type = PKT_DATA;
        pkt.seq_num = seq_num;
        pkt.data_len = bytes_read;
        
        // Ler novamente após memset
        lseek(fd, seq_num * BUFLEN, SEEK_SET);
        bytes_read = read(fd, pkt.data, BUFLEN);
        if (bytes_read <= 0) break;
        
        pkt.data_len = bytes_read;
        
        printf("Enviando pacote %d (%d bytes)\n", seq_num, bytes_read);
        
        if (send_packet_with_ack(sockfd, &pkt, server_addr, addr_len) == -1) {
            printf("❌ Falha ao enviar pacote %d\n", seq_num);
            close(fd);
            return;
        }
        
        seq_num++;
    }
    
    close(fd);
    
    // Enviar pacote END
    memset(&pkt, 0, sizeof(Packet));
    pkt.type = PKT_END;
    pkt.seq_num = seq_num;
    
    printf("Enviando pacote END...\n");
    send_packet_with_ack(sockfd, &pkt, server_addr, addr_len);
    
    printf("\n✓ Upload concluído! (%d pacotes enviados)\n", seq_num);
    printf("═══════════════════════════════════════════\n\n");
}

// Função para fazer DOWNLOAD (cliente recebe arquivo do servidor)
void download_file(int sockfd, const char *filename, struct sockaddr_in *server_addr, socklen_t addr_len)
{
    printf("\n═══════════════════════════════════════════\n");
    printf("DOWNLOAD: %s\n", filename);
    printf("═══════════════════════════════════════════\n");
    
    // Enviar requisição de download
    Packet pkt;
    memset(&pkt, 0, sizeof(Packet));
    pkt.type = PKT_DOWNLOAD_REQUEST;
    pkt.seq_num = 0;
    strncpy(pkt.filename, filename, sizeof(pkt.filename) - 1);
    
    printf("Enviando requisição de download...\n");
    if (sendto(sockfd, &pkt, sizeof(Packet), 0, (struct sockaddr*)server_addr, addr_len) == -1) {
        printf("❌ Erro ao enviar requisição\n");
        return;
    }
    
    // Criar arquivo para escrita
    char download_filename[300];
    snprintf(download_filename, sizeof(download_filename), "downloaded_%s", filename);
    
    int fd = creat(download_filename, 0666);
    if (fd == -1) {
        printf("❌ Erro ao criar arquivo: %s\n", download_filename);
        return;
    }
    
    // Receber pacotes
    int expected_seq = 0;
    
    // Configurar timeout
    struct timeval tv;
    tv.tv_sec = TIMEOUT_SEC * 2; // Timeout maior para recepção
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    while (1) {
        memset(&pkt, 0, sizeof(Packet));
        
        int recv_len = recvfrom(sockfd, &pkt, sizeof(Packet), 0, 
                                (struct sockaddr*)server_addr, &addr_len);
        
        if (recv_len <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("⚠️  Timeout aguardando pacote\n");
                break;
            }
            printf("❌ Erro ao receber pacote\n");
            break;
        }
        
        printf("Recebido pacote tipo=%d seq=%d\n", pkt.type, pkt.seq_num);
        
        if (pkt.type == PKT_ERROR) {
            printf("❌ Erro do servidor: %s\n", pkt.data);
            close(fd);
            unlink(download_filename);
            return;
        }
        
        if (pkt.type == PKT_END) {
            send_ack(sockfd, pkt.seq_num, server_addr, addr_len);
            printf("\n✓ Download concluído: %s\n", download_filename);
            break;
        }
        
        if (pkt.type == PKT_DATA) {
            if (pkt.seq_num == expected_seq) {
                // Escrever dados no arquivo
                write(fd, pkt.data, pkt.data_len);
                printf("Pacote %d escrito (%d bytes)\n", pkt.seq_num, pkt.data_len);
                
                // Enviar ACK
                send_ack(sockfd, pkt.seq_num, server_addr, addr_len);
                expected_seq++;
            } else {
                printf("Pacote fora de ordem: esperado=%d, recebido=%d\n", 
                       expected_seq, pkt.seq_num);
                // Reenviar último ACK
                if (expected_seq > 0) {
                    send_ack(sockfd, expected_seq - 1, server_addr, addr_len);
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
    printf("       CLIENTE FTP UDP - STOP AND WAIT\n");
    printf("═══════════════════════════════════════════\n\n");
    
    // Criar socket UDP
    if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
        die("socket");
    }
    
    // Configurar endereço do servidor
    printf("Digite o IP do servidor (ex: 127.0.0.1): ");
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
    
    // Loop de comandos
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
