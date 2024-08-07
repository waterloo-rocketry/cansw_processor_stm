#ifndef __LOG
#define __LOG

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "stm32h7xx_hal.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"


// TODO: determine optimal numbers for these
/* max length of log data string (bytes) */
#define MAX_MSG_LENGTH 256
/* number of full-length msgs in 1 buffer */
#define MSGS_PER_BUFFER 64
/* size of one log buffer (bytes) */
#define LOG_BUFFER_SIZE MAX_MSG_LENGTH * MSGS_PER_BUFFER
/* number of log buffers */
#define NUM_LOG_BUFFERS 2

/**
 * Log Level
*/
typedef enum {
    LOG_LVL_ERROR = 'E',  // Errors (non-recoverable)
    LOG_LVL_WARN = 'W',   // Warnings (recoverable issues)
    LOG_LVL_INFO = 'I',   // Info (data from sensors, etc)
    LOG_LVL_DEBUG = 'D',  // Only for debugging on the ground
} LogLevel_t;

/**
 * Create buffers, mutexes, etc
*/
bool logInit(void);

/**
 * Log an error-level message
*/
bool logError(const char* source, const char* msg, ...);

/**
 * Log an info-level message
*/
bool logInfo(const char* source, const char* msg, ...);

/**
 * Log a debug-level message
*/
bool logDebug(const char* source, const char* msg, ...);

/**
 * FreeRTOS task for the logger
 * Wait on buffers to become full, then dump buffer to SD/uart
*/
void logTask(void *argument);


#ifdef __cplusplus
}
#endif

#endif
