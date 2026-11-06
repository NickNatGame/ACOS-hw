#include "semaphores.h"
#include "soft_assert.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

sem_t *open_semaphore(sem_t *storage, const char *name, unsigned int value)
{
  (void)storage;
  char full_name[96];

  int length = snprintf(full_name, sizeof(full_name), "/philosophers-%ld-%s",
                         (long)getpid(), name);
  if (length < 0 || (size_t)length >= sizeof(full_name))
  {
    errno = ENAMETOOLONG;
    return NULL;
  }

  sem_t *semaphore = sem_open(full_name, O_CREAT | O_EXCL, 0600, value);
  if (semaphore == SEM_FAILED)
  {
    return NULL;
  }

  /* Дети наследуют открытый семафор, имя больше не требуется */
  if (sem_unlink(full_name) == -1)
  {
    int error = errno;
    int fl_err = 0;
    SOFT_ASSERT_ERR(sem_close(semaphore) == 0);

    /* Повторяем удаление своего объекта при ошибке первого sem_unlink */
    SOFT_ASSERT_ERR(sem_unlink(full_name) == 0);
    if (fl_err != 0)
    {
      dprintf(STDERR_FILENO, "Ошибка очистки семафора %s.\n", full_name);
    }
    errno = error;
    return NULL;
  }
  return semaphore;
}

int close_semaphore(sem_t *semaphore)
{
  return sem_close(semaphore);
}

const char *semaphore_kind(void)
{
  return "именованные";
}
