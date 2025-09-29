#include <commons/log.h>
#include <commons/config.h>
#include <commons/collections/list.h>
#include <commons/collections/queue.h>
#include <commons/string.h>

#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <semaphore.h>
#include <time.h>   
#include <math.h> 
#include <signal.h>

#include <commons/collections/list.h> 
#include <commons/log.h>   
#include <commons/config.h> 

#include "kernel.h"
#include <utils/errors.h>

int socket_dispatch, socket_interrupt, socket_escucha_io;
t_list* cola_new, *cola_ready, *cola_exec, *cola_blocked;
t_list* cola_suspended_ready, *cola_suspended_blocked;
t_list* cola_exit;
t_list *lista_cpus;

t_dictionary* dispositivos_io = NULL;

int ultimo_pid = 0;
t_log *logger;
t_config *config;

char* algoritmo_corto_plazo;
char* algoritmo_largo_plazo;

sem_t sem_procesos_ready; 
sem_t sem_cpu_libre;
sem_t sem_procesos_en_new_o_memoria_libre;
sem_t sem_proceso_desalojado_o_finalizado;
sem_t sem_procesos_en_exit;

pthread_mutex_t mutex_cpu;
pthread_mutex_t mutex_colas;
pthread_mutex_t mutex_pid;

bool cpu_conectada = false;
pthread_mutex_t mutex_cpu_conectada = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_cpu_conectada = PTHREAD_COND_INITIALIZER;
pthread_t hilo_gestion_exit;

char* ALGORITMO_CORTO_PLAZO; // "FIFO", "SJF_SIN_DESALOJO", "SJF_CON_DESALOJO"
float ESTIMACION_INICIAL; 
float ALFA;

extern t_dictionary* dispositivos_io;

static int ultimo_cpu_usado = -1;

int main(int argc, char* argv[]) {

    signal(SIGPIPE, SIG_IGN); 

    if (argc < 4) {
        fprintf(stderr, "Uso: %s [archivo_pseudocodigo] [tamanio_proceso]\n", argv[0]);
        return EXIT_FAILURE;
    }

    char* path_config = argv[1];
    config = config_create(path_config);

    char* path_pseudocodigo = argv[2];
    int tamanio_proceso = atoi(argv[3]);

    char* ip_memoria;
    char* puerto_memoria;
    char* puerto_escucha_dispatch;
    char* puerto_escucha_interrupt;
    char* puerto_escucha_io;

    inicializar_kernel();

    logger = iniciar_logger();
    iniciar_manejador_de_errores(logger);

    if (config == NULL) {
        LOG_ERROR("No fue posible iniciar el archivo de configuración.");
        terminar_programa(logger, config);
        return -1;
    }

    ip_memoria = config_get_string_value(config, "IP_MEMORIA");
    puerto_memoria = config_get_string_value(config, "PUERTO_MEMORIA");  
    puerto_escucha_dispatch = config_get_string_value(config, "PUERTO_ESCUCHA_DISPATCH");
    puerto_escucha_interrupt = config_get_string_value(config, "PUERTO_ESCUCHA_INTERRUPT");
    puerto_escucha_io = config_get_string_value(config, "PUERTO_ESCUCHA_IO");
    ESTIMACION_INICIAL = config_get_double_value(config, "ESTIMACION_INICIAL");
    ALFA = config_get_double_value(config, "ALFA");
    algoritmo_corto_plazo = config_get_string_value(config, "ALGORITMO_CORTO_PLAZO");
    algoritmo_largo_plazo = config_get_string_value(config, "ALGORITMO_INGRESO_A_READY");

    if (config_has_property(config, "ALGORITMO_CORTO_PLAZO")) {
        ALGORITMO_CORTO_PLAZO = config_get_string_value(config, "ALGORITMO_CORTO_PLAZO");
        log_info(logger, "Algoritmo de planificacion de corto plazo configurado: %s", ALGORITMO_CORTO_PLAZO);
    } else {
        LOG_ERROR("Falta la propiedad 'ALGORITMO_CORTO_PLAZO' en el config.");
        config_destroy(config);
        return EXIT_FAILURE;
    }

    if (strcmp(ALGORITMO_CORTO_PLAZO, "SJF") == 0 || strcmp(ALGORITMO_CORTO_PLAZO, "SRT") == 0) {
        if (config_has_property(config, "ESTIMACION_INICIAL") && config_has_property(config, "ALFA")) {
            ESTIMACION_INICIAL = config_get_double_value(config, "ESTIMACION_INICIAL");
            ALFA = config_get_long_value(config, "ALFA");
            if (ALFA < 0 || ALFA > 1) {
                LOG_ERROR("Valor de ALFA (%.2f) inválido. Debe estar entre 0 y 1.", ALFA);
                config_destroy(config);
                return EXIT_FAILURE;
            }
            log_info(logger, "SJF Config: Estimacion Inicial: %.2f, Alfa: %.2f", ESTIMACION_INICIAL, ALFA);
        } else {
            LOG_ERROR("Faltan propiedades 'ESTIMACION_INICIAL' o 'ALFA' para el planificador SJF/SRT.");
            config_destroy(config);
            return EXIT_FAILURE;
        }
    }

    if (!ip_memoria || !puerto_memoria || !puerto_escucha_dispatch || !puerto_escucha_interrupt || !puerto_escucha_io) {
        LOG_ERROR("Faltan valores clave en el archivo de configuración (IPs, Puertos).");
        terminar_programa(logger, config);
        return -1;
    }

	/*------------------------------ INICIO CONEXIONES DE KERNEL --------------------------------------------*/

    socket_dispatch = iniciar_servidor(puerto_escucha_dispatch);
    if (socket_dispatch == -1) {
        LOG_ERROR("No fue posible iniciar el servidor Dispatch en el puerto %s.", puerto_escucha_dispatch);
        terminar_programa(logger, config);
        return -1;
    }
    log_info(logger, "El Kernel - Dispatch esta listo para recibir peticiones");

    socket_interrupt = iniciar_servidor(puerto_escucha_interrupt);
    if (socket_interrupt == -1) {
        LOG_ERROR("No fue posible iniciar el servidor Interrupt en el puerto %s.", puerto_escucha_interrupt);
        terminar_programa(logger, config);
        return -1;
    }
    log_info(logger, "El Kernel - Interrupt esta listo para recibir peticiones");

	socket_escucha_io = iniciar_servidor(puerto_escucha_io);
    if (socket_escucha_io == -1) {
        LOG_ERROR("No fue posible iniciar el servidor de I/O en el puerto %s.", puerto_escucha_io);
        terminar_programa(logger, config);
    }
    log_info(logger, "El Kernel esta listo para recibir conexiones");

	/*------------------------------  FIN   CONEXIONES DE KERNEL --------------------------------------------*/

    /* int result_conexion_memoria = conectar_memoria(ip_memoria, puerto_memoria);
    if (result_conexion_memoria == -1) {
        log_error(logger, "No se pudo conectar con el modulo Memoria !!");
        terminar_programa(logger, config);
        return -1;
    }
    log_info(logger, "El Kernel se conecto con el modulo Memoria correctamente"); */

    crear_pcb(path_pseudocodigo, tamanio_proceso);

    pthread_t hilo_dispatch, hilo_interrupt;
    pthread_t hilo_largo_plazo, hilo_corto_plazo;

    pthread_create(&hilo_dispatch, NULL, (void*)concurrencia_cpu_dispatch, NULL);
    pthread_create(&hilo_interrupt, NULL, (void*)concurrencia_cpu_interrupt, NULL);
    pthread_create(&hilo_gestion_exit, NULL, (void*)gestionar_procesos_finalizados, NULL);

    // LARGO PLAZO
    if (strcmp(algoritmo_largo_plazo, "FIFO") == 0) {
        pthread_create(&hilo_largo_plazo, NULL, planificador_largo_plazo_fifo, NULL);
    } else if (strcmp(algoritmo_largo_plazo, "PMCP") == 0) {
        pthread_create(&hilo_largo_plazo, NULL, planificador_largo_plazo_proceso_mas_chico_primero, NULL);
    } else {
        LOG_ERROR("Algoritmo de largo plazo no reconocido: '%s'.", algoritmo_largo_plazo);
        exit(EXIT_FAILURE);
    }

    pthread_create(&hilo_corto_plazo, NULL, planificador_corto_plazo, NULL);

    pthread_detach(hilo_dispatch);
    pthread_detach(hilo_interrupt);

    pthread_detach(hilo_largo_plazo);
    pthread_detach(hilo_corto_plazo);

    concurrencia_kernel();

    terminar_programa(logger, config);
    return 0;
}

/*-----------------------DECLARACION DE FUNCIONES--------------------------------------------*/

t_log* iniciar_logger(void) {
    return log_create("kernel.log", "Kernel", true, LOG_LEVEL_INFO);
}

t_config* iniciar_config() {
    return config_create("kernel.config");
}

void inicializar_kernel() {
    cola_new = list_create();
    cola_ready = list_create();
    cola_exec = list_create();
    cola_blocked = list_create();
    cola_suspended_ready = list_create();
    cola_suspended_blocked = list_create();
    cola_exit = list_create();

    lista_cpus = list_create();
    dispositivos_io = dictionary_create();

    sem_init(&sem_procesos_ready, 0, 0);
    sem_init(&sem_cpu_libre, 0, 0);
    sem_init(&sem_procesos_en_new_o_memoria_libre, 0, 0);
    sem_init(&sem_procesos_en_exit, 0, 0);

    pthread_mutex_init(&mutex_colas, NULL);
    pthread_mutex_init(&mutex_cpu, NULL);
    pthread_mutex_init(&mutex_pid, NULL);
}

t_pcb* crear_pcb(char* path, int tamanio) {
    t_pcb* pcb = calloc(1, sizeof(t_pcb));

    char* path_absoluto = realpath(path, NULL);
    
    pcb->pid = generar_pid();
    pcb->program_counter = 0;
    pcb->tamanio_proceso = tamanio;
    // pcb->path_pseudocodigo = strdup(path);
    //cambiar_estado(pcb, STATE_NEW);
    pcb->estado = STATE_NEW;
    pcb->metricas.ingresos_a_new = 1;
    pcb->estimacion_rafaga = ESTIMACION_INICIAL;
    pcb->timestamp_ultimo_cambio = get_timestamp_ms();

    if (path_absoluto == NULL) {
        // Si falla, usamos el path original y logueamos un warning.
        // log_warning(logger, "No se pudo resolver la ruta absoluta para: %s. Usando ruta original.", path);
        pcb->path_pseudocodigo = strdup(path);
    } else {
        // Si funciona, guardamos la ruta absoluta.
        pcb->path_pseudocodigo = strdup(path_absoluto);
        
        // MUY IMPORTANTE: Liberar la memoria que reservó realpath().
        free(path_absoluto);
    }

    log_info(logger, "## (%d) Se crea el proceso Estado: NEW", pcb->pid);

    /* pthread_mutex_lock(&mutex_colas);
    list_add(cola_new, pcb);
    sem_post(&sem_procesos_en_new_o_memoria_libre);
    pthread_mutex_unlock(&mutex_colas);
    */
    pthread_mutex_lock(&mutex_colas);
    bool la_cola_estaba_vacia = list_is_empty(cola_new);
    list_add(cola_new, pcb);
    if (la_cola_estaba_vacia) {
        // Solo notifica al planificador si la cola estaba vacía
        sem_post(&sem_procesos_en_new_o_memoria_libre);
    }
    pthread_mutex_unlock(&mutex_colas);

    return pcb;
}

int generar_pid() {
    pthread_mutex_lock(&mutex_pid);
    int pid = ultimo_pid++;
    pthread_mutex_unlock(&mutex_pid);
    return pid;
}

void cerrar_sockets() {
    close(socket_dispatch);
    close(socket_interrupt);
    close(socket_escucha_io);
}

void destruir_pcb(void* data) {
    t_pcb* pcb = (t_pcb*)data;
    if (pcb == NULL) return;
    free(pcb->path_pseudocodigo);
    free(pcb);
}

// Función para liberar la memoria de una estructura t_cpu
void destruir_cpu(void* data) {
    t_cpu* cpu = (t_cpu*)data;
    if (cpu == NULL) return;
    free(cpu->nombre);
    pthread_mutex_destroy(&cpu->mutex);
    free(cpu);
}

// Función para liberar la memoria de una instancia t_io
void destruir_io_instancia(void* data) {
    t_io* instancia = (t_io*)data;
    if (instancia == NULL) return;
    free(instancia->nombre);
    pthread_mutex_destroy(&instancia->mutex);
    pthread_cond_destroy(&instancia->cond);
    // Asumimos que la cola de pendientes se maneja en otro lado o es NULL
    free(instancia);
}

// Función para liberar la memoria de un grupo de I/O (t_grupo_io) y todo su contenido
void destruir_io_grupo(void* data) {
    t_grupo_io* grupo = (t_grupo_io*)data;
    if (grupo == NULL) return;
    
    free(grupo->nombre_dispositivo);
    
    // Destruye la lista de instancias, llamando a destruir_io_instancia para cada una
    list_destroy_and_destroy_elements(grupo->instancias, destruir_io_instancia);
    
    // Destruye la cola de espera, liberando cada solicitud (que era un simple malloc)
    queue_destroy_and_destroy_elements(grupo->cola_espera_compartida, free);
    
    pthread_mutex_destroy(&grupo->mutex_grupo);
    free(grupo);
}

void terminar_programa(t_log* logger, t_config* config) {
    log_info(logger, "Iniciando la finalizacion del Kernel...");

    if (lista_cpus != NULL) {
        list_destroy_and_destroy_elements(lista_cpus, destruir_cpu);
    }

    if (dispositivos_io != NULL) {
        dictionary_destroy_and_destroy_elements(dispositivos_io, destruir_io_grupo);
    }
    
    if (cola_new != NULL) list_destroy_and_destroy_elements(cola_new, destruir_pcb);
    if (cola_ready != NULL) list_destroy_and_destroy_elements(cola_ready, destruir_pcb);
    if (cola_exec != NULL) list_destroy_and_destroy_elements(cola_exec, destruir_pcb);
    if (cola_blocked != NULL) list_destroy_and_destroy_elements(cola_blocked, destruir_pcb);
    if (cola_suspended_ready != NULL) list_destroy_and_destroy_elements(cola_suspended_ready, destruir_pcb);
    if (cola_suspended_blocked != NULL) list_destroy_and_destroy_elements(cola_suspended_blocked, destruir_pcb);
    if (cola_exit != NULL) list_destroy_and_destroy_elements(cola_exit, destruir_pcb);

    log_destroy(logger);
    config_destroy(config);
    cerrar_sockets();
}

void concurrencia_kernel() {
    while (1) {
        pthread_t hilo_cliente;
        int* nuevo_cliente = malloc(sizeof(int));
        *nuevo_cliente = esperar_cliente(socket_escucha_io);
        pthread_create(&hilo_cliente, NULL, atender_cliente, nuevo_cliente);
        pthread_detach(hilo_cliente); 
    }
}

void* atender_cliente(void* arg) {  // Clientes de kernel = > IO y CPU (dispatch & interrupt)
    int cliente_fd = *(int*)arg;
    free(arg);

    handshake_kernel_con_mensaje(cliente_fd);
    return NULL;
}


/* int conectar_memoria(char *ip, char *puerto) {
    socket_memoria = crear_conexion(ip, puerto);
    if(socket_memoria == -1) {
		return -1;
	}

    t_paquete* handshake = crear_paquete(HANDSHAKE_MEMORIA_KERNEL);
    enviar_paquete(handshake, socket_memoria);
    eliminar_paquete(handshake);

    t_paquete* respuesta_memoria = recibir_paquete(socket_memoria);
    // validaciones

    op_code codigo_respuesta = respuesta_memoria->codigo_operacion;
    eliminar_paquete(respuesta_memoria);
    if (codigo_respuesta == -1) {
        log_error(logger, "No se recibio respuesta del Handshake con Memoria");
		return -1;
    }

    log_info(logger, "Respuesta de Memoria: %s", op_code_a_string(codigo_respuesta));
    
    if(codigo_respuesta != HANDSHAKE_OK){
        printf("Error en handshake con memoria\n");
        return -1;
    }
    return 0;
} */


void handshake_kernel_con_mensaje(int socket_cliente) {
    log_info(logger, "Realizando handshake con el cliente (fd=%d)", socket_cliente);

    t_paquete* paquete = recibir_paquete(socket_cliente);
    if (paquete == NULL || paquete->buffer->size == 0) {
        LOG_ERROR("Fallo al recibir el mensaje de handshake del cliente (FD: %d).", socket_cliente);
        close(socket_cliente);
        return;
    }

    op_code cod_op_recibido = paquete->codigo_operacion;
    op_code respuesta = HANDSHAKE_OK;
   
    switch (cod_op_recibido) {
        case HANDSHAKE_CPU_DISPATCH: {

            char* mensaje = malloc(paquete->buffer->size + 1); 
            memcpy(mensaje, paquete->buffer->stream, paquete->buffer->size);
            mensaje[paquete->buffer->size] = '\0';
            eliminar_paquete(paquete);

            t_cpu* nueva_cpu = malloc(sizeof(t_cpu));
            nueva_cpu->nombre = strdup(mensaje);
            nueva_cpu->socket_dispatch = socket_cliente;
            nueva_cpu->socket_interrupt = -1;
            nueva_cpu->cpu_libre = true;
            sem_post(&sem_cpu_libre);
            pthread_mutex_init(&nueva_cpu->mutex, NULL);

            list_add(lista_cpus, nueva_cpu);

            log_info(logger, "CPU '%s' conectada (Dispatch FD: %d)", nueva_cpu->nombre, socket_cliente);
           
            t_paquete* respuesta_cpu = crear_paquete(respuesta);
            enviar_paquete(respuesta_cpu, socket_cliente);
            eliminar_paquete(respuesta_cpu);
            free(mensaje);

            break;
        }
        case HANDSHAKE_CPU_INTERRUPT: {

            char* mensaje = malloc(paquete->buffer->size + 1); 
            memcpy(mensaje, paquete->buffer->stream, paquete->buffer->size);
            mensaje[paquete->buffer->size] = '\0'; 
            eliminar_paquete(paquete);
            printf("Mensaje recibido de la CPU (Interrupt): %s\n", mensaje);


            bool misma_cpu(void* elem) {
                return strcmp(((t_cpu*)elem)->nombre, mensaje) == 0;
            }
    
            t_cpu* cpu = list_find(lista_cpus, misma_cpu);
            if (cpu != NULL) {
                cpu->socket_interrupt = socket_cliente;
                log_info(logger, "CPU '%s' conectada (Interrupt FD: %d)", cpu->nombre, socket_cliente);
            }
    
            t_paquete* respuesta_cpu = crear_paquete(respuesta);
            enviar_paquete(respuesta_cpu, socket_cliente);
            eliminar_paquete(respuesta_cpu);
            free(mensaje);

            pthread_mutex_lock(&mutex_cpu_conectada);
            cpu_conectada = true;
            pthread_cond_signal(&cond_cpu_conectada);  // señal para mostrar mensaje de getChar en Planif. LP
            pthread_mutex_unlock(&mutex_cpu_conectada);
            break;
        }
        case HANDSHAKE_KERNEL_IO: {

            char* nombre_io_recibido = malloc(paquete->buffer->size);
            memcpy(nombre_io_recibido, paquete->buffer->stream, paquete->buffer->size);
            eliminar_paquete(paquete);

            log_info(logger, "Se conecto un dispositivo de I/O con nombre: '%s'", nombre_io_recibido);

            t_io* nueva_instancia_io = malloc(sizeof(t_io));
            nueva_instancia_io->nombre = strdup(nombre_io_recibido);
            nueva_instancia_io->socket_fd = socket_cliente;
            nueva_instancia_io->ocupado = false;
            nueva_instancia_io->cola_pendientes = NULL; 
            pthread_mutex_init(&nueva_instancia_io->mutex, NULL);
            pthread_cond_init(&nueva_instancia_io->cond, NULL);

            pthread_mutex_lock(&mutex_colas);
            t_grupo_io* grupo = dictionary_get(dispositivos_io, nombre_io_recibido);

            if (grupo == NULL) {
                log_info(logger, "Creando nuevo grupo de I/O para '%s'", nombre_io_recibido);
                grupo = malloc(sizeof(t_grupo_io));
                grupo->nombre_dispositivo = strdup(nombre_io_recibido);
                grupo->instancias = list_create();
                grupo->cola_espera_compartida = queue_create();
                pthread_mutex_init(&grupo->mutex_grupo, NULL);
                grupo->ultimo_despacho_idx = -1;
                
                dictionary_put(dispositivos_io, nombre_io_recibido, grupo);
            }

            list_add(grupo->instancias, nueva_instancia_io);
            log_info(logger, "Nueva instancia de '%s' agregada. Total de instancias: %d", nombre_io_recibido, list_size(grupo->instancias));
            
            pthread_mutex_unlock(&mutex_colas);

            pthread_t hilo_listener;
            pthread_create(&hilo_listener, NULL, hilo_escucha_io, (void*)nueva_instancia_io);
            pthread_detach(hilo_listener);

            log_info(logger, "Dispositivo IO '%s' conectado (FD: %d)", nueva_instancia_io->nombre, socket_cliente);
            
            t_paquete* respuesta_cpu = crear_paquete(respuesta);
            enviar_paquete(respuesta_cpu, socket_cliente);
            eliminar_paquete(respuesta_cpu);

            free(nombre_io_recibido);

            pthread_mutex_lock(&grupo->mutex_grupo);
            despachar_io_si_es_posible(grupo);
            pthread_mutex_unlock(&grupo->mutex_grupo);

            break;
        }
        default: {
            log_warning(logger, "Codigo de operacion desconocido: %d", cod_op_recibido);
            break;
        }
    }
}

void concurrencia_cpu_dispatch() {
    while (1) {
        int* nuevo_fd = malloc(sizeof(int));
        *nuevo_fd = esperar_cliente(socket_dispatch);
        log_info(logger, "Nueva CPU (Dispatch) conectada - fd=%d", *nuevo_fd);

        pthread_t hilo_dispatch;
        pthread_create(&hilo_dispatch, NULL, (void*)atender_conexion_cpu, nuevo_fd);
        pthread_detach(hilo_dispatch);
    }
}

void concurrencia_cpu_interrupt() {
    while (1) {
        int* nuevo_fd = malloc(sizeof(int));
        *nuevo_fd = esperar_cliente(socket_interrupt);
        log_info(logger, "Nueva CPU (Interrupt) conectada - fd=%d", *nuevo_fd);

        pthread_t hilo_interrupt;
        pthread_create(&hilo_interrupt, NULL, atender_cliente, nuevo_fd);
        pthread_detach(hilo_interrupt);
    }
}

void* atender_conexion_cpu(void* arg) {
    int cliente_fd = *(int*)arg;
    free(arg);

    // 1. Recibir handshake para identificar la CPU
    t_paquete* paquete = recibir_paquete(cliente_fd);
    if (!paquete) {
        LOG_ERROR("Fallo al recibir handshake de CPU (FD: %d).", cliente_fd);
        close(cliente_fd);
        return NULL;
    }

    char* nombre_cpu = malloc(paquete->buffer->size);
    memcpy(nombre_cpu, paquete->buffer->stream, paquete->buffer->size);
    eliminar_paquete(paquete);

    // 2. Crear o encontrar la estructura de la CPU
    pthread_mutex_lock(&mutex_cpu);
    t_cpu* cpu = buscar_cpu_por_nombre(nombre_cpu);
    if (!cpu) {
        cpu = malloc(sizeof(t_cpu));
        cpu->nombre = strdup(nombre_cpu);
        cpu->socket_interrupt = -1;
        pthread_mutex_init(&cpu->mutex, NULL);
        list_add(lista_cpus, cpu);
    }
    cpu->socket_dispatch = cliente_fd;
    cpu->cpu_libre = true;
    cpu->pcb_en_ejecucion = NULL;
    pthread_mutex_unlock(&mutex_cpu);

    log_info(logger, "CPU '%s' conectada en socket Dispatch %d", cpu->nombre, cliente_fd);

    sem_post(&sem_cpu_libre); // Notifica al planificador que una CPU esta disponible.

    // 3. Responder al handshake
    t_paquete* respuesta_hs = crear_paquete(HANDSHAKE_OK);
    enviar_paquete(respuesta_hs, cliente_fd);
    eliminar_paquete(respuesta_hs);

    // 4. Lanzar el hilo que escuchará las respuestas de esta CPU para siempre
    pthread_t hilo_listener_respuestas;
    pthread_create(&hilo_listener_respuestas, NULL, (void*)atender_respuestas_cpu, (void*)cpu);
    pthread_detach(hilo_listener_respuestas);

    free(nombre_cpu);
    return NULL;
}

//Hilo dedicado a escuchar las respuestas de una sola CPU
void* atender_respuestas_cpu(void* arg) {
    t_cpu* cpu = (t_cpu*)arg;
    log_info(logger, "-> Hilo listener para CPU '%s' (Dispatch FD: %d) iniciado.", cpu->nombre, cpu->socket_dispatch);

    while (1) {
        recibir_y_manejar_respuesta_cpu(cpu);
    }

    return NULL;
}

void* planificador_largo_plazo_fifo() {
    pthread_mutex_lock(&mutex_cpu_conectada);
    while (!cpu_conectada) {
        log_info(logger, "FIFO: Esperando conexion de al menos una CPU antes de iniciar planificacion...");
        pthread_cond_wait(&cond_cpu_conectada, &mutex_cpu_conectada);
    }
    pthread_mutex_unlock(&mutex_cpu_conectada);

    printf("\nCPU conectada. Presione Enter para iniciar la planificacion de largo plazo...\n");
    getchar();

    log_info(logger, "Iniciando planificador de largo plazo con algoritmo: FIFO");

    while (1) {
        sem_wait(&sem_procesos_en_new_o_memoria_libre);

        // Bucle interno para procesar mientras haya candidatos y espacio
        while(1) {
            pthread_mutex_lock(&mutex_colas);

            t_pcb* proceso_a_admitir = NULL;
            bool viene_de_susp = false;

            // 1. Damos prioridad a la cola de SUSPENDED_READY
            if (!list_is_empty(cola_suspended_ready)) {
                proceso_a_admitir = (t_pcb*)list_get(cola_suspended_ready, 0);
                viene_de_susp = true;
            }
            // 2. Si no hay nadie, vamos a la cola NEW
            else if (!list_is_empty(cola_new)) {
                proceso_a_admitir = (t_pcb*)list_get(cola_new, 0);
            }

            // Si no hay ningún proceso para admitir, salimos del bucle interno
            // y esperamos una nueva señal en el semáforo principal.
            if (proceso_a_admitir == NULL) {
                pthread_mutex_unlock(&mutex_colas);
                break; // No hay más candidatos, a esperar.
            }

            // IMPORTANTE: Liberamos el mutex antes de la llamada bloqueante a memoria
            pthread_mutex_unlock(&mutex_colas);
            
            bool admision_ok = solicitar_admision_a_memoria(proceso_a_admitir, viene_de_susp);
            
            // Volvemos a tomar el mutex para modificar las colas de forma segura
            pthread_mutex_lock(&mutex_colas);
            
            if (admision_ok) {
                if(viene_de_susp) {
                    list_remove(cola_suspended_ready, 0);
                } else {
                    list_remove(cola_new, 0);
                }
                
                list_add(cola_ready, proceso_a_admitir);
                cambiar_estado(proceso_a_admitir, STATE_READY);
                sem_post(&sem_procesos_ready);
                
                desalojar_si_es_necesario_srt(proceso_a_admitir);
            } else {
                // Si la admisión falló (por falta de memoria), salimos del bucle interno
                // y no volveremos a intentar hasta que se libere memoria.
                pthread_mutex_unlock(&mutex_colas);
                break; // Falló la admisión, a esperar.
            }
            pthread_mutex_unlock(&mutex_colas);
        }
    }
    return NULL;
}

void* planificador_largo_plazo_proceso_mas_chico_primero() {
    pthread_mutex_lock(&mutex_cpu_conectada);
    while (!cpu_conectada) {
        log_info(logger, "PMCP: Esperando conexion de al menos una CPU antes de iniciar planificacion...");
        pthread_cond_wait(&cond_cpu_conectada, &mutex_cpu_conectada);
    }
    pthread_mutex_unlock(&mutex_cpu_conectada);

    printf("\nCPU conectada. Presione Enter para iniciar la planificacion de largo plazo (Proceso más chico primero)...\n");
    getchar();

    log_info(logger, "Iniciando planificador de largo plazo con algoritmo: PMCP");

    while (1) {
        sem_wait(&sem_procesos_en_new_o_memoria_libre);

        while (1) {
            pthread_mutex_lock(&mutex_colas);

            t_pcb* proceso_a_admitir = NULL;
            bool viene_de_susp = false;
            bool se_pudo_admitir_algo = false;

            if (!list_is_empty(cola_suspended_ready)) {
                list_sort(cola_suspended_ready, comparar_por_memoria);
                proceso_a_admitir = (t_pcb*)list_get(cola_suspended_ready, 0);
                viene_de_susp = true;
            } 
            else if (!list_is_empty(cola_new)) {
                list_sort(cola_new, comparar_por_memoria);
                proceso_a_admitir = (t_pcb*)list_get(cola_new, 0);
            }

            if (proceso_a_admitir == NULL) {
                pthread_mutex_unlock(&mutex_colas);
                break;
            }
            
            // Hacemos una copia del puntero antes de liberar el mutex
            t_pcb* pcb_candidato = proceso_a_admitir;
            pthread_mutex_unlock(&mutex_colas);

            bool admision_ok = solicitar_admision_a_memoria(pcb_candidato, viene_de_susp);

            pthread_mutex_lock(&mutex_colas);

            if (admision_ok) {
                if (viene_de_susp) {
                    list_remove(cola_suspended_ready, 0);
                } else {
                    list_remove(cola_new, 0);
                }
                
                list_add(cola_ready, pcb_candidato);
                cambiar_estado(pcb_candidato, STATE_READY);
                sem_post(&sem_procesos_ready);
                se_pudo_admitir_algo = true;

                desalojar_si_es_necesario_srt(pcb_candidato);
            }

            pthread_mutex_unlock(&mutex_colas);
            if (!se_pudo_admitir_algo) {
                break;
            }
        }
    }
    return NULL;
}


void desalojar_si_es_necesario_srt(t_pcb* pcb_recien_llegado) {
    // 1. Si el algoritmo no es SRT, no hacemos nada.
    if (strcmp(ALGORITMO_CORTO_PLAZO, "SRT") != 0) {
        return;
    }

    pthread_mutex_lock(&mutex_cpu); // Protegemos la lista de CPUs

    t_pcb* pcb_a_desalojar = NULL;
    t_cpu* cpu_victima = NULL;
    double max_tiempo_restante = -1.0; // Usamos -1 para asegurarnos que cualquier tiempo restante positivo sea mayor

    long long tiempo_actual = get_timestamp_ms();

    // 2. Iteramos por todas las CPUs para encontrar la mejor víctima para desalojar.
    for (int i = 0; i < list_size(lista_cpus); i++) {
        t_cpu* cpu_actual = list_get(lista_cpus, i);
        pthread_mutex_lock(&cpu_actual->mutex);

        // Solo nos interesan las CPUs que están ocupadas.
        if (!cpu_actual->cpu_libre && cpu_actual->pcb_en_ejecucion != NULL) {
            t_pcb* pcb_en_ejecucion = cpu_actual->pcb_en_ejecucion;

            // 3. Calculamos el tiempo restante del proceso en ejecución.
            long long tiempo_ejecutado = tiempo_actual - pcb_en_ejecucion->tiempo_inicio_ejecucion_cpu;
            double tiempo_restante = pcb_en_ejecucion->estimacion_rafaga - tiempo_ejecutado;
            
            if (tiempo_restante < 0) tiempo_restante = 0; // El tiempo restante no puede ser negativo

            // 4. Comparamos: ¿La ráfaga del nuevo proceso es más corta que lo que le queda al que está en CPU?
            if (pcb_recien_llegado->estimacion_rafaga < tiempo_restante) {
                
                // Si es un candidato, elegimos al que tenga el MAYOR tiempo restante.
                // Así maximizamos el beneficio del desalojo.
                if (tiempo_restante > max_tiempo_restante) {
                    max_tiempo_restante = tiempo_restante;
                    pcb_a_desalojar = pcb_en_ejecucion;
                    cpu_victima = cpu_actual;
                }
            }
        }
        pthread_mutex_unlock(&cpu_actual->mutex);
    }

    // 5. Si encontramos una víctima, le mandamos la interrupción.
    if (pcb_a_desalojar != NULL) {
        log_info(logger, "## (%d) Desalojado por algoritmo SJF/SRT", pcb_a_desalojar->pid);
        enviar_interrupcion_a_cpu(cpu_victima);
    }

    pthread_mutex_unlock(&mutex_cpu); // Liberamos la lista de CPUs
}


/*bool comparar_por_memoria(void* pcb1, void* pcb2) {
    t_pcb* proceso1 = (t_pcb*)pcb1;
    t_pcb* proceso2 = (t_pcb*)pcb2; 
    return proceso1->tamanio_proceso < proceso2->tamanio_proceso;
}*/
bool comparar_por_memoria(void* pcb1, void* pcb2) {
    t_pcb* proceso1 = (t_pcb*)pcb1;
    t_pcb* proceso2 = (t_pcb*)pcb2;

    if (proceso1->tamanio_proceso < proceso2->tamanio_proceso) {
        return true; // Proceso1 es más pequeño, por lo tanto, debe ir antes.
    }
    // Si proceso1->tamanio_proceso >= proceso2->tamanio_proceso,
    // no hay razón para que proceso1 vaya antes de proceso2.
    // Esto incluye el caso de igualdad, manteniendo el orden relativo.
    return false;
}


/*----------- PLANIFICADOR CORTO PLAZO ----------- */

void* planificador_corto_plazo() {
    log_info(logger, "Planificador de Corto Plazo iniciado con algoritmo: %s", ALGORITMO_CORTO_PLAZO);

    while (1) {
        sem_wait(&sem_procesos_ready); // Espera a que haya AL MENOS un proceso en READY
        sem_wait(&sem_cpu_libre);      // Espera a que haya AL MENOS una CPU libre

        pthread_mutex_lock(&mutex_colas);

        // Ahora que sabemos que hay al menos un candidato y una CPU, tomamos la decisión.
        t_pcb* pcb_a_despachar = NULL;

        if (!list_is_empty(cola_ready)) { // Verificamos que la cola no esté vacía (no debería pasar, pero es seguro)
            
            if (strcmp(ALGORITMO_CORTO_PLAZO, "FIFO") == 0) {
                pcb_a_despachar = list_remove(cola_ready, 0);

            } else if (strcmp(ALGORITMO_CORTO_PLAZO, "SJF") == 0 || strcmp(ALGORITMO_CORTO_PLAZO, "SRT") == 0) {
                // Ordenamos TODA la cola de listos para encontrar al mejor candidato
                list_sort(cola_ready, (void*)comparar_por_rafaga_estimada);
                
                // (Opcional) Log para depurar y ver la cola ordenada
                for(int i = 0; i < list_size(cola_ready); i++) {
                    t_pcb* pcb = list_get(cola_ready, i);
                    log_info(logger, "PCB en cola READY: PID %d, Estimacion %.2fms", pcb->pid, pcb->estimacion_rafaga);
                }

                // Sacamos al más corto (el que quedó al principio de la lista)
                pcb_a_despachar = list_remove(cola_ready, 0);
            }
        }

        // Si por alguna razón no se eligió a nadie, devolvemos los "tickets" a los semáforos
        if (pcb_a_despachar == NULL) {
            sem_post(&sem_procesos_ready);
            sem_post(&sem_cpu_libre);
            pthread_mutex_unlock(&mutex_colas);
            continue; // Volvemos a empezar el ciclo
        }
        
        // Buscamos una CPU libre y despachamos
        t_cpu* cpu_libre = obtener_cpu_libre();
        if (cpu_libre != NULL) {
            list_add(cola_exec, pcb_a_despachar);
            cambiar_estado(pcb_a_despachar, STATE_RUNNING);
            cpu_libre->pcb_en_ejecucion = pcb_a_despachar;

            log_info(logger, "PID %d -> EXEC. Despachando a CPU '%s' (Estimacion: %.2fms)", pcb_a_despachar->pid, cpu_libre->nombre, pcb_a_despachar->estimacion_rafaga);
            enviar_pcb_a_cpu(cpu_libre, pcb_a_despachar);

        } else {
            // No debería pasar nunca que no haya CPU si el sem_wait funcionó, pero por seguridad:
            list_add_in_index(cola_ready, 0, pcb_a_despachar); // Lo devolvemos al principio
            sem_post(&sem_procesos_ready);
        }

        pthread_mutex_unlock(&mutex_colas);
    }
    return NULL;
}

t_cpu* obtener_cpu_libre() {
    pthread_mutex_lock(&mutex_cpu);
    
    // Hacemos hasta dos vueltas completas para asegurar que revisamos todas las CPUs
    for (int i = 0; i < list_size(lista_cpus) * 2; i++) {
        
        // Calculamos el índice para que la búsqueda sea circular (round-robin)
        int indice_actual = (ultimo_cpu_usado + 1 + i) % list_size(lista_cpus);
        
        t_cpu* cpu = list_get(lista_cpus, indice_actual);
        
        pthread_mutex_lock(&cpu->mutex);
        if (cpu->cpu_libre) {
            cpu->cpu_libre = false;
            ultimo_cpu_usado = indice_actual; // Guardamos el índice de la CPU que vamos a usar
            
            pthread_mutex_unlock(&cpu->mutex);
            pthread_mutex_unlock(&mutex_cpu);
            return cpu;
        }
        pthread_mutex_unlock(&cpu->mutex);
    }
    
    // Si después de revisar todo no encontramos ninguna libre (no debería pasar si el semáforo está bien)
    pthread_mutex_unlock(&mutex_cpu);
    return NULL;
}

t_cpu* buscar_cpu_por_nombre(char* nombre) {
    for(int i = 0; i < list_size(lista_cpus); i++) {
        t_cpu* cpu = list_get(lista_cpus, i);
        if (strcmp(cpu->nombre, nombre) == 0) {
            return cpu;
        }
    }
    return NULL;
}

void enviar_pcb_a_cpu(t_cpu* cpu, t_pcb* pcb) {
    enviar_pcb_por_socket(cpu->socket_dispatch, pcb);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    pcb->tiempo_inicio_ejecucion_cpu = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void enviar_pcb_por_socket(int socket_dispatch, t_pcb* pcb) {
    t_paquete* paquete = crear_paquete(OP_PCB);
    agregar_a_paquete(paquete, &(pcb->pid), sizeof(int));
    agregar_a_paquete(paquete, &(pcb->program_counter), sizeof(int));
    agregar_a_paquete(paquete, &(pcb->tamanio_proceso), sizeof(int));
    agregar_a_paquete(paquete, &(pcb->estado), sizeof(op_code_estado));
    int tamanio_path = strlen(pcb->path_pseudocodigo) + 1;
    agregar_a_paquete(paquete, &tamanio_path, sizeof(int));
    agregar_a_paquete(paquete, pcb->path_pseudocodigo, strlen(pcb->path_pseudocodigo) + 1);
    agregar_a_paquete(paquete, &(pcb->estimacion_rafaga), sizeof(double));
    agregar_a_paquete(paquete, &(pcb->rafaga_real_anterior), sizeof(double));

    enviar_paquete(paquete, socket_dispatch);
    eliminar_paquete(paquete);
    log_info(logger, "Se envio el PCB del proceso <%d> a la CPU. PC: %d, Estimacion Ráfaga: %.2f", pcb->pid, pcb->program_counter, pcb->estimacion_rafaga);
}


void recibir_y_manejar_respuesta_cpu(t_cpu* cpu) { 

    t_paquete* paquete_recibido = recibir_paquete(cpu->socket_dispatch);
    if (!paquete_recibido) {
        LOG_ERROR("Error al recibir paquete de CPU '%s'", cpu->nombre);
        sem_trywait(&sem_cpu_libre);

        //La removemos de la lista global de CPUs para que el planificador no la seleccione más.
        pthread_mutex_lock(&mutex_cpu);
        list_remove_element(lista_cpus, cpu);
        pthread_mutex_unlock(&mutex_cpu);

        pthread_mutex_lock(&mutex_colas);
        if (cpu->pcb_en_ejecucion != NULL) {
            log_warning(logger, "Finalizando PID %d por desconexion de CPU '%s'.", cpu->pcb_en_ejecucion->pid, cpu->nombre);
            list_remove_element(cola_exec, cpu->pcb_en_ejecucion);
            finalizar_proceso(cpu->pcb_en_ejecucion, "CPU_DISCONNECTED");
            cpu->pcb_en_ejecucion = NULL;
        }
        pthread_mutex_unlock(&mutex_colas);

        pthread_mutex_lock(&cpu->mutex);
        cpu->cpu_libre = false;
        cpu->socket_dispatch = -1; // Marcar el socket como inválido
        pthread_mutex_unlock(&cpu->mutex);

        // NO HACER sem_post(&sem_cpu_libre);

        pthread_exit(NULL);
    }

    op_code codigo_operacion = paquete_recibido->codigo_operacion;
    void* buffer_recibido = paquete_recibido->buffer->stream;
    
    pthread_mutex_lock(&cpu->mutex);
    t_pcb* pcb_actual_en_cpu = cpu->pcb_en_ejecucion;
    pthread_mutex_unlock(&cpu->mutex);

    if(codigo_operacion != OP_RESPUESTA_EJECUCION) {
        log_info(logger, "## (%d) Solicitó syscall: %s", pcb_actual_en_cpu ? pcb_actual_en_cpu->pid : -1, op_code_a_string(codigo_operacion));
    }

    switch (codigo_operacion) {
        case INIT_PROC: {

            int longitud_path, tamanio_nuevo_proceso;
            int temp_offset = 0; 
            memcpy(&longitud_path, buffer_recibido + temp_offset, sizeof(int));
            temp_offset += sizeof(int);

            char* path_pseudocodigo = malloc(longitud_path + 1);
            memcpy(path_pseudocodigo, buffer_recibido + temp_offset, longitud_path);
            path_pseudocodigo[longitud_path] = '\0';
            temp_offset += longitud_path;
            memcpy(&tamanio_nuevo_proceso, buffer_recibido + temp_offset, sizeof(int));
            crear_pcb(path_pseudocodigo, tamanio_nuevo_proceso);
            free(path_pseudocodigo);

            t_paquete* respuesta_ack = crear_paquete(INIT_PROCESO_OK);
            enviar_paquete(respuesta_ack, cpu->socket_dispatch);
            eliminar_paquete(respuesta_ack);
            break;
        }

        case OP_RESPUESTA_EJECUCION: {
            log_info(logger, "## Respuesta final de ejecucion recibida de CPU '%s'.", cpu->nombre);

            int pid_del_proceso = pcb_actual_en_cpu->pid;
            
            t_respuesta_ejecucion* respuesta = deserializar_respuesta_cpu(buffer_recibido);
            if (!pcb_actual_en_cpu || pcb_actual_en_cpu->pid != respuesta->pid) {
                LOG_ERROR("Error CRÍTICO de sincronización: CPU '%s' devolvió PID %d, se esperaba %d.", cpu->nombre, respuesta->pid, pcb_actual_en_cpu ? pcb_actual_en_cpu->pid : -1);
                
                if (pcb_actual_en_cpu) {
                    list_remove_element(cola_exec, pcb_actual_en_cpu);
                    finalizar_proceso(pcb_actual_en_cpu, "PID_SYNC_ERROR");
                    cpu->pcb_en_ejecucion = NULL;
                }
                destruir_respuesta_ejecucion(respuesta);
                
                eliminar_paquete(paquete_recibido);
                
                pthread_mutex_lock(&cpu->mutex); 
                cpu->cpu_libre = true;
                pthread_mutex_unlock(&cpu->mutex);
                sem_post(&sem_cpu_libre);
                return;
            }

            long long tiempo_actual_ms = get_timestamp_ms();
            long long tiempo_real_ejecutado = tiempo_actual_ms - pcb_actual_en_cpu->tiempo_inicio_ejecucion_cpu;

            // 2. SOLO si la salida no fue por interrupción, actualizamos la ráfaga y la estimación
            if (respuesta->motivo != PROCESO_INTERRUPCION) {
                pcb_actual_en_cpu->rafaga_real_anterior = tiempo_real_ejecutado;
                
                // El cálculo de la siguiente ráfaga ahora solo se hace aquí
                if (strcmp(ALGORITMO_CORTO_PLAZO, "SJF") == 0 || strcmp(ALGORITMO_CORTO_PLAZO, "SRT") == 0) {
                    calcular_siguiente_rafaga(pcb_actual_en_cpu);
                }
            }
            // 3. Si fue por interrupción, debemos actualizar la estimación restante
            else {
                // Le restamos a la estimación lo que ya ejecutó
                pcb_actual_en_cpu->estimacion_rafaga -= tiempo_real_ejecutado;
                if (pcb_actual_en_cpu->estimacion_rafaga < 0) {
                    pcb_actual_en_cpu->estimacion_rafaga = 0; // Evitar estimaciones negativas
                }
            }
                        
            pcb_actual_en_cpu->program_counter = respuesta->program_counter;

            list_remove_element(cola_exec, pcb_actual_en_cpu);

            switch (respuesta->motivo) {
                case EXIT:
                    log_info(logger, "Finaliza el proceso <%d> - Motivo: EXIT", respuesta->pid);
                    list_add(cola_exit, pcb_actual_en_cpu);
                    cambiar_estado(pcb_actual_en_cpu, STATE_EXIT);
                    sem_post(&sem_procesos_en_exit); // Para que el planificador de largo plazo lo recoja
                    pcb_actual_en_cpu = NULL;
                    break;
                
                case PROCESO_BLOQUEANTE_IO:
                    log_info(logger, "Proceso <%d> bloqueado por I/O: %s - Tiempo: %d ms", respuesta->pid, respuesta->nombre_dispositivo_io, respuesta->duracion_bloqueo);
                    bloquear_proceso(pcb_actual_en_cpu, respuesta->nombre_dispositivo_io, respuesta->duracion_bloqueo); // Bloquea y manda a dispositivo I/O
                    break;

                case PROCESO_BLOQUEANTE_DUMP_MEMORY:
                    log_info(logger, "Proceso <%d> bloqueado por DUMP MEMORY ", respuesta->pid);
                    bloquear_proceso_por_dump(pcb_actual_en_cpu);
                    
                    pthread_t tid_dump;
                    pthread_create(&tid_dump, NULL, hilo_manejador_dump, (void*)pcb_actual_en_cpu);
                    pthread_detach(tid_dump);
                    break;

                case PROCESO_INTERRUPCION: // Por desalojo de SRT, por ejemplo
                    log_info(logger, "## (%d) Desalojado por algoritmo SJF/SRT", pcb_actual_en_cpu->pid);
                    list_add(cola_ready, pcb_actual_en_cpu); 
                    cambiar_estado(pcb_actual_en_cpu, STATE_READY);
                    sem_post(&sem_procesos_ready);
                    desalojar_si_es_necesario_srt(pcb_actual_en_cpu);
                    break;
                    
                case ERROR_SEG_FAULT:
                    LOG_ERROR("Proceso %d finalizado por SEGMENTATION FAULT (reportado por CPU).", respuesta->pid);                 
                    finalizar_proceso(pcb_actual_en_cpu, "SEGMENTATION_FAULT");
                    break;

                default:
                    log_warning(logger, "Motivo de finalizacion desconocido: %d. Enviando PID %d a EXIT.", respuesta->motivo, respuesta->pid);
                    finalizar_proceso(pcb_actual_en_cpu, "UNKNOWN_REASON"); 
                    break;
            }
            destruir_respuesta_ejecucion(respuesta);
            
            log_info(logger, "Ráfaga de ejecucion para PID %d finalizada en CPU '%s'. Liberando CPU.", pid_del_proceso, cpu->nombre);pthread_mutex_lock(&cpu->mutex);
            cpu->pcb_en_ejecucion = NULL;
            cpu->cpu_libre = true;
            pthread_mutex_unlock(&cpu->mutex);
            sem_post(&sem_cpu_libre); 
            break;
        }

        default: {
            LOG_ERROR("Operación desconocida (%d) recibida de la CPU '%s'.", codigo_operacion, cpu->nombre);

            if (pcb_actual_en_cpu) {
                list_remove_element(cola_exec, pcb_actual_en_cpu);
                finalizar_proceso(pcb_actual_en_cpu, "UNKNOWN_OPCODE");
            }

            pthread_mutex_lock(&cpu->mutex);
            cpu->pcb_en_ejecucion = NULL;
            cpu->cpu_libre = true;
            pthread_mutex_unlock(&cpu->mutex);
            sem_post(&sem_cpu_libre);
            break;
        }
    }

    eliminar_paquete(paquete_recibido);
}


t_io* buscar_io(char* nombre) {
    return dictionary_get(dispositivos_io, nombre);
}

t_respuesta_ejecucion* deserializar_respuesta_cpu(void* stream) {
    t_respuesta_ejecucion* respuesta = malloc(sizeof(t_respuesta_ejecucion));
    int offset = 0;

    memcpy(&(respuesta->pid), stream + offset, sizeof(int));
    offset += sizeof(int);

    memcpy(&(respuesta->program_counter), stream + offset, sizeof(int));
    offset += sizeof(int);

    memcpy(&(respuesta->motivo), stream + offset, sizeof(op_code));
    offset += sizeof(op_code);

    int nombre_dispositivo_len;
    memcpy(&nombre_dispositivo_len, stream + offset, sizeof(int));
    offset += sizeof(int);

    if (respuesta->motivo == PROCESO_BLOQUEANTE_IO) {
        respuesta->nombre_dispositivo_io = malloc(nombre_dispositivo_len); 
        memcpy(respuesta->nombre_dispositivo_io, stream + offset, nombre_dispositivo_len);
        offset += nombre_dispositivo_len;
    } else {
        respuesta->nombre_dispositivo_io = NULL; 
    }

    memcpy(&(respuesta->duracion_bloqueo), stream + offset, sizeof(int));
    offset += sizeof(int);

    return respuesta;
}


bool solicitar_memoria(t_pcb* pcb) {
    char* ip_memoria = config_get_string_value(config, "IP_MEMORIA");
    char* puerto_memoria = config_get_string_value(config, "PUERTO_MEMORIA");

    printf("Solicitando memoria para el proceso %d\n", pcb->pid);

    // 1. Crear una conexion temporal solo para esta operacion
    int socket_memoria_temporal = crear_conexion(ip_memoria, puerto_memoria);
    if (socket_memoria_temporal == -1) {
        LOG_ERROR("PID %d: No se pudo conectar a Memoria para solicitar espacio.", pcb->pid);
        return false;
    }

    // 2. Realizar Handshake en la nueva conexion
    t_paquete* handshake = crear_paquete(HANDSHAKE_MEMORIA_KERNEL);
    enviar_paquete(handshake, socket_memoria_temporal);
    eliminar_paquete(handshake);

    t_paquete* respuesta_hs = recibir_paquete(socket_memoria_temporal);
    if (respuesta_hs == NULL || respuesta_hs->codigo_operacion != HANDSHAKE_OK) {
        LOG_ERROR("Fallo el handshake con Memoria en conexion temporal. Codigo de operacion recibido: %d", respuesta_hs ? respuesta_hs->codigo_operacion : -1);
        if(respuesta_hs) eliminar_paquete(respuesta_hs);
        close(socket_memoria_temporal);
        return false;
    }
    eliminar_paquete(respuesta_hs);

    // 3. Enviar la peticion de INIT_PROC (tu logica original, corregida)
    t_paquete* paquete = crear_paquete(INIT_PROC);
    agregar_a_paquete(paquete, &pcb->pid, sizeof(int));
    agregar_a_paquete(paquete, &pcb->tamanio_proceso, sizeof(int));
    int largo_path = strlen(pcb->path_pseudocodigo) + 1;
    agregar_a_paquete(paquete, &largo_path, sizeof(int));
    agregar_a_paquete(paquete, pcb->path_pseudocodigo, largo_path);
    
    enviar_paquete(paquete, socket_memoria_temporal);
    eliminar_paquete(paquete);

    // 4. Recibir la respuesta de la operacion
    t_paquete* respuesta = recibir_paquete(socket_memoria_temporal);

    // 5. Cerrar la conexion
    close(socket_memoria_temporal);

    // 6. Procesar el resultado
    if (respuesta == NULL) {
        printf("Error: no se pudo recibir la respuesta de Memoria\n");
        return false;
    }

    op_code codigo_respuesta = respuesta->codigo_operacion;
    eliminar_paquete(respuesta);

    printf("codigo operacion recibido: %s (%d)\n", op_code_a_string(codigo_respuesta), codigo_respuesta);
    if (codigo_respuesta != INIT_PROCESO_OK) {
        printf("Error al solicitar memoria: codigo de operacion %d\n", codigo_respuesta);
        return false;
    }

    return true;
}

void finalizar_proceso(t_pcb* pcb, char* motivo) {
    if (pcb == NULL) {
        log_warning(logger, "Se intento finalizar un PCB NULL.");
        return;
    }

    cambiar_estado(pcb, STATE_EXIT);  // 1. Actualizar el estado del PCB a EXIT

    pthread_mutex_lock(&mutex_colas);
    list_add(cola_exit, pcb); // 2. Mover el PCB a la cola de EXIT
    pthread_mutex_unlock(&mutex_colas);
    log_info(logger, "Proceso %d movido a EXIT. Razon: %s", pcb->pid, motivo);

    log_info(logger, "Iniciando finalizacion de Proceso PID: %d. Solicitando liberacion de memoria.", pcb->pid);

    sem_post(&sem_procesos_en_exit); //Manda señal al hilo q finaliza procesos y maneja cola_exit
}

char* nombre_syscall(op_code codigo_operacion) {
    switch (codigo_operacion) {
        case DUMP_MEMORY: return "DUMP MEMORY";
        case EXIT: return "EXIT";
        case IO: return "IO";
        case INIT_PROC: return "INIT_PROC";
        default: return "Desconocido";
    }
}

void bloquear_proceso(t_pcb* pcb, char* nombre_dispositivo, int tiempo) {
    char* nombre_dispositivo_copia = strdup(nombre_dispositivo);
    bool debe_finalizar = false;
    t_grupo_io* grupo = NULL;

    pthread_mutex_lock(&mutex_colas);

    list_remove_element(cola_exec, pcb);

    grupo = dictionary_get(dispositivos_io, nombre_dispositivo_copia);

    if (!grupo) {
        LOG_ERROR("Dispositivo de I/O '%s' solicitado por PID %d no existe. Finalizando proceso.", nombre_dispositivo_copia, pcb->pid);       
        debe_finalizar = true;
    } else {
        // Si el dispositivo es válido, lo movemos a la cola de bloqueados
        pcb->tiempo_bloqueo_io = tiempo;
        cambiar_estado(pcb, STATE_BLOCKED);
        list_add(cola_blocked, pcb);
        int* pid_para_hilo = malloc(sizeof(int));
        *pid_para_hilo = pcb->pid;
        pthread_t hilo_suspension;
        pthread_create(&hilo_suspension, NULL, hilo_suspension_proceso, pid_para_hilo);
        pthread_detach(hilo_suspension);
        log_info(logger, "## (<%d>) Bloqueado por IO: %s", pcb->pid, nombre_dispositivo_copia);
    }
    pthread_mutex_unlock(&mutex_colas);

    if(debe_finalizar){
        list_remove_element(cola_exec, pcb); // Asumimos que viene de EXEC
        finalizar_proceso(pcb, "INVALID_IO_DEVICE");        
        free(nombre_dispositivo_copia);
        return; 
    }

    pthread_mutex_lock(&grupo->mutex_grupo);
    
    t_solicitud_io* solicitud = malloc(sizeof(t_solicitud_io));
    solicitud->pid = pcb->pid;
    solicitud->tiempo = tiempo;
    solicitud->nombre_dispositivo = nombre_dispositivo_copia;
    queue_push(grupo->cola_espera_compartida, solicitud);

    despachar_io_si_es_posible(grupo);

    pthread_mutex_unlock(&grupo->mutex_grupo);

    //int* pid_para_hilo = malloc(sizeof(int));
    //*pid_para_hilo = pcb->pid;

    //pthread_t hilo_suspension;
    //pthread_create(&hilo_suspension, NULL, hilo_suspension_proceso, pid_para_hilo);
    //pthread_detach(hilo_suspension);

    //free(nombre_dispositivo_copia);
}

void bloquear_proceso_por_dump(t_pcb* pcb) {
    pthread_mutex_lock(&mutex_colas);
    list_remove_element(cola_exec, pcb);
    list_add(cola_blocked, pcb);
    cambiar_estado(pcb, STATE_BLOCKED);
    pthread_mutex_unlock(&mutex_colas);
}

void* hilo_manejador_dump(void* arg) {
    t_pcb* pcb = (t_pcb*)arg;
    if (pcb == NULL) {
        pthread_exit(NULL);
    }

    char* ip_memoria = config_get_string_value(config, "IP_MEMORIA");
    char* puerto_memoria = config_get_string_value(config, "PUERTO_MEMORIA");

    // 1. Conectar a Memoria
    int socket_memoria_temporal = crear_conexion(ip_memoria, puerto_memoria);
    if (socket_memoria_temporal == -1) {
        LOG_ERROR("DUMP para PID %d falló: no se pudo conectar con Memoria.", pcb->pid);
        finalizar_proceso(pcb, "DUMP_MEMORY_FAILED_CONNECTION");
        pthread_exit(NULL);
    }

    // 2. Realizar Handshake (CORRECCIÓN CLAVE)
    t_paquete* handshake = crear_paquete(HANDSHAKE_MEMORIA_KERNEL);
    enviar_paquete(handshake, socket_memoria_temporal);
    eliminar_paquete(handshake);
    
    t_paquete* respuesta_hs = recibir_paquete(socket_memoria_temporal);
    if (respuesta_hs == NULL || respuesta_hs->codigo_operacion != HANDSHAKE_OK) {
        LOG_ERROR("DUMP para PID %d falló: error en el handshake con Memoria.", pcb->pid);
        if(respuesta_hs) eliminar_paquete(respuesta_hs);
        close(socket_memoria_temporal);
        finalizar_proceso(pcb, "DUMP_MEMORY_FAILED_HANDSHAKE");
        pthread_exit(NULL);
    }
    eliminar_paquete(respuesta_hs);

    // 3. Enviar solicitud de DUMP y esperar confirmacion
    log_info(logger, "Solicitud de DUMP para PID <%d> enviada. Esperando confirmacion de Memoria...", pcb->pid);
    t_paquete* paquete_dump = crear_paquete(DUMP_MEMORY);
    agregar_a_paquete(paquete_dump, &(pcb->pid), sizeof(int));
    enviar_paquete(paquete_dump, socket_memoria_temporal);
    eliminar_paquete(paquete_dump);
    
    t_paquete* respuesta_memoria = recibir_paquete(socket_memoria_temporal);

    // 4. Cerrar conexion
    close(socket_memoria_temporal);

    // 5. Procesar la respuesta y el estado del proceso
    bool dump_exitoso = (respuesta_memoria != NULL && respuesta_memoria->codigo_operacion == DUMP_OK);
    
    if (dump_exitoso) {
        log_info(logger, "Memoria confirmo DUMP_OK para PID <%d>.", pcb->pid);
    } else {
        LOG_ERROR("DUMP para PID %d falló: error de comunicación con Memoria.", pcb->pid);
    }
    if(respuesta_memoria) eliminar_paquete(respuesta_memoria);

    pthread_mutex_lock(&mutex_colas);
    t_pcb* pcb_actualizado = buscar_pcb_por_pid(pcb->pid);
    if (pcb_actualizado) {
        if (dump_exitoso) {
            if (pcb_actualizado->estado == STATE_BLOCKED) {
                // El dump termino ANTES de que el proceso se suspendiera
                list_remove_element(cola_blocked, pcb_actualizado);
                list_add(cola_ready, pcb_actualizado);
                cambiar_estado(pcb_actualizado, STATE_READY);
                log_info(logger, "## (%d) DUMP finalizado. Pasa de BLOCKED a READY.", pcb_actualizado->pid);
                sem_post(&sem_procesos_ready);
            } else if (pcb_actualizado->estado == STATE_SUSPENDED_BLOCKED) {
                // El proceso FUE suspendido mientras el dump se realizaba
                list_remove_element(cola_suspended_blocked, pcb_actualizado);
                list_add(cola_suspended_ready, pcb_actualizado);
                cambiar_estado(pcb_actualizado, STATE_SUSPENDED_READY);
                log_info(logger, "## (%d) DUMP finalizado. Pasa de SUSPENDED_BLOCKED a SUSPENDED_READY.", pcb_actualizado->pid);
                sem_post(&sem_procesos_en_new_o_memoria_libre);
            }
        } else {
            // Si el dump fallo, lo mandamos a EXIT sin importar su estado
            if(pcb_actualizado->estado == STATE_BLOCKED) list_remove_element(cola_blocked, pcb_actualizado);
            if(pcb_actualizado->estado == STATE_SUSPENDED_BLOCKED) list_remove_element(cola_suspended_blocked, pcb_actualizado);
            finalizar_proceso(pcb_actualizado, "DUMP_MEMORY_FAILED");
        }
    }
    pthread_mutex_unlock(&mutex_colas);

    pthread_exit(NULL);
}

void manejar_mensaje_io(int socket_origen) {
    t_paquete* paquete = recibir_paquete(socket_origen); 
    if (!paquete) return;

    int offset = 0;
    int len_nombre;
    memcpy(&len_nombre, paquete->buffer->stream + offset, sizeof(int));
    offset += sizeof(int);

    char* nombre_io = malloc(len_nombre + 1);
    memcpy(nombre_io, paquete->buffer->stream + offset, len_nombre);
    nombre_io[len_nombre] = '\0';
    offset += len_nombre;

    int pid;
    memcpy(&pid, paquete->buffer->stream + offset, sizeof(int));
    offset += sizeof(int);

    int tipo_mensaje;
    memcpy(&tipo_mensaje, paquete->buffer->stream + offset, sizeof(int));

    eliminar_paquete(paquete);

    t_io* io = dictionary_get(dispositivos_io, nombre_io);
    if (!io) {
        log_warning(logger, "Mensaje de IO <%s> recibido pero no registrado. Ignorando.", nombre_io);
        free(nombre_io);
        return;
    }

    switch (tipo_mensaje) {
        case FINALIZO_IO:
            log_info(logger, "IO <%s> finalizo ejecucion del proceso <%d>", nombre_io, pid);
            manejar_fin_io(pid, socket_origen);
            break;

        case DESCONECTADO_IO:
            log_warning(logger, "IO <%s> se ha desconectado mientras procesaba PID <%d>", nombre_io, pid);
            pthread_mutex_lock(&io->mutex);
            pthread_mutex_lock(&mutex_colas);
            t_pcb* pcb = buscar_pcb_por_pid(pid);
            if (pcb) {
                pthread_mutex_lock(&mutex_colas);
                list_remove_element(cola_blocked, pcb);
                cambiar_estado(pcb, STATE_EXIT);
                pthread_mutex_unlock(&mutex_colas);
                finalizar_proceso(pcb, "IO_DESCONECTADA");
            }
            pthread_mutex_unlock(&mutex_colas);
            io->ocupado = false;
            pthread_mutex_unlock(&io->mutex);
            break;

        default:
            log_warning(logger, "Tipo de mensaje IO no reconocido: %d. Ignorando.", tipo_mensaje);
            break;
    }

    free(nombre_io);
}


// Se encarga de la limpieza segura tras la desconexion de un dispositivo de I/O.
void manejar_desconexion_io(t_io* instancia_muerta) {
    if (instancia_muerta == NULL) return;

    log_warning(logger, "Se perdió la conexión con I/O '%s' (Socket: %d). Iniciando limpieza...", instancia_muerta->nombre, instancia_muerta->socket_fd);

    t_pcb* pcb_en_ejecucion = NULL;
    t_grupo_io* grupo_a_destruir = NULL;
    t_list* pcbs_para_finalizar = list_create();

    pthread_mutex_lock(&mutex_colas);
    t_grupo_io* grupo = dictionary_get(dispositivos_io, instancia_muerta->nombre);

    if (grupo) {
        pthread_mutex_lock(&grupo->mutex_grupo);

        if (instancia_muerta->ocupado) {
            pcb_en_ejecucion = buscar_pcb_por_pid(instancia_muerta->pid_en_ejecucion);
            if (pcb_en_ejecucion != NULL) {
                t_solicitud_io* solicitud_recuperada = malloc(sizeof(t_solicitud_io));
                solicitud_recuperada->pid = pcb_en_ejecucion->pid;
                solicitud_recuperada->tiempo = pcb_en_ejecucion->tiempo_bloqueo_io;
                solicitud_recuperada->nombre_dispositivo = strdup(grupo->nombre_dispositivo);
                list_add_in_index(grupo->cola_espera_compartida->elements, 0, solicitud_recuperada);
                log_warning(logger, "## (<%d>) fue re-encolado a '%s' debido a desconexión de I/O.", pcb_en_ejecucion->pid, grupo->nombre_dispositivo);
            }
        }

        list_remove_element(grupo->instancias, instancia_muerta);
        log_info(logger, "Instancia de I/O (Socket: %d) eliminada. Instancias restantes de '%s': %d", instancia_muerta->socket_fd, grupo->nombre_dispositivo, list_size(grupo->instancias));

        if (list_is_empty(grupo->instancias)) {
            // --- ESTA ES LA PARTE CLAVE ---
            log_warning(logger, "Era la ultima instancia de '%s'. Finalizando %d procesos en cola de espera.",
                        grupo->nombre_dispositivo, queue_size(grupo->cola_espera_compartida));
            
            // Este `while` se asegura de vaciar TODA la cola
            while (!queue_is_empty(grupo->cola_espera_compartida)) {
                t_solicitud_io* solicitud = queue_pop(grupo->cola_espera_compartida);
                t_pcb* pcb_a_finalizar = buscar_pcb_por_pid(solicitud->pid);
                if (pcb_a_finalizar) {
                    if (pcb_a_finalizar->estado == STATE_BLOCKED) list_remove_element(cola_blocked, pcb_a_finalizar);
                    if (pcb_a_finalizar->estado == STATE_SUSPENDED_BLOCKED) list_remove_element(cola_suspended_blocked, pcb_a_finalizar);
                    list_add(pcbs_para_finalizar, pcb_a_finalizar);
                }
                free(solicitud->nombre_dispositivo);
                free(solicitud);
            }
            grupo_a_destruir = dictionary_remove(dispositivos_io, grupo->nombre_dispositivo);
        } else {
            despachar_io_si_es_posible(grupo);
        }

        pthread_mutex_unlock(&grupo->mutex_grupo);
    }
    pthread_mutex_unlock(&mutex_colas);

    for (int i = 0; i < list_size(pcbs_para_finalizar); i++) {
        t_pcb* pcb_a_finalizar = list_get(pcbs_para_finalizar, i);
        finalizar_proceso(pcb_a_finalizar, "IO_DEVICE_DISCONNECTED");
    }
    list_destroy(pcbs_para_finalizar);

    destruir_io_instancia(instancia_muerta); 
    if (grupo_a_destruir != NULL) {
        destruir_io_grupo(grupo_a_destruir);
    }
}

void* hilo_escucha_io(void* arg) {
    t_io* instancia_io = (t_io*)arg;
    int socket_dispositivo = instancia_io->socket_fd;
    log_info(logger, "-> Hilo listener iniciado para el dispositivo '%s' (Socket: %d)", instancia_io->nombre, socket_dispositivo);

    while(1) {
        t_paquete* paquete = recibir_paquete(socket_dispositivo);

        if (paquete == NULL) {
            // manejo la desconexion con otro hilo, aca solo la detecta
            manejar_desconexion_io(instancia_io);
            break;
        }

        /* if (paquete == NULL) {
            LOG_ERROR("Se perdió la conexión con el dispositivo de I/O '%s' (Socket: %d).", instancia_io->nombre, socket_dispositivo);
            
            t_list* pcbs_a_finalizar = list_create();

            t_grupo_io* grupo_a_destruir = NULL;
        
            // 1. INICIAMOS LA ÚNICA SECCIÓN CRÍTICA
            pthread_mutex_lock(&mutex_colas);
            t_grupo_io* grupo = dictionary_get(dispositivos_io, instancia_io->nombre);
        
            if (grupo) {
                pthread_mutex_lock(&grupo->mutex_grupo);
        
                // 2. Recolectamos el PCB que estaba corriendo y lo quitamos de su cola.
                if (instancia_io->ocupado) {
                    t_pcb* pcb_ejecutando = buscar_pcb_por_pid(instancia_io->pid_en_ejecucion);
                    if (pcb_ejecutando) {
                        list_remove_element(cola_blocked, pcb_ejecutando);
                        list_remove_element(cola_suspended_blocked, pcb_ejecutando);
                        list_add(pcbs_a_finalizar, pcb_ejecutando);
                    }
                }
        
                // 3. Eliminamos la instancia de la lista del grupo.
                bool fue_removido = list_remove_element(grupo->instancias, instancia_io);
                if(fue_removido) {
                    log_info(logger, "Instancia de I/O (Socket: %d) eliminada de la lista del grupo. Instancias restantes de '%s': %d",
                            socket_dispositivo, grupo->nombre_dispositivo, list_size(grupo->instancias));
                }
        
                // 4. Si era la última, recolectamos y quitamos a TODOS los que esperaban.
                if (list_is_empty(grupo->instancias)) {
                    log_warning(logger, "Era la ultima instancia de '%s'. Marcando para finalizar a %d procesos en cola.", 
                                grupo->nombre_dispositivo, queue_size(grupo->cola_espera_compartida));
                    
                    while (!queue_is_empty(grupo->cola_espera_compartida)) {
                        t_solicitud_io* solicitud = queue_pop(grupo->cola_espera_compartida);
                        t_pcb* pcb_en_espera = buscar_pcb_por_pid(solicitud->pid);
                        if (pcb_en_espera) {
                            list_remove_element(cola_blocked, pcb_en_espera);
                            list_remove_element(cola_suspended_blocked, pcb_en_espera);
                            list_add(pcbs_a_finalizar, pcb_en_espera);
                        }
                        free(solicitud->nombre_dispositivo);
                        free(solicitud);
                    }

                    grupo_a_destruir = dictionary_remove(dispositivos_io, grupo->nombre_dispositivo);
                }
                
                pthread_mutex_unlock(&grupo->mutex_grupo);
            }
            
            // 5. FINALIZAMOS LA SECCIÓN CRÍTICA
            pthread_mutex_unlock(&mutex_colas);
        
            // 6. Ahora, fuera de los locks, mandamos a finalizar los procesos que recolectamos.
            for (int i = 0; i < list_size(pcbs_a_finalizar); i++) {
                t_pcb* pcb = list_get(pcbs_a_finalizar, i);
                log_warning(logger, "## (<%d>) Finalizado por desconexion de su dispositivo de I/O.", pcb->pid);
                finalizar_proceso(pcb, "IO_DEVICE_DISCONNECTED");
            }
            
            list_destroy(pcbs_a_finalizar);

            // 7. FINALMENTE, LIBERAMOS LOS RECURSOS DE LA INSTANCIA
            if (grupo_a_destruir != NULL) {
                destruir_io_grupo(grupo_a_destruir);
            }

            log_info(logger, "Liberando recursos para la instancia de I/O '%s' (Socket: %d).", instancia_io->nombre, socket_dispositivo);
            close(socket_dispositivo); 
            free(instancia_io->nombre);   
            free(instancia_io);    
            
            break; // Salimos del bucle del hilo
        } */

        int pid_finalizado;
        int offset = 0, len_nombre, tipo_mensaje;

        memcpy(&len_nombre, paquete->buffer->stream + offset, sizeof(int));
        offset += sizeof(int) + len_nombre;
        memcpy(&pid_finalizado, paquete->buffer->stream + offset, sizeof(int));
        offset += sizeof(int);
        memcpy(&tipo_mensaje, paquete->buffer->stream + offset, sizeof(int));
        
        if(tipo_mensaje == FINALIZO_IO) {
            log_info(logger, "## Recibido fin de I/O del dispositivo '%s' para el PID <%d>", instancia_io->nombre, pid_finalizado);
            manejar_fin_io(pid_finalizado, socket_dispositivo);
        }

        eliminar_paquete(paquete);
    }
    return NULL;
}


void manejar_fin_io(int pid_proceso_finalizado, int socket_origen) {
    t_grupo_io* grupo_encontrado = NULL;
    t_io* instancia_encontrada = NULL;
    bool fue_encontrado = false;

    // 1. Bloquear el mutex general para acceder al diccionario de forma segura
    pthread_mutex_lock(&mutex_colas);

    // 2. Buscar el grupo y la instancia MIENTRAS se tiene el lock general
    t_list* nombres_de_grupos = dictionary_keys(dispositivos_io);
    for (int i = 0; i < list_size(nombres_de_grupos) && !fue_encontrado; i++) {
        char* nombre_grupo = list_get(nombres_de_grupos, i);
        t_grupo_io* grupo_actual = dictionary_get(dispositivos_io, nombre_grupo);
        if(grupo_actual == NULL) continue;

        for (int j = 0; j < list_size(grupo_actual->instancias) && !fue_encontrado; j++) {
            t_io* instancia_actual = list_get(grupo_actual->instancias, j);
            if (instancia_actual->socket_fd == socket_origen) {
                grupo_encontrado = grupo_actual;
                instancia_encontrada = instancia_actual;
                fue_encontrado = true;
            }
        }
    }
    list_destroy(nombres_de_grupos);

    // 3. Si encontramos el grupo, ahora bloqueamos su mutex específico
    if (fue_encontrado) {
        pthread_mutex_lock(&grupo_encontrado->mutex_grupo); // <-- LOCK 2 (Específico)

        instancia_encontrada->ocupado = false;
        log_info(logger, "Instancia de IO '%s' (Socket: %d) ha quedado libre.", grupo_encontrado->nombre_dispositivo, instancia_encontrada->socket_fd);
        
        t_pcb* pcb = buscar_pcb_por_pid(pid_proceso_finalizado); 
        if (pcb) {
            if (pcb->estado == STATE_BLOCKED) {
                list_remove_element(cola_blocked, pcb);
                list_add(cola_ready, pcb);
                cambiar_estado(pcb, STATE_READY);
                log_info(logger, "## (<%d>) finalizo IO y pasa a READY", pcb->pid);
                sem_post(&sem_procesos_ready);
                desalojar_si_es_necesario_srt(pcb); 
            } else if (pcb->estado == STATE_SUSPENDED_BLOCKED) {
                list_remove_element(cola_suspended_blocked, pcb);
                list_add(cola_suspended_ready, pcb);
                cambiar_estado(pcb, STATE_SUSPENDED_READY);
                log_info(logger, "## (<%d>) finalizo IO y pasa a SUSPENDED_READY", pcb->pid);
                sem_post(&sem_procesos_en_new_o_memoria_libre);
            }
        }
        despachar_io_si_es_posible(grupo_encontrado);

        pthread_mutex_unlock(&grupo_encontrado->mutex_grupo); // Desbloqueamos el específico
    } else {
        LOG_ERROR("Se recibió fin de I/O de un socket desconocido: %d.", socket_origen);
    }

    // 4. Finalmente, desbloqueamos el mutex general
    pthread_mutex_unlock(&mutex_colas);
}

void find_io_iterator(char* nombre_grupo, void* data) {
    find_io_by_socket_data* search_data = (find_io_by_socket_data*)data;

    if (search_data->instancia_encontrada != NULL) {
        return;
    }

    t_grupo_io* grupo_actual = (t_grupo_io*)dictionary_get(dispositivos_io, nombre_grupo);

    for (int i = 0; i < list_size(grupo_actual->instancias); i++) {
        t_io* instancia_actual = list_get(grupo_actual->instancias, i);
        if (instancia_actual->socket_fd == search_data->socket_a_buscar) {
            search_data->grupo_encontrado = grupo_actual;
            search_data->instancia_encontrada = instancia_actual;
            return; 
        }
    }
}


void despachar_io_si_es_posible(t_grupo_io* grupo) {
    // Intentamos despachar trabajo mientras haya procesos en espera
    while (!queue_is_empty(grupo->cola_espera_compartida)) {
        t_io* instancia_libre = NULL;
        int indice_encontrado = -1;

        int total_instancias = list_size(grupo->instancias);
        if (total_instancias == 0) {
            break; // No hay instancias a las que despachar
        }

        // --- Lógica Round-Robin para encontrar una instancia libre ---
        // Recorremos la lista de forma circular, empezando desde la siguiente a la última usada.
        for (int i = 0; i < total_instancias; i++) {
            int indice_a_probar = (grupo->ultimo_despacho_idx + 1 + i) % total_instancias;
            t_io* instancia_actual = list_get(grupo->instancias, indice_a_probar);
            
            if (!instancia_actual->ocupado) {
                instancia_libre = instancia_actual;
                indice_encontrado = indice_a_probar;
                break; // Encontramos una libre
            }
        }

        // Si después de recorrer todas las instancias no encontramos ninguna libre, salimos.
        if (instancia_libre == NULL) {
            break;
        }

        // --- Despacho del proceso ---
        t_solicitud_io* solicitud = queue_pop(grupo->cola_espera_compartida);
        
        instancia_libre->ocupado = true;
        instancia_libre->pid_en_ejecucion = solicitud->pid;
        grupo->ultimo_despacho_idx = indice_encontrado; // ¡Actualizamos el índice para la próxima vez!

        // (El resto de tu código para crear y enviar el paquete a la instancia de I/O va aquí)
        t_paquete* paquete_a_io = crear_paquete(IO);
        int len_nombre = strlen(grupo->nombre_dispositivo) + 1;
        agregar_a_paquete(paquete_a_io, &len_nombre, sizeof(int));
        agregar_a_paquete(paquete_a_io, grupo->nombre_dispositivo, len_nombre);
        agregar_a_paquete(paquete_a_io, &(solicitud->pid), sizeof(int));
        agregar_a_paquete(paquete_a_io, &(solicitud->tiempo), sizeof(int));
        
        enviar_paquete(paquete_a_io, instancia_libre->socket_fd);
        eliminar_paquete(paquete_a_io);

        log_info(logger, "## (<%d>) Despachado a la instancia de IO '%s' por %d ms (Socket: %d)", solicitud->pid, grupo->nombre_dispositivo, solicitud->tiempo, instancia_libre->socket_fd);
        
        free(solicitud->nombre_dispositivo);
        free(solicitud);
    }
}


t_pcb* buscar_pcb_por_pid(int pid) {
    t_pcb* resultado = NULL;
    bool encontrado = false;

    bool _find_by_pid(void* elemento) {
        return ((t_pcb*)elemento)->pid == pid;
    }

    if (!encontrado) resultado = list_find(cola_exec, _find_by_pid);
    if (resultado) encontrado = true;
    
    if (!encontrado) resultado = list_find(cola_ready, _find_by_pid);
    if (resultado) encontrado = true;
    
    if (!encontrado) resultado = list_find(cola_blocked, _find_by_pid);
    if (resultado) encontrado = true;
    
    if (!encontrado) resultado = list_find(cola_suspended_blocked, _find_by_pid);
    if (resultado) encontrado = true;
    
    if (!encontrado) resultado = list_find(cola_suspended_ready, _find_by_pid);
    
    return resultado;
}


void destruir_respuesta_ejecucion(t_respuesta_ejecucion* respuesta) {
    if (respuesta == NULL) return;
    if (respuesta->nombre_dispositivo_io != NULL) {
        free(respuesta->nombre_dispositivo_io);
    }
    free(respuesta);
}


// Comparador para ordenar procesos por estimacion de ráfaga (ascendente)
bool comparar_por_rafaga_estimada(void* pcb1, void* pcb2) {
    t_pcb* proceso1 = (t_pcb*)pcb1;
    t_pcb* proceso2 = (t_pcb*)pcb2;

    // Primero, comparamos por la estimación de ráfaga
    if (proceso1->estimacion_rafaga < proceso2->estimacion_rafaga) {
        return true;
    }
    
    // Si las estimaciones son iguales, desempatamos por PID (el más bajo primero)
    if (proceso1->estimacion_rafaga == proceso2->estimacion_rafaga) {
        return proceso1->pid < proceso2->pid;
    }

    return false;
}

void calcular_siguiente_rafaga(t_pcb* pcb) {
    double nueva_estimacion = ALFA * pcb->rafaga_real_anterior + (1.0 - ALFA) * pcb->estimacion_rafaga;

    if (nueva_estimacion > 0) {
        pcb->estimacion_rafaga = nueva_estimacion;
    } else {
        pcb->estimacion_rafaga = 1;
    }
}

/* ----------- PLANIFICADOR MEDIANO PLAZO ----------- */

void* hilo_suspension_proceso(void* arg) {

    int pid_a_vigilar = *(int*)arg;
    free(arg);
    
    int tiempo_suspension_ms = config_get_int_value(config, "TIEMPO_SUSPENSION");
    usleep(tiempo_suspension_ms * 1000);

    bool necesita_suspension = false;

  
    pthread_mutex_lock(&mutex_colas);

    t_pcb* pcb_actual = buscar_pcb_por_pid(pid_a_vigilar);

    // SOLO si el proceso existe Y su estado sigue siendo BLOCKED, lo suspendemos
    if (pcb_actual != NULL && pcb_actual->estado == STATE_BLOCKED) {
        cambiar_estado(pcb_actual, STATE_SUSPENDED_BLOCKED);
        list_remove_element(cola_blocked, pcb_actual);
        list_add(cola_suspended_blocked, pcb_actual);

        necesita_suspension = true;
    }

    pthread_mutex_unlock(&mutex_colas);

    if (necesita_suspension) {
        log_info(logger, "## (%d) Suspendido por tiempo de bloqueo excesivo (esperando dispositivo).", pid_a_vigilar);
        solicitar_suspension_a_memoria(pid_a_vigilar);
        //sem_post(&sem_procesos_en_new_o_memoria_libre);
    }

    return NULL;
}

void solicitar_suspension_a_memoria(int pid) {
    char* ip_memoria = config_get_string_value(config, "IP_MEMORIA");
    char* puerto_memoria = config_get_string_value(config, "PUERTO_MEMORIA");
    
    t_paquete* paquete_respuesta = NULL;

    log_info(logger, "## (%d) Solicitando suspension a Memoria.", pid);

    // 1. Crear conexion efimera
    int socket_memoria_temporal = crear_conexion(ip_memoria, puerto_memoria);
    if (socket_memoria_temporal == -1) {
        LOG_ERROR("PID %d: No se pudo conectar a Memoria para solicitar suspensión.", pid);
        return;
    }

    // 2. Realizar Handshake
    t_paquete* handshake = crear_paquete(HANDSHAKE_MEMORIA_KERNEL);
    enviar_paquete(handshake, socket_memoria_temporal);
    eliminar_paquete(handshake);

    t_paquete* respuesta_hs = recibir_paquete(socket_memoria_temporal);
    if (respuesta_hs != NULL && respuesta_hs->codigo_operacion == HANDSHAKE_OK) {
        eliminar_paquete(respuesta_hs); // Handshake OK, continuamos

        // 3. Enviar la peticion real de suspension
        t_paquete* paquete_solicitud = crear_paquete(SUSPENDER_PROCESO);
        agregar_a_paquete(paquete_solicitud, &pid, sizeof(int));
        enviar_paquete(paquete_solicitud, socket_memoria_temporal);
        eliminar_paquete(paquete_solicitud);

        paquete_respuesta = recibir_paquete(socket_memoria_temporal);

    } else {
        LOG_ERROR("PID %d: Falló el handshake con Memoria al intentar suspender.", pid);
        if(respuesta_hs) eliminar_paquete(respuesta_hs);
    }
    
    // 4. Cerrar la conexion (la comunicacion ya termino)
    close(socket_memoria_temporal);

    // 5. Procesar la respuesta de la operacion (si es que la hubo)
    if (paquete_respuesta == NULL) {
        LOG_ERROR("PID %d: No se recibió respuesta de Memoria a la solicitud de suspensión.", pid);
        return;
    }

    switch(paquete_respuesta->codigo_operacion) {
        case SUSPENSION_OK:
            log_info(logger, "## (%d) Memoria confirmo la suspension.", pid);
              // La memoria del proceso PID ha sido liberada en RAM y movida a SWAP.
            // Esto significa que hay espacio libre en memoria principal.
            sem_post(&sem_procesos_en_new_o_memoria_libre); 
            break;
        case ERROR_SWAP_FULL:
            LOG_ERROR("PID %d: Memoria reportó SWAP LLENO. La suspensión falló.", pid);
            break;
        default:
            log_warning(logger, "## (%d) Memoria respondio con un codigo inesperado (%s) a la solicitud de suspension.", pid, op_code_a_string(paquete_respuesta->codigo_operacion));
            break;
    }

    eliminar_paquete(paquete_respuesta);
}

bool solicitar_admision_a_memoria(t_pcb* pcb, bool viene_de_susp) {
    char* ip_memoria = config_get_string_value(config, "IP_MEMORIA");
    char* puerto_memoria = config_get_string_value(config, "PUERTO_MEMORIA");
    op_code codigo_solicitud;
    op_code codigo_respuesta_ok;

    int socket_memoria_temporal = crear_conexion(ip_memoria, puerto_memoria);
    if (socket_memoria_temporal == -1) {
        LOG_ERROR("PID %d: No se pudo conectar a Memoria para solicitar admisión.", pcb->pid);
        return false;
    }

    t_paquete* handshake = crear_paquete(HANDSHAKE_MEMORIA_KERNEL);
    enviar_paquete(handshake, socket_memoria_temporal);
    eliminar_paquete(handshake);
    t_paquete* respuesta_hs = recibir_paquete(socket_memoria_temporal);
    if (respuesta_hs == NULL || respuesta_hs->codigo_operacion != HANDSHAKE_OK) {
        LOG_ERROR("PID %d: Falló el handshake con Memoria al solicitar admisión.", pcb->pid);
        if(respuesta_hs) eliminar_paquete(respuesta_hs);
        close(socket_memoria_temporal);
        return false;
    }
    eliminar_paquete(respuesta_hs);

    if (viene_de_susp) {
        log_info(logger, "## (%d) Solicitando a Memoria traer de SWAP.", pcb->pid);
        codigo_solicitud = DESUSPENDER_PROCESO;
        codigo_respuesta_ok = DESUSPENSION_OK;
    } else {
        log_info(logger, "## (%d) Solicitando a Memoria espacio para nuevo proceso.", pcb->pid);
        codigo_solicitud = INIT_PROC;
        codigo_respuesta_ok = INIT_PROCESO_OK;
    }

    t_paquete* paquete_solicitud = crear_paquete(codigo_solicitud);
    
    if (!viene_de_susp) {
        agregar_a_paquete(paquete_solicitud, &pcb->pid, sizeof(int));
        agregar_a_paquete(paquete_solicitud, &pcb->tamanio_proceso, sizeof(int));
        int largo_path = strlen(pcb->path_pseudocodigo) + 1;
        agregar_a_paquete(paquete_solicitud, &largo_path, sizeof(int));
        agregar_a_paquete(paquete_solicitud, pcb->path_pseudocodigo, largo_path);
    } else {
        agregar_a_paquete(paquete_solicitud, &pcb->pid, sizeof(int));
    }

    enviar_paquete(paquete_solicitud, socket_memoria_temporal);
    eliminar_paquete(paquete_solicitud);

    t_paquete* paquete_respuesta = recibir_paquete(socket_memoria_temporal);
    close(socket_memoria_temporal);

    if (paquete_respuesta == NULL) {
        LOG_ERROR("PID %d: Error de comunicación con Memoria al solicitar admisión.", pcb->pid);
        return false;
    }

    bool admision_exitosa = (paquete_respuesta->codigo_operacion == codigo_respuesta_ok);
    
    if(admision_exitosa) {
        log_info(logger, "## (%d) Memoria confirmo la admision.", pcb->pid);
    } else {
        log_warning(logger, "## (%d) Memoria rechazo la admision (Motivo: %s).", pcb->pid, op_code_a_string(paquete_respuesta->codigo_operacion));
    }

    eliminar_paquete(paquete_respuesta);
    return admision_exitosa;
}


void enviar_interrupcion_a_cpu(t_cpu* cpu) {
    if (cpu == NULL || cpu->socket_interrupt == -1) {
        log_error(logger, "Intento de interrumpir una CPU inválida o sin conexión de interrupción.");
        return;
    }
    t_paquete* interrumpir_proceso = crear_paquete(PROCESO_INTERRUPCION); 
    enviar_paquete(interrumpir_proceso, cpu->socket_interrupt);
    eliminar_paquete(interrumpir_proceso);
}

void gestionar_procesos_finalizados() {
    char* ip_memoria = config_get_string_value(config, "IP_MEMORIA");
    char* puerto_memoria = config_get_string_value(config, "PUERTO_MEMORIA");

    while (true) { 
        // 1. Espera a que un proceso llegue a la cola de EXIT
        sem_wait(&sem_procesos_en_exit);

        t_pcb* pcb_a_liberar = NULL;

        // 2. Saca el PCB de la cola de forma segura
        pthread_mutex_lock(&mutex_colas);
        if (!list_is_empty(cola_exit)) {
            pcb_a_liberar = (t_pcb*)list_remove(cola_exit, 0);
        }
        pthread_mutex_unlock(&mutex_colas);

        if (pcb_a_liberar == NULL) {
            LOG_ERROR("Inconsistencia: sem_procesos_en_exit fue señalado pero la cola_exit está vacía.");
            continue; 
        }

        // 3. Hacemos el ultimo cambio de estado para acumular el tiempo final del proceso
        cambiar_estado(pcb_a_liberar, STATE_EXIT);

        log_info(logger, "## (%d) Finaliza el proceso", pcb_a_liberar->pid);

        // 4. Imprimimos el log de métricas de tiempo del Kernel, como pide el enunciado
        log_info(logger, 
            "## (%d) Métricas de Estado - NEW: (%d) %lldms, READY: (%d) %lldms, RUNNING: (%d) %lldms, BLOCKED: (%d) %lldms, SUSPENDED: (%d) %lldms",
            pcb_a_liberar->pid,
            pcb_a_liberar->metricas.ingresos_a_new, pcb_a_liberar->metricas.tiempo_en_new,
            pcb_a_liberar->metricas.ingresos_a_ready, pcb_a_liberar->metricas.tiempo_en_ready,
            pcb_a_liberar->metricas.ingresos_a_running, pcb_a_liberar->metricas.tiempo_en_running,
            pcb_a_liberar->metricas.ingresos_a_blocked, pcb_a_liberar->metricas.tiempo_en_blocked,
            pcb_a_liberar->metricas.ingresos_a_suspended, pcb_a_liberar->metricas.tiempo_en_suspended
        );

        // 5. Le avisamos a Memoria que libere los recursos de este proceso (marcos, tablas, etc.)
        log_info(logger, "Notificando a Memoria para liberar recursos del PID %d.", pcb_a_liberar->pid);

        int socket_memoria_temporal = crear_conexion(ip_memoria, puerto_memoria);
        if (socket_memoria_temporal != -1) {
            // La conexion fue exitosa, ahora realizamos la transaccion
            // 1. Realizar handshake en la nueva conexion
            t_paquete* handshake = crear_paquete(HANDSHAKE_MEMORIA_KERNEL);
            enviar_paquete(handshake, socket_memoria_temporal);
            eliminar_paquete(handshake);
            
            t_paquete* respuesta_hs = recibir_paquete(socket_memoria_temporal);
            if (respuesta_hs != NULL && respuesta_hs->codigo_operacion == HANDSHAKE_OK) {
                // El handshake fue exitoso, ahora enviamos la peticion real
                eliminar_paquete(respuesta_hs);

                t_paquete* paquete_op = crear_paquete(FINALIZAR_PROCESO);
                agregar_a_paquete(paquete_op, &(pcb_a_liberar->pid), sizeof(int));
                enviar_paquete(paquete_op, socket_memoria_temporal);
                eliminar_paquete(paquete_op);

                // Esperamos la confirmacion de la operacion
                t_paquete* respuesta_op = recibir_paquete(socket_memoria_temporal);
                if (respuesta_op != NULL && respuesta_op->codigo_operacion == FINALIZACION_OK) {
                    log_info(logger, "Memoria confirmo la liberacion de recursos para el PID: %d.", pcb_a_liberar->pid);
                    sem_post(&sem_procesos_en_new_o_memoria_libre);
                } else {
                    LOG_ERROR("Memoria no confirmó la liberación de recursos para el PID %d.", pcb_a_liberar->pid);
                }
                if(respuesta_op) eliminar_paquete(respuesta_op);

            } else {
                LOG_ERROR("Falló el handshake con Memoria al intentar liberar PID %d.", pcb_a_liberar->pid);
                if(respuesta_hs) eliminar_paquete(respuesta_hs);
            }
        
            close(socket_memoria_temporal);

        } else {
            LOG_ERROR("No se pudo conectar a Memoria para liberar los recursos del PID %d.", pcb_a_liberar->pid);
        }

        // 6. Finalmente, liberamos la memoria que ocupaba el PCB en el Kernel
        log_info(logger, "Limpiando estructuras del PCB para el PID %d en Kernel.", pcb_a_liberar->pid);
        if (pcb_a_liberar->path_pseudocodigo != NULL) {
            free(pcb_a_liberar->path_pseudocodigo);
        }
        
        free(pcb_a_liberar);
    }
}

/* --------- METRICAS ---------- */

long long get_timestamp_ms() {
    struct timespec ts;
    // CLOCK_MONOTONIC es un reloj que no se ve afectado por cambios en la hora del sistema
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}


void cambiar_estado(t_pcb* pcb, op_code_estado nuevo_estado) {
    if (pcb == NULL || pcb->estado == nuevo_estado) {
        return;
    }

    long long tiempo_actual_ms = get_timestamp_ms();
    long long tiempo_transcurrido = tiempo_actual_ms - pcb->timestamp_ultimo_cambio;
    op_code_estado estado_anterior = pcb->estado;

    switch (pcb->estado) {
        case STATE_NEW:
            pcb->metricas.tiempo_en_new += tiempo_transcurrido;
            break;
        case STATE_READY:
            pcb->metricas.tiempo_en_ready += tiempo_transcurrido;
            break;
        case STATE_RUNNING:
            pcb->metricas.tiempo_en_running += tiempo_transcurrido;
            // Las siguientes líneas se mueven a recibir_y_manejar_respuesta_cpu
            // pcb->rafaga_real_anterior = tiempo_transcurrido; 
            // if (strcmp(ALGORITMO_CORTO_PLAZO, "SJF") == 0 || strcmp(ALGORITMO_CORTO_PLAZO, "SRT") == 0) {
            //     calcular_siguiente_rafaga(pcb);
            // }
            break;
        case STATE_BLOCKED:
            pcb->metricas.tiempo_en_blocked += tiempo_transcurrido;
            break;
        case STATE_SUSPENDED_BLOCKED:
        case STATE_SUSPENDED_READY:
            pcb->metricas.tiempo_en_suspended += tiempo_transcurrido;
            break;
        case STATE_EXIT:
            break;
    }

    switch (nuevo_estado) {
        case STATE_NEW:
            pcb->metricas.ingresos_a_new++;
            break;
        case STATE_READY:
            pcb->metricas.ingresos_a_ready++;
            break;
        case STATE_RUNNING:
            pcb->metricas.ingresos_a_running++;
            break;
        case STATE_BLOCKED:
            pcb->metricas.ingresos_a_blocked++;
            break;
        case STATE_SUSPENDED_BLOCKED:
        case STATE_SUSPENDED_READY:
            pcb->metricas.ingresos_a_suspended++;
            break;
        case STATE_EXIT:
            break;
    }

    log_info(logger, "## (%d) Pasa del estado %s al estado %s", pcb->pid, estado_a_string(estado_anterior), estado_a_string(nuevo_estado));
    
    pcb->estado = nuevo_estado;
    pcb->timestamp_ultimo_cambio = tiempo_actual_ms;
}

bool _is_proceso_activo_en_io(int pid) {
    t_list* grupos_io_keys = dictionary_keys(dispositivos_io);
    bool encontrado = false;

    for (int i = 0; i < list_size(grupos_io_keys); i++) {
        char* nombre_grupo = list_get(grupos_io_keys, i);
        t_grupo_io* grupo = dictionary_get(dispositivos_io, nombre_grupo);

        for (int j = 0; j < list_size(grupo->instancias); j++) {
            t_io* instancia = list_get(grupo->instancias, j);
            if (instancia->ocupado && instancia->pid_en_ejecucion == pid) {
                encontrado = true;
                break;
            }
        }
        if (encontrado) break;
    }

    list_destroy(grupos_io_keys);
    return encontrado;
}

