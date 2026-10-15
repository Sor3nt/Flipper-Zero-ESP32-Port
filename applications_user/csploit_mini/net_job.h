#pragma once
#include "backend.h"
#include "net_protocol.h"
#include <furi.h>
#include <stdatomic.h>
typedef enum {
  JobDiscover,
  JobScan,
  JobDnsLookup,
  JobHttp,
  JobTcp,
  JobUdp,
  JobArp,
  JobDnsServer,
  JobLlmnr,
  JobNbns,
  JobExposure
} JobKind;
typedef struct {
  char target[16], name[64], reply[16], dns[16], payload[128];
  unsigned first_port, last_port, port, timeout_ms;
  bool redirect;
} NetConfig;
typedef struct {
  char text[128];
} NetEvent;
typedef struct {
  FuriThread *thread;
  FuriMessageQueue *events;
  atomic_bool cancel, done;
  atomic_uint dropped;
  JobKind kind;
  NetConfig config;
  const CsNetApi *api;
} NetJob;
NetJob *net_start(JobKind kind, const NetConfig *config);
bool net_poll(NetJob *job, NetEvent *event);
void net_cancel(NetJob *job);
void net_free(NetJob *job);
const char *net_job_name(JobKind kind);
