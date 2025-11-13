#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PORTA 8080
#define BUFFER_SIZE 1024

// Estrutura para representar um pacote de dados
typedef struct {
	char ip_origem[16];
	char ip_destino[16];
	char dados[256];
} Pacote;

// Variáveis globais
int sock_global = 0;
char ip_origem_global[16];
pthread_mutex_t mutex_terminal = PTHREAD_MUTEX_INITIALIZER;
int enviando_dados = 0;

void limpar_buffer() {
	int c;
	while ((c = getchar()) != '\n' && c != EOF);
}

// Thread para receber dados do roteador
void* thread_receber(void* arg) {
	Pacote pacote_recebido;

	pthread_detach(pthread_self());

	while (1) {
		// Aguardar se estiver enviando dados
		pthread_mutex_lock(&mutex_terminal);
		int esta_enviando = enviando_dados;
		pthread_mutex_unlock(&mutex_terminal);

		if (esta_enviando) {
			usleep(100000);  // 100ms
			continue;
		}

		// Receber dados (bloqueante, mas com timeout)
		struct timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 100000;  // 100ms timeout
		setsockopt(sock_global, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		int bytes_recebidos = recv(sock_global, &pacote_recebido, sizeof(Pacote), 0);

		if (bytes_recebidos > 0) {
			// Verificar se o pacote é destinado a este IP
			if (strcmp(pacote_recebido.ip_destino, ip_origem_global) == 0) {
				pthread_mutex_lock(&mutex_terminal);
				printf("\n========================================\n");
				printf("PACOTE RECEBIDO!\n");
				printf("========================================\n");
				printf("De: %s\n", pacote_recebido.ip_origem);
				printf("Para: %s\n", pacote_recebido.ip_destino);
				printf("Dados: %s\n", pacote_recebido.dados);
				printf("========================================\n\n");
				printf("> ");  // Re-exibir prompt se estiver esperando entrada
				fflush(stdout);
				pthread_mutex_unlock(&mutex_terminal);
			}
		} else if (bytes_recebidos == 0) {
			// Conexão fechada pelo servidor
			pthread_mutex_lock(&mutex_terminal);
			printf("\n[ERRO] Conexão com o roteador foi perdida!\n");
			printf("Encerrando programa...\n");
			pthread_mutex_unlock(&mutex_terminal);
			exit(1);
		}

		usleep(50000);  // 50ms entre verificações
	}

	return NULL;
}

// Thread para enviar dados
void* thread_enviar(void* arg) {
	pthread_detach(pthread_self());

	// Exibir prompt inicial uma vez
	pthread_mutex_lock(&mutex_terminal);
	printf("\n> Digite 'enviar' para enviar um pacote (ou Ctrl+C para sair): ");
	fflush(stdout);
	pthread_mutex_unlock(&mutex_terminal);

	while (1) {
		char comando[50];

		// Usar select para verificar se há entrada disponível (não bloqueante)
		fd_set readfds;
		struct timeval tv;

		FD_ZERO(&readfds);
		FD_SET(STDIN_FILENO, &readfds);

		tv.tv_sec = 1;  // Timeout de 1 segundo
		tv.tv_usec = 0;

		int retval = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);

		if (retval == -1) {
			perror("select()");
			sleep(1);
			continue;
		} else if (retval == 0) {
			// Timeout - nenhuma entrada disponível, continuar loop
			continue;
		}

		// Há entrada disponível, ler
		pthread_mutex_lock(&mutex_terminal);

		if (fgets(comando, sizeof(comando), stdin) == NULL) {
			pthread_mutex_unlock(&mutex_terminal);
			continue;
		}
		comando[strcspn(comando, "\n")] = 0;

		// Verificar se o comando é para enviar
		if (strcmp(comando, "enviar") != 0) {
			printf("Comando não reconhecido. Use 'enviar' para enviar pacotes.\n");
			printf("> Digite 'enviar' para enviar um pacote (ou Ctrl+C para sair): ");
			fflush(stdout);
			pthread_mutex_unlock(&mutex_terminal);
			continue;
		}

		// Iniciar processo de envio
		enviando_dados = 1;

		Pacote pacote;
		strncpy(pacote.ip_origem, ip_origem_global, sizeof(pacote.ip_origem) - 1);

		printf("\n===========================================\n");
		printf("ENVIAR NOVO PACOTE\n");
		printf("===========================================\n");
		printf("Digite o IP de destino: ");
		fflush(stdout);

		char input[256];
		fgets(input, sizeof(input), stdin);
		input[strcspn(input, "\n")] = 0;

		strncpy(pacote.ip_destino, input, sizeof(pacote.ip_destino) - 1);

		printf("Digite os dados a serem enviados: ");
		fflush(stdout);
		fgets(pacote.dados, sizeof(pacote.dados), stdin);
		pacote.dados[strcspn(pacote.dados, "\n")] = 0;

		printf("\nEnviando pacote para o roteador...\n");
		int bytes_enviados = send(sock_global, &pacote, sizeof(Pacote), 0);

		if (bytes_enviados < 0) {
			printf("[ERRO] Falha ao enviar pacote!\n");
			printf("Conexão com o roteador pode ter sido perdida.\n");
		} else {
			printf("Pacote enviado com sucesso!\n");
		}

		printf("===========================================\n");
		printf("\n> Digite 'enviar' para enviar um pacote (ou Ctrl+C para sair): ");
		fflush(stdout);

		enviando_dados = 0;
		pthread_mutex_unlock(&mutex_terminal);
	}

	return NULL;
}

int main() {
	struct sockaddr_in endereco_servidor;
	char ip_roteador[16];

	printf("===========================================\n");
	printf("    SIMULADOR DE MÁQUINA LOCAL\n");
	printf("===========================================\n\n");

	// Solicitar informações ao usuário
	printf("Digite o IP do roteador (ex: 192.168.1.1): ");
	fgets(ip_roteador, sizeof(ip_roteador), stdin);
	ip_roteador[strcspn(ip_roteador, "\n")] = 0;

	// Gerar IP falso no formato 192.168.3.X (onde X é aleatório)
	srand(time(NULL));
	int x_aleatorio = (rand() % 254) + 2;  // Gera entre 2 e 255
	snprintf(ip_origem_global, sizeof(ip_origem_global), "192.168.3.%d", x_aleatorio);

	printf("Seu IP gerado na rede: %s\n\n", ip_origem_global);

	printf("Conectando ao roteador...\n");

	// Criar socket
	if ((sock_global = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		printf("\nErro ao criar socket\n");
		return -1;
	}

	endereco_servidor.sin_family = AF_INET;
	endereco_servidor.sin_port = htons(PORTA);

	// Converter endereço IP
	if (inet_pton(AF_INET, ip_roteador, &endereco_servidor.sin_addr) <= 0) {
		printf("\nEndereço inválido ou não suportado\n");
		return -1;
	}

	// Conectar ao roteador
	if (connect(sock_global, (struct sockaddr*)&endereco_servidor, sizeof(endereco_servidor)) < 0) {
		printf("\nFalha na conexão com o roteador\n");
		return -1;
	}

	// Obter IP local da conexão estabelecida (IP real da conexão TCP)
	struct sockaddr_in endereco_local;
	socklen_t addr_size = sizeof(endereco_local);
	getsockname(sock_global, (struct sockaddr*)&endereco_local, &addr_size);
	char ip_local[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &endereco_local.sin_addr, ip_local, INET_ADDRSTRLEN);

	printf("Conectado com sucesso!\n");
	printf("IP real da conexão: %s\n", ip_local);
	printf("IP simulado na rede: %s\n", ip_origem_global);
	printf("===========================================\n\n");

	// Enviar pacote inicial para registrar no roteador
	printf("Registrando no roteador...\n");
	Pacote pacote_registro;
	strncpy(pacote_registro.ip_origem, ip_origem_global, sizeof(pacote_registro.ip_origem) - 1);
	strncpy(pacote_registro.ip_destino, "0.0.0.0", sizeof(pacote_registro.ip_destino) - 1);
	strncpy(pacote_registro.dados, "[REGISTRO]", sizeof(pacote_registro.dados) - 1);

	if (send(sock_global, &pacote_registro, sizeof(Pacote), 0) < 0) {
		printf("Erro ao registrar no roteador!\n");
		return -1;
	}
	printf("Registrado com sucesso!\n\n");

	// Criar threads para receber e enviar
	pthread_t tid_receber, tid_enviar;

	printf("Iniciando threads de comunicação...\n");
	printf("Conexão permanente estabelecida com o roteador.\n");
	printf("===========================================\n");
	printf("COMANDOS:\n");
	printf("  - Digite 'enviar' para enviar um pacote\n");
	printf("  - Pacotes recebidos serão exibidos automaticamente\n");
	printf("  - Ctrl+C para sair\n");
	printf("===========================================\n\n");

	if (pthread_create(&tid_receber, NULL, thread_receber, NULL) != 0) {
		perror("Erro ao criar thread de recepção");
		return -1;
	}

	if (pthread_create(&tid_enviar, NULL, thread_enviar, NULL) != 0) {
		perror("Erro ao criar thread de envio");
		return -1;
	}

	// Aguardar threads (loop infinito)
	pthread_join(tid_receber, NULL);
	pthread_join(tid_enviar, NULL);

	close(sock_global);
	return 0;
}
