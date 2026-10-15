#include "net_job.h"
#include <errno.h>
#include <lwip/sockets.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wifi/wlan_hal.h>
static const char *const names[] = {
    "LAN discovery",         "TCP port scan",       "DNS lookup",
    "HTTP headers",          "TCP terminal",        "UDP terminal",
    "ARP relay test",        "DNS redirect server", "LLMNR name responder",
    "NBT-NS name responder", "Exposure indicator"};
const char *net_job_name(JobKind k) {
  return (unsigned)k < 11 ? names[k] : "?";
}
static uint32_t now_ms(void) {
  return (uint64_t)furi_get_tick() * 1000u / furi_kernel_get_tick_frequency();
}
static void log_line(NetJob *j, const char *fmt, ...) {
  NetEvent e;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.text, sizeof e.text, fmt, ap);
  va_end(ap);
  if (furi_message_queue_put(j->events, &e, 0) != FuriStatusOk)
    atomic_fetch_add(&j->dropped, 1);
}
static bool cancelled(NetJob *j) { return atomic_load(&j->cancel); }
static bool nonblock(int fd) {
  int one = 1;
  return lwip_ioctl(fd, FIONBIO, &one) == 0;
}
static bool address(const char *ip, unsigned port, struct sockaddr_in *out) {
  uint8_t bytes[4];
  if (!net_ip_parse(ip, bytes) || port < 1 || port > 65535)
    return false;
  memset(out, 0, sizeof(*out));
  out->sin_family = AF_INET;
  out->sin_port = htons(port);
  memcpy(&out->sin_addr.s_addr, bytes, 4);
  return true;
}
static int connection(NetJob *j, const char *ip, unsigned port,
                      unsigned timeout, int *result) {
  *result = -1;
  if (!j->api)
    return -1;
  struct sockaddr_in to;
  if (!address(ip, port, &to))
    return -1;
  int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  if (!nonblock(fd)) {
    lwip_close(fd);
    return -1;
  }
  int rc = lwip_connect(fd, (struct sockaddr *)&to, sizeof to);
  if (rc < 0 && errno != EINPROGRESS && errno != EALREADY &&
      errno != EWOULDBLOCK) {
    *result = -errno;
    lwip_close(fd);
    return -1;
  }
  uint32_t start = now_ms();
  while (!cancelled(j) && now_ms() - start < timeout) {
    int status = j->api->connect_status(fd);
    if (status == 1) {
      *result = 1;
      return fd;
    }
    if (status < 0) {
      *result = status;
      lwip_close(fd);
      return -1;
    }
    furi_delay_ms(5);
  }
  *result = -ETIMEDOUT;
  lwip_close(fd);
  return -1;
}
static const char *port_name(unsigned p) {
  switch (p) {
  case 21:
    return "FTP?";
  case 22:
    return "SSH?";
  case 23:
    return "Telnet?";
  case 25:
    return "SMTP?";
  case 53:
    return "DNS?";
  case 80:
    return "HTTP?";
  case 443:
    return "HTTPS?";
  case 445:
    return "SMB?";
  case 3389:
    return "RDP?";
  default:
    return "unknown";
  }
}
static void scan(NetJob *j) {
  if (!j->api) {
    log_line(j, "[NEEDS FW] Network extension missing");
    return;
  }
  unsigned open = 0, checked = 0;
  log_line(j, "[SCAN] %s TCP %u-%u", j->config.target, j->config.first_port,
           j->config.last_port);
  for (unsigned p = j->config.first_port;
       p <= j->config.last_port && !cancelled(j); p++) {
    int result;
    int fd = connection(j, j->config.target, p, j->config.timeout_ms, &result);
    checked++;
    if (fd >= 0) {
      open++;
      log_line(j, "[OPEN] TCP %u %s (port-name guess)", p, port_name(p));
      if (p == 21 || p == 22 || p == 25) {
        char b[81];
        for (unsigned k = 0; k < 5 && !cancelled(j); k++) {
          int n = lwip_recv(fd, b, sizeof b - 1, 0);
          if (n > 0) {
            b[n] = 0;
            for (int i = 0; i < n; i++)
              if ((unsigned char)b[i] < 32 || (unsigned char)b[i] > 126)
                b[i] = ' ';
            log_line(j, "[BANNER] %s", b);
            break;
          }
          furi_delay_ms(20);
        }
      }
      lwip_close(fd);
    }
    if (checked % 25 == 0)
      log_line(j, "[SCAN] %u ports checked, %u open", checked, open);
  }
  log_line(j, "[SCAN] %u checked, %u open; timeouts unknown", checked, open);
}
static void discover(NetJob *j) {
  CsNetInfo info;
  if (!j->api || !j->api->info(&info)) {
    log_line(j, "[NEEDS FW/WIFI] Connect in Wi-Fi app first");
    return;
  }
  uint8_t host[4];
  memcpy(host, info.local_ip, 4);
  unsigned found = 0;
  log_line(j, "[DISCOVER] Local /24, constrained by netmask");
  log_line(j, "[DISCOVER] TCP 80/443/445; silence is unknown");
  const unsigned ports[] = {80, 443, 445};
  for (unsigned h = 1; h < 255 && !cancelled(j); h++) {
    host[3] = h;
    bool same = true;
    for (unsigned b = 0; b < 4; b++)
      if ((host[b] & info.mask[b]) != (info.local_ip[b] & info.mask[b]))
        same = false;
    if (!same || !memcmp(host, info.local_ip, 4))
      continue;
    char ip[16];
    net_ip_text(host, ip);
    for (unsigned p = 0; p < 3 && !cancelled(j); p++) {
      int result;
      int fd = connection(j, ip, ports[p], j->config.timeout_ms, &result);
      if (fd >= 0)
        lwip_close(fd);
      if (result == 1 || result == -ECONNREFUSED) {
        log_line(j, "[HOST] %s %s", ip,
                 result == 1 ? "open service" : "TCP rejection");
        found++;
        break;
      }
    }
    if (h % 32 == 0)
      log_line(j, "[DISCOVER] Through .%u; %u responding", h, found);
  }
  log_line(j, "[DISCOVER] %u responding hosts", found);
}
static bool send_all(NetJob *j, int fd, const uint8_t *bytes, size_t n) {
  uint32_t start = now_ms();
  size_t sent = 0;
  while (sent < n && !cancelled(j) && now_ms() - start < 3000) {
    int z = lwip_send(fd, bytes + sent, n - sent, 0);
    if (z > 0)
      sent += z;
    else if (z < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      return false;
    else
      furi_delay_ms(10);
  }
  return sent == n;
}
static void show_bytes(NetJob *j, const uint8_t *p, size_t n) {
  char b[97];
  for (size_t at = 0; at < n; at += 96) {
    size_t len = n - at > 96 ? 96 : n - at;
    for (size_t i = 0; i < len; i++) {
      unsigned c = p[at + i];
      b[i] = c >= 32 && c < 127 ? c : ' ';
    }
    b[len] = 0;
    log_line(j, "[RX] %s", b);
  }
}
static void terminal(NetJob *j) {
  bool udp = j->kind == JobUdp, http = j->kind == JobHttp;
  int fd = -1, result;
  struct sockaddr_in to;
  if (!address(j->config.target, j->config.port, &to)) {
    log_line(j, "[ERROR] Invalid target/port");
    return;
  }
  if (udp) {
    fd = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0 && (!nonblock(fd) ||
                    lwip_connect(fd, (struct sockaddr *)&to, sizeof to) < 0)) {
      lwip_close(fd);
      fd = -1;
    }
  } else
    fd = connection(j, j->config.target, j->config.port, 3000, &result);
  if (fd < 0) {
    log_line(j, "[ERROR] Connection failed / needs firmware");
    return;
  }
  char request[320];
  size_t len;
  if (http)
    len = snprintf(request, sizeof request,
                   "HEAD / HTTP/1.0\r\nHost: %s\r\nConnection: "
                   "close\r\nUser-Agent: T-Embed Native Tools-diagnostic/1.0\r\n\r\n",
                   j->config.name);
  else
    len = snprintf(request, sizeof request, "%s\n", j->config.payload);
  bool sent = udp ? lwip_send(fd, request, len, 0) == (int)len
                  : send_all(j, fd, (const uint8_t *)request, len);
  if (!sent) {
    log_line(j, "[ERROR] Send failed");
    lwip_close(fd);
    return;
  }
  log_line(j, "[TX] %u bytes to %s:%u", (unsigned)len, j->config.target,
           j->config.port);
  uint32_t start = now_ms();
  size_t total = 0;
  bool headers_done = false;
  uint8_t b[257];
  unsigned tail = 0;
  while (!cancelled(j) && now_ms() - start < 5000 && total < 4096 &&
         !headers_done) {
    int n = lwip_recv(fd, b, sizeof b - 1, 0);
    if (n > 0) {
      size_t use = n;
      for (int i = 0; i < n && http; i++) {
        tail = (tail << 8) | b[i];
        if (tail == 0x0d0a0d0a) {
          use = i + 1;
          headers_done = true;
          break;
        }
      }
      show_bytes(j, b, use);
      total += use;
      if (udp)
        break;
    } else if (!n)
      break;
    else if (errno != EAGAIN && errno != EWOULDBLOCK)
      break;
    else
      furi_delay_ms(10);
  }
  memset(b, 0, sizeof b);
  memset(request, 0, sizeof request);
  lwip_close(fd);
  log_line(j, "[RESULT] %u bytes received (limit 4096)", (unsigned)total);
}
static void lookup(NetJob *j) {
  uint8_t request[512], answer[512], ips[8][4];
  uint16_t id = (uint16_t)now_ms();
  size_t n = ns_query(j->config.name, id, request, sizeof request);
  struct sockaddr_in to;
  if (!n || !address(j->config.dns, 53, &to)) {
    log_line(j, "[ERROR] Invalid hostname or DNS address");
    return;
  }
  int fd = lwip_socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return;
  if (!nonblock(fd) ||
      lwip_connect(fd, (struct sockaddr *)&to, sizeof to) < 0 ||
      lwip_send(fd, request, n, 0) != (int)n) {
    log_line(j, "[ERROR] DNS send failed");
    lwip_close(fd);
    return;
  }
  uint32_t start = now_ms();
  bool received = false;
  while (!cancelled(j) && now_ms() - start < 3000) {
    int z = lwip_recv(fd, answer, sizeof answer, 0);
    if (z >= 12 && answer[0] == request[0] && answer[1] == request[1]) {
      unsigned count = ns_addresses(answer, z, id, ips, 8);
      for (unsigned i = 0; i < count; i++) {
        char ip[16];
        net_ip_text(ips[i], ip);
        log_line(j, "[DNS] %s -> %s", j->config.name, ip);
      }
      if (!count)
        log_line(j, "[DNS] No IPv4 answer; rcode=%u TC=%u", answer[3] & 15,
                 !!(answer[2] & 2));
      received = true;
      break;
    }
    furi_delay_ms(10);
  }
  if (!received)
    log_line(j, "[DNS] Timed out");
  lwip_close(fd);
}
static int listener(unsigned port, int type) {
  int fd = lwip_socket(AF_INET, type, 0);
  if (fd < 0)
    return -1;
  int one = 1;
  lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
  if (!nonblock(fd) ||
      lwip_bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
    lwip_close(fd);
    return -1;
  }
  return fd;
}
static void name_server(NetJob *j) {
  NsProtocol proto = j->kind == JobDnsServer ? NsDns
                     : j->kind == JobLlmnr   ? NsLlmnr
                                             : NsNbns;
  unsigned port = proto == NsDns ? 53 : proto == NsLlmnr ? 5355 : 137;
  uint8_t ip[4];
  if (!net_ip_parse(j->config.reply, ip) ||
      (proto == NsNbns && strlen(j->config.name) > 15)) {
    log_line(j, "[ERROR] Reply address/name invalid (NBNS max 15)");
    return;
  }
  int fd = listener(port, SOCK_DGRAM);
  if (fd < 0) {
    log_line(j, "[ERROR] Port %u bind failed / already in use", port);
    return;
  }
  struct ip_mreq group = {0};
  bool joined = false;
  if (proto == NsLlmnr) {
    uint8_t bytes[] = {224, 0, 0, 252};
    memcpy(&group.imr_multiaddr.s_addr, bytes, 4);
    int ttl = 255;
    if (lwip_setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group,
                        sizeof group) < 0 ||
        lwip_setsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, sizeof ttl) < 0) {
      log_line(j, "[ERROR] LLMNR multicast setup failed");
      lwip_close(fd);
      return;
    }
    joined = true;
  }
  log_line(j, "[LISTEN] UDP %u; exact name %s", port, j->config.name);
  log_line(j, "[ANSWER] %s; TTL 30s; BACK stops", j->config.reply);
  if (proto == NsDns)
    log_line(j, "[DNS] Client must use this device as DNS");
  uint8_t query[513], reply[512];
  unsigned requests = 0, answers = 0;
  uint32_t window = now_ms();
  unsigned rate = 0;
  while (!cancelled(j) && wlan_hal_is_connected()) {
    struct sockaddr_in from;
    socklen_t flen = sizeof from;
    int n = lwip_recvfrom(fd, query, sizeof query, 0, (struct sockaddr *)&from,
                          &flen);
    if (n < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        log_line(j, "[ERROR] UDP receive failed");
        break;
      }
      furi_delay_ms(10);
      continue;
    }
    uint32_t now = now_ms();
    if (now - window >= 1000) {
      window = now;
      rate = 0;
    }
    if (++rate > 20)
      continue;
    requests++;
    size_t bytes =
        ns_answer(query, n, proto, j->config.name, ip, reply, sizeof reply);
    if (bytes) {
      bool sent = lwip_sendto(fd, reply, bytes, 0, (struct sockaddr *)&from,
                              flen) == (int)bytes;
      char source[16];
      net_ip_text((uint8_t *)&from.sin_addr.s_addr, source);
      if (sent)
        answers++;
      log_line(j, "[%s] %s -> %s (%s)", sent ? "REPLY" : "TX ERROR", source,
               j->config.name, j->config.reply);
    }
  }
  if (joined)
    lwip_setsockopt(fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &group, sizeof group);
  lwip_close(fd);
  log_line(j, "[STOP] %u queries, %u answers", requests, answers);
}
static void exposure(NetJob *j) {
  unsigned port = j->config.port;
  int fd = listener(port, SOCK_STREAM);
  if (fd < 0 || lwip_listen(fd, 2) < 0) {
    if (fd >= 0)
      lwip_close(fd);
    log_line(j, "[ERROR] Test port unavailable");
    return;
  }
  log_line(j, "[TEST] HTTP exposure indicator, port %u", port);
  log_line(j, "[PRIVACY] No challenges. Values discarded.");
  log_line(j, "[LIMIT] HTTP only; no NTLM/SMB collection.");
  unsigned contacts = 0, auth = 0;
  while (!cancelled(j) && wlan_hal_is_connected()) {
    struct sockaddr_in from;
    socklen_t flen = sizeof from;
    int c = lwip_accept(fd, (struct sockaddr *)&from, &flen);
    if (c < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        break;
      furi_delay_ms(20);
      continue;
    }
    contacts++;
    if (!nonblock(c)) {
      lwip_close(c);
      continue;
    }
    uint8_t request[2049];
    size_t used = 0;
    uint32_t start = now_ms();
    bool complete = false;
    while (!cancelled(j) && now_ms() - start < 1500 &&
           used < sizeof request - 1) {
      int n = lwip_recv(c, request + used, sizeof request - 1 - used, 0);
      if (n > 0) {
        used += n;
        request[used] = 0;
        if (strstr((char *)request, "\r\n\r\n")) {
          complete = true;
          break;
        }
      } else if (!n)
        break;
      else if (errno != EAGAIN && errno != EWOULDBLOCK)
        break;
      else
        furi_delay_ms(10);
    }
    bool present = net_http_auth_present(request, used);
    if (present)
      auth++;
    volatile uint8_t *wipe = request;
    for (size_t i = 0; i < sizeof request; i++)
      wipe[i] = 0;
    char ip[16];
    net_ip_text((uint8_t *)&from.sin_addr.s_addr, ip);
    log_line(j, "[CONTACT] %s auth-header=%s headers=%s", ip,
             present ? "present; discarded" : "absent",
             complete ? "complete" : "partial");
    const char response[] = "HTTP/1.0 200 OK\r\nContent-Type: "
                            "text/plain\r\nConnection: close\r\n\r\nExposure "
                            "test endpoint. Credential values are discarded.\n";
    send_all(j, c, (const uint8_t *)response, sizeof response - 1);
    lwip_close(c);
  }
  lwip_close(fd);
  log_line(j, "[STOP] %u contacts, %u auth-header indicators", contacts, auth);
}
static bool send_arp(NetJob *j, const CsNetInfo *i, const CsScope *s,
                     bool restore) {
  uint8_t frame[60];
  bool ok;
  net_arp(frame, true, i->local_mac, s->target_mac,
          restore ? s->gateway_mac : i->local_mac, i->gateway_ip, s->target_mac,
          s->target_ip);
  ok = j->api->send(frame, sizeof frame);
  net_arp(frame, true, i->local_mac, s->gateway_mac,
          restore ? s->target_mac : i->local_mac, s->target_ip, s->gateway_mac,
          i->gateway_ip);
  return j->api->send(frame, sizeof frame) && ok;
}
static void arp_test(NetJob *j) {
  CsNetInfo info;
  CsScope scope = {0};
  if (!j->api || !j->api->info(&info)) {
    log_line(j, "[NEEDS FW/WIFI] Packet interface unavailable");
    return;
  }
  if (!net_ip_parse(j->config.target, scope.target_ip) ||
      scope.target_ip[0] == 0 || scope.target_ip[0] >= 224 ||
      !memcmp(scope.target_ip, info.local_ip, 4) ||
      !memcmp(scope.target_ip, info.gateway_ip, 4)) {
    log_line(j, "[ERROR] Select one other LAN host");
    return;
  }
  bool host_bits = false, broadcast = true;
  for (unsigned k = 0; k < 4; k++) {
    if ((scope.target_ip[k] & info.mask[k]) !=
        (info.local_ip[k] & info.mask[k])) {
      log_line(j, "[ERROR] Target outside current subnet");
      return;
    }
    unsigned host = scope.target_ip[k] & ~info.mask[k];
    host_bits |= host != 0;
    broadcast &= host == ((~info.mask[k]) & 255);
  }
  if (!host_bits || broadcast) {
    log_line(j, "[ERROR] Network/broadcast address refused");
    return;
  }
  if (!j->api->open(&scope)) {
    log_line(j, "[ERROR] Packet receiver busy/unavailable");
    return;
  }
  bool target = false, gateway = false;
  uint8_t frame[60], broadcast_mac[6] = {255, 255, 255, 255, 255, 255},
                     zero[6] = {0};
  CsFrame *rx = malloc(sizeof(*rx));
  uint8_t *dns = malloc(600);
  if (!rx || !dns) {
    free(rx);
    free(dns);
    j->api->close();
    return;
  }
  log_line(j, "[ARP] Resolving target and gateway MACs");
  uint32_t start = now_ms(), request_at = start - 1000;
  unsigned forwarded = 0, redirected = 0;
  bool poisoned = false;
  while (!cancelled(j) && now_ms() - start < 6000 && (!target || !gateway)) {
    if (now_ms() - request_at >= 1000) {
      request_at = now_ms();
      if (!target) {
        net_arp(frame, false, info.local_mac, broadcast_mac, info.local_mac,
                info.local_ip, zero, scope.target_ip);
        j->api->send(frame, 60);
      }
      if (!gateway) {
        net_arp(frame, false, info.local_mac, broadcast_mac, info.local_mac,
                info.local_ip, zero, info.gateway_ip);
        j->api->send(frame, 60);
      }
    }
    if (j->api->poll(rx, 20)) {
      uint8_t mac[6], ip[4];
      if (net_arp_read(rx->bytes, rx->length, mac, ip)) {
        if (!memcmp(ip, scope.target_ip, 4)) {
          memcpy(scope.target_mac, mac, 6);
          target = true;
        }
        if (!memcmp(ip, info.gateway_ip, 4)) {
          memcpy(scope.gateway_mac, mac, 6);
          gateway = true;
        }
      }
    }
  }
  if (!target || !gateway) {
    log_line(j, "[STOP] ARP resolution incomplete; no poisoning");
    goto cleanup;
  }
  if (!memcmp(scope.target_mac, scope.gateway_mac, 6) ||
      !memcmp(scope.target_mac, info.local_mac, 6)) {
    log_line(j, "[STOP] Invalid/proxy ARP topology");
    goto cleanup;
  }
  scope.relay = true;
  if (!j->api->scope(&scope)) {
    log_line(j, "[STOP] Receive path could not be enabled");
    goto cleanup;
  }
  poisoned = true;
  if (!send_arp(j, &info, &scope, false)) {
    log_line(j, "[STOP] ARP transmit failed");
    goto cleanup;
  }
  log_line(j, "[ARP] One-target relay active; maximum 5 min");
  log_line(j, "[LIMIT] IPv4 only. BACK restores ARP caches.");
  uint8_t reply[4];
  bool use_dns = j->config.redirect && net_ip_parse(j->config.reply, reply);
  if (use_dns)
    log_line(j, "[DNS] Exact name %s -> %s", j->config.name, j->config.reply);
  start = request_at = now_ms();
  uint32_t report_at = start;
  unsigned failures = 0;
  while (!cancelled(j) && wlan_hal_is_connected() &&
         now_ms() - start < 300000) {
    if (j->api->poll(rx, 10)) {
      size_t response =
          use_dns ? net_dns_redirect(rx->bytes, rx->length, info.local_mac,
                                     scope.target_mac, scope.target_ip,
                                     j->config.name, reply, dns, 600)
                  : 0;
      if (response) {
        if (j->api->send(dns, response))
          redirected++;
        else
          failures++;
      } else if (net_relay(rx->bytes, rx->length, info.local_mac,
                           scope.target_mac, scope.gateway_mac,
                           scope.target_ip)) {
        if (j->api->send(rx->bytes, rx->length))
          forwarded++;
        else
          failures++;
      }
    }
    uint32_t now = now_ms();
    if (now - request_at >= 2000) {
      request_at = now;
      if (!send_arp(j, &info, &scope, false)) {
        log_line(j, "[STOP] ARP refresh failed");
        break;
      }
    }
    CsNetStats stats;
    j->api->stats(&stats);
    if (stats.dropped || failures) {
      log_line(j, "[STOP] Relay backpressure: drop=%u txfail=%u",
               (unsigned)stats.dropped, failures);
      break;
    }
    if (now - report_at >= 2000) {
      report_at = now;
      log_line(j, "[FLOW] Forwarded=%u DNS-replies=%u", forwarded, redirected);
    }
  }
cleanup:
  if (poisoned) {
    log_line(j, "[RESTORE] Advertising real peer MAC addresses");
    unsigned repaired = 0;
    for (unsigned i = 0; i < 3; i++) {
      if (send_arp(j, &info, &scope, true))
        repaired++;
      uint32_t until = now_ms() + 250;
      while ((int32_t)(until - now_ms()) > 0) {
        if (j->api->poll(rx, 10) &&
            net_relay(rx->bytes, rx->length, info.local_mac, scope.target_mac,
                      scope.gateway_mac, scope.target_ip))
          j->api->send(rx->bytes, rx->length);
      }
    }
    log_line(j, "[RESTORE] %u/3 reply pairs sent; client receipt unknown",
             repaired);
  }
  j->api->close();
  memset(dns, 0, 600);
  free(dns);
  free(rx);
  log_line(j, "[RESULT] Forwarded %u frames; DNS replies %u", forwarded,
           redirected);
}
static int32_t worker(void *ctx) {
  NetJob *j = ctx;
  log_line(j, "[START] %s", net_job_name(j->kind));
  if (!wlan_hal_is_connected()) {
    log_line(j, "[ERROR] Connect using the firmware Wi-Fi app first");
    goto finished;
  }
  switch (j->kind) {
  case JobDiscover:
    discover(j);
    break;
  case JobScan:
    scan(j);
    break;
  case JobDnsLookup:
    lookup(j);
    break;
  case JobHttp:
  case JobTcp:
  case JobUdp:
    terminal(j);
    break;
  case JobArp:
    arp_test(j);
    break;
  case JobDnsServer:
  case JobLlmnr:
  case JobNbns:
    name_server(j);
    break;
  case JobExposure:
    exposure(j);
    break;
  }
finished:
  log_line(j, "[DONE] %s; event drops=%u",
           cancelled(j) ? "Stopped" : "Finished", atomic_load(&j->dropped));
  atomic_store(&j->done, true);
  return 0;
}
NetJob *net_start(JobKind k, const NetConfig *config) {
  if ((unsigned)k > JobExposure || !config)
    return NULL;
  NetJob *j = calloc(1, sizeof(*j));
  if (!j)
    return NULL;
  j->kind = k;
  j->config = *config;
  j->api = cs_backend();
  j->events = furi_message_queue_alloc(64, sizeof(NetEvent));
  j->thread = furi_thread_alloc_ex("CSMiniNet", 24 * 1024, worker, j);
  if (!j->events || !j->thread) {
    if (j->events)
      furi_message_queue_free(j->events);
    if (j->thread)
      furi_thread_free(j->thread);
    free(j);
    return NULL;
  }
  furi_thread_start(j->thread);
  return j;
}
bool net_poll(NetJob *j, NetEvent *e) {
  return furi_message_queue_get(j->events, e, 0) == FuriStatusOk;
}
void net_cancel(NetJob *j) {
  if (j)
    atomic_store(&j->cancel, true);
}
void net_free(NetJob *j) {
  if (!j)
    return;
  net_cancel(j);
  furi_thread_join(j->thread);
  furi_thread_free(j->thread);
  furi_message_queue_free(j->events);
  memset(&j->config, 0, sizeof j->config);
  free(j);
}
