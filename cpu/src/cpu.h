#ifndef CPU_H_
#define CPU_H_

#include <commons/config.h>
#include <commons/log.h>
#include <utils/utils.h>  
#include <utils/global.h>        
#include <stdbool.h>            
#include <pthread.h>            
#include <semaphore.h>           

typedef struct {
    int pagina;         // numero de pagina
    char* contenido;    // datos de la pagina (tamanio_pagina bytes)
    bool bit_uso;       // para CLOCK
    bool bit_modificado; // para CLOCK-M
    bool ocupado;       // si esta en uso la entrada
    int pid;            // PID del proceso al que pertenece la pagina en cache
    int marco;          // marco de memoria fisica donde esta la pagina
} entrada_cache;

typedef struct {
    int pagina;         // Numero de pagina logica
    int marco;          // Marco de memoria fisica
    uint64_t timestamp; // Para LRU (tiempo de ultimo acceso)
    bool ocupada;       // Si la entrada de la TLB esta en uso
    int pid;            // PID del proceso al que pertenece esta entrada de TLB
} entrada_tlb;

extern int socket_memoria;
extern int socket_kernel_dispatch;
extern int socket_kernel_interrupt;
extern t_log *logger;

extern entrada_tlb* tlb;
extern int total_entradas_tlb;
extern char* algoritmo_tlb; // "FIFO" o "LRU"
extern int puntero_tlb_fifo;

extern entrada_cache* cache_paginas;
extern int entradas_cache;
extern char* algoritmo_cache;  // "CLOCK" o "CLOCK-M"
extern int puntero_clock_cache; // Para algoritmo CLOCK de cache
extern int tam_pagina_memoria;


extern pthread_mutex_t mutex_tlb;
extern pthread_mutex_t mutex_cache;
extern pthread_mutex_t mutex_interrupcion_pendiente;
extern pthread_cond_t cond_interrupcion_pendiente;
extern bool INTERRUPCION_PENDIENTE;


t_config* iniciar_config(void);
t_log* iniciar_logger(char* nombre_cpu);
void terminar_programa(t_log* logger, t_config* config);

int conectar_memoria();
int conectar_kernel_dispatch(char* ip, char* puerto, char* nombre_cpu);
int conectar_kernel_interrupt(char* ip, char* puerto, char* nombre_cpu);

void concurrencia_cpu();
void ciclo_instruccion(t_pcb* pcb);
t_instruccion* fetch(int pid, int pc);
resultado_ejecucion_instruccion_cpu ejecutar_instruccion(t_pcb* pcb, t_instruccion* instruccion, char** nombre_dispositivo_io_out, int* duracion_bloqueo_out);
bool manejar_instruccion_read(t_pcb* pcb, int direccion_logica, int tamanio_a_leer);
bool manejar_instruccion_write(t_pcb* pcb, int direccion_logica, char* datos);
void manejar_instruccion_init_proceso(t_pcb* pcb, t_instruccion* instruccion);

void enviar_respuesta_al_kernel(int pid, int pc, op_code motivo, char* nombre_dispositivo_io, int duracion_bloquente);
void* atender_conexiones_dispatch();
void* hilo_escucha_interrupciones(void* arg);

void iniciar_mmu();
void inicializar_cache();
void inicializar_tlb();
int obtener_pagina(int direccion_logica);
int obtener_offset(int direccion_logica);
char* buscar_en_cache(int pid, int pagina_logica);
entrada_tlb* buscar_en_tlb(int pid, int pagina);
uint64_t get_timestamp(); 
int pedir_traduccion_a_memoria(int pid, int direccion_logica); 
void agregar_a_tlb(int pid, int pagina, int marco);
void reemplazar_en_tlb(int pid, int pagina, int marco); 
void insertar_en_cache(int pid, int pagina_logica, int marco_fisico, char* contenido, bool modificado);
void aplicar_reemplazo_cache(int indice, int pid, int pagina, int marco, char* contenido, bool modificado);
void* seleccionar_victima_cache(int pid_a_insertar, int pagina_a_insertar, int marco_a_insertar, char* contenido_a_insertar, bool modificado); // Algoritmos CLOCK/CLOCK-M
char* leer_bytes_de_memoria_principal(int marco_fisico, int offset, int tamanio_a_leer, int pid);
void enviar_pagina_a_memoria_principal(int marco_fisico, char* contenido_pagina, int pid);
void limpiar_tlb(); 

void limpiar_cache_de_proceso(int pid); 
void escribir_bytes_en_memoria_principal(int marco_fisico, int offset, int tamanio_a_escribir, char* valor, int pid);
void realizar_write_back_cache(int pid);

void destruir_instruccion(t_instruccion* instruccion);
char* concatenar_parametros(t_instruccion* instruccion);

t_pcb* recibir_pcb(t_paquete* paquete);


#endif /* CPU_H_ */