/* Подключается только к тестовым программам через --wrap. */
#include "mq_philosophers.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int selected(const char *failure)
{
  const char *value = getenv("PHILOSOPHERS_MQ_FAILURE");
  return value && strcmp(value, failure) == 0;
}

mqd_t __real_mq_open(const char *name, int flags, ...);
int __real_mq_timedsend(mqd_t queue, const char *message, size_t size,
                       unsigned int priority, const struct timespec *deadline);
ssize_t __real_mq_timedreceive(mqd_t queue, char *message, size_t size,
                             unsigned int *priority, const struct timespec *deadline);
sem_t *__real_sem_open(const char *name, int flags, ...);
int __real_sem_post(sem_t *semaphore);
int __real_clock_gettime(clockid_t clock, struct timespec *time);

mqd_t __wrap_mq_open(const char *name, int flags, ...)
{
  static int calls;
  calls++;
  if ((selected("mq_open") && calls == 1) || (selected("mq_partial") && calls == 3))
  {
    errno = ENOSPC;
    return (mqd_t)-1;
  }
  if (flags & O_CREAT)
  {
    va_list arguments;
    va_start(arguments, flags);
    mode_t mode = va_arg(arguments, mode_t);
    struct mq_attr *attributes = va_arg(arguments, struct mq_attr *);
    va_end(arguments);
    return __real_mq_open(name, flags, mode, attributes);
  }
  return __real_mq_open(name, flags);
}

int __wrap_mq_timedsend(mqd_t queue, const char *message, size_t size,
                      unsigned int priority, const struct timespec *deadline)
{
  static int calls;
  if (++calls == 1 && selected("mq_send"))
  {
    errno = ETIMEDOUT;
    return -1;
  }
  return __real_mq_timedsend(queue, message, size, priority, deadline);
}

ssize_t __wrap_mq_timedreceive(mqd_t queue, char *message, size_t size,
                             unsigned int *priority, const struct timespec *deadline)
{
  static int calls;
  if (++calls == 1 && selected("mq_receive"))
  {
    errno = EIO;
    return -1;
  }
  return __real_mq_timedreceive(queue, message, size, priority, deadline);
}

sem_t *__wrap_sem_open(const char *name, int flags, ...)
{
  static int calls;
  if (++calls == 3 && selected("sem_open"))
  {
    errno = ENOSPC;
    return SEM_FAILED;
  }
  if (flags & O_CREAT)
  {
    va_list arguments;
    va_start(arguments, flags);
    mode_t mode = va_arg(arguments, mode_t);
    unsigned int value = va_arg(arguments, unsigned int);
    va_end(arguments);
    return __real_sem_open(name, flags, mode, value);
  }
  return __real_sem_open(name, flags);
}

int __wrap_sem_post(sem_t *semaphore)
{
  static int calls;
  if (++calls == 1 && selected("sem_post"))
  {
    errno = EOVERFLOW;
    return -1;
  }
  return __real_sem_post(semaphore);
}

int __wrap_clock_gettime(clockid_t clock, struct timespec *time)
{
  static int calls;
  if (++calls == 1 && selected("clock"))
  {
    errno = EIO;
    return -1;
  }
  return __real_clock_gettime(clock, time);
}
