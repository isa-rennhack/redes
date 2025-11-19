/*
    Simple udp client
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>

#define SERVER "127.0.0.1"
#define BUFLEN 512
#define PORT 8888
#define TIMEOUT_SEC 2
#define MAX_RETRIES 3

void die(const char *s)
{
    perror(s);
    exit(1);
}

int main(void)
{
    struct sockaddr_in si_other;
    int s, i;
    socklen_t slen = sizeof(si_other);
    char buf[BUFLEN];
    char message[BUFLEN];

    if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    {
        die("socket");
    }

    memset((char *)&si_other, 0, sizeof(si_other));

    si_other.sin_family = AF_INET;
    si_other.sin_port = htons(PORT);

    if (inet_aton(SERVER, &si_other.sin_addr) == 0)
    {
        fprintf(stderr, "inet_aton() failed\n");
        exit(1);
    }

    while (1)
    {
        printf("Enter message : ");
        fgets(message, BUFLEN, stdin);
        message[strcspn(message, "\n")] = 0; // remove newline

        struct timeval tv;
        tv.tv_sec = TIMEOUT_SEC;
        tv.tv_usec = 0;
        if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        {
            die("setsockopt");
        }

        int tentativa = 0;
        int ack_recebido = 0;

        while (tentativa < MAX_RETRIES && !ack_recebido)
        {
            // send the message
            if (sendto(s, message, strlen(message), 0, (struct sockaddr *)&si_other, slen) == -1)
            {
                die("sendto()");
            }

            printf("Waiting ACK...\n");

            // receive a reply and print it
            // clear the buffer by filling null, it might have previously received data
            memset(buf, '\0', BUFLEN);
            // try to receive some data, this is a blocking call
            if (recvfrom(s, buf, BUFLEN, 0, (struct sockaddr *)&si_other, &slen) == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    printf("⚠️  Timeout: Nenhuma resposta recebida!\n");
                    tentativa++;
                }
                else
                {
                    die("recvfrom()");
                }
            }
            else
            {
                printf("✓ Mensagem confirmada: %s\n", buf);
                ack_recebido = 1;
            }
        }

        if (!ack_recebido) {
            printf("Fail\n");
        }

    }
    close(s);
    return 0;
}