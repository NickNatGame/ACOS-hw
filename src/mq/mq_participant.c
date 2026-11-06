#include "mq_philosophers.h"
#include "console.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct
{
  MqResources resources;
  int id;
  pid_t coordinator;
  long long next_health_check;
} MqParticipant;

static int send_request(MqParticipant *participant, int type, int value)
{
  MqMessage message = {.version = MQ_PROTOCOL_VERSION, .type = type,
      .id = participant->id, .pid = getpid(), .value = value};

  return mq_send_message(participant->resources.requests, &message, 100);
}

/* Ожидание прерывается по сигналу или исчезновению координатора */
static int receive_reply(MqParticipant *participant, MqMessage *message, int milliseconds)
{
  long long now = mq_now_ms();
  if (now < 0 || received_signal() || output_failed())
  {
    return -1;
  }

  if (participant->coordinator && now >= participant->next_health_check)
  {
    if (!mq_peer_alive(participant->coordinator))
    {
      return -1;
    }
    participant->next_health_check = now + 100;
  }

  int result = mq_wait_notice(participant->resources.wake[participant->id], milliseconds);
  if (result != 0)
  {
    return result;
  }

  result = mq_receive_message(participant->resources.replies[participant->id], message, 100);
  if (result != 0 || message->id != participant->id || message->pid <= 1 ||
      (participant->coordinator && message->pid != participant->coordinator))
  {
    return -1;
  }
  return 0;
}

static int stopped_result(const MqMessage *message)
{
  return message->value == 0 || message->value == 1 || message->value == 2 ||
         message->value == 130 || message->value == 143 ? message->value : 1;
}

static int wait_response(MqParticipant *participant, int expected, int timeout, int *value)
{
  long long since = mq_now_ms();
  if (since < 0)
  {
    return -1;
  }

  while (1)
  {
    MqMessage message;

    int result = receive_reply(participant, &message, 10);
    if (result == -1)
    {
      return -1;
    }
    if (result == 0)
    {
      if (message.type == MQ_STOP)
      {
        *value = stopped_result(&message);
        return 1;
      }
      if (message.type != expected || message.value < 0)
      {
        return -1;
      }
      if (expected == MQ_CONFIG)
      {
        if (message.parameters.count < 2 || message.parameters.count > MAX_PHILOSOPHERS ||
            participant->id >= message.parameters.count || message.parameters.cycles < 1)
        {
          return -1;
        }
        participant->coordinator = message.pid;
      }
      *value = message.value;
      return 0;
    }

    long long now = mq_now_ms();
    if (now < 0 || (timeout > 0 && now - since >= timeout))
    {
      return -1;
    }
  }
}

static int wait_duration(MqParticipant *participant, int duration, int *value)
{
  long long since = mq_now_ms();
  if (since < 0)
  {
    return -1;
  }

  while(1)
  {
    long long now = mq_now_ms();
    if (now < 0 || received_signal())
    {
      return -1;
    }

    long long remaining = since + duration - now;
    if (remaining <= 0)
    {
      return 0;
    }

    MqMessage message;
    int result = receive_reply(participant, &message, remaining > 10 ? 10 : (int)remaining);
    if (result == -1)
    {
      return -1;
    }
    if (result == 0)
    {
      if (message.type != MQ_STOP)
      {
        return -1;
      }
      *value = stopped_result(&message);
      return 1;
    }
  }
}

int run_mq_participant(const char *session, int id)
{
  MqParticipant participant = {.id = id};

  if (mq_connect_resources(&participant.resources, session, id) == -1)
  {
    dprintf(STDERR_FILENO, "Ошибка подключения MQ (сессия/номер недоступны или место занято): %s\n",
            strerror(errno));
    mq_close_resources(&participant.resources);
    return 1;
  }

  int value = 1;
  int result = -1;
  int joined = send_request(&participant, MQ_JOIN, 0) == 0;

  if (joined)
  {
    result = wait_response(&participant, MQ_CONFIG, 2000, &value);
    if (result == 0)
    {
      result = wait_response(&participant, MQ_START, MQ_CONNECT_TIMEOUT + 2000, &value);
    }
  }

  while (joined && result == 0)
  {
    if (send_request(&participant, MQ_THINK, 0) != 0)
    {
      result = -1;
      break;
    }
    result = wait_response(&participant, MQ_THINK_ALLOWED, 0, &value);
    if (result != 0)
    {
      break;
    }
    result = wait_duration(&participant, value, &value);
    if (result != 0)
    {
      break;
    }
    if (send_request(&participant, MQ_HUNGRY, 0) != 0)
    {
      result = -1;
      break;
    }
    result = wait_response(&participant, MQ_EAT_ALLOWED, 0, &value);
    if (result != 0)
    {
      break;
    }
    result = wait_duration(&participant, value, &value);
    if (result != 0)
    {
      break;
    }
    if (send_request(&participant, MQ_FINISH, 0) != 0)
    {
      result = -1;
      break;
    }

    /* После последнего цикла координатор пришлёт общий MQ_STOP вместо CONTINUE */
    result = wait_response(&participant, MQ_CONTINUE, 0, &value);
  }

  int exit_code = result == 1 ? value : (received_signal() ? 128 + received_signal() : 1);
  if (joined)
  {
    if (result != 1)
    {
      dprintf(STDERR_FILENO, "Ошибка ожидания/отправки сообщения или прерывание философа %d.\n", id + 1);
      send_request(&participant, MQ_ABORT, exit_code);
    }
    if (send_request(&participant, MQ_EXIT, exit_code) != 0)
    {
      exit_code = 1;
    }
  }
  if (mq_close_resources(&participant.resources) == -1)
  {
    exit_code = 1;
  }
  return exit_code;
}
