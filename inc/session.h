#ifndef PHILOSOPHERS_SESSION_H
#define PHILOSOPHERS_SESSION_H

#include "philosophers.h"

int valid_session_name(const char *name);
void configure_session_semaphores(const char *name, int create);
int unlink_session_semaphores(void);
int run_coordinator(const char *name, const Parameters *parameters);
int run_participant(const char *name, int id);

#endif
