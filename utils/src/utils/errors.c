
#include "error.h"

#include <stdio.h>
#include <stddef.h> 
#include <stdarg.h>
#include <string.h>
#include <errno.h> 
#include <commons/log.h> 

static t_log* logger_global = NULL;

void iniciar_manejador_de_errores(t_log* logger) {
    if (logger != NULL) {
        logger_global = logger;
    }
}

void log_error_custom(const char* file, const char* func, int line, const char* format, ...) {
    if (logger_global == NULL) {
        // Fallback por si alguien olvida llamar a iniciar_manejador_de_errores
        printf("ERROR: El manejador de errores no fue inicializado. Mensaje original: %s\n", format);
        return;
    }

    // Buffer para nuestro mensaje de error enriquecido
    char buffer[1024];
    
    // 1. Añadimos el prefijo con el contexto
    int len = snprintf(buffer, sizeof(buffer), "[ %s:%d | %s() ] ", file, line, func);

    // 2. Añadimos el mensaje de error específico del programador
    va_list args;
    va_start(args, format);
    vsnprintf(buffer + len, sizeof(buffer) - len, format, args);
    va_end(args);

    // 3. (Opcional pero recomendado) Añadimos el error del sistema
    if (errno != 0) {
        snprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer), " (System Error: %s)", strerror(errno));
    }
    
    // 4. Enviamos el mensaje completo al logger de commons
    log_error(logger_global, "%s", buffer);

    // 5. Reseteamos errno para no arrastrar el error a la siguiente llamada
    errno = 0;
}