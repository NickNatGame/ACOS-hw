#include "semaphores.h"

#include <stddef.h>

sem_t *open_semaphore(sem_t *storage, const char *name, unsigned int value)
{
  (void)name;
  return sem_init(storage, 1, value) == 0 ? storage : NULL;
}

int close_semaphore(sem_t *semaphore)
{
  return sem_destroy(semaphore);
}

const char *semaphore_kind(void)
{
  return "неименованные";
}
