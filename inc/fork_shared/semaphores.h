#ifndef PHILOSOPHERS_SEMAPHORES_H
#define PHILOSOPHERS_SEMAPHORES_H

#include <semaphore.h>

/* storage используется неименованной версией, name - именованной */
sem_t *open_semaphore(sem_t *storage, const char *name, unsigned int value);
int close_semaphore(sem_t *semaphore);
const char *semaphore_kind(void);

#endif
