#ifndef IO_H_
#define IO_H_

#include <commons/config.h>
#include <commons/log.h>
#include <utils/utils.h>

extern int socket_kernel;

t_config* iniciar_config(void);
t_log* iniciar_logger(void);
void terminar_programa(t_log* logger, t_config* config);
int conectar_kernel(char* ip, char* puerto, char* io_nombre);
void escuchar_solicitudes_io(t_log* logger, t_io* dispositivo_io);
void enviar_finalizacion(t_log* logger, int pid, t_io* dispositivo_io);
void enviar_mensaje_desconexion(t_io* dispositivo_io);


#endif /* IO_H_ */