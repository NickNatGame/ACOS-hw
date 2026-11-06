/* Подмены подключаются линкером только к тестовой программе philosophers_faults. */
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int child_process = 0;

static int selected(const char *name)
{
  const char *failure = getenv("PHILOSOPHERS_TEST_FAILURE");
  return failure != NULL && strcmp(failure, name) == 0;
}

int __real_shm_open(const char *name, int flags, mode_t mode);
int __wrap_shm_open(const char *name, int flags, mode_t mode)
{
  if (selected("shm_open"))
  {
    errno = EACCES;
    return -1;
  }
  return __real_shm_open(name, flags, mode);
}

int __real_posix_fallocate(int fd, off_t offset, off_t size);
int __wrap_posix_fallocate(int fd, off_t offset, off_t size)
{
  return selected("space") ? ENOSPC : __real_posix_fallocate(fd, offset, size);
}

void *__real_mmap(void *address, size_t size, int protection, int flags,
                   int fd, off_t offset);
void *__wrap_mmap(void *address, size_t size, int protection, int flags,
                   int fd, off_t offset)
{
  if (selected("mmap"))
  {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  return __real_mmap(address, size, protection, flags, fd, offset);
}

int __real_sem_init(sem_t *semaphore, int shared, unsigned int value);
int __wrap_sem_init(sem_t *semaphore, int shared, unsigned int value)
{
  static int calls = 0;
  calls++;
  if ((selected("mutex") && calls == 1) ||
      (selected("semaphore") && calls == 3))
  {
    errno = ENOSPC;
    return -1;
  }
  return __real_sem_init(semaphore, shared, value);
}

pid_t __real_fork(void);
pid_t __wrap_fork(void)
{
  static int calls = 0;
  calls++;
  if (selected("fork") && calls == 4)
  {
    errno = EAGAIN;
    return -1;
  }
  pid_t child = __real_fork();
  if (child == 0)
  {
    child_process = 1;
  }
  else if (child > 0 && selected("startup_signal") && calls == 2)
  {
    raise(SIGINT);
  }
  return child;
}

int __real_sem_timedwait(sem_t *semaphore, const struct timespec *deadline);
int __wrap_sem_timedwait(sem_t *semaphore, const struct timespec *deadline)
{
  static int acquired = 0;
  if (child_process && selected("sem_wait"))
  {
    errno = EINVAL;
    return -1;
  }
  int result = __real_sem_timedwait(semaphore, deadline);
  if (child_process && result == 0 && ++acquired == 2)
  {
    /* Первое получение - старт, второе - общий семафор: он останется занят. */
    if (selected("lock_death"))
    {
      _exit(7);
    }
    if (selected("lock_stop"))
    {
      raise(SIGSTOP);
    }
  }
  return result;
}

int __real_sem_post(sem_t *semaphore);
int __wrap_sem_post(sem_t *semaphore)
{
  static int calls = 0;
  if (!child_process && selected("sem_post") && ++calls == 1)
  {
    errno = EOVERFLOW;
    return -1;
  }
  return __real_sem_post(semaphore);
}

int __real_clock_gettime(clockid_t clock, struct timespec *time);
int __wrap_clock_gettime(clockid_t clock, struct timespec *time)
{
  if (selected("clock") && clock == CLOCK_MONOTONIC)
  {
    errno = EIO;
    return -1;
  }
  return __real_clock_gettime(clock, time);
}
