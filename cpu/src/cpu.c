#include "cpu.h"
#include <utils/errors.h> 

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/time.h>

int socket_memoria;
int socket_kernel_dispatch;
int socket_kernel_interrupt;
t_log* logger;
t_config* cpu_config;

char *ip_memoria;
char *puerto_memoria;
char *ip_kernel;
char *puerto_kernel_dispatch;
char* puerto_kernel_interrupt;

entrada_tlb* tlb; 
int total_entradas_tlb;
int puntero_tlb_clock = 0;
char* algoritmo_tlb; // "FIFO" o "LRU"
int puntero_tlb_fifo = 0;

entrada_cache* cache_paginas;
int entradas_cache;
char* algoritmo_cache; //"CLOCK" o "CLOCK-M"
int puntero_clock_cache = 0;
int tam_pagina_memoria;
int retardo_cache;

pthread_mutex_t mutex_tlb;
pthread_mutex_t mutex_cache;
pthread_mutex_t mutex_interrupcion_pendiente;
pthread_cond_t cond_interrupcion_pendiente;
bool INTERRUPCION_PENDIENTE = false;


int main(int argc, char* argv[]) {
    if(argc < 2) {
        printf("Argumentos insuficientes. Se requiere el nombre de la CPU.\n");
        return -1;
    }

    char* nombre_cpu = argv[2];
    printf("Nombre CPU: %s\n", nombre_cpu);

    logger = iniciar_logger(nombre_cpu);
    cpu_config = config_create(argv[1]);

    iniciar_manejador_de_errores(logger); 

    if (cpu_config == NULL) {
        LOG_ERROR("No se pudo cargar el archivo de configuración cpu.config.");
        terminar_programa(logger, cpu_config);
        return EXIT_FAILURE;
    }

    ip_memoria = config_get_string_value(cpu_config, "IP_MEMORIA");
    puerto_memoria = config_get_string_value(cpu_config, "PUERTO_MEMORIA");
    ip_kernel = config_get_string_value(cpu_config, "IP_KERNEL");
    puerto_kernel_dispatch = config_get_string_value(cpu_config, "PUERTO_KERNEL_DISPATCH");
    puerto_kernel_interrupt = config_get_string_value(cpu_config, "PUERTO_KERNEL_INTERRUPT");

    if (!ip_memoria || !puerto_memoria || !ip_kernel || !puerto_kernel_dispatch || !puerto_kernel_interrupt) {
        LOG_ERROR("Faltan valores de configuración esenciales (IPs/Puertos).");
        terminar_programa(logger, cpu_config);
        return EXIT_FAILURE;
    }

    total_entradas_tlb = config_get_int_value(cpu_config, "ENTRADAS_TLB");
    entradas_cache = config_get_int_value(cpu_config, "ENTRADAS_CACHE");
    algoritmo_cache = config_get_string_value(cpu_config, "REEMPLAZO_CACHE");
    algoritmo_tlb = config_get_string_value(cpu_config, "REEMPLAZO_TLB");
    retardo_cache = config_get_int_value(cpu_config, "RETARDO_CACHE");

    pthread_mutex_init(&mutex_tlb, NULL);
    pthread_mutex_init(&mutex_cache, NULL);
    pthread_mutex_init(&mutex_interrupcion_pendiente, NULL);
    pthread_cond_init(&cond_interrupcion_pendiente, NULL);

    iniciar_mmu();
    
    socket_memoria = conectar_memoria(); // Asignar a la variable GLOBAL
    if (socket_memoria == -1) {
        terminar_programa(logger, cpu_config);
        return EXIT_FAILURE;
    }

    socket_kernel_dispatch = conectar_kernel_dispatch(ip_kernel, puerto_kernel_dispatch, nombre_cpu);
    if (socket_kernel_dispatch == -1) {
        LOG_ERROR("No se pudo conectar al Kernel (Dispatch).");
        terminar_programa(logger, cpu_config);
        return EXIT_FAILURE;
    }
    log_info(logger, "Conexion exitosa con Kernel Dispatch. Socket FD: %d", socket_kernel_dispatch);

    socket_kernel_interrupt = conectar_kernel_interrupt(ip_kernel, puerto_kernel_interrupt, nombre_cpu);
    if (socket_kernel_interrupt == -1) {
        LOG_ERROR("No se pudo conectar al Kernel (Interrupt).");
        terminar_programa(logger, cpu_config);
        return EXIT_FAILURE;
    }
    log_info(logger, "Conexion exitosa con Kernel Interrupt. Socket FD: %d", socket_kernel_interrupt);

    concurrencia_cpu();

    terminar_programa(logger, cpu_config);
    return EXIT_SUCCESS;
}

// ------------------------------------ INICIALIZACION Y CONEXIONES ------------------------------------
t_config* iniciar_config(void) {
    return config_create("./cpu.config");
}

t_log* iniciar_logger(char* nombre_cpu) {
    char* log_path = string_from_format("./%s.log", nombre_cpu);
    t_log* new_logger = log_create(log_path, "CPU", 1, LOG_LEVEL_INFO);
    free(log_path);
    return new_logger;
}

void terminar_programa(t_log* logger, t_config* config) {
    if(logger) log_destroy(logger);
    if(config) config_destroy(config);
    if(tlb) free(tlb);
    if(cache_paginas) {
        for(int i = 0; i < entradas_cache; i++) {
            if(cache_paginas[i].ocupado) {
                free(cache_paginas[i].contenido);
            }
        }
        free(cache_paginas);
    }
    pthread_mutex_destroy(&mutex_tlb);
    pthread_mutex_destroy(&mutex_cache);
    pthread_mutex_destroy(&mutex_interrupcion_pendiente);
    pthread_cond_destroy(&cond_interrupcion_pendiente);


    if(socket_kernel_dispatch != -1) close(socket_kernel_dispatch);
    if(socket_kernel_interrupt != -1) close(socket_kernel_interrupt);
}

int conectar_memoria() {
    int socket_fd = crear_conexion(ip_memoria, puerto_memoria);
    if(socket_fd == -1) return -1;

    t_paquete* handshake_cpu_memoria = crear_paquete(HANDSHAKE_MEMORIA_CPU);
    enviar_paquete(handshake_cpu_memoria, socket_fd);
    eliminar_paquete(handshake_cpu_memoria);

    t_paquete* paquete_respuesta = recibir_paquete(socket_fd);
    if (paquete_respuesta == NULL) {
        log_error(logger, "Fallo al recibir respuesta del handshake con Memoria.");
        close(socket_fd);
        return -1;
    }

    op_code respuesta = paquete_respuesta->codigo_operacion;
    memcpy(&tam_pagina_memoria, paquete_respuesta->buffer->stream, sizeof(int));

    eliminar_paquete(paquete_respuesta);
   
    if (respuesta == HANDSHAKE_OK) {
        log_info(logger, "Handshake con Memoria exitoso, tamaño de pagina: %d bytes", tam_pagina_memoria);

        return socket_fd;
    } else {
        log_error(logger, "Handshake con Memoria fallido. Codigo: %d", respuesta);
        close(socket_fd);
        return -1;
    }
}

/* bool handshake_tam_pagina() {
    op_code hs_code = HANDSHAKE_MEMORIA_CPU;
   
    t_paquete* handshake = crear_paquete(hs_code);
    enviar_paquete(handshake, socket_memoria);
    eliminar_paquete(handshake);
   
    t_paquete* paquete_respuesta = recibir_paquete(socket_memoria);
    if (paquete_respuesta == NULL || paquete_respuesta->codigo_operacion != HANDSHAKE_OK) {
        return false;
    }

    eliminar_paquete(paquete_respuesta);
    return true;
} */

int conectar_kernel_dispatch(char* ip, char* puerto, char* nombre_cpu) {
    int socket_fd = crear_conexion(ip, puerto);
    if (socket_fd == -1) return -1;

    t_paquete* handshake_cpu_dispatch = crear_paquete(HANDSHAKE_CPU_DISPATCH);
    agregar_a_paquete(handshake_cpu_dispatch, nombre_cpu, strlen(nombre_cpu) + 1);
    enviar_paquete(handshake_cpu_dispatch, socket_fd);
    eliminar_paquete(handshake_cpu_dispatch);

    t_paquete* paquete_respuesta_kernel = recibir_paquete(socket_fd);
    if (paquete_respuesta_kernel == NULL) {
        LOG_ERROR("Fallo al recibir respuesta del handshake con Kernel Dispatch.");
        close(socket_fd);
        return -1;
    }
    op_code respuesta = paquete_respuesta_kernel->codigo_operacion;
    eliminar_paquete(paquete_respuesta_kernel);
    
    if (respuesta == HANDSHAKE_OK) {
        log_info(logger, "Handshake con Kernel Dispatch exitoso.");
        return socket_fd;
    } else {
        LOG_ERROR("Handshake con Kernel Dispatch fallido. Codigo: %d", respuesta);
        close(socket_fd);
        return -1;
    }
}

int conectar_kernel_interrupt(char* ip, char* puerto, char* nombre_cpu) {
    int socket_fd = crear_conexion(ip, puerto);
    if (socket_fd == -1) return -1;

    t_paquete* handshake_cpu_interrupt = crear_paquete(HANDSHAKE_CPU_INTERRUPT);
    agregar_a_paquete(handshake_cpu_interrupt, nombre_cpu, strlen(nombre_cpu) + 1);
    enviar_paquete(handshake_cpu_interrupt, socket_fd);
    eliminar_paquete(handshake_cpu_interrupt);

    t_paquete* paquete_respuesta_kernel = recibir_paquete(socket_fd);
    if (paquete_respuesta_kernel == NULL) {
        LOG_ERROR("Fallo al recibir respuesta del handshake con Kernel Interrupt.");
        close(socket_fd);
        return -1;
    }
    op_code respuesta = paquete_respuesta_kernel->codigo_operacion;
    eliminar_paquete(paquete_respuesta_kernel);

    if (respuesta == HANDSHAKE_OK) {
        log_info(logger, "Handshake con Kernel Interrupt exitoso.");
        return socket_fd;
    } else {
        LOG_ERROR("Handshake con Kernel Interrupt fallido. Codigo: %d", respuesta);
        close(socket_fd);
        return -1;
    }
}

// ------------------------------------ CICLO DE INSTRUCCIÓN ------------------------------------

void concurrencia_cpu() {
    log_info(logger, "Iniciando concurrencia de CPU...");
    
    pthread_t hilo_interrupciones;
    if (pthread_create(&hilo_interrupciones, NULL, hilo_escucha_interrupciones, NULL) != 0) {
        LOG_ERROR("Error al crear hilo de escucha de interrupciones.");
        terminar_programa(logger, cpu_config);
        exit(EXIT_FAILURE);
    }
    pthread_detach(hilo_interrupciones);

    pthread_t hilo_dispatch;
    if (pthread_create(&hilo_dispatch, NULL, atender_conexiones_dispatch, NULL) != 0) {
        LOG_ERROR("Error al crear hilo de dispatch.");
        terminar_programa(logger, cpu_config);
        exit(EXIT_FAILURE);
    }
    pthread_detach(hilo_dispatch);

    // Mantener el hilo principal de la CPU activo
    log_info(logger, "CPU en espera de solicitudes de Kernel.");
    while (true) {
        sleep(60);
    }
}


void* atender_conexiones_dispatch() {
    log_info(logger, "Esperando conexiones de Dispatch...");

    while (true) {
        t_paquete* paquete = recibir_paquete(socket_kernel_dispatch);
        if (paquete == NULL) {
            LOG_ERROR("El Kernel cerró la conexión Dispatch. Finalizando CPU.");
            break;
        }
        
        op_code cod_op = paquete->codigo_operacion;
        if (cod_op == OP_PCB) {
           
            t_pcb* pcb = recibir_pcb(paquete);
            if (!pcb) {
                LOG_ERROR("Fallo al deserializar el PCB recibido.");
                eliminar_paquete(paquete);
                continue;
            }
    
            log_info(logger, "Recibido PCB PID %d. PC: %d", pcb->pid, pcb->program_counter);
           
            limpiar_tlb();
            limpiar_cache_de_proceso(pcb->pid);
            ciclo_instruccion(pcb);
    
            free(pcb->path_pseudocodigo);
            free(pcb);
        } else if (cod_op == -1) {
            LOG_ERROR("El Kernel cerró la conexión Dispatch. Finalizando CPU.");
            break;
        } else {
            log_warning(logger, "Operacion desconocida (%d) recibida del Kernel Dispatch.", cod_op);
        }
        eliminar_paquete(paquete);
    }
    return NULL;
}

t_pcb* recibir_pcb(t_paquete* paquete_pcb) {

    t_pcb* pcb = malloc(sizeof(t_pcb));
    int offset = 0;

    memcpy(&pcb->pid, paquete_pcb->buffer->stream + offset, sizeof(int)); offset += sizeof(int);
    memcpy(&pcb->program_counter, paquete_pcb->buffer->stream + offset, sizeof(int)); offset += sizeof(int);
    memcpy(&pcb->tamanio_proceso, paquete_pcb->buffer->stream + offset, sizeof(int)); offset += sizeof(int);
    memcpy(&pcb->estado, paquete_pcb->buffer->stream + offset, sizeof(op_code_estado)); offset += sizeof(op_code_estado);

    int path_len;
    memcpy(&path_len, paquete_pcb->buffer->stream + offset, sizeof(int)); offset += sizeof(int);
    pcb->path_pseudocodigo = malloc(path_len);
    memcpy(pcb->path_pseudocodigo, paquete_pcb->buffer->stream + offset, path_len); offset += path_len;

    memcpy(&pcb->estimacion_rafaga, paquete_pcb->buffer->stream + offset, sizeof(double));
    offset += sizeof(double);

    memcpy(&pcb->rafaga_real_anterior, paquete_pcb->buffer->stream + offset, sizeof(double));
    offset += sizeof(double);

    return pcb;
}

void ciclo_instruccion(t_pcb* pcb_recibido) {
    int pid = pcb_recibido->pid;
    
    bool continuar_ejecucion_este_pcb = true; 
    op_code motivo_finalizacion = EJECUCION_OK; 

    char* nombre_dispositivo_io_bloqueo = NULL;
    int duracion_bloqueo_io = 0;

    while(continuar_ejecucion_este_pcb) {
        t_instruccion* instruccion_actual = fetch(pcb_recibido->pid, pcb_recibido->program_counter);

        if (instruccion_actual == NULL) {
            LOG_ERROR("Error al hacer fetch de la instrucción para PID %d.", pid);
            motivo_finalizacion = ERROR_EJECUCION_FATAL;
            continuar_ejecucion_este_pcb = false;
            break; 
        }

        resultado_ejecucion_instruccion_cpu resultado_operacion = ejecutar_instruccion(pcb_recibido, instruccion_actual, &nombre_dispositivo_io_bloqueo, &duracion_bloqueo_io);

        switch (resultado_operacion) {
            case EJECUCION_OK:
                break;
            case PROCESO_BLOQUEADO_IO:
                motivo_finalizacion = PROCESO_BLOQUEANTE_IO;
                continuar_ejecucion_este_pcb = false;
                break;
            case PROCESO_BLOQUEADO_DUMP_MEMORY:
                motivo_finalizacion = PROCESO_BLOQUEANTE_DUMP_MEMORY;
                continuar_ejecucion_este_pcb = false;
                break;
            case PROCESO_FINALIZADO_EXIT:
                motivo_finalizacion = EXIT;
                continuar_ejecucion_este_pcb = false;
                break;
            case ERROR_EJECUCION_FATAL:
                motivo_finalizacion = ERROR_EJECUCION_FATAL;
                continuar_ejecucion_este_pcb = false;
                break;
            case SEGMENTATION_FAULT:
                motivo_finalizacion = SEGMENTATION_FAULT;
                continuar_ejecucion_este_pcb = false;
                break;    
            default:
                log_warning(logger, "PID: %d - Resultado desconocido de ejecucion de instruccion: %d", pid, resultado_operacion);
                motivo_finalizacion = ERROR_EJECUCION_FATAL;
                continuar_ejecucion_este_pcb = false;
                break;
        }

        pthread_mutex_lock(&mutex_interrupcion_pendiente);
        if (INTERRUPCION_PENDIENTE) {
            log_info(logger, "Interrupcion asincrona detectada para PID %d. PC: %d", pid, pcb_recibido->program_counter);
            INTERRUPCION_PENDIENTE = false; 
            motivo_finalizacion = PROCESO_INTERRUPCION; 
            continuar_ejecucion_este_pcb = false;
        }
        pthread_mutex_unlock(&mutex_interrupcion_pendiente);


        if (continuar_ejecucion_este_pcb && strcmp(instruccion_actual->opcode, "GOTO") != 0) {
            pcb_recibido->program_counter++;
        }
        
        destruir_instruccion(instruccion_actual);
    }

    log_info(logger, "Finaliza rafaga de ejecucion para PID %d. Realizando limpieza de TLB y Cache.", pid);
    
    limpiar_tlb();
    limpiar_cache_de_proceso(pid);
    
    enviar_respuesta_al_kernel(pcb_recibido->pid, pcb_recibido->program_counter, motivo_finalizacion, nombre_dispositivo_io_bloqueo, duracion_bloqueo_io);

    if (nombre_dispositivo_io_bloqueo != NULL) {
        free(nombre_dispositivo_io_bloqueo);
    }

    log_info(logger, "Ciclo de instruccion para PID %d ha terminado. La CPU esta libre.", pid);
}


t_instruccion* fetch(int pid, int pc) {
    log_info(logger, "## PID: %d - FETCH - Program Counter: %d", pid, pc);
    t_instruccion* instruccion = NULL;

    t_paquete* solicitud_memoria = crear_paquete(PEDIR_INSTRUCCIONES);
    agregar_a_paquete(solicitud_memoria, &pid, sizeof(int));
    agregar_a_paquete(solicitud_memoria, &pc, sizeof(int));
    enviar_paquete(solicitud_memoria, socket_memoria);
    eliminar_paquete(solicitud_memoria);

    t_paquete* respuesta_memoria = recibir_paquete(socket_memoria);
    if (respuesta_memoria == NULL) {
        LOG_ERROR("PID: %d - Memoria desconectada o error al recibir paquete de respuesta.", pid);
        return NULL;
    }

    op_code cod_op_recibido = respuesta_memoria->codigo_operacion;

    switch (cod_op_recibido) {
        case INSTRUCCION_OK: {
            instruccion = malloc(sizeof(t_instruccion));

            instruccion->opcode = NULL; 
            for (int i = 0; i < MAX_PARAMETROS; i++) {
                instruccion->parametros[i] = NULL;
            }

            int offset = 0;

            if (respuesta_memoria->buffer->size < offset + sizeof(int)) {
                LOG_ERROR("PID: %d - Error de buffer en INSTRUCCION_OK: no hay suficiente espacio para opcode_lenght.", pid);
                destruir_instruccion(instruccion);
                eliminar_paquete(respuesta_memoria);
                return NULL;
            }
            memcpy(&(instruccion->opcode_lenght), respuesta_memoria->buffer->stream + offset, sizeof(int));
            offset += sizeof(int);

            if (respuesta_memoria->buffer->size < offset + instruccion->opcode_lenght) {
                LOG_ERROR("PID: %d - Error de buffer en INSTRUCCION_OK: no hay suficiente espacio para opcode (%d).", pid, instruccion->opcode_lenght);
                destruir_instruccion(instruccion);
                eliminar_paquete(respuesta_memoria);
                return NULL;
            }
            instruccion->opcode = malloc(instruccion->opcode_lenght);
            memcpy(instruccion->opcode, respuesta_memoria->buffer->stream + offset, instruccion->opcode_lenght);
            offset += instruccion->opcode_lenght;

            if (respuesta_memoria->buffer->size < offset + sizeof(int)) {
                LOG_ERROR("PID: %d - Error de buffer en INSTRUCCION_OK: no hay suficiente espacio para cantidad_parametros.", pid);
                destruir_instruccion(instruccion);
                eliminar_paquete(respuesta_memoria);
                return NULL;
            }
            memcpy(&(instruccion->cantidad_parametros), respuesta_memoria->buffer->stream + offset, sizeof(int));
            offset += sizeof(int);
            
            if (instruccion->cantidad_parametros > MAX_PARAMETROS) {
                LOG_ERROR("PID: %d - La cantidad de parametros recibida (%d) excede MAX_PARAMETROS (%d). Posible error de deserializacion.", pid, instruccion->cantidad_parametros, MAX_PARAMETROS);
                destruir_instruccion(instruccion);
                eliminar_paquete(respuesta_memoria);
                return NULL;
            }

            for (int i = 0; i < instruccion->cantidad_parametros; i++) {
                int param_len;
                if (respuesta_memoria->buffer->size < offset + sizeof(int)) {
                    LOG_ERROR("PID: %d - Error de buffer en INSTRUCCION_OK: no hay suficiente espacio para la longitud del parametro %d.", pid, i);
                    destruir_instruccion(instruccion);
                    eliminar_paquete(respuesta_memoria);
                    return NULL;
                }
                memcpy(&param_len, respuesta_memoria->buffer->stream + offset, sizeof(int));
                offset += sizeof(int);

                if (param_len > 0) {
                    if (respuesta_memoria->buffer->size < offset + param_len) {
                        LOG_ERROR("PID: %d - Error de buffer en INSTRUCCION_OK: no hay suficiente espacio para el parametro %d (longitud %d).", pid, i, param_len);
                        destruir_instruccion(instruccion);
                        eliminar_paquete(respuesta_memoria);
                        return NULL;
                    }
                    instruccion->parametros[i] = malloc(param_len);
                    memcpy(instruccion->parametros[i], respuesta_memoria->buffer->stream + offset, param_len);
                    offset += param_len;
                } else {
                    instruccion->parametros[i] = NULL;
                }
            }

            for (int i = instruccion->cantidad_parametros; i < MAX_PARAMETROS; i++) {
                instruccion->parametros[i] = NULL;
            }
            
            break;

        }
        case ERROR_PID_NO_ENCONTRADO: {
            LOG_ERROR("PID: %d - Error de Memoria: PID no encontrado. PC: %d", pid, pc);
            if (respuesta_memoria->buffer->size >= sizeof(int)) {
                int pid_err;
                memcpy(&pid_err, respuesta_memoria->buffer->stream, sizeof(int));
                LOG_ERROR("  PID reportado por Memoria: %d. Posiblemente el proceso ya ha finalizado.", pid_err);
            }
            eliminar_paquete(respuesta_memoria);
            return NULL;
        }
        case ERROR_PC_FUERA_DE_RANGO: {
            LOG_ERROR("PID: %d - Error de Memoria: PC fuera de rango.", pid);
            if (respuesta_memoria->buffer->size >= sizeof(int)) {
                int pc_err;
                memcpy(&pc_err, respuesta_memoria->buffer->stream, sizeof(int));
                LOG_ERROR("  PID reportado por Memoria: %d. Posiblemente el proceso ya ha finalizado.", pc_err);
            }
            eliminar_paquete(respuesta_memoria);
            return NULL;
        }
        default: {
            LOG_ERROR("PID: %d - Respuesta de Memoria desconocida o inesperada. Codigo: %d", pid, cod_op_recibido);
            eliminar_paquete(respuesta_memoria);
            return NULL;
        }
    }

    eliminar_paquete(respuesta_memoria);
    log_info(logger, "## PID: %d - Fetched instruccion: %s, PC: %d", pid, instruccion->opcode, pc);
    
    return instruccion;
}

resultado_ejecucion_instruccion_cpu ejecutar_instruccion(t_pcb* pcb, t_instruccion* instruccion, char** nombre_dispositivo_io_out, int* duracion_bloqueo_out) {
    if (pcb == NULL || instruccion == NULL) {
        LOG_ERROR("PCB o instruccion nulos en ejecutar_instruccion.");
        return ERROR_EJECUCION_FATAL;
    }

    char* parametros = NULL;
    if(instruccion->cantidad_parametros > 0) {
        parametros = concatenar_parametros(instruccion);
    }

    log_info(logger, "## PID: %d - Ejecutando: %s - Parametros: %s", pcb->pid, instruccion->opcode, instruccion->cantidad_parametros > 0 ? parametros : "N/A");

    free(parametros);

    if (strcmp(instruccion->opcode, "NOOP") == 0) {
        
        return EJECUCION_OK;
   
    } else if (strcmp(instruccion->opcode, "READ") == 0) {
        
        bool ejecucion_exitosa = manejar_instruccion_read(pcb, atoi(instruccion->parametros[0]), atoi(instruccion->parametros[1]));
        if (!ejecucion_exitosa) {
            return SEGMENTATION_FAULT;
        }
        return EJECUCION_OK;

    } else if (strcmp(instruccion->opcode, "WRITE") == 0) {

        int direccion_logica = atoi(instruccion->parametros[0]);
        bool ejecucion_exitosa = manejar_instruccion_write(pcb, direccion_logica, instruccion->parametros[1]);
        if (!ejecucion_exitosa) {
            return SEGMENTATION_FAULT;
        }
        return EJECUCION_OK;

    } else if (strcmp(instruccion->opcode, "GOTO") == 0) {

        pcb->program_counter = atoi(instruccion->parametros[0]);
        return EJECUCION_OK;

    } else if (strcmp(instruccion->opcode, "IO") == 0) {
        
        if (instruccion->parametros[0] != NULL && instruccion->parametros[1] != NULL) {
            *nombre_dispositivo_io_out = strdup(instruccion->parametros[0]);
            *duracion_bloqueo_out = atoi(instruccion->parametros[1]);
        } else {
            LOG_ERROR("PID: %d - Error en IO: parametros invalidos.", pcb->pid);
            return ERROR_EJECUCION_FATAL;
        }
    
        pcb->program_counter++; 
        
        return PROCESO_BLOQUEADO_IO;

    } else if (strcmp(instruccion->opcode, "EXIT") == 0) { 
        return PROCESO_FINALIZADO_EXIT; 

    } else if (strcmp(instruccion->opcode, "INIT_PROC") == 0) {
        manejar_instruccion_init_proceso(pcb, instruccion);
        return EJECUCION_OK;

    } else if (strcmp(instruccion->opcode, "DUMP_MEMORY") == 0) {
        pcb->program_counter++; 
        return PROCESO_BLOQUEADO_DUMP_MEMORY;

    } else {
        LOG_ERROR("PID: %d - Instruccion desconocida: %s. Reportando error fatal.", pcb->pid, instruccion->opcode);
        return ERROR_EJECUCION_FATAL;
    }
}

// NO SE USA
/* void realizar_write_back_cache(int pid) { // Se usa cuando un proceso finaliza, para escribir las paginas modificadas en memoria
    if(entradas_cache > 0) {
        pthread_mutex_lock(&mutex_cache);
        for (int i = 0; i < entradas_cache; i++) {
            if (cache_paginas[i].ocupado && cache_paginas[i].pid == pid && cache_paginas[i].bit_modificado) {
                enviar_pagina_a_memoria_principal(cache_paginas[i].marco, cache_paginas[i].contenido, pid);
                cache_paginas[i].bit_modificado = false;
            }
        }
        pthread_mutex_unlock(&mutex_cache);
    }
} */


// ------------------------------------ MANEJO DE INSTRUCCIONES ------------------------------------

entrada_cache* obtener_entrada_cache_para_lectura(int pid, int pagina_logica) {
    if(entradas_cache > 0) {
        usleep(retardo_cache * 1000);

        pthread_mutex_lock(&mutex_cache);
        for (int i = 0; i < entradas_cache; i++) {
            if (cache_paginas[i].ocupado && cache_paginas[i].pid == pid && cache_paginas[i].pagina == pagina_logica) {
                pthread_mutex_unlock(&mutex_cache);
                log_info(logger, "PID: %d - Cache Hit - Pagina: %d", pid, pagina_logica);
                return &cache_paginas[i]; // Retorna un puntero a la entrada de cache
            }
        }
        log_info(logger, "PID: %d - Cache Miss - Pagina: %d", pid, pagina_logica);
        pthread_mutex_unlock(&mutex_cache);
    }
    return NULL;
}


bool manejar_instruccion_read(t_pcb* pcb, int direccion_logica, int tamanio_a_leer) {
    int pagina_logica = obtener_pagina(direccion_logica);
    int offset_logico = obtener_offset(direccion_logica);

    if (offset_logico + tamanio_a_leer > tam_pagina_memoria) {
        LOG_ERROR("PID: %d - SEGFAULT: Lectura excede el limite de la pagina. Direccion Logica: %d, Offset: %d, Tamanio a Leer: %d", pcb->pid, direccion_logica, offset_logico, tamanio_a_leer);
        return false;
    }

    entrada_cache* entrada_leida = obtener_entrada_cache_para_lectura(pcb->pid, pagina_logica);
    char* datos_a_enviar_al_kernel = NULL; 

    if (entrada_leida != NULL) { // Cache Hit
        datos_a_enviar_al_kernel = malloc(tamanio_a_leer + 1);
        memcpy(datos_a_enviar_al_kernel, entrada_leida->contenido + offset_logico, tamanio_a_leer);
        datos_a_enviar_al_kernel[tamanio_a_leer] = '\0';
        
        pthread_mutex_lock(&mutex_cache);
        entrada_leida->bit_uso = true;
        pthread_mutex_unlock(&mutex_cache);

    } else { // Cache Miss

        int marco_fisico = -1;
        entrada_tlb* tlb_entry = buscar_en_tlb(pcb->pid, pagina_logica);

        if (tlb_entry != NULL) { // TLB Hit
            marco_fisico = tlb_entry->marco;
            
        } else { // TLB Miss
            marco_fisico = pedir_traduccion_a_memoria(pcb->pid, direccion_logica);
            if (marco_fisico == -1) {
                LOG_ERROR("PID: %d - SEGFAULT: No se pudo traducir direccion logica %d para READ.", pcb->pid, direccion_logica);
                return false;
            }
            agregar_a_tlb(pcb->pid, pagina_logica, marco_fisico);
        }

        char* pagina_completa_desde_memoria = leer_bytes_de_memoria_principal(marco_fisico, 0, tam_pagina_memoria, pcb->pid); 
        if (pagina_completa_desde_memoria != NULL) {
            insertar_en_cache(pcb->pid, pagina_logica, marco_fisico, pagina_completa_desde_memoria, false); 
            datos_a_enviar_al_kernel = malloc(tamanio_a_leer + 1); // +1 para el terminador nulo
            memcpy(datos_a_enviar_al_kernel, pagina_completa_desde_memoria + offset_logico, tamanio_a_leer);
            datos_a_enviar_al_kernel[tamanio_a_leer] = '\0';
        
            free(pagina_completa_desde_memoria);
        } else {
            LOG_ERROR("PID: %d - Error al leer pagina completa del marco %d desde Memoria para READ.", pcb->pid, marco_fisico);
            return false;
        }
    }

    if (datos_a_enviar_al_kernel != NULL) {
        free(datos_a_enviar_al_kernel);
    }
    
    return true;
}

bool manejar_instruccion_write(t_pcb* pcb, int direccion_logica, char* datos) {
    int tamanio_datos = strlen(datos);
    int pagina_logica = obtener_pagina(direccion_logica);
    int offset_logico = obtener_offset(direccion_logica);

    if (offset_logico + tamanio_datos > tam_pagina_memoria) {
        LOG_ERROR("PID: %d - SEGFAULT: Escritura excede el limite de la pagina. Direccion Logica: %d, Offset: %d, Tamanio a Escribir: %d", pcb->pid, direccion_logica, offset_logico, tamanio_datos);
        return false;
    }

    if (entradas_cache > 0) {

        usleep(retardo_cache * 1000);

        entrada_cache* entrada_hit = NULL;
        pthread_mutex_lock(&mutex_cache);
        for(int i = 0; i < entradas_cache; i++) {
            if(cache_paginas[i].ocupado && cache_paginas[i].pid == pcb->pid && cache_paginas[i].pagina == pagina_logica) {
                entrada_hit = &cache_paginas[i];
                break;
            }
        }
        pthread_mutex_unlock(&mutex_cache);

        if (entrada_hit != NULL) { // Cache Hit
            log_info(logger, "PID: %d - Cache Hit para escritura en pagina %d.", pcb->pid, pagina_logica);
            memcpy(entrada_hit->contenido + offset_logico, datos, tamanio_datos);
            entrada_hit->bit_modificado = true;
            entrada_hit->bit_uso = true;
            return true;
        } else { // Cache Miss
            log_info(logger, "PID: %d - Cache Miss para escritura en pagina %d.", pcb->pid, pagina_logica);
            int marco_fisico = pedir_traduccion_a_memoria(pcb->pid, direccion_logica);
            if (marco_fisico == -1) {
                return false;
            }

            char* pagina_completa_desde_memoria = leer_bytes_de_memoria_principal(marco_fisico, 0, tam_pagina_memoria, pcb->pid);
            if (pagina_completa_desde_memoria != NULL) {
                memcpy(pagina_completa_desde_memoria + offset_logico, datos, tamanio_datos);
                insertar_en_cache(pcb->pid, pagina_logica, marco_fisico, pagina_completa_desde_memoria, true);
                free(pagina_completa_desde_memoria);
                return true;
            } else {
                LOG_ERROR("PID: %d - Error al leer pagina completa del marco %d desde Memoria para WRITE.", pcb->pid, marco_fisico);
                return false;
            }
        }
    } else {
        
        log_info(logger, "PID: %d - Escribiendo directamente a Memoria (Cache deshabilitada).", pcb->pid);

        // Traducir direccion para obtener el marco fisico
        int marco_fisico = -1;
        entrada_tlb* tlb_entry = buscar_en_tlb(pcb->pid, pagina_logica);

        if (tlb_entry != NULL) { // TLB Hit
            marco_fisico = tlb_entry->marco;
            tlb_entry->timestamp = get_timestamp();
        } else { // TLB Miss
            marco_fisico = pedir_traduccion_a_memoria(pcb->pid, direccion_logica);
            if (marco_fisico == -1) {
                LOG_ERROR("PID: %d - Error al traducir direccion logica %d para escritura.", pcb->pid, direccion_logica);
                return false;
            }
            agregar_a_tlb(pcb->pid, pagina_logica, marco_fisico);
        }

        log_info(logger, "PID: %d - Memory Update - Página: %d, Frame: %d", pcb->pid, pagina_logica, marco_fisico);
        escribir_bytes_en_memoria_principal(marco_fisico, offset_logico, tamanio_datos, datos, pcb->pid);
        return true;
    }
}

void manejar_instruccion_init_proceso(t_pcb* pcb, t_instruccion* instruccion) {
    char* path_pseudocodigo = instruccion->parametros[0];
    int tamanio_proceso = atoi(instruccion->parametros[1]);
 
    t_paquete* paquete = crear_paquete(INIT_PROC);
    int longitud_path = strlen(path_pseudocodigo); // sin +1
    agregar_a_paquete(paquete, &longitud_path, sizeof(int));
    agregar_a_paquete(paquete, path_pseudocodigo, longitud_path);
    agregar_a_paquete(paquete, &tamanio_proceso, sizeof(int));
    enviar_paquete(paquete, socket_kernel_dispatch);
    eliminar_paquete(paquete);

    t_paquete* respuesta_kernel = recibir_paquete(socket_kernel_dispatch);
    if (respuesta_kernel == NULL) {
        LOG_ERROR("PID: %d - Error al recibir respuesta del Kernel en INIT_PROC.", pcb->pid);
        return;
    }
    op_code respuesta_op = respuesta_kernel->codigo_operacion;
    eliminar_paquete(respuesta_kernel);

    if (respuesta_op == INIT_PROCESO_OK) {
        log_info(logger, "INIT_PROC ejecutada correctamente.");
    } else {
        LOG_ERROR("PID: %d - Error al ejecutar INIT_PROC. Codigo de respuesta: %d", pcb->pid, respuesta_op);
    }
}

// ------------------------------------ COMUNICACIÓN CON KERNEL ------------------------------------

void enviar_respuesta_al_kernel(int pid, int pc, op_code motivo, char* nombre_dispositivo_io, int duracion_bloqueo) {
    t_paquete* paquete = crear_paquete(OP_RESPUESTA_EJECUCION);
    
    agregar_a_paquete(paquete, &pid, sizeof(int));
    agregar_a_paquete(paquete, &pc, sizeof(int));
    agregar_a_paquete(paquete, &motivo, sizeof(op_code));

    if(motivo == PROCESO_BLOQUEANTE_IO) {
        log_info(logger, "Nombre del dispositivo antes de enviar: %s", nombre_dispositivo_io);
        log_info(logger, "Tiempo de uso del dispositivo antes de enviar: %d", duracion_bloqueo);
    }

    if (motivo == PROCESO_BLOQUEANTE_IO) { //Bloqueante por: IO o DUMP_MEMORY
        log_info(logger, "Syscall Bloqueante: IO. PID: %d",pid);
        int len_nombre_dispositivo = strlen(nombre_dispositivo_io) + 1;
        agregar_a_paquete(paquete, &len_nombre_dispositivo, sizeof(int));
        agregar_a_paquete(paquete, nombre_dispositivo_io, len_nombre_dispositivo);
        agregar_a_paquete(paquete, &duracion_bloqueo, sizeof(int));

    } else if(motivo == PROCESO_BLOQUEANTE_DUMP_MEMORY){
        log_info(logger, "Syscall Bloqueante: DUMP_MEMORY. PID: %d", pid);
        int cero = 0;
        agregar_a_paquete(paquete, &cero, sizeof(int)); 
        agregar_a_paquete(paquete, &cero, sizeof(int)); 
        agregar_a_paquete(paquete, &duracion_bloqueo, sizeof(int)); 
    } else { 
        int cero = 0;
        agregar_a_paquete(paquete, &cero, sizeof(int));
        agregar_a_paquete(paquete, &cero, sizeof(int));
    }
    
    enviar_paquete(paquete, socket_kernel_dispatch);
    eliminar_paquete(paquete);

    log_info(logger, "PID: %d - PCB enviado al Kernel con motivo: %s. PC: %d", pid, op_code_a_string(motivo), pc);
}

// Hilo para escuchar interrupciones
void* hilo_escucha_interrupciones(void* arg) {
    while (true) {

        t_paquete* paquete = recibir_paquete(socket_kernel_interrupt);
        if (paquete == NULL) {
            LOG_ERROR("Error al recibir paquete o Kernel ha cerrado la conexion Interrupt. Terminando hilo de interrupciones.");
            break; 
        }

        op_code cod_op = paquete->codigo_operacion;

        if (cod_op == PROCESO_INTERRUPCION) { // El Kernel envia una señal para interrumpir el proceso actual

            log_info(logger, "## Llega interrupcion al puerto Interrupt.");

            pthread_mutex_lock(&mutex_interrupcion_pendiente);
            INTERRUPCION_PENDIENTE = true;
            pthread_mutex_unlock(&mutex_interrupcion_pendiente);

        } else if (cod_op == -1) {
            LOG_ERROR("El Kernel ha cerrado la conexion Interrupt. Terminando hilo de interrupciones.");
            break;
        } else {
            log_warning(logger, "Operacion (%d) no reconocida recibida del Kernel Interrupt.", cod_op);
        }
        eliminar_paquete(paquete);
    }
    return NULL;
}


// ------------------------------------ FUNCIONALIDAD MMI ------------------------------------

void iniciar_mmu() {
    inicializar_tlb();
    inicializar_cache();
}

void inicializar_cache() {
    if(entradas_cache > 0) {
        pthread_mutex_init(&mutex_cache, NULL);
        cache_paginas = malloc(sizeof(entrada_cache) * entradas_cache);
        for (int i = 0; i < entradas_cache; i++) {
            cache_paginas[i].ocupado = false;
            cache_paginas[i].pid = -1;
            cache_paginas[i].pagina = -1;
            cache_paginas[i].contenido = malloc(tam_pagina_memoria);
            memset(cache_paginas[i].contenido, 0, tam_pagina_memoria);
            cache_paginas[i].bit_uso = false;
            cache_paginas[i].bit_modificado = false;
            cache_paginas[i].marco = -1;
        }
        log_info(logger, "Cache de Paginas inicializada con %d entradas. Algoritmo: %s", entradas_cache, algoritmo_cache);
    }
}

void inicializar_tlb() {
    if(total_entradas_tlb > 0) {
        pthread_mutex_init(&mutex_tlb, NULL);
        tlb = malloc(sizeof(entrada_tlb) * total_entradas_tlb);
        for (int i = 0; i < total_entradas_tlb; i++) {
            tlb[i].ocupada = false;
            tlb[i].pagina = -1;
            tlb[i].marco = -1;
            tlb[i].pid = -1;
            tlb[i].timestamp = 0;
        } 
        puntero_tlb_fifo = 0;
        log_info(logger, "TLB inicializada con %d entradas.", total_entradas_tlb);
    }
}

void limpiar_tlb() {
    if(total_entradas_tlb > 0) {
        pthread_mutex_lock(&mutex_tlb);
        for (int i = 0; i < total_entradas_tlb; i++) {
            tlb[i].ocupada = false;
            tlb[i].pid = -1;
            tlb[i].pagina = -1;
            tlb[i].marco = -1;
            tlb[i].timestamp = 0;
        }
        puntero_tlb_fifo = 0;
        log_info(logger, "TLB limpiada.");
        pthread_mutex_unlock(&mutex_tlb);
    }
}

void limpiar_cache_de_proceso(int pid) {
    if (entradas_cache <= 0) {
        return;
    }

    pthread_mutex_lock(&mutex_cache);
    log_info(logger, "Iniciando limpieza de cache para PID %d.", pid);

    for (int i = 0; i < entradas_cache; i++) {
        if (cache_paginas[i].ocupado && cache_paginas[i].pid == pid) {
            
            if (cache_paginas[i].bit_modificado) {
                log_warning(logger, "PID %d: Pagina %d de cache esta MODIFICADA. Realizando Write-Back a memoria.", pid, cache_paginas[i].pagina);
                enviar_pagina_a_memoria_principal(cache_paginas[i].marco, cache_paginas[i].contenido, pid);
            }

            free(cache_paginas[i].contenido);
            cache_paginas[i].contenido = NULL;
            cache_paginas[i].ocupado = false;
            cache_paginas[i].pid = -1;
            cache_paginas[i].pagina = -1;
            cache_paginas[i].marco = -1;
            cache_paginas[i].bit_uso = false;
            cache_paginas[i].bit_modificado = false;
        }
    }
    
    log_info(logger, "Limpieza de cache para PID %d finalizada.", pid);
    pthread_mutex_unlock(&mutex_cache);
}

void escribir_bytes_en_memoria_principal(int marco_fisico, int offset, int tamanio_a_escribir, char* valor, int pid) {
   
    t_paquete* paquete = crear_paquete(ESCRIBIR_MEMORIA);
    agregar_a_paquete(paquete, &pid, sizeof(int)); 
    agregar_a_paquete(paquete, &marco_fisico, sizeof(int));
    agregar_a_paquete(paquete, &offset, sizeof(int));
    agregar_a_paquete(paquete, &tamanio_a_escribir, sizeof(int));
    agregar_a_paquete(paquete, valor, tamanio_a_escribir);
    enviar_paquete(paquete, socket_memoria);
    eliminar_paquete(paquete);

    t_paquete* respuesta_memoria = recibir_paquete(socket_memoria);
    if (respuesta_memoria == NULL) {
        LOG_ERROR("PID: %d - Error al recibir respuesta de Memoria al escribir bytes.", pid);
        return;
    }
    
    op_code respuesta_op = respuesta_memoria->codigo_operacion;
    
    if (respuesta_op != OPERACION_ESCRITURA_OK) {
        LOG_ERROR("PID: %d - Error al escribir bytes en Memoria. Respuesta: %s", pid, op_code_a_string(respuesta_op));
    } else {
        log_info(logger, "PID: %d - Accion: ESCRIBIR - Dirección Física: %d, Valor: '%s'", pid, marco_fisico, valor);
    }
    eliminar_paquete(respuesta_memoria);
}

int obtener_pagina(int direccion_logica) {
    return direccion_logica / tam_pagina_memoria;
}

int obtener_offset(int direccion_logica) {
    return direccion_logica % tam_pagina_memoria;
}

// NO SE USA
/* char* buscar_en_cache(int pid, int pagina_logica) {
    if(entradas_cache > 0) {
        pthread_mutex_lock(&mutex_cache);
        for (int i = 0; i < entradas_cache; i++) {
            if (cache_paginas[i].ocupado && cache_paginas[i].pid == pid && cache_paginas[i].pagina == pagina_logica) {
                if (strcmp(algoritmo_cache, "CLOCK") == 0 || strcmp(algoritmo_cache, "CLOCK-M") == 0) {
                    cache_paginas[i].bit_uso = true;
                }
                char* contenido_copia = malloc(tam_pagina_memoria);
                memcpy(contenido_copia, cache_paginas[i].contenido, tam_pagina_memoria);
                pthread_mutex_unlock(&mutex_cache);
    
                log_info(logger, "Cache Hit: PID %d, Pagina %d", pid, pagina_logica);
                return contenido_copia;
            }
        }
        log_info(logger, "Cache Miss: PID %d, Pagina %d", pid, pagina_logica);
        pthread_mutex_unlock(&mutex_cache);
    }
    return NULL;
} */

entrada_tlb* buscar_en_tlb(int pid, int pagina) {
    if(total_entradas_tlb > 0) {
        pthread_mutex_lock(&mutex_tlb);
        for (int i = 0; i < total_entradas_tlb; i++) {
            if (tlb[i].ocupada && tlb[i].pid == pid && tlb[i].pagina == pagina) {
                if (strcmp(algoritmo_tlb, "LRU") == 0) {
                    tlb[i].timestamp = get_timestamp();
                }
                pthread_mutex_unlock(&mutex_tlb);
                log_info(logger, "PID: %d - TLB HIT - Pagina: %d", pid, pagina);
                return &tlb[i];
            }
        }
        log_info(logger, "TLB Miss: PID %d, Pagina %d", pid, pagina);
        pthread_mutex_unlock(&mutex_tlb);
    }
    return NULL;
}


uint64_t get_timestamp() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000LL) + (tv.tv_usec / 1000);
}

int pedir_traduccion_a_memoria(int pid, int direccion_logica) {

    log_info(logger, "PID: %d - Solicitando traduccion de direccion logica %d a Memoria.", pid, direccion_logica);

    t_paquete* paquete = crear_paquete(TRADUCIR_TABLA);
    agregar_a_paquete(paquete, &pid, sizeof(int));
    agregar_a_paquete(paquete, &direccion_logica, sizeof(int));
    enviar_paquete(paquete, socket_memoria);
    eliminar_paquete(paquete);

    t_paquete* paquete_respuesta = recibir_paquete(socket_memoria);
    if (paquete_respuesta == NULL) {
        LOG_ERROR("PID %d: Error al recibir respuesta de Memoria al traducir direccion logica %d.", pid, direccion_logica);
        return -1;
    }

    int code_op = paquete_respuesta->codigo_operacion;
    op_code cod_operacion = (op_code)code_op;
    
    if (cod_operacion == TRADUCCION_OK) {

        int pid_recibido;
        int direccion_logica_recibida;
        int marco_recibido;
        int offset = 0;

        memcpy(&pid_recibido, paquete_respuesta->buffer->stream + offset, sizeof(int));
        offset += sizeof(int);
        memcpy(&direccion_logica_recibida, paquete_respuesta->buffer->stream + offset, sizeof(int));
        offset += sizeof(int);
        memcpy(&marco_recibido, paquete_respuesta->buffer->stream + offset, sizeof(int));
    
        log_info(logger, "PID: %d - OBTENER MARCO - Pagina: %d - Marco: %d ", pid_recibido, obtener_pagina(direccion_logica_recibida), marco_recibido);
        return marco_recibido;

    } else {
        LOG_ERROR("PID: %d - Error al traducir direccion logica %d. Codigo de error de Memoria: %s", pid, direccion_logica, op_code_a_string(cod_operacion));
        return -1;
    }
    eliminar_paquete(paquete_respuesta);
}


void agregar_a_tlb(int pid, int pagina, int marco) {
    if(total_entradas_tlb > 0) {
        pthread_mutex_lock(&mutex_tlb);
    
        // 1. Intentar encontrar una entrada vacia
        for (int i = 0; i < total_entradas_tlb; i++) {
            if (!tlb[i].ocupada) {
                tlb[i].pid = pid;
                tlb[i].pagina = pagina;
                tlb[i].marco = marco;
                tlb[i].timestamp = get_timestamp();
                tlb[i].ocupada = true;
                log_info(logger, "TLB - Agregada nueva entrada en espacio libre: PID %d, Pagina %d -> Marco %d (Indice %d)", pid, pagina, marco, i);
                pthread_mutex_unlock(&mutex_tlb);
                return;
            }
        }
    
        // 2. Si no hay espacio libre, aplicar algoritmo de reemplazo
        log_warning(logger, "TLB llena. Aplicando algoritmo de reemplazo '%s' para PID %d, Pagina %d", algoritmo_tlb, pid, pagina);
        reemplazar_en_tlb(pid, pagina, marco);
        pthread_mutex_unlock(&mutex_tlb);
    }
}

// Reemplazo en TLB - LRU o FIFO
void reemplazar_en_tlb(int pid, int pagina, int marco) {
    if (strcmp(algoritmo_tlb, "LRU") == 0) {
        // Encontrar la entrada menos recientemente usada (timestamp mas antiguo)
        uint64_t min_timestamp = ULLONG_MAX;
        int indice_victima = -1;

        for (int i = 0; i < total_entradas_tlb; i++) {
            if (tlb[i].timestamp < min_timestamp) {
                min_timestamp = tlb[i].timestamp;
                indice_victima = i;
            }
        }
        if (indice_victima != -1) {
            log_info(logger, "TLB - Reemplazo LRU: Victima PID %d, Pagina %d (Indice %d). Reemplazando por PID %d, Pagina %d -> Marco %d",
                     tlb[indice_victima].pid, tlb[indice_victima].pagina, indice_victima, pid, pagina, marco);
            tlb[indice_victima].pid = pid;
            tlb[indice_victima].pagina = pagina;
            tlb[indice_victima].marco = marco;
            tlb[indice_victima].timestamp = get_timestamp();
            tlb[indice_victima].ocupada = true;
        }
    } else if (strcmp(algoritmo_tlb, "FIFO") == 0) {
        log_info(logger, "TLB - Reemplazo FIFO: Victima PID %d, Pagina %d (Indice %d). Reemplazando por PID %d, Pagina %d -> Marco %d",
                 tlb[puntero_tlb_fifo].pid, tlb[puntero_tlb_fifo].pagina, puntero_tlb_fifo, pid, pagina, marco);

        tlb[puntero_tlb_fifo].pid = pid;
        tlb[puntero_tlb_fifo].pagina = pagina;
        tlb[puntero_tlb_fifo].marco = marco;
        tlb[puntero_tlb_fifo].timestamp = get_timestamp();
        tlb[puntero_tlb_fifo].ocupada = true;
        puntero_tlb_fifo = (puntero_tlb_fifo + 1) % total_entradas_tlb; // Avanzar puntero FIFO

    } else {
        LOG_ERROR("Algoritmo de reemplazo de TLB no reconocido: %s. Debe ser 'LRU' o 'FIFO'.", algoritmo_tlb);
    }
}

void insertar_en_cache(int pid, int pagina_logica, int marco_fisico, char* contenido, bool modificado) {
    if(entradas_cache > 0) {
        pthread_mutex_lock(&mutex_cache);
    
        for (int i = 0; i < entradas_cache; i++) {
            if (!cache_paginas[i].ocupado) {
                aplicar_reemplazo_cache(i, pid, pagina_logica, marco_fisico, contenido, modificado);
                pthread_mutex_unlock(&mutex_cache);
                return;
            }
        }
    
        log_warning(logger, "Cache llena. Aplicando algoritmo de reemplazo '%s' para PID %d, Pagina %d", algoritmo_cache, pid, pagina_logica);
        void* contenido_victima_reemplazo = seleccionar_victima_cache(pid, pagina_logica, marco_fisico, contenido, modificado);
        if(contenido_victima_reemplazo) {
            free(contenido_victima_reemplazo);
        }
        pthread_mutex_unlock(&mutex_cache);
    }
}

void aplicar_reemplazo_cache(int indice, int pid, int pagina, int marco, char* contenido, bool modificado) {
    if(entradas_cache > 0) {
        cache_paginas[indice].contenido = malloc(tam_pagina_memoria); 
        if (cache_paginas[indice].contenido == NULL) {
            LOG_ERROR("PID: %d - Error al asignar memoria para el contenido de la cache en entrada %d.", pid, indice);
            return;
        }
        memcpy(cache_paginas[indice].contenido, contenido, tam_pagina_memoria);     
    
        cache_paginas[indice].pid = pid;
        cache_paginas[indice].pagina = pagina;
        cache_paginas[indice].marco = marco;
        cache_paginas[indice].ocupado = true;
        cache_paginas[indice].bit_uso = true;
        cache_paginas[indice].bit_modificado = modificado;

        log_info(logger, "PID: %d - Cache Add - Pagina: <%d>", pid, pagina);
    }
}


// Implementacion de CLOCK y CLOCK-M
void* seleccionar_victima_cache(int pid_a_insertar, int pagina_a_insertar, int marco_a_insertar, char* contenido_a_insertar, bool modificado_a_insertar) {
    if(entradas_cache > 0) {
        // log_estado_cache_tabla(logger, cache_paginas, entradas_cache, puntero_clock_cache, "ANTES del reemplazo");
        if (strcmp(algoritmo_cache, "CLOCK") == 0) {
            while (true) {
                if (!cache_paginas[puntero_clock_cache].ocupado) {
                    log_info(logger, "Cache CLOCK: Entrada no ocupada en puntero %d. Aplicando reemplazo.", puntero_clock_cache);
                    aplicar_reemplazo_cache(puntero_clock_cache, pid_a_insertar, pagina_a_insertar, marco_a_insertar, contenido_a_insertar, modificado_a_insertar);
                    break;
                }
    
                if (cache_paginas[puntero_clock_cache].bit_uso == false) {
                    if (cache_paginas[puntero_clock_cache].bit_modificado) {
                        log_warning(logger, "Cache CLOCK: Pagina modificada (PID %d, Pagina %d) desalojada.", cache_paginas[puntero_clock_cache].pid, cache_paginas[puntero_clock_cache].pagina);
                        enviar_pagina_a_memoria_principal(cache_paginas[puntero_clock_cache].marco, cache_paginas[puntero_clock_cache].contenido, pid_a_insertar);
                    } else {
                        free(cache_paginas[puntero_clock_cache].contenido);
                    }
    
                    aplicar_reemplazo_cache(puntero_clock_cache, pid_a_insertar, pagina_a_insertar, marco_a_insertar, contenido_a_insertar, modificado_a_insertar);
                    break;
    
                } else {
                    cache_paginas[puntero_clock_cache].bit_uso = false;
                }
                puntero_clock_cache = (puntero_clock_cache + 1) % entradas_cache;
            }
        } else if (strcmp(algoritmo_cache, "CLOCK-M") == 0) {
            int victima_encontrada = -1;
    
            // BUCLE 1: Busca (U=0, M=0). Si no lo encuentra, baja los bits de uso.
            while(victima_encontrada == -1) {
                // PASADA 1.1: Buscar (0,0) sin modificar nada.
                for (int i=0; i<entradas_cache; i++) {
                    int idx = (puntero_clock_cache + i) % entradas_cache;
                    if (cache_paginas[idx].bit_uso == 0 && cache_paginas[idx].bit_modificado == 0) {
                        victima_encontrada = idx;
                        break;
                    }
                }
                if (victima_encontrada != -1) break;
    
                // PASADA 1.2: Buscar (0,1) y bajar bits de uso.
                for (int i=0; i<entradas_cache; i++) {
                     if (cache_paginas[puntero_clock_cache].bit_uso == 0 && cache_paginas[puntero_clock_cache].bit_modificado == 1) {
                        victima_encontrada = puntero_clock_cache;
                        break;
                    }
                    cache_paginas[puntero_clock_cache].bit_uso = 0;
                    puntero_clock_cache = (puntero_clock_cache + 1) % entradas_cache;
                }
                if (victima_encontrada != -1) break;
            }
            
            // --- PROCESAR LA VÍCTIMA ENCONTRADA ---
            log_info(logger, "Cache CLOCK-M: Victima seleccionada en entrada %d (PID %d, Pagina %d).", victima_encontrada, cache_paginas[victima_encontrada].pid, cache_paginas[victima_encontrada].pagina);
    
            if (cache_paginas[victima_encontrada].bit_modificado) {
                log_warning(logger, "Pagina modificada. Escribiendo a memoria...");
                enviar_pagina_a_memoria_principal(cache_paginas[victima_encontrada].marco, cache_paginas[victima_encontrada].contenido, pid_a_insertar);
            }
    
            free(cache_paginas[victima_encontrada].contenido);
            aplicar_reemplazo_cache(victima_encontrada, pid_a_insertar, pagina_a_insertar, marco_a_insertar, contenido_a_insertar, modificado_a_insertar);
            puntero_clock_cache = (victima_encontrada + 1) % entradas_cache;
    
        } else {
            LOG_ERROR("Algoritmo de reemplazo de cache desconocido: %s.", algoritmo_cache);
            return NULL;
        }
        
        // log_estado_cache_tabla(logger, cache_paginas, entradas_cache, puntero_clock_cache, "DESPUES del reemplazo");
    }
    return NULL;
}

char* leer_bytes_de_memoria_principal(int marco_fisico, int offset, int tamanio_a_leer, int pid) {
    t_paquete* paquete = crear_paquete(LEER_MEMORIA);
    agregar_a_paquete(paquete, &pid, sizeof(int));
    agregar_a_paquete(paquete, &marco_fisico, sizeof(int));
    agregar_a_paquete(paquete, &offset, sizeof(int));
    agregar_a_paquete(paquete, &tamanio_a_leer, sizeof(int));
    enviar_paquete(paquete, socket_memoria);
    eliminar_paquete(paquete);

    t_paquete* respuesta = recibir_paquete(socket_memoria);
    if (respuesta == NULL) {
        LOG_ERROR("PID %d: Error al recibir respuesta de Memoria al leer bytes.", pid);
        return NULL;
    }

    op_code respuesta_op = respuesta->codigo_operacion;

    if (respuesta_op != OPERACION_LECTURA_OK) {
        LOG_ERROR("PID %d: Error al leer bytes de Memoria. Codigo de error: %s", pid, op_code_a_string(respuesta_op));
        eliminar_paquete(respuesta);
        return NULL;
    }

    if(respuesta->buffer->size <= 0) {
        LOG_ERROR("PID: %d - Error: Tamaño de buffer recibido de Memoria es cero o negativo.", pid);
        eliminar_paquete(respuesta);
        return NULL;
    }

    char* datos_leidos = malloc(respuesta->buffer->size + 1); 
    if (datos_leidos == NULL) {
        LOG_ERROR("PID: %d - Error de asignacion de memoria para datos leidos.", pid);
        eliminar_paquete(respuesta);
        return NULL;
    }

    memcpy(datos_leidos, respuesta->buffer->stream, respuesta->buffer->size);
    datos_leidos[respuesta->buffer->size] = '\0';

    log_info(logger, "PID: %d - Accion: LEER - Direccion Fisica: %d - Valor: '%s'", pid, marco_fisico, datos_leidos);
    eliminar_paquete(respuesta);
    return datos_leidos;
    
}

void enviar_pagina_a_memoria_principal(int marco_fisico, char* contenido_pagina, int pid) {
    escribir_bytes_en_memoria_principal(marco_fisico, 0, tam_pagina_memoria, contenido_pagina, pid);
}


// ----------------------------------------------------------------------------------------------
// ------------------------------------ FUNCIONES AUXILIARES ------------------------------------
// ----------------------------------------------------------------------------------------------


void destruir_instruccion(t_instruccion* instruccion) {
    if (instruccion == NULL) return;

    if (instruccion->opcode != NULL) {
        free(instruccion->opcode);
    }

    for (int i = 0; i < instruccion->cantidad_parametros; i++) {
        if (instruccion->parametros[i] != NULL) {
            free(instruccion->parametros[i]);
        }
    }

    free(instruccion);
}

char* concatenar_parametros(t_instruccion* instruccion) {
    char* resultado = string_new();
    for (int i = 0; i < instruccion->cantidad_parametros; i++) {
        string_append(&resultado, instruccion->parametros[i]);
        if (i < instruccion->cantidad_parametros - 1)
            string_append(&resultado, " ");
    }
    return resultado;
}

/* void log_estado_cache_tabla(t_log* logger, entrada_cache* cache, int entradas, int puntero_actual, const char* evento) {
    if (entradas <= 0) return;

    char* header = string_from_format("======= ESTADO CACHE: %s =======", evento);
    char* footer = string_repeat('=', strlen(header));
    
    log_info(logger, header);
    log_info(logger, "Puntero CLOCK -> [%d]", puntero_actual);
    log_info(logger, "----------------------------------------------------");
    log_info(logger, "| Indice | Ocupado | PID | Pag | Marco | Uso (U) | Mod (M) |");
    log_info(logger, "----------------------------------------------------");

    for (int i = 0; i < entradas; i++) {
        entrada_cache* entrada = &cache[i];
        log_info(logger, "|   %d    |    %d    | %-3d | %-3d |  %-4d |    %d    |    %d    |",
                 i,
                 entrada->ocupado,
                 entrada->ocupado ? entrada->pid : -1,
                 entrada->ocupado ? entrada->pagina : -1,
                 entrada->ocupado ? entrada->marco : -1,
                 entrada->bit_uso,
                 entrada->bit_modificado);
    }
    log_info(logger, "----------------------------------------------------");
    free(header);
    free(footer);
} */