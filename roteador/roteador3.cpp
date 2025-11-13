//server
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORTA 8080
#define BUFFER_SIZE 1024
#define MAX_CLIENTES 100

// Estrutura para representar um pacote de dados
typedef struct {
	char ip_origem[16];
	char ip_destino[16];
	char dados[256];
} Pacote;

// Estrutura para armazenar cliente conectado
typedef struct {
	char ip_simulado[16];
	int socket;
	pthread_t thread_id;
	int ativo;
} Cliente;

// Tabela de clientes conectados
Cliente tabela_clientes[MAX_CLIENTES];
int num_clientes = 0;
pthread_mutex_t mutex_clientes = PTHREAD_MUTEX_INITIALIZER;

// Função para verificar se dois IPs estão na mesma rede
int mesma_rede(const char* ip1, const char* ip2, const char* mascara) {
	struct in_addr addr1, addr2, mask;

	inet_pton(AF_INET, ip1, &addr1);
	inet_pton(AF_INET, ip2, &addr2);
	inet_pton(AF_INET, mascara, &mask);

	// Aplica a máscara de rede em ambos os IPs
	unsigned int rede1 = addr1.s_addr & mask.s_addr;
	unsigned int rede2 = addr2.s_addr & mask.s_addr;

	return rede1 == rede2;
}

// Função para rotear o pacote
void rotear_pacote(Pacote* pacote, const char* rede_local) {
	printf("\n========================================\n");
	printf("ROTEADOR: Pacote recebido\n");
	printf("========================================\n");
	printf("IP Origem: %s\n", pacote->ip_origem);
	printf("IP Destino: %s\n", pacote->ip_destino);
	printf("Dados: %s\n", pacote->dados);
	printf("----------------------------------------\n");

	// Verifica se o destino está na mesma rede (usando máscara /24 = 255.255.255.0)
	if (mesma_rede(pacote->ip_origem, pacote->ip_destino, "255.255.255.0")) {
		printf("DECISÃO: Destino está na MESMA REDE LOCAL\n");
		printf("Ação: Encaminhar diretamente via switch/hub\n");
	} else {
		printf("DECISÃO: Destino está em REDE EXTERNA\n");
		printf("Ação: Encaminhar para gateway/roteador externo\n");
	}
	printf("========================================\n\n");
}

// Adicionar cliente à tabela
void adicionar_cliente(const char* ip_simulado, int socket, pthread_t thread_id) {
	pthread_mutex_lock(&mutex_clientes);

	if (num_clientes < MAX_CLIENTES) {
		strncpy(tabela_clientes[num_clientes].ip_simulado, ip_simulado, sizeof(tabela_clientes[num_clientes].ip_simulado) - 1);
		tabela_clientes[num_clientes].socket = socket;
		tabela_clientes[num_clientes].thread_id = thread_id;
		tabela_clientes[num_clientes].ativo = 1;
		num_clientes++;

		printf("[TABELA] Cliente %s adicionado. Total de clientes: %d\n", ip_simulado, num_clientes);
	}

	pthread_mutex_unlock(&mutex_clientes);
}

// Remover cliente da tabela
void remover_cliente(pthread_t thread_id) {
	pthread_mutex_lock(&mutex_clientes);

	for (int i = 0; i < num_clientes; i++) {
		if (tabela_clientes[i].thread_id == thread_id) {
			printf("[TABELA] Cliente %s removido.\n", tabela_clientes[i].ip_simulado);

			// Move os clientes seguintes para preencher o espaço
			for (int j = i; j < num_clientes - 1; j++) {
				tabela_clientes[j] = tabela_clientes[j + 1];
			}
			num_clientes--;
			break;
		}
	}

	pthread_mutex_unlock(&mutex_clientes);
}

// Buscar socket do cliente pelo IP simulado
int buscar_socket_cliente(const char* ip_destino) {
	int socket_encontrado = -1;

	pthread_mutex_lock(&mutex_clientes);

	for (int i = 0; i < num_clientes; i++) {
		if (strcmp(tabela_clientes[i].ip_simulado, ip_destino) == 0 && tabela_clientes[i].ativo) {
			socket_encontrado = tabela_clientes[i].socket;
			break;
		}
	}

	pthread_mutex_unlock(&mutex_clientes);

	return socket_encontrado;
}

// Enviar pacote para o destino
int enviar_pacote_destino(Pacote* pacote) {
	int socket_destino = buscar_socket_cliente(pacote->ip_destino);

	if (socket_destino != -1) {
		int bytes_enviados = send(socket_destino, pacote, sizeof(Pacote), 0);
		if (bytes_enviados > 0) {
			printf("[ROTEADOR] Pacote encaminhado para %s\n", pacote->ip_destino);
			return 1;
		} else {
			printf("[ERRO] Falha ao enviar pacote para %s\n", pacote->ip_destino);
			return 0;
		}
	} else {
		printf("[ERRO] Cliente com IP %s não encontrado na rede\n", pacote->ip_destino);
		return 0;
	}
}

// Estrutura para passar dados para a thread
typedef struct {
	int socket;
	struct sockaddr_in endereco_cliente;
} ThreadArgs;

// Função executada por cada thread para processar uma conexão
void* processar_conexao(void* arg) {
	ThreadArgs* args = (ThreadArgs*)arg;
	int socket_cliente = args->socket;
	struct sockaddr_in endereco_cliente = args->endereco_cliente;
	Pacote pacote;
	char ip_simulado_cliente[16] = {0};

	// Detach da thread para liberar recursos automaticamente
	pthread_detach(pthread_self());

	// Obter IP real do cliente
	char ip_cliente_real[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &endereco_cliente.sin_addr, ip_cliente_real, INET_ADDRSTRLEN);

	printf("Nova conexão estabelecida! [Thread ID: %lu]\n", pthread_self());
	printf("IP real do cliente: %s\n", ip_cliente_real);

	// Aguardar primeiro pacote para registrar o cliente
	int bytes_recebidos = recv(socket_cliente, &pacote, sizeof(Pacote), 0);

	if (bytes_recebidos > 0) {
		strncpy(ip_simulado_cliente, pacote.ip_origem, sizeof(ip_simulado_cliente) - 1);
		printf("IP simulado (origem): %s\n", ip_simulado_cliente);

		// Adicionar cliente à tabela
		adicionar_cliente(ip_simulado_cliente, socket_cliente, pthread_self());

		// Processar e rotear o primeiro pacote
		rotear_pacote(&pacote, "192.168.1.0");

		// Enviar o pacote para o destino
		if (mesma_rede(pacote.ip_origem, pacote.ip_destino, "255.255.255.0")) {
			enviar_pacote_destino(&pacote);
		} else {
			printf("[ROTEADOR] Pacote para rede externa - descartado (sem gateway configurado)\n");
		}
	} else {
		printf("[ERRO] Falha ao receber primeiro pacote\n");
		close(socket_cliente);
		free(args);
		return NULL;
	}

	// Loop para manter conexão persistente
	while (1) {
		bytes_recebidos = recv(socket_cliente, &pacote, sizeof(Pacote), 0);

		if (bytes_recebidos <= 0) {
			// Conexão fechada ou erro
			printf("[CONEXÃO] Cliente %s desconectado [Thread ID: %lu]\n", ip_simulado_cliente, pthread_self());
			break;
		}

		// Processar pacote recebido
		printf("\n========================================\n");
		printf("ROTEADOR: Pacote recebido de %s\n", ip_simulado_cliente);
		printf("========================================\n");
		printf("IP Origem: %s\n", pacote.ip_origem);
		printf("IP Destino: %s\n", pacote.ip_destino);
		printf("Dados: %s\n", pacote.dados);
		printf("----------------------------------------\n");

		// Verificar roteamento
		if (mesma_rede(pacote.ip_origem, pacote.ip_destino, "255.255.255.0")) {
			printf("DECISÃO: Destino está na MESMA REDE LOCAL\n");
			enviar_pacote_destino(&pacote);
		} else {
			printf("DECISÃO: Destino está em REDE EXTERNA\n");
			printf("[ROTEADOR] Pacote descartado (sem gateway configurado)\n");
		}
		printf("========================================\n\n");
	}

	// Remover cliente da tabela
	remover_cliente(pthread_self());
	close(socket_cliente);
	free(args);

	return NULL;
}

int main() {
	int servidor_fd, novo_socket;
	struct sockaddr_in endereco;
	int opt = 1;
	int addrlen = sizeof(endereco);
	Pacote pacote;

	printf("===========================================\n");
	printf("       SIMULADOR DE ROTEADOR\n");
	printf("===========================================\n");
	printf("Porta de escuta: %d\n", PORTA);
	printf("Roteamento entre redes 192.168.x.0/24\n");
	printf("===========================================\n\n");

	// Criar socket
	if ((servidor_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
		perror("Erro ao criar socket");
		exit(EXIT_FAILURE);
	}

	// Configurar opções do socket
	if (setsockopt(servidor_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
		perror("Erro no setsockopt");
		exit(EXIT_FAILURE);
	}

	endereco.sin_family = AF_INET;
	endereco.sin_addr.s_addr = INADDR_ANY;
	endereco.sin_port = htons(PORTA);

	// Bind do socket
	if (bind(servidor_fd, (struct sockaddr*)&endereco, sizeof(endereco)) < 0) {
		perror("Erro no bind");
		exit(EXIT_FAILURE);
	}

	// Escutar conexões
	if (listen(servidor_fd, 10) < 0) {
		perror("Erro no listen");
		exit(EXIT_FAILURE);
	}

	printf("Roteador aguardando conexões...\n");
	printf("Aceitando até %d clientes simultâneos.\n\n", MAX_CLIENTES);

	// Loop principal do roteador
	while (1) {
		// Aceitar conexão
		if ((novo_socket = accept(servidor_fd, (struct sockaddr*)&endereco, (socklen_t*)&addrlen)) < 0) {
			perror("Erro no accept");
			continue;
		}

		// Criar estrutura de argumentos para a thread
		ThreadArgs* args = (ThreadArgs*) malloc(sizeof(ThreadArgs));
		args->socket = novo_socket;
		args->endereco_cliente = endereco;

		// Criar thread para processar a conexão
		pthread_t thread_id;
		if (pthread_create(&thread_id, NULL, processar_conexao, (void*)args) != 0) {
			perror("Erro ao criar thread");
			close(novo_socket);
			free(args);
			continue;
		}
	}

	close(servidor_fd);
	return 0;
}