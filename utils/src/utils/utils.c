#include "utils/utils.h"
#include "global.h"

#include<stdio.h>
#include<stdlib.h>
#include<sys/socket.h>
#include<unistd.h>
#include<netdb.h>
#include<commons/log.h>
#include<commons/collections/list.h>
#include<string.h>

#include <commons/config.h>
#include <unistd.h>

int iniciar_servidor(char* puerto_escucha) {
	int socket_servidor;

	struct addrinfo hints, *servinfo;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	int err = getaddrinfo(NULL, puerto_escucha, &hints, &servinfo);

	if(err != 0) {
		return -1;
	}

	socket_servidor = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);

    int opt = 1;
    setsockopt(socket_servidor, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    // sirve para que no haya problemas de "address already in use" al reiniciar el servidor

	bind(socket_servidor, servinfo->ai_addr, servinfo->ai_addrlen);
	listen(socket_servidor, SOMAXCONN);
	freeaddrinfo(servinfo);

	return socket_servidor;
}

int esperar_cliente(int socket_servidor) {
	int socket_cliente = accept(socket_servidor, NULL, NULL);
	return socket_cliente;
}

t_paquete* recibir_paquete(int socket_cliente) {
	t_paquete* paquete = malloc(sizeof(t_paquete));
	paquete->buffer = malloc(sizeof(t_buffer));
	paquete->buffer->stream = NULL;

	// Recibir código de operación
	if (recv(socket_cliente, &(paquete->codigo_operacion), sizeof(int), MSG_WAITALL) <= 0) {
		free(paquete->buffer);
		free(paquete);
		return NULL;
	}

	// Recibir tamaño del buffer
	if (recv(socket_cliente, &(paquete->buffer->size), sizeof(int), MSG_WAITALL) <= 0) {
		free(paquete->buffer);
		free(paquete);
		return NULL;
	}

    if(paquete->buffer->size > 0) {
		// Recibir el contenido del buffer
        paquete->buffer->stream = malloc(paquete->buffer->size);
        if (recv(socket_cliente, paquete->buffer->stream, paquete->buffer->size, MSG_WAITALL) <= 0) {
			free(paquete->buffer->stream);
            free(paquete->buffer);
            free(paquete);
            return NULL;
        }
    }

	return paquete;
}


int crear_conexion(char *ip, char* puerto) {
	struct addrinfo hints;
	struct addrinfo *server_info;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	int err = getaddrinfo(ip, puerto, &hints, &server_info);

	// Error en la conexion
	if(err != 0) { 
		return -1;
	}

	int socket_cliente = socket(server_info->ai_family,server_info->ai_socktype, server_info->ai_protocol);

	if (connect(socket_cliente, server_info->ai_addr, server_info->ai_addrlen) == -1) {
		perror("connect");
		freeaddrinfo(server_info);
		close(socket_cliente);
		return -1;
	}

	freeaddrinfo(server_info);

	return socket_cliente;
}

t_paquete* crear_paquete(op_code codigo) {
    t_paquete* paquete = malloc(sizeof(t_paquete));
    paquete->codigo_operacion = codigo;
    paquete->buffer = malloc(sizeof(t_buffer));
    paquete->buffer->size = 0;
    paquete->buffer->stream = NULL;
    return paquete;
}

void agregar_a_paquete(t_paquete* paquete, void* valor, int tamanio) {
    paquete->buffer->stream = realloc(paquete->buffer->stream, paquete->buffer->size + tamanio);
    memcpy(paquete->buffer->stream + paquete->buffer->size, valor, tamanio);
    paquete->buffer->size += tamanio;
}

void enviar_paquete(t_paquete* paquete, int socket_cliente) {
	// Envía el código de operación
    send(socket_cliente, &(paquete->codigo_operacion), sizeof(op_code), 0);
    
    // Envía el tamaño del buffer (incluso si es 0)
    send(socket_cliente, &(paquete->buffer->size), sizeof(int), 0);
	
    // Solo envía el stream si hay datos (size > 0)
    if (paquete->buffer->size > 0 && paquete->buffer->stream != NULL) {
		send(socket_cliente, paquete->buffer->stream, paquete->buffer->size, 0);
    }

	// log_info(logger, "Enviado paquete: %s", op_code_a_string(paquete->codigo_operacion));
}

void eliminar_paquete(t_paquete* paquete) {
    if (paquete == NULL) return;
    if (paquete->buffer != NULL) {
        free(paquete->buffer->stream);
        free(paquete->buffer);
    }
    free(paquete);
}

char* op_code_a_string(op_code codigo) {
    switch (codigo) {
        case MENSAJE: return "MENSAJE";
        case HANDSHAKE: return "HANDSHAKE";
        case HANDSHAKE_KERNEL_IO: return "HANDSHAKE_KERNEL_IO";
        case HANDSHAKE_MEMORIA_KERNEL: return "HANDSHAKE_MEMORIA_KERNEL";
        case HANDSHAKE_MEMORIA_CPU: return "HANDSHAKE_MEMORIA_CPU";
        case HANDSHAKE_CPU_DISPATCH: return "HANDSHAKE_CPU_DISPATCH";
        case HANDSHAKE_CPU_INTERRUPT: return "HANDSHAKE_CPU_INTERRUPT";
        case HANDSHAKE_OK: return "HANDSHAKE_OK";
        case HANDSHAKE_TAM_MEMORIA: return "HANDSHAKE_TAM_MEMORIA";
        case PEDIR_INSTRUCCIONES: return "PEDIR_INSTRUCCIONES";
        case PEDIR_ESPACIO_LIBRE: return "PEDIR_ESPACIO_LIBRE";
        case PEDIDO_IO: return "PEDIDO_IO";
        case FINALIZO_IO: return "FINALIZO_IO";
        case DESCONECTADO_IO: return "DESCONECTADO_IO";
        case PROCESO_DESBLOQUEADO: return "PROCESO_DESBLOQUEADO";
        case IO: return "IO";
        case INIT_PROC: return "INIT_PROC";
        case EXIT: return "EXIT";
        case DUMP_MEMORY: return "DUMP_MEMORY";
        case WRITE: return "WRITE";
        case READ: return "READ";
        case NOOP: return "NOOP";
        case GOTO: return "GOTO";
        case OP_PCB: return "OP_PCB";
        case OP_RESPUESTA_EJECUCION: return "OP_RESPUESTA_EJECUCION";
        case PROCESO_BLOQUEANTE: return "PROCESO_BLOQUEANTE";
        case PROCESO_BLOQUEANTE_IO: return "PROCESO_BLOQUEANTE_IO";
        case PROCESO_BLOQUEANTE_DUMP_MEMORY: return "PROCESO_BLOQUEANTE_DUMP_MEMORY";
        case PROCESO_INTERRUPCION: return "PROCESO_INTERRUPCION";
        case PEDIR_CONTENIDO: return "PEDIR_CONTENIDO";
        case TRADUCIR_TABLA: return "TRADUCIR_TABLA";
        case FINALIZAR_PROCESO: return "FINALIZAR_PROCESO";
        case SOLICITAR_PAGINAS: return "SOLICITAR_PAGINAS";
        case LIBERAR_PAGINAS: return "LIBERAR_PAGINAS";
        case LEER_MEMORIA: return "LEER_MEMORIA";
        case ESCRIBIR_MEMORIA: return "ESCRIBIR_MEMORIA";
        case INSTRUCCION_OK: return "INSTRUCCION_OK";
        case ERROR_PID_NO_ENCONTRADO: return "ERROR_PID_NO_ENCONTRADO";
        case ERROR_PC_FUERA_DE_RANGO: return "ERROR_PC_FUERA_DE_RANGO";
        case PROCESO_NO_ALMACENADO: return "PROCESO_NO_ALMACENADO";
        case TRADUCCION_OK: return "TRADUCCION_OK";
        case ERROR_TRADUCCION: return "ERROR_TRADUCCION";
        case MARCO_NO_PRESENTE: return "MARCO_NO_PRESENTE";
        case NO_HAY_MARCOS_LIBRES: return "NO_HAY_MARCOS_LIBRES";
        case OPERACION_LECTURA_OK: return "OPERACION_LECTURA_OK";
        case OPERACION_ESCRITURA_OK: return "OPERACION_ESCRITURA_OK";
        case INIT_PROCESO_OK: return "INIT_PROCESO_OK";
        case DISPOSITIVO_IO: return "DISPOSITIVO_IO";
        case FINALIZACION_OK: return "FINALIZACION_OK";
        case DUMP_OK: return "DUMP_OK";
        case ERROR_DUMP: return"ERROR_DUMP";
        case SUSPENDER_PROCESO: return "SUSPENDER_PROCESO";
        case DESUSPENDER_PROCESO: return "DESUSPENDER_PROCESO";
        case SUSPENSION_OK: return "SUSPENSION_OK";

        case DESUSPENSION_OK: return "DESUSPENSION_OK";
        case ERROR_SWAP_FULL: return "ERROR_SWAP_FULL";
        case ERROR_MEMORIA_INSUFICIENTE: return "ERROR_MEMORIA_INSUFICIENTE";
        case ERROR_SEG_FAULT: return "ERROR_SEG_FAULT";
        default: return "UNKNOWN_OP_CODE";
    }
}


char* estado_a_string(op_code_estado estado) {
    switch (estado) {
        case STATE_NEW: return "NEW";
        case STATE_READY: return "READY";
        case STATE_RUNNING: return "RUNNING";
        case STATE_BLOCKED: return "BLOCKED";
        case STATE_EXIT: return "EXIT";
        case STATE_SUSPENDED_BLOCKED: return "SUSP_BLOCKED";
        case STATE_SUSPENDED_READY: return "SUSP_READY";
        default: return "UNKNOWN_ESTADO";
    }
}

char* resultado_ejecucion_a_string(resultado_ejecucion_instruccion_cpu resultado) {
    switch (resultado) {
        case EJECUCION_OK: return "EJECUCION_OK";
        case PROCESO_BLOQUEADO: return "PROCESO_BLOQUEADO";
        case PROCESO_FINALIZADO_EXIT: return "PROCESO_FINALIZADO_EXIT";
        case ERROR_EJECUCION_FATAL: return "ERROR_EJECUCION_FATAL";
        case ERROR_LECTURA_MEMORIA: return "ERROR_LECTURA_MEMORIA";
        case ERROR_ESCRITURA_MEMORIA: return "ERROR_ESCRITURA_MEMORIA";
        case PROCESO_BLOQUEADO_IO: return "PROCESO_BLOQUEADO_IO";
        case PROCESO_BLOQUEADO_DUMP_MEMORY: return "PROCESO_BLOQUEADO_DUMP_MEMORY";
        case SEGMENTATION_FAULT: return "SEGMENTATION_FAULT";
        default: return "UNKNOWN_RESULTADO_EJECUCION";
    }
}


