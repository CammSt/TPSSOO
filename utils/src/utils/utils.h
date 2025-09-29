#ifndef UTILS_H_
#define UTILS_H_

#include "global.h"

#define IP "127.0.0.1"

typedef struct {
	int size;
	void* stream;
} t_buffer;

typedef struct {
	op_code codigo_operacion;
	t_buffer* buffer;
} t_paquete;

int iniciar_servidor(char* puerto_escucha);
int esperar_cliente(int socket_servidor);
t_paquete* recibir_paquete(int socket_cliente);


int crear_conexion(char* ip, char* puerto);
void eliminar_paquete(t_paquete* paquete);
t_paquete* crear_paquete(op_code codigo);
void agregar_a_paquete(t_paquete* paquete, void* valor, int tamanio);
void enviar_paquete(t_paquete* paquete, int socket);
char* op_code_a_string(op_code codigo);
char* estado_a_string(op_code_estado estado);

#endif /* UTILS_H_ */