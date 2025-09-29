#include <stdio.h>
#include <commons/log.h>
#include <commons/config.h>

#include <pthread.h>
#include <utils/hello.h>
#include <utils/utils.h>
#include <unistd.h>

#include "io.h"
#include <utils/global.h>
#include <utils/errors.h>

int socket_kernel;
t_log* logger;
t_config *config;

char *ip_kernel;
char *puerto_kernel;
char* io_nombre;


int main(int argc, char* argv[]) {

	if(argc < 3) {
		printf("Argumentos insuficientes\n");
		return -1;
	}
	
	io_nombre = argv[2];
	printf("Nombre IO: %s\n", io_nombre);

    saludar("io");
    /*------------------------------LOGGER Y CONFIG--------------------------------------------------*/

	logger = iniciar_logger();
    iniciar_manejador_de_errores(logger);
    config = config_create(argv[1]);

	if (config == NULL) {
        LOG_ERROR("Error al iniciar el archivo de configuración.");
		terminar_programa(logger, config);
		return -1;
	}

	ip_kernel = config_get_string_value(config, "IP_KERNEL");
	puerto_kernel = config_get_string_value(config, "PUERTO_KERNEL");

    /*-------------------------------CONEXION A KERNEL---------------------------------------------------------------*/
    
	int result_conexion_kernel = conectar_kernel(ip_kernel, puerto_kernel, io_nombre);
	if (result_conexion_kernel == -1) {
		terminar_programa(logger, config);
		return -1;
	}
	log_info(logger, "IO se conecto con el modulo Kernel correctamente");

    t_io* dispositivo_io = malloc(sizeof(t_io));
    dispositivo_io->nombre = strdup(io_nombre);
    dispositivo_io->socket_fd = socket_kernel;
   
    escuchar_solicitudes_io(logger, dispositivo_io);

    terminar_programa(logger, config);
    free(dispositivo_io->nombre);
    free(dispositivo_io);
    return 0;
}

/*-----------------------DECLARACION DE FUNCIONES--------------------------------------------*/

t_log* iniciar_logger(void) {
	t_log *nuevo_logger = log_create("io.log", "IO", true, LOG_LEVEL_INFO); 
	return nuevo_logger;
}

t_config* iniciar_config(void) {
	t_config *nueva_config = config_create("io.config");
	return nueva_config;
}

void terminar_programa(t_log *logger, t_config *config) {
	log_destroy(logger);
	config_destroy(config);
	close(socket_kernel);
}

int conectar_kernel(char *ip, char *puerto, char* io_nombre) {
	socket_kernel = crear_conexion(ip, puerto);
    if (socket_kernel == -1) {
        LOG_ERROR("No se pudo conectar con el modulo Kernel.");
        return -1;
    }

    t_paquete* paquete = crear_paquete(HANDSHAKE_KERNEL_IO);
    if (paquete == NULL) { 
        LOG_ERROR("Fallo al crear el paquete de handshake.");
        close(socket_kernel);
        return -1;
    }
    agregar_a_paquete(paquete, io_nombre, strlen(io_nombre) + 1);
    enviar_paquete(paquete, socket_kernel); 
    eliminar_paquete(paquete); 

	t_paquete* paquete_respuesta_kernel = recibir_paquete(socket_kernel);

    if (paquete_respuesta_kernel == NULL) {
        LOG_ERROR("Fallo al recibir respuesta del handshake con Kernel.");
        close(socket_kernel);
        return -1;
    }

    op_code respuestaHSKernel = paquete_respuesta_kernel->codigo_operacion;
    eliminar_paquete(paquete_respuesta_kernel);

    if (respuestaHSKernel != HANDSHAKE_OK) {
        LOG_ERROR("Handshake con Kernel fallido. Código: %d", respuestaHSKernel);
        close(socket_kernel);
        return -1;
    }
	
    log_info(logger, "Handshake con Kernel exitoso. Socket FD: %d", socket_kernel);

	return 0;
}


void escuchar_solicitudes_io(t_log* logger, t_io* dispositivo_io) {
    while (1) {
        op_code cod_op;

        int offset = 0;
        int len_nombre;
        int pid, tiempo;

        t_paquete* paquete = recibir_paquete(socket_kernel);
    
        if (!paquete) {
            LOG_ERROR("Error al recibir paquete de Kernel.");
            return;
        }
        log_info(logger, "Paquete recibido correctamente, procediendo a parsear.");

        cod_op = paquete->codigo_operacion;

        memcpy(&len_nombre, paquete->buffer->stream + offset, sizeof(int));
        offset += sizeof(int);

        char* nombre = malloc(len_nombre + 1);
        memcpy(nombre, paquete->buffer->stream + offset, len_nombre);
        nombre[len_nombre] = '\0';
        offset += len_nombre;

        memcpy(&pid, paquete->buffer->stream + offset, sizeof(int));
        offset += sizeof(int);

        memcpy(&tiempo, paquete->buffer->stream + offset, sizeof(int));
        offset += sizeof(int);
        
        eliminar_paquete(paquete);
        
        if (cod_op == IO) { 
            log_info(logger, "Se recibió IO <%s> para PID <%d> por <%d>ms", nombre, pid, tiempo);
            free(nombre);
        
            log_info(logger, "## PID: %d - Inicio de IO - Tiempo: %d", pid, tiempo);
        
            usleep(tiempo * 1000); // dormir el tiempo solicitado
        
            log_info(logger, "## PID: %d - Fin de IO", pid);
        
            enviar_finalizacion(logger, pid, dispositivo_io);
        }
    }
}

void enviar_finalizacion(t_log* logger, int pid,  t_io* dispositivo_io) {
    t_paquete* paquete = crear_paquete(PROCESO_DESBLOQUEADO);
    int len_nombre = strlen(dispositivo_io->nombre);
    int tipo_mensaje = FINALIZO_IO;

    agregar_a_paquete(paquete, &len_nombre, sizeof(int));
    agregar_a_paquete(paquete, dispositivo_io->nombre, len_nombre);
    agregar_a_paquete(paquete, &pid, sizeof(int));
    agregar_a_paquete(paquete, &tipo_mensaje, sizeof(int));
    enviar_paquete(paquete, dispositivo_io->socket_fd);

    eliminar_paquete(paquete);
    log_info(logger, "Proceso <%d> finalizó IO '%s', notificado al Kernel", pid, dispositivo_io->nombre);
}


void enviar_mensaje_desconexion(t_io* dispositivo_io) {
    t_paquete* paquete = crear_paquete(PROCESO_DESBLOQUEADO);
    int len_nombre = strlen(dispositivo_io->nombre);
    int pid_ficticio = -1;
    int tipo_mensaje = DESCONECTADO_IO;

    agregar_a_paquete(paquete, &len_nombre, sizeof(int));
    agregar_a_paquete(paquete, dispositivo_io->nombre, len_nombre);
    agregar_a_paquete(paquete, &pid_ficticio, sizeof(int));
    agregar_a_paquete(paquete, &tipo_mensaje, sizeof(int));

    enviar_paquete(paquete, socket_kernel);
    eliminar_paquete(paquete);
}
