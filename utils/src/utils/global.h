#ifndef GLOBAL_H_
#define GLOBAL_H_

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <commons/collections/list.h>
#include <commons/collections/dictionary.h>
#include <commons/collections/queue.h>
#include <commons/temporal.h>
#include <commons/log.h>
#include <commons/string.h>
#include <pthread.h> 

#define MAX_PARAMETROS 3

extern t_log* logger;

typedef enum {
    MENSAJE,                   // 0
    HANDSHAKE,                 // 1
    HANDSHAKE_KERNEL_IO,       // 2
    HANDSHAKE_MEMORIA_KERNEL,  // 3
    HANDSHAKE_MEMORIA_CPU,     // 4
    HANDSHAKE_CPU_DISPATCH,    // 5
    HANDSHAKE_CPU_INTERRUPT,   // 6
    HANDSHAKE_OK,              // 7
    HANDSHAKE_TAM_MEMORIA,     // 8
    
    PEDIR_INSTRUCCIONES,      // 9
    PEDIR_ESPACIO_LIBRE,      // 10
    
    //Syscalls
    PEDIDO_IO,               // 11 - Solicitud de un proceso al Kernel para usar un dispositivo IO
    FINALIZO_IO,             // 12 - El modulo IO informa al Kernel que finalizo una operacion
    DESCONECTADO_IO,         // 13 - El modulo IO informa al Kernel que se ha desconectado
    PROCESO_DESBLOQUEADO,    // 14 - El Kernel informa a la CPU que un proceso bloqueado esta listo

    IO,                      // 15 - Tipo de instruccion IO
    INIT_PROC,               // 16 - Kernel solicita a Memoria inicializar un proceso
    EXIT,                    // 17 - El proceso finaliza su ejecucion
    DUMP_MEMORY,             // 18 - Kernel solicita a Memoria realizar un dump

    //Instrucciones CPU
    WRITE,                   // 19 - Escribir en memoria
    READ,                    // 20 - Leer de memoria
    NOOP,                    // 21 - No hacer nada
    GOTO,                    // 22 - Saltar a una instruccion

    OP_PCB,                  // 23 - El Kernel le envia a la CPU el PCB del proceso a ejecutar
    OP_RESPUESTA_EJECUCION,  // 24 - La CPU le envia al Kernel la respuesta de la ejecucion del PCB
    
    PROCESO_BLOQUEANTE,      // 25 - La CPU informa al Kernel que el proceso se bloquea por IO
    PROCESO_INTERRUPCION,    // 26 - La CPU informa al Kernel que se produjo una interrupcion (ej: fin de quantum)

    PEDIR_CONTENIDO,         // 27 - (Podría ser para Memoria: pedir contenido de una dirección)

    TRADUCIR_TABLA,          // 28 - CPU pide a Memoria una traducción de Dirección Lógica a Marco
    FINALIZAR_PROCESO,       // 29 - Kernel solicita a Memoria liberar recursos de un proceso
    SOLICITAR_PAGINAS,       // 30 - Kernel solicita a Memoria asignar páginas a un proceso
    LIBERAR_PAGINAS,         // 31 - Kernel solicita a Memoria liberar páginas de un proceso
    LEER_MEMORIA,            // 32 - CPU/Kernel solicita a Memoria leer N bytes de una dirección física
    ESCRIBIR_MEMORIA,        // 33 - CPU/Kernel solicita a Memoria escribir N bytes en una dirección física
    INSTRUCCION_OK,          // 34 - La CPU confirma que la instrucción se ejecutó correctamente
    ERROR_PID_NO_ENCONTRADO, // 35 - El PID no se encuentra en memoria
    ERROR_PC_FUERA_DE_RANGO, // 36 - El PC está fuera del rango de instrucciones del proceso
    PROCESO_NO_ALMACENADO,   // 37 - El proceso no está almacenado en memoria (Memoria a Kernel)
    TRADUCCION_OK,           // 38 - La traducción de dirección lógica a marco fue exitosa (Memoria a CPU)
    ERROR_TRADUCCION,        // 39 - Error al traducir dirección lógica a marco (Memoria a CPU)
    MARCO_NO_PRESENTE,       // 40 - El marco solicitado no está presente en memoria (Memoria a CPU/Kernel)
    NO_HAY_MARCOS_LIBRES,    // 41 - No hay marcos libres disponibles en memoria (Memoria a Kernel)
    OPERACION_LECTURA_OK,    // 42 - La operación de lectura en memoria fue exitosa (Memoria a CPU/Kernel)
    OPERACION_ESCRITURA_OK,  // 43 - La operación de escritura en memoria fue exitosa (Memoria a CPU/Kernel)
    ERROR_FUERA_DE_PAGINA,   // 44 - Error de acceso fuera de segmento (Memoria a CPU/Kernel)
    PAGINA_FUERA_DE_RANGO,   // 45 - Error de acceso a una página fuera del rango permitido (Memoria a CPU/Kernel)
    INIT_PROCESO_OK,         // 46 - El proceso se inicializó correctamente en memoria (Memoria a Kernel)
    DISPOSITIVO_IO,          // 47 - Identificador para un tipo de mensaje o solicitud que se dirige a un dispositivo IO específico.
    INTERRUPCION,            // 48 - Indica que se ha producido una interrupción en la CPU (Memoria a Kernel)
    FINALIZACION_OK,         // 49 - La finalización del proceso fue exitosa (Memoria a Kernel)

    SUSPENDER_PROCESO,
    DESUSPENDER_PROCESO,
    SUSPENSION_OK,
    DESUSPENSION_OK,
    ERROR_SWAP_FULL,
    ERROR_MEMORIA_INSUFICIENTE,

    PROCESO_BLOQUEANTE_IO, // Indica que un proceso se bloquea esperando un recurso IO
    PROCESO_BLOQUEANTE_DUMP_MEMORY,

    DUMP_OK,
    ERROR_DUMP,

    ERROR_SEG_FAULT,
    
} op_code;

typedef enum {
    STATE_NEW,               // 0 - Proceso recién creado
    STATE_READY,             // 1 - Proceso listo para ejecutar
    STATE_RUNNING,           // 2 - Proceso en ejecución
    STATE_BLOCKED,           // 3 - Proceso bloqueado esperando un recurso IO
    STATE_EXIT,              // 4 - Proceso finalizado
    STATE_SUSPENDED_READY,   // 5 - Proceso suspendido y listo para reanudarse
    STATE_SUSPENDED_BLOCKED, // 6 - Proceso suspendido y bloqueado, esperando un recurso IO
} op_code_estado;

typedef enum {
    EJECUCION_OK,              // La instrucción se ejecutó y el proceso continúa
    PROCESO_BLOQUEADO,         // La instrucción causó un bloqueo (ej: IO)
    PROCESO_BLOQUEADO_IO,
    PROCESO_BLOQUEADO_DUMP_MEMORY, // El proceso se bloquea esperando dump
    PROCESO_FINALIZADO_EXIT,   // La instrucción fue un EXIT
    ERROR_EJECUCION_FATAL,      // Error que impide continuar el proceso (ej: PC fuera de rango, instrucción desconocida)
    ERROR_LECTURA_MEMORIA,     // Error al leer de memoria
    ERROR_ESCRITURA_MEMORIA,   // Error al escribir en memoria
    SEGMENTATION_FAULT
} resultado_ejecucion_instruccion_cpu;

typedef struct {
    long long tiempo_en_new;
    long long tiempo_en_ready;
    long long tiempo_en_running;
    long long tiempo_en_blocked;
    long long tiempo_en_suspended; // Suma de SUSPENDED_BLOCKED y SUSPENDED_READY

    int ingresos_a_new;
    int ingresos_a_ready;
    int ingresos_a_running;
    int ingresos_a_blocked;
    int ingresos_a_suspended;
} t_metricas_tiempos;

typedef struct {
    int pid;
    int program_counter;
    int tamanio_proceso;
    op_code_estado estado;
    char* path_pseudocodigo;

    double estimacion_rafaga;      // Estimación actual de la ráfaga de CPU
    double rafaga_real_anterior; // Última ráfaga real de CPU ejecutada (para SRT)
    long tiempo_llegada_ready;     // Timestamp de cuando el proceso entró a Ready (para cálculo de tiempo en Ready)

    int tiempo_inicio_ejecucion_cpu;

    t_metricas_tiempos metricas;
    long long timestamp_ultimo_cambio;    // Para saber cuándo fue el último cambio de estado
    int tiempo_bloqueo_io;
} t_pcb;

typedef struct {
	int opcode_lenght;
	char* opcode;
	int parametro1_lenght;
	int parametro2_lenght;
	int parametro3_lenght;
    int cantidad_parametros; 
	char* parametros[MAX_PARAMETROS];
} t_instruccion;

/*  para guardar en kernel un listado de cpus e ios conectados */
typedef struct {
    int id_cpu;                 // ID único de la CPU
    char* nombre;               // Nombre (ej: "CPU_01")
    char* ip;                   // IP de la CPU
    char* puerto_dispatch;      // Puerto para comunicación de dispatch
    char* puerto_interrupt;     // Puerto para comunicación de interrupt
    int socket_dispatch;
    int socket_interrupt;
    bool cpu_libre;
    pthread_t hilo_atencion_cpu;
    pthread_mutex_t mutex;
    t_pcb* pcb_en_ejecucion; 
} t_cpu;

typedef struct {
    char* nombre;                 // Nombre del dispositivo (ej: "DISCO", "IMPRESORA")
    char* ip;                     // IP del módulo IO
    char* puerto;                 // Puerto del módulo IO
    int socket_fd;                // Socket del modulo IO conectado
    bool ocupado;                 // Si está procesando una solicitud
    int pid_en_ejecucion; 
    t_queue* cola_pendientes;     // Cola FIFO de solicitudes: cada elemento es un t_solicitud_io*
    pthread_mutex_t mutex;        // Mutex para proteger acceso al dispositivo
    pthread_cond_t cond;          // Condición para notificar nuevas solicitudes
    pthread_t hilo_atencion;      // Hilo que procesa las solicitudes para este dispositivo
} t_io;

typedef struct {
    int pid;
    char* nombre_dispositivo; // Nombre del dispositivo IO solicitado (ej: "DISCO")
    int tiempo;               // Tiempo de uso del dispositivo en milisegundos
} t_solicitud_io;

extern t_dictionary* dispositivos_io; // clave: string (nombre del dispositivo), valor: t_io*

typedef struct {
    int pid;
    int program_counter;        // PC del proceso al momento de la respuesta
    op_code motivo;             // Causa de la finalización/bloqueo (EXIT, PROCESO_BLOQUEANTE, PROCESO_INTERRUPCION)
    char* nombre_dispositivo_io; // Solo si motivo == PROCESO_BLOQUEANTE
    int duracion_bloqueo;       // Solo si motivo == PROCESO_BLOQUEANTE
} t_respuesta_ejecucion;

typedef struct {
    char* nombre_dispositivo;
    t_list* instancias;             // Lista de t_io* (cada una es una instancia conectada)
    t_queue* cola_espera_compartida; // Cola de t_solicitud_io* (procesos esperando por este TIPO de dispositivo)
    pthread_mutex_t mutex_grupo;    // Un mutex para proteger este grupo (la lista y la cola)
    int ultimo_despacho_idx;        // Índice del último despacho para round-robin
} t_grupo_io;

typedef struct {
    int socket_a_buscar;
    t_grupo_io* grupo_encontrado;
    t_io* instancia_encontrada;
} find_io_by_socket_data;


#endif /* GLOBAL_H_ */