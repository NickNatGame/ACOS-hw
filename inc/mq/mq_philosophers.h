#ifndef MQ_PHILOSOPHERS_H
#define MQ_PHILOSOPHERS_H

#include "philosophers.h"

#include <mqueue.h>
#include <semaphore.h>
#include <sys/types.h>

#define MQ_PROTOCOL_VERSION 1
#define MQ_CONNECT_TIMEOUT 10000

typedef enum
{
  MQ_JOIN = 1,
  MQ_THINK,
  MQ_HUNGRY,
  MQ_FINISH,
  MQ_ABORT,
  MQ_EXIT,
  MQ_CONFIG,
  MQ_START,
  MQ_THINK_ALLOWED,
  MQ_EAT_ALLOWED,
  MQ_CONTINUE,
  MQ_STOP
} MqMessageType;

/* Передаём значения, а не указатели из адресного пространства отправителя */
typedef struct
{
  int version;
  int type;
  int id;
  pid_t pid;
  int value;
  Parameters parameters;
} MqMessage;

typedef struct
{
  char session[33];
  int owner;
  int claimed;
  int id;
  mqd_t requests;
  mqd_t replies[MAX_PHILOSOPHERS];
  sem_t *wake[MAX_PHILOSOPHERS];
  sem_t *claim[MAX_PHILOSOPHERS];
} MqResources;

void mq_resource_name(char name[96], const char *session, const char *kind, int id);
int mq_create_resources(MqResources *resources, const char *session, int count);
int mq_connect_resources(MqResources *resources, const char *session, int id);
int mq_close_resources(MqResources *resources);

/* 0 - успех, 1 - тайм-аут/сигнал, -1 - ошибка */
int mq_send_message(mqd_t queue, const MqMessage *message, int milliseconds);
int mq_receive_message(mqd_t queue, MqMessage *message, int milliseconds);
int mq_wait_notice(sem_t *semaphore, int milliseconds);
long long mq_now_ms(void);
int mq_peer_alive(pid_t pid);

int run_mq_coordinator(const char *session, const Parameters *parameters);
int run_mq_participant(const char *session, int id);

#endif
