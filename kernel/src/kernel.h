#ifndef KERNEL_H_
#define KERNEL_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <commons/log.h>
#include <commons/string.h>
#include <commons/config.h>
#include <commons/collections/node.h>
#include <commons/collections/list.h>
#include <commons/collections/queue.h>
#include <pthread.h>
#include <readline/readline.h>

#include <utils/global.h>
#include <utils/utils.h>


t_log* iniciar_logger(void);
t_config* iniciar_config(void);
void inicializar_kernel();
t_pcb* crear_pcb(char* path, int tamanio);
int generar_pid();
void cerrar_sockets();
void terminar_programa(t_log* logger, t_config* config);
void concurrencia_kernel();
void* atender_cliente(void* arg);
int conectar_memoria(char *ip, char *puerto);

void handshake_kernel_con_mensaje(int socket_cliente);
void concurrencia_cpu_dispatch();
void concurrencia_cpu_interrupt();

void* planificador_largo_plazo_fifo();
void* planificador_largo_plazo_proceso_mas_chico_primero();
bool solicitar_memoria(t_pcb* pcb);
void* planificador_corto_plazo();
void* planificador_corto_plazo_fifo();
void* planificador_corto_plazo_sjf();
void* planificador_corto_plazo_srt();
bool comparar_por_memoria(void* pcb1, void* pcb2);
void planificar_y_despachar(t_pcb* pcb_desalojado_o_finalizado);
void enviar_interrupcion_a_cpu();

t_cpu* obtener_cpu_libre();
void enviar_pcb_a_cpu(t_cpu* cpu, t_pcb* pcb);

void enviar_pcb_por_socket(int socket_dispatch, t_pcb* pcb);
void recibir_respuesta_de_cpu(int socket_dispatch, t_pcb* pcb, t_cpu* cpu);

t_respuesta_ejecucion* deserializar_respuesta_cpu(void* stream);

char* nombre_syscall(op_code codigo_operacion);
t_io* buscar_io(char* nombre);

void bloquear_proceso(t_pcb* pcb, char* dispositivo, int tiempo);
void manejar_mensaje_io(int socket_origen);
void manejar_fin_io(int pid_proceso_finalizado, int socket_origen);
void find_io_iterator(char* nombre_grupo, void* data);
void despachar_io_si_es_posible(t_grupo_io* grupo);
t_pcb* buscar_pcb_por_pid(int pid);
void finalizar_proceso(t_pcb* pcb, char* motivo);
void destruir_respuesta_ejecucion(t_respuesta_ejecucion* respuesta);
bool comparar_por_rafaga_estimada(void* pcb1, void* pcb2);

void enviar_dump_a_memoria(int pid);
void gestionar_procesos_finalizados();

void* hilo_suspension_proceso(void* arg);
void solicitar_suspension_a_memoria(int pid);
bool solicitar_admision_a_memoria(t_pcb* pcb, bool viene_de_susp);

void* atender_conexion_cpu(void* arg);
void* atender_respuestas_cpu(void* arg);
t_cpu* buscar_cpu_por_nombre(char* nombre);
void* hilo_manejador_dump(void* arg);

void recibir_y_manejar_respuesta_cpu(t_cpu* cpu);
void calcular_siguiente_rafaga(t_pcb* pcb);
void bloquear_proceso_por_dump(t_pcb* pcb);

long long get_timestamp_ms();
void cambiar_estado(t_pcb* pcb, op_code_estado nuevo_estado);
void* hilo_escucha_io(void* arg);
bool _is_proceso_activo_en_io(int pid);
void desalojar_si_es_necesario_srt(t_pcb* pcb_recien_llegado);
void manejar_desconexion_io(t_io* instancia_muerta);

#endif /* KERNEL_H_ */