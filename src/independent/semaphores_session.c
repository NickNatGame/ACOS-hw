#include "semaphores.h"
#include "session.h"
#include "soft_assert.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>

static char session[33];
static int creating;
static char created[MAX_PHILOSOPHERS + 1][96];
static int created_count;

void configure_session_semaphores(const char *name, int create)
{
  snprintf(session, sizeof(session), "%s", name);
  creating = create;
  created_count = 0;
}

sem_t *open_semaphore(sem_t *storage, const char *name, unsigned int value)
{
  (void)storage;
  char full_name[96];

  snprintf(full_name, sizeof(full_name), "/philosophers-session-%s-%s", session, name);
  sem_t *semaphore;

  if (creating)
  {
    semaphore = sem_open(full_name, O_CREAT | O_EXCL, 0600, value);
  }
  else
  {
    semaphore = sem_open(full_name, 0);
  }

  if (semaphore == SEM_FAILED)
  {
    return NULL;
  }

  if (creating)
  {
    snprintf(created[created_count++], sizeof(created[0]), "%s", full_name);
  }
  return semaphore;
}

int close_semaphore(sem_t *semaphore)
{
  return sem_close(semaphore);
}

int unlink_session_semaphores(void)
{
  int fl_err = 0;
  for (int i = 0; i < created_count; i++)
  {
    SOFT_ASSERT_ERR(sem_unlink(created[i]) == 0);
  }
  if (fl_err == 0)
  {
    return 0;
  }
  return -1;
}

const char *semaphore_kind(void)
{
  return "именованные (независимые процессы)";
}
