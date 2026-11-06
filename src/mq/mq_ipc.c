#include "mq_philosophers.h"
#include "soft_assert.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

void mq_resource_name(char name[96], const char *session, const char *kind, int id)
{
  if (id < 0)
  {
    snprintf(name, 96, "/philosophers-mq-%s-%s", session, kind);
  }
  else
  {
    snprintf(name, 96, "/philosophers-mq-%s-%s-%d", session, kind, id);
  }
}

static void init_resources(MqResources *resources, const char *session, int owner)
{
  memset(resources, 0, sizeof(*resources));
  snprintf(resources->session, sizeof(resources->session), "%s", session);
  resources->owner = owner;
  resources->requests = (mqd_t)-1;
  for (int i = 0; i < MAX_PHILOSOPHERS; i++)
  {
    resources->replies[i] = (mqd_t)-1;
  }
}

static mqd_t open_queue(const char *name, int flags)
{
  struct mq_attr attributes = {0};
  attributes.mq_maxmsg = 4;
  attributes.mq_msgsize = sizeof(MqMessage);
  mqd_t queue = mq_open(name, flags, 0600, &attributes);

  if (queue != (mqd_t)-1 && !(flags & O_CREAT))
  {
    if (mq_getattr(queue, &attributes) == -1 ||
        attributes.mq_msgsize != (long)sizeof(MqMessage))
    {
      mq_close(queue);
      errno = EPROTO;
      return (mqd_t)-1;
    }
  }

  return queue;
}

int mq_create_resources(MqResources *resources, const char *session, int count)
{
  init_resources(resources, session, 1);
  char name[96];
  mq_resource_name(name, session, "requests", -1);

  resources->requests = open_queue(name, O_RDONLY | O_CREAT | O_EXCL);
  if (resources->requests == (mqd_t)-1)
  {
    return -1;
  }

  for (int i = 0; i < count; i++)
  {
    mq_resource_name(name, session, "reply", i);
    resources->replies[i] = open_queue(name, O_WRONLY | O_CREAT | O_EXCL);
    if (resources->replies[i] == (mqd_t)-1)
    {
      return -1;
    }

    mq_resource_name(name, session, "wake", i);
    sem_t *wake = sem_open(name, O_CREAT | O_EXCL, 0600, 0);
    if (wake == SEM_FAILED)
    {
      return -1;
    }
    resources->wake[i] = wake;

    mq_resource_name(name, session, "claim", i);
    sem_t *claim = sem_open(name, O_CREAT | O_EXCL, 0600, 1);
    if (claim == SEM_FAILED)
    {
      return -1;
    }
    resources->claim[i] = claim;
  }
  return 0;
}

int mq_connect_resources(MqResources *resources, const char *session, int id)
{
  init_resources(resources, session, 0);
  resources->id = id;
  char name[96];
  mq_resource_name(name, session, "requests", -1);

  resources->requests = open_queue(name, O_WRONLY);
  if (resources->requests == (mqd_t)-1)
  {
    return -1;
  }

  /* До получения ответов занимаем место, так второй читатель не украдёт сообщение */
  mq_resource_name(name, session, "claim", id);
  sem_t *claim = sem_open(name, 0);
  if (claim == SEM_FAILED)
  {
    return -1;
  }
  resources->claim[id] = claim;
  if (sem_trywait(claim) == -1)
  {
    return -1;
  }
  resources->claimed = 1;

  /* Тут ждем уведомления */
  mq_resource_name(name, session, "wake", id);
  sem_t *wake = sem_open(name, 0);
  if (wake == SEM_FAILED)
  {
    return -1;
  }
  resources->wake[id] = wake;

  /* Читаем только свои ответы */
  mq_resource_name(name, session, "reply", id);
  resources->replies[id] = open_queue(name, O_RDONLY);

  return resources->replies[id] == (mqd_t)-1 ? -1 : 0;
}

int mq_close_resources(MqResources *resources)
{
  int fl_err = 0;
  char name[96];
  if (resources->claimed)
  {
    SOFT_ASSERT_ERR(sem_post(resources->claim[resources->id]) == 0);
  }

  for (int i = 0; i < MAX_PHILOSOPHERS; i++)
  {
    if (resources->replies[i] != (mqd_t)-1)
    {
      SOFT_ASSERT_ERR(mq_close(resources->replies[i]) == 0);
      if (resources->owner)
      {
        mq_resource_name(name, resources->session, "reply", i);
        SOFT_ASSERT_ERR(mq_unlink(name) == 0);
      }
    }

    sem_t *semaphores[] = {resources->wake[i], resources->claim[i]};
    const char *kinds[] = {"wake", "claim"};
    for (int j = 0; j < 2; j++)
    {
      if (semaphores[j])
      {
        SOFT_ASSERT_ERR(sem_close(semaphores[j]) == 0);
        if (resources->owner)
        {
          mq_resource_name(name, resources->session, kinds[j], i);
          SOFT_ASSERT_ERR(sem_unlink(name) == 0);
        }
      }
    }

  }

  /* Общую очередь удаляем последней, т.к. до этого имя сессии ещё занято */
  if (resources->requests != (mqd_t)-1)
  {
    SOFT_ASSERT_ERR(mq_close(resources->requests) == 0);
    if (resources->owner)
    {
      mq_resource_name(name, resources->session, "requests", -1);
      SOFT_ASSERT_ERR(mq_unlink(name) == 0);
    }
  }

  return fl_err ? -1 : 0;
}

static int deadline_after(struct timespec *deadline, int milliseconds)
{
  if (clock_gettime(CLOCK_REALTIME, deadline) == -1)
  {
    return -1;
  }
  deadline->tv_sec += milliseconds / 1000;
  deadline->tv_nsec += (long)(milliseconds % 1000) * 1000000L;
  if (deadline->tv_nsec >= 1000000000L)
  {
    deadline->tv_sec++;
    deadline->tv_nsec -= 1000000000L;
  }
  return 0;
}

static int wait_error(void)
{
  return errno == EINTR || errno == ETIMEDOUT || errno == EAGAIN ? 1 : -1;
}

int mq_send_message(mqd_t queue, const MqMessage *message, int milliseconds)
{
  struct timespec deadline;
  if (deadline_after(&deadline, milliseconds) == -1)
  {
    return -1;
  }

  /* Запросы идут FIFO, но STOP обгоняет устаревшие ответы при аварии */
  unsigned int priority = message->type == MQ_STOP ? 1 : 0;
  return mq_timedsend(queue, (const char *)message, sizeof(*message), priority, &deadline) == 0
      ? 0 : wait_error();
}

int mq_receive_message(mqd_t queue, MqMessage *message, int milliseconds)
{
  struct timespec deadline;
  if (deadline_after(&deadline, milliseconds) == -1)
  {
    return -1;
  }

  unsigned int priority;
  ssize_t size = mq_timedreceive(queue, (char *)message, sizeof(*message), &priority, &deadline);
  if (size == -1)
  {
    return wait_error();
  }

  if (size != (ssize_t)sizeof(*message) || message->version != MQ_PROTOCOL_VERSION ||
      priority != (unsigned int)(message->type == MQ_STOP ? 1 : 0))
  {
    errno = EPROTO;
    return -1;
  }
  return 0;
}

int mq_wait_notice(sem_t *semaphore, int milliseconds)
{
  struct timespec deadline;
  if (deadline_after(&deadline, milliseconds) == -1)
  {
    return -1;
  }

  return sem_timedwait(semaphore, &deadline) == 0 ? 0 : wait_error();
}

long long mq_now_ms(void)
{
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
  {
    return -1;
  }
  return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int mq_peer_alive(pid_t pid)
{
  if (pid <= 1 || (kill(pid, 0) == -1 && errno == ESRCH))
  {
    return 0;
  }
  char path[64], line[1024];
  snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
  FILE *file = fopen(path, "r");
  int alive = 1;

  if (file)
  {
    if (fgets(line, sizeof(line), file))
    {
      char *end = strrchr(line, ')');
      if (end && end[1] == ' ' && end[2])
      {
        alive = end[2] != 'Z' && end[2] != 'X' && end[2] != 'T' && end[2] != 't';
      }
    }
    fclose(file);
  }

  return alive;
}
