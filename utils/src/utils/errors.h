#ifndef UTILS_ERROR_H
#define UTILS_ERROR_H

#include <commons/log.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

/**
 * @brief Inicializa el manejador de errores con el logger principal del módulo.
 * * @param logger El logger creado en el main de tu programa (ej: Memoria, Kernel).
 * * Debe ser llamada una única vez al inicio del programa.
 */
void iniciar_manejador_de_errores(t_log* logger);

/**
 * @brief Función interna que formatea y registra un mensaje de error detallado.
 * NO USAR DIRECTAMENTE. Usar la macro LOG_ERROR en su lugar.
 */
void log_error_custom(const char* file, const char* func, int line, const char* format, ...);

/**
 * @brief Macro para registrar un error. Captura automáticamente archivo, función, 
 * línea y el mensaje de error del sistema (errno).
 * * @param format El mensaje de error, como en printf.
 * @param ... Argumentos variables para el formato.
 */
#define LOG_ERROR(format, ...) log_error_custom(__FILE__, __func__, __LINE__, format, ##__VA_ARGS__)

#endif // UTILS_ERROR_H