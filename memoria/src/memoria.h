#ifndef MEMORIA_H_
#define MEMORIA_H_

#include <commons/config.h>
#include <commons/log.h>

#include <commons/collections/list.h>       
#include <commons/collections/dictionary.h> 
#include <stdbool.h>           
#include <math.h>            
#include <pthread.h> 
#include <commons/bitarray.h>      

struct _proceso_memoria;
struct _tabla_pagina;

typedef struct {
    bool presente;          // true si la pagina esta en memoria, false si no
    int numero_marco;       // Numero de marco de memoria fisica si es el ultimo nivel de tabla
    void* siguiente_nivel;  // Puntero a la siguiente tabla de paginas (tipo pagina*) si no es el ultimo nivel
    int id_siguiente_tabla; // ID de la tabla de paginas a la que apunta (util para la CPU en un esquema iterativo)
    int ultimo_acceso;      // Timestamp o contador para algoritmos de reemplazo (ej. LRU)
    bool modificado;        // Bit de modificacdo => true si la pagina fue modificada en memoria
    int posicion_swap;      // guarda el indice del slot en el bitmap de SWAP -- Cuando una pagina se mueva a SWAP, pondremos presente = false y guardaremos el indice del slot de SWAP en posicion_swap
} entrada_tabla;

typedef struct _tabla_pagina {
    entrada_tabla* entradas; // Array dinamico de entradas
    int nivel;               // Nivel de la tabla (1 para la raiz, 2 para la siguiente, etc.)
    int id_tabla;            // ID unico para esta tabla de paginas
} tabla_pagina;

typedef struct {
    int accesos_tabla_paginas;
    int instrucciones_solicitadas;
    int escrituras_swap; // "Bajadas a SWAP"
    int lecturas_swap;   // "Subidas a Memoria Principal"
    int lecturas_memoria;
    int escrituras_memoria;
} t_metricas_memoria;
typedef struct _proceso_memoria {
    int pid;
    tabla_pagina* tabla_nivel_1; // Puntero a la tabla de paginas de primer nivel
    t_list* instrucciones;       // Lista de instrucciones del proceso (ya cargadas del archivo)
    int tam_proceso;             // Tamaño total del proceso en bytes (del enunciado)
    int paginas_asignadas;       // Cantidad de paginas asignadas actualmente al proceso
    t_metricas_memoria metricas;
    pthread_mutex_t mutex_proceso;
} proceso_memoria;


extern bool* bitmap_marcos;
extern int cantidad_marcos;
extern t_log *logger;
extern int tam_pagina;
extern int cantidad_niveles;
extern int entradas_por_tabla;
extern void* memoria_usuario;
extern t_dictionary* diccionario_procesos;
extern int global_tabla_id_counter;
extern int socket_escucha_memoria;
extern char *puerto_escucha;
extern int tam_memoria;

extern pthread_mutex_t mutex_bitmap;
extern pthread_mutex_t mutex_diccionario_procesos;

extern int bits_offset;
extern int bits_por_nivel;
extern int mascara_offset;
extern int mascara_nivel;

t_config* iniciar_config(void);
t_log* iniciar_logger(void);
void terminar_programa(t_log* logger, t_config* config, int socket);
void iniciar_swap_file();

void concurrencia_memoria();
void* atender_cliente(void* arg);
void handshake_memoria(int socket_cliente, t_log* logger);
void* atender_post_handshake(void* arg);

void enviar_instruccion_segun_pid_y_pc(int socket_cliente, int pid, int pc);
void enviar_espacio_libre(int socket_cliente);

void almacenar_proceso_nuevo(int cliente_fd, int pid, int tamanio_proceso, char* path_pseudocodigo);
void destruir_proceso_wrapper(void* p);
void liberar_elementos_instruccion(t_instruccion* inst);
void destruir_proceso(int pid);

tabla_pagina* crear_tabla_pagina(int nivel_actual);
void liberar_tablas_proceso(tabla_pagina* tabla);

int buscar_marco_libre();
void ocupar_marco(int nro_marco);
void liberar_marco(int nro_marco);

void manejar_traduccion_tabla(int socket_cpu, int pid, int direccion_logica);
int obtener_indice_de_pagina(int numero_pagina, int nivel_actual);

void* leer_memoria_fisica(int marco_fisico, int tamanio);
int escribir_memoria_fisica(int direccion_fisica, void* valor, int tamanio);

void manejar_lectura_direccion_fisica(int cliente_fd, int pid, int marco_fisico, int offset, int tamanio_a_leer); 
void manejar_escritura_direccion_fisica(int cliente_fd, int pid, int marco_fisico, int offset, int tamanio_a_escribir, void* contenido_a_escribir); 


void mostrar_estado_paginacion();
bool asignar_paginas_recursivo(tabla_pagina* tabla_actual, int nivel_actual, int* paginas_restantes_a_asignar);
void dump_contenido_memoria();

int buscar_slot_libre_swap();
int escribir_en_swap(void* contenido_pagina);
void leer_de_swap(void* puntero_marco_destino, int slot_swap);
void manejar_suspension_proceso(int pid, int cliente_fd);
void suspender_paginas_recursivo(proceso_memoria* proceso, tabla_pagina* tabla, int nivel);
void manejar_desuspension_proceso(int pid, int cliente_fd);
void desuspender_paginas_recursivo(proceso_memoria* proceso, tabla_pagina* tabla, int nivel);

int contar_slots_libres_swap();
int contar_paginas_presentes(tabla_pagina* tabla_raiz);
void contar_paginas_recursivo_presentes(tabla_pagina* tabla, int nivel, int* contador);
int contar_paginas_en_swap(tabla_pagina* tabla_raiz);
void dumpear_paginas_recursivo(FILE* dump_file, tabla_pagina* tabla, int nivel);

bool manejar_solicitud_dump(int pid);
void recolectar_paginas_para_dump(t_list* lista_contenidos, tabla_pagina* tabla, int nivel);

int contar_marcos_libres();
int buscar_slot_libre_swap();
void liberar_slot_swap(int slot);

#endif /* MEMORIA_H_ */
