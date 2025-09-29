#include <commons/log.h>
#include <commons/config.h>
#include <commons/collections/dictionary.h>
#include <commons/string.h>

#include <utils/hello.h>    
#include <utils/utils.h>
#include <utils/errors.h>
#include <unistd.h>             
#include <pthread.h>          
#include <stdlib.h>        
#include <string.h>           

#include "memoria.h"

int socket_escucha_memoria;
t_log *logger;
t_config *config;

void* memoria_usuario;
t_dictionary* diccionario_procesos;
char* path_instrucciones;
char* dump_path;

char *puerto_escucha;
int tam_memoria;
int tam_pagina;
int cantidad_niveles;
int entradas_por_tabla;

bool* bitmap_marcos;
int cantidad_marcos;

char* path_swap;
int retardo_swap;
int retardo_memoria;
t_bitarray* bitmap_swap;

pthread_mutex_t mutex_bitmap = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_diccionario_procesos = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_swap = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_memoria_principal = PTHREAD_MUTEX_INITIALIZER;

int global_tabla_id_counter = 0; 

int bits_offset;
int bits_por_nivel;
int mascara_nivel; 


int main(int argc, char* argv[]) {
    saludar("memoria");

    config = config_create(argv[1]);
    logger = iniciar_logger();
    iniciar_manejador_de_errores(logger);

    if (config == NULL) {
        LOG_ERROR("No fue posible iniciar el archivo de configuración.");
        terminar_programa(logger, config, socket_escucha_memoria);
        return 1; 
    }

    // Carga de configuracion
    puerto_escucha = config_get_string_value(config, "PUERTO_ESCUCHA");
    tam_memoria = config_get_int_value(config, "TAM_MEMORIA");
    tam_pagina = config_get_int_value(config, "TAM_PAGINA");
    path_instrucciones = config_get_string_value(config, "PATH_INSTRUCCIONES");
    cantidad_niveles = config_get_int_value(config, "CANTIDAD_NIVELES");
    entradas_por_tabla = config_get_int_value(config, "ENTRADAS_POR_TABLA");
    path_swap = config_get_string_value(config, "PATH_SWAPFILE");
    retardo_swap = config_get_int_value(config, "RETARDO_SWAP");
    retardo_memoria = config_get_int_value(config, "RETARDO_MEMORIA");
    dump_path = config_get_string_value(config, "DUMP_PATH");

    // Validaciones basicas de configuracion
    if (!puerto_escucha || tam_memoria <= 0 || tam_pagina <= 0 || !path_instrucciones || cantidad_niveles <= 0 || entradas_por_tabla <= 0) {
        LOG_ERROR("Valores de configuración faltantes o inválidos.");
        terminar_programa(logger, config, socket_escucha_memoria);
        return 1;
    }
    
    if (tam_memoria % tam_pagina != 0) {
        LOG_ERROR("TAM_MEMORIA (%d) debe ser múltiplo de TAM_PAGINA (%d).", tam_memoria, tam_pagina);
        terminar_programa(logger, config, socket_escucha_memoria);
        return 1;
    }

    // Validar que ENTRADAS_POR_TABLA sea potencia de 2 para log2
    if ((entradas_por_tabla & (entradas_por_tabla - 1)) != 0 && entradas_por_tabla != 0) {
        LOG_ERROR("ENTRADAS_POR_TABLA (%d) debe ser una potencia de 2.", entradas_por_tabla);
        terminar_programa(logger, config, socket_escucha_memoria);
        return 1;
    }

    // Validar que TAM_PAGINA sea potencia de 2 para log2
    if ((tam_pagina & (tam_pagina - 1)) != 0 && tam_pagina != 0) {
        LOG_ERROR("TAM_PAGINA (%d) debe ser una potencia de 2.", tam_pagina);
        terminar_programa(logger, config, socket_escucha_memoria);
        return 1;
    }

    iniciar_swap_file();

    // Calcular valores derivados para la traduccion de direcciones
    bits_offset = (int)log2(tam_pagina);
    bits_por_nivel = (int)log2(entradas_por_tabla);
    mascara_nivel = (1 << bits_por_nivel) - 1;

    diccionario_procesos = dictionary_create();
    memoria_usuario = malloc(tam_memoria);
    if (memoria_usuario == NULL) {
        LOG_ERROR("Fallo al alocar %d bytes para la memoria de usuario.", tam_memoria);
        terminar_programa(logger, config, socket_escucha_memoria);
        return 1;
    }
    memset(memoria_usuario, 0, tam_memoria);

    cantidad_marcos = tam_memoria / tam_pagina;
    bitmap_marcos = malloc(sizeof(bool) * cantidad_marcos);
    if (bitmap_marcos == NULL) {
        LOG_ERROR("Fallo al alocar el bitmap para %d marcos.", cantidad_marcos);
        free(memoria_usuario);
        terminar_programa(logger, config, socket_escucha_memoria);
        return 1;
    }

    for (int i = 0; i < cantidad_marcos; i++) {
        bitmap_marcos[i] = false;
    }

    log_info(logger, "Memoria fisica inicializada. Tamaño: %d bytes, Paginas: %d bytes cada una, Cantidad de marcos: %d", tam_memoria, tam_pagina, cantidad_marcos);

    socket_escucha_memoria = iniciar_servidor(puerto_escucha);
    if (socket_escucha_memoria == -1) {
        LOG_ERROR("No fue posible iniciar el servidor en el puerto %s.", puerto_escucha);
        free(memoria_usuario);
        free(bitmap_marcos);  
        terminar_programa(logger, config, socket_escucha_memoria);
        return 1;
    };

    log_info(logger, "La memoria esta lista para recibir peticiones");

    concurrencia_memoria();
   
    log_info(logger, "Finalizando programa de memoria.");
    free(memoria_usuario);
    free(bitmap_marcos);

    dictionary_destroy_and_destroy_elements(diccionario_procesos, (void(*)(void*))destruir_proceso_wrapper);
    terminar_programa(logger, config, socket_escucha_memoria);
  
    return 0;
}


/*------------------------------ FUNCIONES AUXILIARES ------------------------------------------------------*/

t_config* iniciar_config(void) {
    return config_create("memoria.config");
}

t_log* iniciar_logger(void) {
    return log_create("memoria.log", "Memoria", true, LOG_LEVEL_INFO);
}

void terminar_programa(t_log *logger, t_config *config, int socket_escucha_memoria) {
    if (logger) log_destroy(logger);
    if (config) config_destroy(config);
    if (socket_escucha_memoria != -1) close(socket_escucha_memoria);
}

void iniciar_swap_file() {
    FILE* swap_file = fopen(path_swap, "wb");
    if (swap_file == NULL) {
        LOG_ERROR("No se pudo crear/truncar el archivo de SWAP en %s", path_swap);
        exit(EXIT_FAILURE);
    }
    fclose(swap_file);

    char* data = calloc(1, 1);
    bitmap_swap = bitarray_create_with_mode(data, 1, LSB_FIRST);

    log_info(logger, "Archivo de SWAP dinamico inicializado en %s (vacio).", path_swap);
}

void concurrencia_memoria() {
    while (1) {
        pthread_t hilo_cliente;

        int* nuevo_cliente = malloc(sizeof(int));
        *nuevo_cliente = esperar_cliente(socket_escucha_memoria);
        if (*nuevo_cliente == -1) {
            LOG_ERROR("Fallo al aceptar nueva conexión de cliente.");
            free(nuevo_cliente);
            sleep(1);
            continue;
        }
        log_info(logger, "Nuevo cliente conectado: fd=%d", *nuevo_cliente);

        pthread_create(&hilo_cliente, NULL, atender_cliente, nuevo_cliente);
        pthread_detach(hilo_cliente);
    }
}

void* atender_cliente(void* arg) {  // Clientes de memoria => Kernel y CPU
    int cliente_fd = *(int*)arg;
    free(arg);

    handshake_memoria(cliente_fd, logger); 

    // No cerramos el socket: lo pasamos a un nuevo hilo
    pthread_t hilo_flujo;
    int* fd_post_handshake = malloc(sizeof(int));
    *fd_post_handshake = cliente_fd;
    pthread_create(&hilo_flujo, NULL, atender_post_handshake, fd_post_handshake);
    pthread_detach(hilo_flujo);

    return NULL;
}

void handshake_memoria(int socket_cliente, t_log* logger) {

    t_paquete* paquete = recibir_paquete(socket_cliente);
    if (paquete == NULL) {
        LOG_ERROR("Error al recibir el paquete de handshake desde el cliente FD: %d", socket_cliente);
        close(socket_cliente);
        return;
    }

    op_code cod_op_recibido = paquete->codigo_operacion;
    eliminar_paquete(paquete); 
    op_code respuesta = HANDSHAKE_OK;

    switch (cod_op_recibido) {
        case HANDSHAKE_MEMORIA_CPU:
            t_paquete* handshake_cpu = crear_paquete(respuesta);
            agregar_a_paquete(handshake_cpu, &tam_pagina, sizeof(int));
            enviar_paquete(handshake_cpu, socket_cliente);
            eliminar_paquete(handshake_cpu);

            log_info(logger, "## CPU Conectada - FD del socket: %d", socket_cliente);
        
            break;
        case HANDSHAKE_MEMORIA_KERNEL:
            t_paquete* handshake_kernel = crear_paquete(respuesta);
            enviar_paquete(handshake_kernel, socket_cliente);
            eliminar_paquete(handshake_kernel);

            log_info(logger, "## Kernel Conectado - FD del socket: %d", socket_cliente);
            break;
        default:
            log_warning(logger, "Codigo de operacion desconocido en handshake: %d desde FD: %d", cod_op_recibido, socket_cliente);
            break;
    }
}

void* atender_post_handshake(void* arg) {
    int cliente_fd = *(int*)arg;
    free(arg);

    log_info(logger, "Iniciando atencion post-handshake para cliente FD %d", cliente_fd);

    t_paquete* paquete;

    while ((paquete = recibir_paquete(cliente_fd)) != NULL) {
        op_code cod_op = paquete->codigo_operacion;
        if (cod_op == -1) {
            log_info(logger, "Cliente FD %d desconectado (recibido COD_OPERACION -1)", cliente_fd);
            eliminar_paquete(paquete);
            break;
        }

        switch (cod_op) {
            case PEDIR_INSTRUCCIONES: {
                log_info(logger, "Recibido PEDIR_INSTRUCCIONES de FD: %d", cliente_fd);
                
                int pid,pc;
                int offset = 0;

                memcpy(&pid, paquete->buffer->stream + offset, sizeof(int));
                offset += sizeof(int);
                memcpy(&pc, paquete->buffer->stream + offset, sizeof(int));

                enviar_instruccion_segun_pid_y_pc(cliente_fd, pid, pc);
                break;
            }
            case PEDIR_ESPACIO_LIBRE: {
                log_info(logger, "Recibido PEDIR_ESPACIO_LIBRE de FD: %d", cliente_fd);
                enviar_espacio_libre(cliente_fd); 
                break;
            }
            case INIT_PROC: {
                log_info(logger, "Recibido INIT_PROC de FD: %d", cliente_fd);
               
                int pid_proceso;
                int tamanio_proceso;
                int largo_path;
                char* path_pseudocodigo;
                int offset = 0;

                memcpy(&pid_proceso, paquete->buffer->stream + offset, sizeof(int));
                offset += sizeof(int);
                memcpy(&tamanio_proceso, paquete->buffer->stream + offset, sizeof(int));
                offset += sizeof(int);
                memcpy(&largo_path, paquete->buffer->stream + offset, sizeof(int));
                offset += sizeof(int);
                
                path_pseudocodigo = malloc(largo_path + 1);
                memcpy(path_pseudocodigo, paquete->buffer->stream + offset, largo_path);
                path_pseudocodigo[largo_path] = '\0';

                log_info(logger, "Datos INIT_PROC: PID %d, Tamaño %d, Path '%s'", pid_proceso, tamanio_proceso, path_pseudocodigo);

                almacenar_proceso_nuevo(cliente_fd, pid_proceso, tamanio_proceso, path_pseudocodigo);
                
                free(path_pseudocodigo);
                // mostrar_estado_paginacion();
                break;
            }
            case TRADUCIR_TABLA: { // Este es el que la CPU va a usar para traducir DL a marco fisico
                log_info(logger, "Recibido TRADUCIR_TABLA (solicitud de traduccion) de FD: %d", cliente_fd);

                int pid_proceso;
                int direccion_logica;
                int offset = 0;

                memcpy(&pid_proceso, paquete->buffer->stream + offset, sizeof(int));
                offset += sizeof(int);
                memcpy(&direccion_logica, paquete->buffer->stream + offset, sizeof(int));

                manejar_traduccion_tabla(cliente_fd, pid_proceso, direccion_logica);
                break;
            }
            case FINALIZAR_PROCESO: { 
                log_info(logger, "Recibido FINALIZAR_PROCESO de FD: %d", cliente_fd);
               
                int pid_a_finalizar;
                memcpy(&pid_a_finalizar, paquete->buffer->stream, sizeof(int));

                destruir_proceso(pid_a_finalizar);

                t_paquete* respuesta = crear_paquete(FINALIZACION_OK);
                enviar_paquete(respuesta, cliente_fd);
                eliminar_paquete(respuesta);
                break;
            }
            case LEER_MEMORIA: {
                log_info(logger, "Recibido LEER_MEMORIA de FD: %d", cliente_fd);
                int pid, marco_fisico, offset, tamanio_a_leer;
                int offsetPaquete = 0;

                memcpy(&pid, paquete->buffer->stream + offsetPaquete, sizeof(int));
                offsetPaquete += sizeof(int);
                memcpy(&marco_fisico, paquete->buffer->stream + offsetPaquete, sizeof(int));
                offsetPaquete += sizeof(int);
                memcpy(&offset, paquete->buffer->stream + offsetPaquete, sizeof(int));
                offsetPaquete += sizeof(int);
                memcpy(&tamanio_a_leer, paquete->buffer->stream + offsetPaquete, sizeof(int));

                manejar_lectura_direccion_fisica(cliente_fd, pid, marco_fisico, offset, tamanio_a_leer);
                break;
            }
            case ESCRIBIR_MEMORIA: {
                log_info(logger, "Recibido ESCRIBIR_MEMORIA de FD: %d", cliente_fd);

                int pid, marco_fisico, offset, tamanio_a_escribir;
                char* contenido_a_escribir;
                int offsetPaquete = 0;

                memcpy(&pid, paquete->buffer->stream + offsetPaquete, sizeof(int));
                offsetPaquete += sizeof(int);
				memcpy(&marco_fisico, paquete->buffer->stream + offsetPaquete, sizeof(int));
				offsetPaquete += sizeof(int);
				memcpy(&offset, paquete->buffer->stream + offsetPaquete, sizeof(int));
				offsetPaquete += sizeof(int);
				memcpy(&tamanio_a_escribir, paquete->buffer->stream + offsetPaquete, sizeof(int));
				offsetPaquete += sizeof(int);
				contenido_a_escribir = malloc(tamanio_a_escribir + 1);
				memcpy(contenido_a_escribir, paquete->buffer->stream + offsetPaquete, tamanio_a_escribir);
				contenido_a_escribir[tamanio_a_escribir] = '\0';

                manejar_escritura_direccion_fisica(cliente_fd, pid, marco_fisico, offset, tamanio_a_escribir, contenido_a_escribir);
				free(contenido_a_escribir);
                break;
            }
            case SUSPENDER_PROCESO: {
                log_info(logger, "Recibido SUSPENDER_PROCESO de FD: %d", cliente_fd);
                
                int pid_a_suspender;
                memcpy(&pid_a_suspender, paquete->buffer->stream, sizeof(int));
                usleep(retardo_swap * 1000);
                manejar_suspension_proceso(pid_a_suspender, cliente_fd);
                break;
            }
            case DESUSPENDER_PROCESO: {
                log_info(logger, "Recibido DESUSPENDER_PROCESO de FD: %d", cliente_fd);
                
                int pid_a_desuspender;
                memcpy(&pid_a_desuspender, paquete->buffer->stream, sizeof(int));
                usleep(retardo_swap * 1000);
                manejar_desuspension_proceso(pid_a_desuspender, cliente_fd);
                break;
            }
            case DUMP_MEMORY: {
                log_info(logger, "Recibido DUMP_MEMORY de FD: %d", cliente_fd);
    
                int pid_a_dumpear;
                memcpy(&pid_a_dumpear, paquete->buffer->stream, sizeof(int));
                
                bool exito = manejar_solicitud_dump(pid_a_dumpear);
                usleep(retardo_memoria * 1000);
                
                op_code respuesta_op = exito ? DUMP_OK : ERROR_DUMP;
                t_paquete* paquete_respuesta = crear_paquete(respuesta_op);
                agregar_a_paquete(paquete_respuesta, &pid_a_dumpear, sizeof(int));
                enviar_paquete(paquete_respuesta, cliente_fd);
                eliminar_paquete(paquete_respuesta);
                
                log_info(logger, "Respuesta de DUMP enviada al Kernel para PID <%d> con resultado: %s", pid_a_dumpear, op_code_a_string(respuesta_op));
                break;
            }
            default:
                log_warning(logger, "Codigo de operacion desconocido: %d desde FD: %d", cod_op, cliente_fd);
                break;
        }
        eliminar_paquete(paquete);
    }

    log_info(logger, "Cliente desconectado (FD %d)", cliente_fd);
    close(cliente_fd);
    return NULL;
}

/*------------------------------ FUNCIONES DE GESTION DE INSTRUCCIONES Y PROCESOS ------------------------------------------------------*/

void enviar_instruccion_segun_pid_y_pc(int socket_cliente, int pid, int pc) {

    usleep(retardo_memoria * 1000);

    pthread_mutex_lock(&mutex_diccionario_procesos);
    char* clave = string_itoa(pid);
    proceso_memoria* proceso = dictionary_get(diccionario_procesos, clave); 
    pthread_mutex_unlock(&mutex_diccionario_procesos);
    free(clave);

    t_paquete* paquete_respuesta = NULL;
    op_code cod_respuesta;

    if (proceso == NULL) {
        log_warning(logger, "PID %d no encontrado en Memoria para solicitud de instruccion.", pid);
        cod_respuesta = ERROR_PID_NO_ENCONTRADO;
        paquete_respuesta = crear_paquete(cod_respuesta);
        agregar_a_paquete(paquete_respuesta, &pid, sizeof(int)); 

    } else if (pc >= list_size(proceso->instrucciones) || pc < 0) {
        log_warning(logger, "PID: %d - PC invalido: %d. Fuera de rango de instrucciones.", pid, pc);
        cod_respuesta = ERROR_PC_FUERA_DE_RANGO;
        paquete_respuesta = crear_paquete(cod_respuesta);
        agregar_a_paquete(paquete_respuesta, &pc, sizeof(int));
    } else {
        t_instruccion* instruccion = list_get(proceso->instrucciones, pc);
        
        char* instruccion_completa = string_duplicate(instruccion->opcode);
        for (int i = 0; i < instruccion->cantidad_parametros; i++) {
            if (instruccion->parametros[i] != NULL) {
                string_append_with_format(&instruccion_completa, " %s", instruccion->parametros[i]);
            }
        }
        log_info(logger, "## PID: %d - Obtener instrucción: %d - Instrucción: %s", pid, pc, instruccion_completa);
        free(instruccion_completa);

        cod_respuesta = INSTRUCCION_OK;
        paquete_respuesta = crear_paquete(cod_respuesta);

        agregar_a_paquete(paquete_respuesta, &(instruccion->opcode_lenght), sizeof(int));
        agregar_a_paquete(paquete_respuesta, instruccion->opcode, instruccion->opcode_lenght);
        agregar_a_paquete(paquete_respuesta, &(instruccion->cantidad_parametros), sizeof(int));

        for (int i = 0; i < instruccion->cantidad_parametros; i++) {
            if (instruccion->parametros[i] != NULL) {
                int len_param = strlen(instruccion->parametros[i]) + 1; 
                agregar_a_paquete(paquete_respuesta, &len_param, sizeof(int));
                agregar_a_paquete(paquete_respuesta, instruccion->parametros[i], len_param);
            } else {
                int len_param = 0;
                agregar_a_paquete(paquete_respuesta, &len_param, sizeof(int));
            }
        }
    }

    proceso->metricas.instrucciones_solicitadas++;

    if (paquete_respuesta != NULL) {
        enviar_paquete(paquete_respuesta, socket_cliente);
        eliminar_paquete(paquete_respuesta); 
    }
}

void enviar_espacio_libre(int socket_cliente) {
    int espacio_libre = 0;
    pthread_mutex_lock(&mutex_bitmap);
    for (int i = 0; i < cantidad_marcos; i++) {
        if (!bitmap_marcos[i]) {
            espacio_libre += tam_pagina;
        }
    }
    pthread_mutex_unlock(&mutex_bitmap);
    send(socket_cliente, &espacio_libre, sizeof(int), 0);
    log_info(logger, "Espacio libre actual en memoria: %d bytes", espacio_libre);
}

void almacenar_proceso_nuevo(int cliente_fd, int pid, int tam_proceso, char* path_archivo) {
    // log_info(logger, "## PID: %d - Proceso Creado - Tamaño: %d", pid, tam_proceso);

    // MODIFICACION: Si el tamaño del proceso es 0, se requieren 0 paginas de memoria.
    // De lo contrario, se calcula el número de páginas necesarias.
    int paginas_requeridas = (tam_proceso == 0) ? 0 : (int)ceil((double)tam_proceso / tam_pagina);

    // Solo se verifica la disponibilidad de marcos si se requieren páginas.
    if (paginas_requeridas > 0) {
        int marcos_disponibles = contar_marcos_libres();
        if (paginas_requeridas > marcos_disponibles) {
            LOG_ERROR("Memoria insuficiente para PID %d. Requiere %d marcos, pero solo hay %d disponibles.", pid, paginas_requeridas, marcos_disponibles);

           // list_destroy_and_destroy_elements(lista_instrucciones, (void(*)(void*))liberar_elementos_instruccion);

            t_paquete* paquete_error = crear_paquete(NO_HAY_MARCOS_LIBRES);
            enviar_paquete(paquete_error, cliente_fd);
            eliminar_paquete(paquete_error);
            return;
        }
    }



    char* path_completo = string_from_format("%s%s", path_instrucciones, path_archivo);
    t_list* lista_instrucciones = list_create();
    FILE* archivo = fopen(path_completo, "r");

    if (!archivo) {
        LOG_ERROR("No se pudo abrir el archivo de instrucciones '%s' para PID %d", path_completo, pid);
        free(path_completo);
        t_paquete* paquete_error = crear_paquete(PROCESO_NO_ALMACENADO);
        enviar_paquete(paquete_error, cliente_fd);
        eliminar_paquete(paquete_error);
        list_destroy(lista_instrucciones);
        return;
    }

    char linea[300];
    while (fgets(linea, sizeof(linea), archivo)) {
        char* pos;
        if ((pos = strchr(linea, '\n')) != NULL)  // Elimina el salto de linea
            *pos = '\0';

        if (strlen(linea) == 0) continue;

        t_instruccion* inst = malloc(sizeof(t_instruccion));

        if (inst == NULL) {
            LOG_ERROR("Fallo al alocar memoria para una instruccion del PID %d", pid);
            fclose(archivo);
            list_destroy_and_destroy_elements(lista_instrucciones, (void(*)(void*))liberar_elementos_instruccion);
            free(path_completo);
            t_paquete* paquete_error = crear_paquete(PROCESO_NO_ALMACENADO);
            enviar_paquete(paquete_error, cliente_fd);
            eliminar_paquete(paquete_error);
            return;
        }

        inst->parametros[0] = inst->parametros[1] = inst->parametros[2] = NULL;

        char* linea_copy = strdup(linea); // strdup para strtok - crea una copia de la linea original
        char* token = strtok(linea_copy, " "); // Divide la linea en tokens

        inst->opcode = strdup(token);
        inst->opcode_lenght = strlen(inst->opcode) + 1;

        for (int i = 0; i < MAX_PARAMETROS && (token = strtok(NULL, " ")); i++) {
            inst->parametros[i] = strdup(token);
        }

        inst->parametro1_lenght = inst->parametros[0] ? strlen(inst->parametros[0]) + 1 : 0;
        inst->parametro2_lenght = inst->parametros[1] ? strlen(inst->parametros[1]) + 1 : 0;
        inst->parametro3_lenght = inst->parametros[2] ? strlen(inst->parametros[2]) + 1 : 0;

        inst->cantidad_parametros = 0;
        for (int j = 0; j < MAX_PARAMETROS; j++) {
            if (inst->parametros[j] != NULL) {
                inst->cantidad_parametros++;
            }
        }

        list_add(lista_instrucciones, inst);
        log_info(logger, "Instruccion agregada: %s con %d parametros", inst->opcode, inst->cantidad_parametros);
        free(linea_copy);
    }

    fclose(archivo);
    free(path_completo);



    pthread_mutex_lock(&mutex_memoria_principal);
    proceso_memoria* proceso = malloc(sizeof(proceso_memoria));
    if (proceso == NULL) {
        LOG_ERROR("Fallo al alocar la estructura principal para el PID %d", pid);
        list_destroy_and_destroy_elements(lista_instrucciones, (void(*)(void*))liberar_elementos_instruccion);
        pthread_mutex_unlock(&mutex_memoria_principal);
        t_paquete* paquete_error = crear_paquete(PROCESO_NO_ALMACENADO);
        enviar_paquete(paquete_error, cliente_fd);
        eliminar_paquete(paquete_error);
        return;
    }

    proceso->pid = pid;
    proceso->instrucciones = lista_instrucciones;
    proceso->tam_proceso = tam_proceso;
    proceso->paginas_asignadas = 0; // Se actualizará si se asignan páginas
    memset(&proceso->metricas, 0, sizeof(t_metricas_memoria));
    pthread_mutex_init(&proceso->mutex_proceso, NULL);

    // Se crea la tabla de paginas de nivel 1 solo si se requieren paginas
    if (paginas_requeridas > 0) {
        proceso->tabla_nivel_1 = crear_tabla_pagina(1); // Nivel raiz
        if (proceso->tabla_nivel_1 == NULL) {
            LOG_ERROR("No se pudo crear la tabla de páginas raíz (nivel 1) para PID %d", pid);
            free(proceso);
            list_destroy_and_destroy_elements(lista_instrucciones, (void(*)(void*))liberar_elementos_instruccion);
            pthread_mutex_unlock(&mutex_memoria_principal);
            t_paquete* paquete_error = crear_paquete(PROCESO_NO_ALMACENADO);
            enviar_paquete(paquete_error, cliente_fd);
            eliminar_paquete(paquete_error);
            return;
        }
    } else {
        proceso->tabla_nivel_1 = NULL; // No hay tabla de páginas si no hay memoria asignada
    }


    log_info(logger, "Proceso PID %d requiere %d paginas. Asignando marcos...", pid, paginas_requeridas);
    int paginas_a_asignar_temp = paginas_requeridas;

    // Solo se asignan paginas si paginas_requeridas es mayor que 0
    if (paginas_requeridas > 0) {
        // Esta llamada ahora tiene garantizado el éxito porque ya verificamos el espacio.
        asignar_paginas_recursivo(proceso->tabla_nivel_1, 1, &paginas_a_asignar_temp);
        proceso->paginas_asignadas = paginas_requeridas;
    }


    // Guardamos el proceso en el diccionario global.
    pthread_mutex_lock(&mutex_diccionario_procesos);
    char* pid_str = string_itoa(pid);
    dictionary_put(diccionario_procesos, pid_str, proceso);
    free(pid_str);
    pthread_mutex_unlock(&mutex_diccionario_procesos);

    log_info(logger, "## PID: %d - Proceso Creado - Tamaño: %d", pid, tam_proceso);
    log_info(logger, "Proceso PID %d almacenado con %d instrucciones y %d paginas asignadas.", pid, list_size(lista_instrucciones), proceso->paginas_asignadas);

    pthread_mutex_unlock(&mutex_memoria_principal);

    // Enviamos confirmación al Kernel.
    t_paquete* paquete_ok = crear_paquete(INIT_PROCESO_OK);
    enviar_paquete(paquete_ok, cliente_fd);
    eliminar_paquete(paquete_ok);

    return;
}



/*void almacenar_proceso_nuevo(int cliente_fd, int pid, int tam_proceso, char* path_archivo) {
    // log_info(logger, "## PID: %d - Proceso Creado - Tamaño: %d", pid, tam_proceso);

    char* path_completo = string_from_format("%s%s", path_instrucciones, path_archivo);
    t_list* lista_instrucciones = list_create();
    FILE* archivo = fopen(path_completo, "r");

    if (!archivo) {
        LOG_ERROR("No se pudo abrir el archivo de instrucciones '%s' para PID %d", path_completo, pid);
        free(path_completo);
        t_paquete* paquete_error = crear_paquete(PROCESO_NO_ALMACENADO);
        enviar_paquete(paquete_error, cliente_fd);
        eliminar_paquete(paquete_error);
        list_destroy(lista_instrucciones);
        return;
    }

    char linea[300];
    while (fgets(linea, sizeof(linea), archivo)) {
        char* pos;
        if ((pos = strchr(linea, '\n')) != NULL)  // Elimina el salto de linea
            *pos = '\0';

        if (strlen(linea) == 0) continue;

        t_instruccion* inst = malloc(sizeof(t_instruccion));

        if (inst == NULL) {
            LOG_ERROR("Fallo al alocar memoria para una instruccion del PID %d", pid);
            fclose(archivo);
            list_destroy_and_destroy_elements(lista_instrucciones, (void(*)(void*))liberar_elementos_instruccion);
            free(path_completo);
            t_paquete* paquete_error = crear_paquete(PROCESO_NO_ALMACENADO);
            enviar_paquete(paquete_error, cliente_fd);
            eliminar_paquete(paquete_error);
            return;
        }

        inst->parametros[0] = inst->parametros[1] = inst->parametros[2] = NULL;

        char* linea_copy = strdup(linea); // strdup para strtok - crea una copia de la linea original
        char* token = strtok(linea_copy, " "); // Divide la linea en tokens

        inst->opcode = strdup(token);
        inst->opcode_lenght = strlen(inst->opcode) + 1;

        for (int i = 0; i < MAX_PARAMETROS && (token = strtok(NULL, " ")); i++) {
            inst->parametros[i] = strdup(token);
        }

        inst->parametro1_lenght = inst->parametros[0] ? strlen(inst->parametros[0]) + 1 : 0;
        inst->parametro2_lenght = inst->parametros[1] ? strlen(inst->parametros[1]) + 1 : 0;
        inst->parametro3_lenght = inst->parametros[2] ? strlen(inst->parametros[2]) + 1 : 0;

        inst->cantidad_parametros = 0;
        for (int j = 0; j < MAX_PARAMETROS; j++) {
            if (inst->parametros[j] != NULL) {
                inst->cantidad_parametros++;
            }
        }
        
        list_add(lista_instrucciones, inst);
        log_info(logger, "Instruccion agregada: %s con %d parametros", inst->opcode, inst->cantidad_parametros);
        free(linea_copy);
    }

    fclose(archivo);
    free(path_completo);

    int paginas_requeridas = (tam_proceso == 0) ? 1 : (int)ceil((double)tam_proceso / tam_pagina);
    int marcos_disponibles = contar_marcos_libres();
    if (paginas_requeridas > marcos_disponibles) {
        LOG_ERROR("Memoria insuficiente para PID %d. Requiere %d marcos, pero solo hay %d disponibles.", pid, paginas_requeridas, marcos_disponibles);
        
        list_destroy_and_destroy_elements(lista_instrucciones, (void(*)(void*))liberar_elementos_instruccion);
        
        t_paquete* paquete_error = crear_paquete(NO_HAY_MARCOS_LIBRES);
        enviar_paquete(paquete_error, cliente_fd);
        eliminar_paquete(paquete_error);
        return;
    }

    pthread_mutex_lock(&mutex_memoria_principal);
    proceso_memoria* proceso = malloc(sizeof(proceso_memoria));
    if (proceso == NULL) {
        LOG_ERROR("Fallo al alocar la estructura principal para el PID %d", pid);
        list_destroy_and_destroy_elements(lista_instrucciones, (void(*)(void*))liberar_elementos_instruccion);
        pthread_mutex_unlock(&mutex_memoria_principal);        
        t_paquete* paquete_error = crear_paquete(PROCESO_NO_ALMACENADO);
        enviar_paquete(paquete_error, cliente_fd);
        eliminar_paquete(paquete_error);
        return;
    }

    proceso->pid = pid;
    proceso->instrucciones = lista_instrucciones;
    proceso->tam_proceso = tam_proceso;
    proceso->paginas_asignadas = 0;
    memset(&proceso->metricas, 0, sizeof(t_metricas_memoria));
    pthread_mutex_init(&proceso->mutex_proceso, NULL);
    
    proceso->tabla_nivel_1 = crear_tabla_pagina(1); // Nivel raiz
    if (proceso->tabla_nivel_1 == NULL) {
        LOG_ERROR("No se pudo crear la tabla de páginas raíz (nivel 1) para PID %d", pid);
        free(proceso);
        list_destroy_and_destroy_elements(lista_instrucciones, (void(*)(void*))liberar_elementos_instruccion);
        free(path_completo);
        pthread_mutex_unlock(&mutex_memoria_principal);
        t_paquete* paquete_error = crear_paquete(PROCESO_NO_ALMACENADO);
        enviar_paquete(paquete_error, cliente_fd);
        eliminar_paquete(paquete_error);
        return;
    }

    log_info(logger, "Proceso PID %d requiere %d paginas. Asignando marcos...", pid, paginas_requeridas);
    int paginas_a_asignar_temp = paginas_requeridas;
    
    // Esta llamada ahora tiene garantizado el éxito porque ya verificamos el espacio.
    asignar_paginas_recursivo(proceso->tabla_nivel_1, 1, &paginas_a_asignar_temp);
    proceso->paginas_asignadas = paginas_requeridas;

    // Guardamos el proceso en el diccionario global.
    pthread_mutex_lock(&mutex_diccionario_procesos);
    char* pid_str = string_itoa(pid);
    dictionary_put(diccionario_procesos, pid_str, proceso);
    free(pid_str);
    pthread_mutex_unlock(&mutex_diccionario_procesos);

    log_info(logger, "## PID: %d - Proceso Creado - Tamaño: %d", pid, tam_proceso);
    log_info(logger, "Proceso PID %d almacenado con %d instrucciones y %d paginas asignadas.", pid, list_size(lista_instrucciones), proceso->paginas_asignadas);

    pthread_mutex_unlock(&mutex_memoria_principal);

    // Enviamos confirmación al Kernel.
    t_paquete* paquete_ok = crear_paquete(INIT_PROCESO_OK);
    enviar_paquete(paquete_ok, cliente_fd);
    eliminar_paquete(paquete_ok);
   
    return;
}*/

void destruir_proceso_wrapper(void* p) {
    proceso_memoria* proceso = (proceso_memoria*)p;
    if (proceso != NULL) {
        destruir_proceso(proceso->pid); 
    }
}

void liberar_elementos_instruccion(t_instruccion* inst) {
    if (inst == NULL) return;
    free(inst->opcode);
    for (int j = 0; j < MAX_PARAMETROS; j++) {
        if (inst->parametros[j] != NULL) {
            free(inst->parametros[j]);
        }
    }
    free(inst);
}

void destruir_proceso(int pid) {
    pthread_mutex_lock(&mutex_diccionario_procesos);
    char* clave = string_itoa(pid);
    proceso_memoria* proceso = dictionary_remove(diccionario_procesos, clave);
    pthread_mutex_unlock(&mutex_diccionario_procesos);
    
    if (proceso == NULL) {
        log_warning(logger, "Intento de destruir PID %d que no fue encontrado.", pid);
        free(clave);
        return;
    }

    pthread_mutex_lock(&proceso->mutex_proceso);
    // 1. Liberar todas las tablas de paginas y marcos (recursivamente)
    log_info(logger, "## PID: %d - Liberando marcos y tablas de pagina...", pid);
    liberar_tablas_proceso(proceso->tabla_nivel_1);

    // 2. Liberar la lista de instrucciones
    list_destroy_and_destroy_elements(proceso->instrucciones, (void(*)(void*))liberar_elementos_instruccion);
    
    int total_swap = proceso->metricas.lecturas_swap + proceso->metricas.escrituras_swap;
    int total_mem_prin = proceso->metricas.lecturas_memoria + proceso->metricas.escrituras_memoria;

    pthread_mutex_unlock(&proceso->mutex_proceso);

    log_info(logger, "## PID: %d - Proceso Destruido - Métricas - Acc.T.Pag: %d; Inst.Sol.: %d; SWAP: %d; Mem.Prin.: %d; Lec.Mem.: %d; Esc.Mem.: %d",
        proceso->pid,
        proceso->metricas.accesos_tabla_paginas,
        proceso->metricas.instrucciones_solicitadas,
        total_swap,
        total_mem_prin,
        proceso->metricas.lecturas_memoria,
        proceso->metricas.escrituras_memoria
    );
    
    pthread_mutex_destroy(&proceso->mutex_proceso);
    // 3. Liberar la estructura del proceso y su clave
    free(proceso);
    
    // 4. Logs para verificar que se borraron las estructuras
    char* clave_verificacion = string_itoa(pid);
    if (!dictionary_has_key(diccionario_procesos, clave_verificacion)) {
        log_info(logger, "## PID: %d - Verificacion: Proceso eliminado correctamente del diccionario.", pid);
    }
    free(clave_verificacion);

    char* estado_bitmap = string_new();
    string_append(&estado_bitmap, "Bitmap de marcos post-liberacion: ");
    pthread_mutex_lock(&mutex_bitmap);
    for (int i = 0; i < cantidad_marcos; i++) {
        string_append_with_format(&estado_bitmap, "[%d]", bitmap_marcos[i] ? 1 : 0);
    }
    pthread_mutex_unlock(&mutex_bitmap);
    log_info(logger, "## PID: %d - Verificacion: %s", pid, estado_bitmap);
    free(estado_bitmap);
    
    free(clave);
}

bool asignar_paginas_recursivo(tabla_pagina* tabla_actual, int nivel_actual, int* paginas_restantes_a_asignar) {
    if (tabla_actual == NULL) {
        LOG_ERROR("Se intentó asignar páginas a una tabla nula.");
        return false;
    }

    for (int i = 0; i < entradas_por_tabla && *paginas_restantes_a_asignar > 0; i++) {
        entrada_tabla* entrada = &(tabla_actual->entradas[i]);
        
        if (nivel_actual < cantidad_niveles) {
            if (entrada->siguiente_nivel == NULL) {
                entrada->siguiente_nivel = crear_tabla_pagina(nivel_actual + 1);
                if (entrada->siguiente_nivel == NULL) {
                    LOG_ERROR("Fallo al crear tabla de páginas para el nivel %d.", nivel_actual + 1);
                    return false;
                }
                entrada->id_siguiente_tabla = ((tabla_pagina*)entrada->siguiente_nivel)->id_tabla;
            }
            
            if (!asignar_paginas_recursivo((tabla_pagina*)entrada->siguiente_nivel, nivel_actual + 1, paginas_restantes_a_asignar)) {
                return false;
            }
        } else { // Es el ultimo nivel, asignar un marco fisico
            if (!entrada->presente) {
                pthread_mutex_lock(&mutex_bitmap);
                int nro_marco = buscar_marco_libre();
                if (nro_marco == -1) {
                    pthread_mutex_unlock(&mutex_bitmap);
                    LOG_ERROR("Memoria llena. No se encontraron marcos libres para asignar.");
                    return false;
                }
                ocupar_marco(nro_marco);
                pthread_mutex_unlock(&mutex_bitmap);

                entrada->numero_marco = nro_marco;
                entrada->presente = true;
                (*paginas_restantes_a_asignar)--;
            }
        }
    }
    return true;
}

/*------------------------------ FUNCIONES DE PAGINACION ------------------------------------------------------*/

tabla_pagina* crear_tabla_pagina(int nivel_actual) {
    tabla_pagina* tabla = malloc(sizeof(tabla_pagina));
    if (tabla == NULL) {
        LOG_ERROR("Fallo al alocar memoria para una nueva tabla de páginas.");
        return NULL;
    }
    
    tabla->entradas = malloc(sizeof(entrada_tabla) * entradas_por_tabla);
    if (tabla->entradas == NULL) {
        LOG_ERROR("Fallo al alocar memoria para las entradas de una tabla.");
        free(tabla);
        return NULL;
    }

    pthread_mutex_lock(&mutex_diccionario_procesos);
    tabla->id_tabla = global_tabla_id_counter++;
    pthread_mutex_unlock(&mutex_diccionario_procesos);

    tabla->nivel = nivel_actual;

    for (int i = 0; i < entradas_por_tabla; i++) {
        tabla->entradas[i].presente = false;
        tabla->entradas[i].numero_marco = -1;
        tabla->entradas[i].siguiente_nivel = NULL;
        tabla->entradas[i].id_siguiente_tabla = -1;
        tabla->entradas[i].ultimo_acceso = 0;
        tabla->entradas[i].modificado = false;
        tabla->entradas[i].posicion_swap = -1; 
    }

    log_debug(logger, "Tabla de paginas de nivel %d creada con ID: %d", nivel_actual, tabla->id_tabla);
    return tabla;
}

void liberar_tablas_proceso(tabla_pagina* tabla) {
    if (tabla == NULL) return;

    for (int i = 0; i < entradas_por_tabla; i++) {
        entrada_tabla* entrada = &tabla->entradas[i];

        if (tabla->nivel < cantidad_niveles && entrada->siguiente_nivel != NULL) {
            liberar_tablas_proceso((tabla_pagina*)entrada->siguiente_nivel);
        } 
        else if (tabla->nivel == cantidad_niveles) {
            if (entrada->presente) {
                liberar_marco(entrada->numero_marco);
            }
            else if (entrada->posicion_swap != -1) {
                // Si no está presente pero sí en SWAP, liberamos el slot.
                liberar_slot_swap(entrada->posicion_swap);
            }
        }
    }   

    free(tabla->entradas);
    free(tabla);
}


int buscar_marco_libre() {
    for (int i = 0; i < cantidad_marcos; i++) {
        if (!bitmap_marcos[i]) {
            return i;
        }
    }
    return -1; 
}

void ocupar_marco(int nro_marco) {
    if (nro_marco >= 0 && nro_marco < cantidad_marcos) {
        bitmap_marcos[nro_marco] = true;
    } else {
        log_warning(logger, "Intento de ocupar marco invalido: %d", nro_marco);
    }
}

void liberar_marco(int nro_marco) {
    if (nro_marco >= 0 && nro_marco < cantidad_marcos) {
        bitmap_marcos[nro_marco] = false;
    } else {
        log_warning(logger, "Intento de liberar marco invalido: %d", nro_marco);
    }
}

int contar_marcos_libres() {
    int libres = 0;
    pthread_mutex_lock(&mutex_bitmap);
    for (int i = 0; i < cantidad_marcos; i++) {
        if (!bitmap_marcos[i]) {
            libres++;
        }
    }
    pthread_mutex_unlock(&mutex_bitmap);
    return libres;
}

/*------------------------------ FUNCIONES DE TRADUCCION DE DIRECCIONES ------------------------------------------------------*/

// CPU envia una direccion logica y Memoria responde con el marco o error
void manejar_traduccion_tabla(int socket_cpu, int pid, int direccion_logica) {
    log_info(logger, "[Memoria] Solicitud de traduccion: PID=%d, DL=%d", pid, direccion_logica);

    op_code respuesta_op;
    int marco_final = -1;

    // 1. Buscar el proceso en el diccionario
    pthread_mutex_lock(&mutex_diccionario_procesos);
    char* clave_pid = string_itoa(pid);
    proceso_memoria* proceso = dictionary_get(diccionario_procesos, clave_pid);
    pthread_mutex_unlock(&mutex_diccionario_procesos);
    free(clave_pid);

    if (proceso == NULL) {
        LOG_ERROR("Traducción fallida: PID %d no existe en memoria.", pid);
        t_paquete* paquete_error = crear_paquete(ERROR_PID_NO_ENCONTRADO);
        enviar_paquete(paquete_error, socket_cpu);
        eliminar_paquete(paquete_error);
        return;
    }


    // 2. Validar que la direccion logica este dentro de los limites del proceso (SEGFAULT check)
    int numero_pagina_logica = direccion_logica / tam_pagina;
    int total_paginas_proceso = (proceso->tam_proceso + tam_pagina - 1) / tam_pagina;

    if (numero_pagina_logica >= total_paginas_proceso || direccion_logica < 0) {
        LOG_ERROR("SEGFAULT: Dirección lógica %d fuera de los límites del proceso PID %d.", direccion_logica, pid);
        t_paquete* paquete_error = crear_paquete(PAGINA_FUERA_DE_RANGO);
        enviar_paquete(paquete_error, socket_cpu);
        eliminar_paquete(paquete_error);
        return;
    }

    // 3. Recorrer la jerarquia de tablas de pagina
    tabla_pagina* tabla_actual = proceso->tabla_nivel_1;
    entrada_tabla* entrada_actual = NULL;

    for (int nivel = 1; nivel <= cantidad_niveles; nivel++) {
        log_info(logger, "Accediendo a tabla de paginas Nivel %d para PID %d. Retardo: %d ms.", nivel, pid, retardo_memoria);
        usleep(retardo_memoria * 1000);
        int indice = obtener_indice_de_pagina(numero_pagina_logica, nivel);

        if (tabla_actual == NULL || indice < 0 || indice >= entradas_por_tabla) {
            LOG_ERROR("Fallo de traducción para PID %d: Índice %d inválido en tabla de nivel %d.", pid, indice, nivel);
            t_paquete* paquete_error = crear_paquete(ERROR_TRADUCCION);
            enviar_paquete(paquete_error, socket_cpu);
            eliminar_paquete(paquete_error);
            return;
        }

        entrada_actual = &(tabla_actual->entradas[indice]);

        if (nivel == cantidad_niveles) { // Si estamos en el ultimo nivel
            if (entrada_actual->presente) {
                marco_final = entrada_actual->numero_marco;
                log_info(logger, "[Memoria] Traduccion exitosa: PID=%d, DL=%d -> Marco=%d", pid, direccion_logica, marco_final);
                respuesta_op = TRADUCCION_OK;
            } else {
                log_info(logger, "[Memoria] Page Fault: La pagina para la DL=%d no esta presente en memoria (PID=%d).", direccion_logica, pid);
                respuesta_op = MARCO_NO_PRESENTE; // La pagina no esta en memoria (podria estar en SWAP)
            }
        } else { // Si es un nivel intermedio, avanzamos a la siguiente tabla
            tabla_actual = (tabla_pagina*)entrada_actual->siguiente_nivel;
        }
    }

    // 4. Incrementar metrica de acceso a tablas
    proceso->metricas.accesos_tabla_paginas++;

    // 5. Enviar respuesta a la CPU
    t_paquete* paquete_respuesta = crear_paquete(respuesta_op);
    if (paquete_respuesta == NULL) {
        LOG_ERROR("No se pudo crear el paquete de respuesta de traducción para PID %d.", pid);
        return;
    }
    
    agregar_a_paquete(paquete_respuesta, &pid, sizeof(int));
    agregar_a_paquete(paquete_respuesta, &direccion_logica, sizeof(int));
    agregar_a_paquete(paquete_respuesta, &marco_final, sizeof(int)); // Se envia el marco (-1 si no se encontro)
    
    enviar_paquete(paquete_respuesta, socket_cpu);
    eliminar_paquete(paquete_respuesta);
}

int obtener_indice_de_pagina(int numero_pagina, int nivel_actual) {
    // Bits a desplazar para aislar el indice del nivel actual y los niveles superiores.
    int bits_de_niveles_inferiores = (cantidad_niveles - nivel_actual) * bits_por_nivel;
    
    // Desplazamos para eliminar los bits de los niveles inferiores
    int pagina_desplazada = numero_pagina >> bits_de_niveles_inferiores;

    // Aplicamos la mascara para obtener unicamente los bits del nivel actual
    return pagina_desplazada & mascara_nivel;
}

/*------------------------------ FUNCIONES DE GESTION DE MEMORIA FISICA (LEER/ESCRIBIR) ------------------------------------------------------*/

void* leer_memoria_fisica(int direccion_fisica, int tamanio) {

	if (direccion_fisica < 0 || direccion_fisica + tamanio > tam_memoria) {
        LOG_ERROR("Intento de lectura fuera de los límites de la memoria física.");
        return NULL;
    }

	char* datos_leidos = malloc(tamanio + 1);
	if (datos_leidos == NULL) {
		LOG_ERROR("Fallo al alocar buffer para la lectura de memoria.");
		return NULL;
	}
	memcpy(datos_leidos, memoria_usuario + direccion_fisica, tamanio);
	datos_leidos[tamanio] = '\0'; 

	// log_info(logger, "Lectura de memoria fisica: Dir=%d, Tamaño=%d", direccion_fisica, tamanio);
	return datos_leidos;
}

int escribir_memoria_fisica(int direccion_fisica, void* valor, int tamanio) {
    if (direccion_fisica < 0 || direccion_fisica + tamanio > tam_memoria) {
        LOG_ERROR("Intento de escritura fuera de los límites de la memoria física.");
        return -1;
    }
    memcpy(memoria_usuario + direccion_fisica, valor, tamanio);
    // log_info(logger, "Escritura en memoria fisica: Dir=%d, Tamaño=%d", direccion_fisica, tamanio);
    return 0;
}

void manejar_lectura_direccion_fisica(int cliente_fd, int pid, int marco_fisico, int offset, int tamanio_a_leer) {
    
    char* clave_pid = string_itoa(pid);
    proceso_memoria* proceso = dictionary_get(diccionario_procesos, clave_pid);
    if(proceso) {
        proceso->metricas.lecturas_memoria++;
    }
    free(clave_pid);

    usleep(retardo_memoria * 1000);
    int direccion_fisica = (marco_fisico * tam_pagina) + offset;

    log_info(logger, "## PID: %d - Lectura - Dir. Física: %d - Tamaño: %d", pid, direccion_fisica, tamanio_a_leer);

    void* datos = leer_memoria_fisica(direccion_fisica, tamanio_a_leer);
    
    if (datos != NULL) {
		t_paquete* paquete_respuesta = crear_paquete(OPERACION_LECTURA_OK);
		if (paquete_respuesta == NULL) {
			LOG_ERROR("No se pudo crear el paquete de respuesta para la operación de lectura.");
			free(datos);
			return;
		}

		agregar_a_paquete(paquete_respuesta, datos, tamanio_a_leer);
		enviar_paquete(paquete_respuesta, cliente_fd);
		eliminar_paquete(paquete_respuesta);
        free(datos);
    } else {
		t_paquete* paquete_error = crear_paquete(ERROR_LECTURA_MEMORIA);
		if (paquete_error == NULL) {
			LOG_ERROR("No se pudo crear el paquete de error para la operación de lectura.");
			return;
		}
		enviar_paquete(paquete_error, cliente_fd);
		eliminar_paquete(paquete_error);
        LOG_ERROR("Error al leer memoria fisica. Marco: %d, Offset: %d, Tamaño: %d", marco_fisico, offset, tamanio_a_leer);
    }
}

void manejar_escritura_direccion_fisica(int cliente_fd, int pid, int marco_fisico, int offset, int tamanio_a_escribir, void* contenido_a_escribir) {
    
    char* clave_pid = string_itoa(pid);
    proceso_memoria* proceso = dictionary_get(diccionario_procesos, clave_pid);
    if(proceso) {
        proceso->metricas.escrituras_memoria++;
    }
    free(clave_pid);
    
    int direccion_fisica = (marco_fisico * tam_pagina) + offset;

    log_info(logger, "## PID: %d - Escritura - Dir. Física: %d - Tamaño: %d", pid, direccion_fisica, tamanio_a_escribir);

    usleep(retardo_memoria * 1000);
    int resultadoEscritura = escribir_memoria_fisica(direccion_fisica, contenido_a_escribir, tamanio_a_escribir);

    if (resultadoEscritura < 0) {
        LOG_ERROR("La escritura en memoria física falló.");
        
		t_paquete* paquete_error = crear_paquete(ERROR_ESCRITURA_MEMORIA);
		if (paquete_error == NULL) {
			LOG_ERROR("No se pudo crear el paquete de error para la operación de escritura.");
			return;
		}
		enviar_paquete(paquete_error, cliente_fd);
		eliminar_paquete(paquete_error);
        return;
    }


	t_paquete* paquete_respuesta = crear_paquete(OPERACION_ESCRITURA_OK);
	if (paquete_respuesta == NULL) {
		LOG_ERROR("No se pudo crear el paquete de respuesta para la operación de escritura.");
		return;
	}
	enviar_paquete(paquete_respuesta, cliente_fd);
	eliminar_paquete(paquete_respuesta);
}


/*------------------------------ FUNCIONES DE SWAP ------------------------------------------------------*/

int buscar_slot_libre_swap() {
    int i = 0;
    while(true) {
        if (i >= bitarray_get_max_bit(bitmap_swap)) {
            return i;
        }
        if (!bitarray_test_bit(bitmap_swap, i)) {
            return i;
        }
        i++;
    }
}

int escribir_en_swap(void* contenido_pagina) {
    pthread_mutex_lock(&mutex_swap);

    log_info(logger, "Accediendo a SWAP para escritura");
   // usleep(retardo_swap * 1000);

    // 1. Buscar un slot libre en el bitmap
    int slot_libre = buscar_slot_libre_swap();

    // 2. Abrir el archivo para escritura/actualización ("r+b")
    FILE* swap_file = fopen(path_swap, "r+b"); 
    if (swap_file == NULL) {
        LOG_ERROR("Error al abrir el archivo de SWAP en modo 'r+b'. Intentando crearlo.");
        swap_file = fopen(path_swap, "w+b"); // Si falla, intenta crearlo
        if (swap_file == NULL) {
            LOG_ERROR("Fallo crítico: No se pudo abrir ni crear el archivo de SWAP en %s.", path_swap);
            pthread_mutex_unlock(&mutex_swap);
            return -1;
        }
    }

    // 3. Calcular el offset y posicionarse en el archivo
    long offset = slot_libre * tam_pagina;
    fseek(swap_file, offset, SEEK_SET); 

    // 4. Escribir la página en la posición correcta
    size_t written = fwrite(contenido_pagina, 1, tam_pagina, swap_file);
    if (written != tam_pagina) {
        LOG_ERROR("Error al escribir la página completa en el slot %d de SWAP.", slot_libre);
        fclose(swap_file);
        pthread_mutex_unlock(&mutex_swap);
        return -1;
    }
    
    fclose(swap_file);

    // 5. Marcar el slot como ocupado en el bitmap
    // Si el slot está fuera del tamaño, el bitarray necesita crecer.
    if(slot_libre >= bitarray_get_max_bit(bitmap_swap)) {
        // Redimensionamos el buffer del bitmap.
        size_t new_size_bytes = (slot_libre / 8) + 1;
        bitmap_swap->bitarray = realloc(bitmap_swap->bitarray, new_size_bytes);
        bitmap_swap->size = new_size_bytes;
    }
    bitarray_set_bit(bitmap_swap, slot_libre);

    pthread_mutex_unlock(&mutex_swap);

    log_info(logger, "Pagina escrita en SWAP. Slot: %d, Offset: %ld.", slot_libre, offset);
    return slot_libre;
}

void liberar_slot_swap(int slot) {
    pthread_mutex_lock(&mutex_swap);
    if (slot >= 0 && slot < bitarray_get_max_bit(bitmap_swap)) {
        bitarray_clean_bit(bitmap_swap, slot);
        log_info(logger, "Slot de SWAP %d liberado.", slot);
    } else {
        log_warning(logger, "Intento de liberar un slot de SWAP inválido: %d", slot);
    }
    pthread_mutex_unlock(&mutex_swap);
}


void leer_de_swap(void* puntero_marco_destino, int slot_swap) {
    pthread_mutex_lock(&mutex_swap);

    log_info(logger, "Accediendo a SWAP para lectura de slot %d.", slot_swap);
   // usleep(retardo_swap * 1000);

    FILE* swap_file = fopen(path_swap, "rb");
    if (swap_file == NULL) {
        LOG_ERROR("No se pudo abrir el archivo de SWAP para lectura en %s.", path_swap);
        pthread_mutex_unlock(&mutex_swap);
        return;
    }

    long offset = slot_swap * tam_pagina;
    fseek(swap_file, offset, SEEK_SET);
    fread(puntero_marco_destino, 1, tam_pagina, swap_file);
    fclose(swap_file);

    pthread_mutex_unlock(&mutex_swap);
    log_info(logger, "Pagina leida de SWAP desde el slot %d.", slot_swap);
}

void manejar_suspension_proceso(int pid, int cliente_fd) {
    pthread_mutex_lock(&mutex_diccionario_procesos);
    char* clave_pid = string_itoa(pid);
    proceso_memoria* proceso = dictionary_get(diccionario_procesos, clave_pid);
    pthread_mutex_unlock(&mutex_diccionario_procesos);
    free(clave_pid);

    if (proceso == NULL) {
        LOG_ERROR("Se intentó suspender el PID %d, pero no fue encontrado.", pid);
        t_paquete* paquete_error = crear_paquete(ERROR_PID_NO_ENCONTRADO);
        enviar_paquete(paquete_error, cliente_fd);
        eliminar_paquete(paquete_error);
        return;
    }

    pthread_mutex_lock(&proceso->mutex_proceso);
    log_info(logger, "Iniciando suspension del proceso PID: %d", pid);
    suspender_paginas_recursivo(proceso, proceso->tabla_nivel_1, 1);
    
    log_info(logger, "Proceso PID: %d suspendido correctamente.", pid);
    pthread_mutex_unlock(&proceso->mutex_proceso);

    t_paquete* paquete_respuesta = crear_paquete(SUSPENSION_OK);
    enviar_paquete(paquete_respuesta, cliente_fd);
    eliminar_paquete(paquete_respuesta);
}


void suspender_paginas_recursivo(proceso_memoria* proceso, tabla_pagina* tabla, int nivel) {
    if (tabla == NULL) return;

    for (int i = 0; i < entradas_por_tabla; i++) {
        entrada_tabla* entrada = &tabla->entradas[i];

        if (nivel < cantidad_niveles) {
            if (entrada->siguiente_nivel != NULL) {
                suspender_paginas_recursivo(proceso, (tabla_pagina*)entrada->siguiente_nivel, nivel + 1);
            }
        } else if (entrada->presente) {
            log_info(logger, "## PID: %d - Suspendiendo Pagina. Marco: %d", proceso->pid, entrada->numero_marco);

            void* contenido_pagina = memoria_usuario + (entrada->numero_marco * tam_pagina);

            int slot_destino = escribir_en_swap(contenido_pagina);
            if (slot_destino == -1) {
                LOG_ERROR("Fallo al escribir en SWAP para el marco %d. La página no pudo ser suspendida.", entrada->numero_marco);
                continue;
            }

            entrada->presente = false;
            entrada->posicion_swap = slot_destino;

            proceso->metricas.escrituras_swap++;

            liberar_marco(entrada->numero_marco);
            log_info(logger, "## PID: %d - Marco %d liberado. Pagina ahora en SWAP (slot %d)", proceso->pid, entrada->numero_marco, slot_destino);
            entrada->numero_marco = -1;
        }
    }
}

void manejar_desuspension_proceso(int pid, int cliente_fd) {
    pthread_mutex_lock(&mutex_diccionario_procesos);
    char* clave_pid = string_itoa(pid);
    proceso_memoria* proceso = dictionary_get(diccionario_procesos, clave_pid);
    pthread_mutex_unlock(&mutex_diccionario_procesos);
    free(clave_pid);

    if (proceso == NULL) {
        LOG_ERROR("Se intentó desuspender el PID %d, pero no fue encontrado.", pid);
        t_paquete* paquete_error = crear_paquete(ERROR_PID_NO_ENCONTRADO);
        enviar_paquete(paquete_error, cliente_fd);
        eliminar_paquete(paquete_error);
        return;
    }

    //  Se cuenta cuantas paginas necesita el proceso y cuantos marcos hay libres.
    int paginas_a_cargar = contar_paginas_en_swap(proceso->tabla_nivel_1);
    int marcos_libres = 0;
    pthread_mutex_lock(&mutex_bitmap);
    for(int i = 0; i < cantidad_marcos; i++) {
        if(!bitmap_marcos[i]) {
            marcos_libres++;
        }
    }
    pthread_mutex_unlock(&mutex_bitmap);

    if (marcos_libres < paginas_a_cargar) {
        LOG_ERROR("Memoria insuficiente para desuspender PID %d. Requiere %d marcos, disponibles %d.", pid, paginas_a_cargar, marcos_libres);
        t_paquete* paquete_error = crear_paquete(ERROR_MEMORIA_INSUFICIENTE);
        enviar_paquete(paquete_error, cliente_fd);
        eliminar_paquete(paquete_error);
        return;
    }

    log_info(logger, "Iniciando de-suspension del proceso PID: %d", pid);
    desuspender_paginas_recursivo(proceso, proceso->tabla_nivel_1, 1);
    
    log_info(logger, "Proceso PID: %d de-suspendido correctamente.", pid);

    t_paquete* paquete_respuesta = crear_paquete(DESUSPENSION_OK);
    enviar_paquete(paquete_respuesta, cliente_fd);
    eliminar_paquete(paquete_respuesta);
}

void desuspender_paginas_recursivo(proceso_memoria* proceso, tabla_pagina* tabla, int nivel) {
    if (tabla == NULL) return;

    for (int i = 0; i < entradas_por_tabla; i++) {
        entrada_tabla* entrada = &tabla->entradas[i];

        if (nivel < cantidad_niveles) {
            if (entrada->siguiente_nivel != NULL) {
                desuspender_paginas_recursivo(proceso, (tabla_pagina*)entrada->siguiente_nivel, nivel + 1);
            }
        } else if (!entrada->presente && entrada->posicion_swap != -1) { // Si es el ultimo nivel y la pagina NO esta presente PERO Si esta en SWAP
            // 1. Buscar un marco libre en la memoria principal.
            int marco_destino = buscar_marco_libre();
            if (marco_destino == -1) {
                pthread_mutex_unlock(&mutex_bitmap);
                LOG_ERROR("Memoria llena. No se pudo cargar la página desde el slot de SWAP %d.", entrada->posicion_swap);
                return;
            }
            ocupar_marco(marco_destino);

            // 2. Obtener un puntero al marco destino en RAM.
            void* puntero_marco_destino = memoria_usuario + (marco_destino * tam_pagina);
            leer_de_swap(puntero_marco_destino, entrada->posicion_swap);

            int slot_a_liberar = entrada->posicion_swap; // Guardamos el slot antes de resetearlo

            entrada->presente = true;
            entrada->numero_marco = marco_destino;
            entrada->posicion_swap = -1;

            liberar_slot_swap(slot_a_liberar);

            proceso->metricas.lecturas_swap++;

            log_info(logger, "Pagina traida desde SWAP (slot %d) al marco %d.", slot_a_liberar, marco_destino);

            // 3. Leer la pagina desde SWAP y cargarla en el marco.
            /* leer_de_swap(puntero_marco_destino, entrada->posicion_swap);

            // 4. Actualizar la entrada de la tabla de paginas.
            entrada->presente = true;
            entrada->numero_marco = marco_destino;
            entrada->posicion_swap = -1; // Ya no esta en SWAP

            proceso->metricas.lecturas_swap++;

            log_info(logger, "Pagina traida desde SWAP (slot %d) al marco %d.", entrada->posicion_swap, marco_destino); */
        }
    }
}

int contar_paginas_en_swap(tabla_pagina* tabla_raiz) {
    if (tabla_raiz == NULL) return 0;
    int contador = 0;

    void contar_recursivo(tabla_pagina* tabla, int nivel, int* p_contador) {
        if (!tabla) return;
        for (int i = 0; i < entradas_por_tabla; i++) {
            entrada_tabla* entrada = &tabla->entradas[i];
            if (nivel < cantidad_niveles) {
                if(entrada->siguiente_nivel != NULL) contar_recursivo(entrada->siguiente_nivel, nivel + 1, p_contador);
            } else {
                if (!entrada->presente && entrada->posicion_swap != -1) {
                    (*p_contador)++;
                }
            }
        }
    }

    contar_recursivo(tabla_raiz, 1, &contador);
    return contador;
}

int contar_paginas_presentes(tabla_pagina* tabla_raiz) { // Cuenta cuantas paginas estan presentes en la tabla de paginas del proceso
    if (tabla_raiz == NULL) {
        return 0;
    }
    int contador = 0;
    contar_paginas_recursivo_presentes(tabla_raiz, 1, &contador);
    return contador;
}

void contar_paginas_recursivo_presentes(tabla_pagina* tabla, int nivel, int* contador) { // Funcion recursiva para contar paginas presentes en RAM
    if (tabla == NULL) return;

    for (int i = 0; i < entradas_por_tabla; i++) {
        entrada_tabla* entrada = &tabla->entradas[i];

        // Si es un nivel intermedio, llamamos recursivamente.
        if (nivel < cantidad_niveles) {
            if (entrada->siguiente_nivel != NULL) {
                contar_paginas_recursivo_presentes((tabla_pagina*)entrada->siguiente_nivel, nivel + 1, contador);
            }
        } else if (entrada->presente) { // Si es el ultimo nivel, contamos si la pagina esta presente en RAM.
            (*contador)++;
        }
    }
}

/*------------------------------ FUNCIONES DE DUMP ------------------------------------------------------*/

bool manejar_solicitud_dump(int pid) {

    log_info(logger, "## PID: %d - Memory Dump solicitado", pid);

    pthread_mutex_lock(&mutex_diccionario_procesos);
    char* clave_pid = string_itoa(pid);
    proceso_memoria* proceso = dictionary_get(diccionario_procesos, clave_pid);
    pthread_mutex_unlock(&mutex_diccionario_procesos);
    free(clave_pid);

    if (proceso == NULL) {
        LOG_ERROR("DUMP fallido: Proceso con PID %d no encontrado.", pid);
        return false;
    }

    // 1. Generar el nombre del archivo con el formato correcto
    time_t ahora = time(NULL);
    struct tm* tiempo_local = localtime(&ahora);

    // Buffer para almacenar la fecha y hora formateada (ej: "20250624-231428")
    char timestamp_formateado[20]; 

    // Formatear la fecha y hora en el formato AAAAmmdd-HHMMSS
    strftime(timestamp_formateado, sizeof(timestamp_formateado), "%Y%m%d-%H%M%S", tiempo_local);

    char* file_path = string_from_format("%s/%d-%s.dmp", dump_path, pid, timestamp_formateado);

    // 2. Crear y abrir el archivo para escritura binaria
    FILE* dump_file = fopen(file_path, "ab"); 
    if (dump_file == NULL) {
        LOG_ERROR("DUMP Fallido: No se pudo crear el archivo de dump en '%s'.", file_path);
        free(file_path);        
        return false;
    }

    // 3. Recorrer las tablas y escribir el contenido de los marcos en el archivo
    pthread_mutex_lock(&mutex_memoria_principal);
    dumpear_paginas_recursivo(dump_file, proceso->tabla_nivel_1, 1);
    pthread_mutex_unlock(&mutex_memoria_principal);

    // 4. Cerrar el archivo y liberar memoria
    fclose(dump_file);
    free(file_path);

    log_info(logger, "## Memory Dump para PID %d completado exitosamente.", pid);

    return true;
}

// Funcion recursiva para escribir los datos
void dumpear_paginas_recursivo(FILE* dump_file, tabla_pagina* tabla, int nivel) {
    if (tabla == NULL) {
        log_warning(logger, "DUMP: Se intento dumpear una tabla de nivel %d que es NULA.", nivel);
        return;
    }

    for (int i = 0; i < entradas_por_tabla; i++) {
        entrada_tabla* entrada = &tabla->entradas[i];

        if (nivel < cantidad_niveles) { // Es un nivel intermedio, solo nos interesa si apunta a otra tabla
            if (entrada->siguiente_nivel != NULL) {
                dumpear_paginas_recursivo(dump_file, (tabla_pagina*)entrada->siguiente_nivel, nivel + 1);
            }
        } else { // Es el ultimo nivel, aqui estan los datos que nos interesan
            if (entrada->presente) {
                // La pagina esta en RAM
                log_info(logger, "DUMP: Pagina PRESENTE encontrada en la entrada %d. Escribiendo marco %d...", i, entrada->numero_marco);
                void* contenido_pagina = memoria_usuario + (entrada->numero_marco * tam_pagina);
                fwrite(contenido_pagina, 1, tam_pagina, dump_file);
            } else if (entrada->posicion_swap != -1) {
                // La pagina esta en SWAP
                log_info(logger, "DUMP: Pagina en SWAP encontrada en la entrada %d. Escribiendo desde slot %d...", i, entrada->posicion_swap);
                void* buffer_temporal = malloc(tam_pagina);
                if (buffer_temporal) {
                    leer_de_swap(buffer_temporal, entrada->posicion_swap);
                    fwrite(buffer_temporal, 1, tam_pagina, dump_file);
                    free(buffer_temporal);
                }
            }
        }
    }
}


// =====================================================================================
//          FUNCIONES PARA MOSTRAR ESTADO DE LA MEMORIA (PAGINACION Y CONTENIDO)
// =====================================================================================

// Helper para mostrar un hexdump de un rango de memoria usando printf
static void _hexdump_printf(void* data, int size, int base_address) {
    if (data == NULL || size <= 0) {
        printf("      (No hay datos para mostrar o tamaño invalido)\n");
        return;
    }

    int bytes_por_linea = 16;
    for (int i = 0; i < size; i += bytes_por_linea) {
        printf("      Dir. %08X: ", base_address + i); // Direccion inicial de la linea

        // Parte hexadecimal
        for (int j = 0; j < bytes_por_linea; j++) {
            if (i + j < size) {
                printf("%02X ", ((unsigned char*)data)[i + j]);
            } else {
                printf("   "); // Rellenar con espacios si es el final
            }
        }
        printf(" "); // Separador

        // Parte ASCII
        for (int j = 0; j < bytes_por_linea; j++) {
            if (i + j < size) {
                char c = ((unsigned char*)data)[i + j];
                printf("%c", (c >= 32 && c <= 126) ? c : '.'); // Caracter imprimible o '.'
            } else {
                printf(" "); // Rellenar con espacios
            }
        }
        printf("\n");
    }
}

// Helper para mostrar el contenido de un marco de memoria especifico
static void _mostrar_contenido_marco_legible(int numero_marco) {
    if (numero_marco < 0 || numero_marco >= cantidad_marcos) {
        printf("    ERROR: Marco invalido %d para mostrar contenido.\n", numero_marco);
        return;
    }
    int direccion_fisica = numero_marco * tam_pagina;

    void* contenido_marco = leer_memoria_fisica(direccion_fisica, tam_pagina);

    if (contenido_marco == NULL) {
        printf("    ERROR: No se pudo leer el contenido del marco %d (puede estar vacio o haber un problema).\n", numero_marco);
        return;
    }

    printf("    Contenido del Marco %d (Direccion Fisica: %d - %d):\n", numero_marco, direccion_fisica, direccion_fisica + tam_pagina - 1);
    _hexdump_printf(contenido_marco, tam_pagina, direccion_fisica);
    free(contenido_marco);
}

// Helper para mostrar el estado del bitmap de marcos
static void _mostrar_bitmap_marcos_legible() {
    pthread_mutex_lock(&mutex_bitmap); // Proteger el acceso al bitmap
    if (!bitmap_marcos) {
        printf("  Bitmap de marcos no inicializado.\n");
        pthread_mutex_unlock(&mutex_bitmap);
        return;
    }
    printf("  Estado del Bitmap de Marcos (%d marcos):\n  [", cantidad_marcos);
    for (int i = 0; i < cantidad_marcos; i++) {
        printf("%d", bitmap_marcos[i] ? 1 : 0);
        if (i < cantidad_marcos - 1) {
            printf(" ");
        }
    }
    printf("]\n");
    pthread_mutex_unlock(&mutex_bitmap);
}

// Funcion recursiva para mostrar una tabla de paginas y sus subtablas
// `prefix` se usa para la indentacion en la salida por consola
static void _mostrar_tabla_pagina_recursivo(tabla_pagina* tabla, int nivel_actual, int pid, const char* prefix) {
    if (!tabla) {
        printf("%s  Tabla en Nivel %d es NULA para PID %d.\n", prefix, nivel_actual, pid);
        return;
    }

    char next_prefix[256]; // Suficiente espacio para indentacion
    snprintf(next_prefix, sizeof(next_prefix), "%s    ", prefix); // Añadir 4 espacios extra para el siguiente nivel

    printf("%sTabla Nivel %d (ID: %d) - Entradas: %d\n", prefix, nivel_actual, tabla->id_tabla, entradas_por_tabla);

    for (int i = 0; i < entradas_por_tabla; i++) {
        entrada_tabla entrada = tabla->entradas[i];
        printf("%s  Entrada %d:\n", prefix, i);

        if (nivel_actual == cantidad_niveles) { // ultimo nivel (apunta a marco fisico)
            printf("%s    Presente: %s\n", prefix, entrada.presente ? "SI" : "NO");
            if (entrada.presente) {
                printf("%s    Numero Marco: %d\n", prefix, entrada.numero_marco);
                printf("%s    Modificado: %s\n", prefix, entrada.modificado ? "SI" : "NO");
                printf("%s    Ultimo Acceso (timestamp): %d\n", prefix, entrada.ultimo_acceso);
                _mostrar_contenido_marco_legible(entrada.numero_marco); // Mostrar contenido del marco
            } else {
                printf("%s    (Pagina no presente en memoria fisica)\n", prefix);
            }
        } else { // Nivel intermedio (apunta a otra tabla de paginas)
            printf("%s    Puntero a Siguiente Tabla (ID): %d\n", prefix, entrada.id_siguiente_tabla);
            if (entrada.siguiente_nivel) {
                _mostrar_tabla_pagina_recursivo((tabla_pagina*)entrada.siguiente_nivel, nivel_actual + 1, pid, next_prefix);
            } else {
                printf("%s    Siguiente Nivel: NULO (no asignado)\n", prefix);
            }
        }
    }
}


// Funcion principal para mostrar el estado completo de la paginacion y el contenido
void mostrar_estado_paginacion() {
    printf("\n====================================================\n");
    printf("--- ESTADO ACTUAL DE LA MEMORIA (PAGINACION JERARQUICA) ---\n");
    printf("====================================================\n");

    printf("\n== Configuracion General de Memoria ==\n");
    printf("  Tamaño Total de Memoria: %d bytes\n", tam_memoria);
    printf("  Tamaño de Pagina:        %d bytes\n", tam_pagina);
    printf("  Cantidad de Marcos:      %d\n", cantidad_marcos);
    printf("  Entradas por Tabla:      %d\n", entradas_por_tabla);
    printf("  Cantidad de Niveles:     %d\n", cantidad_niveles);

    printf("\n== Estado del Bitmap de Marcos ==\n");
    _mostrar_bitmap_marcos_legible();

    printf("\n== Procesos Almacenados en Memoria ==\n");
    pthread_mutex_lock(&mutex_diccionario_procesos); // Proteger el acceso al diccionario
    if (dictionary_is_empty(diccionario_procesos)) {
        printf("  No hay procesos cargados en memoria.\n");
    } else {
        void _process_iterator_for_display(char* key, void* value) {
            proceso_memoria* proceso = (proceso_memoria*)value;
            printf("\n--- Proceso PID: %d ---\n", proceso->pid);
            printf("  Tamaño Proceso: %d bytes\n", proceso->tam_proceso);
            printf("  Paginas Asignadas: %d\n", proceso->paginas_asignadas);
            if (proceso->tabla_nivel_1) {
                printf("  Tabla de Paginas de Nivel 1 (Base para PID %d):\n", proceso->pid);
                _mostrar_tabla_pagina_recursivo(proceso->tabla_nivel_1, 1, proceso->pid, "    "); // Prefijo inicial
            } else {
                printf("  No tiene tabla de paginas de nivel 1 asignada.\n");
            }
            printf("----------------------------------\n");
        }
        dictionary_iterator(diccionario_procesos, _process_iterator_for_display);
    }
    pthread_mutex_unlock(&mutex_diccionario_procesos);

    printf("\n====================================================\n");
    printf("--- FIN ESTADO DE LA MEMORIA ---\n");
    printf("====================================================\n\n");
}