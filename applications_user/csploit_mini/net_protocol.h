/* Native network protocol helpers. MIT. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef enum { NsDns, NsLlmnr, NsNbns } NsProtocol;
typedef struct {
  char name[254];
  uint16_t id, type, klass;
  size_t question_end;
} NsQuestion;
bool ns_question(const uint8_t *p, size_t n, NsProtocol proto, NsQuestion *q);
/* Answers only the exact configured name. No wildcard or upstream recursion. */
size_t ns_answer(const uint8_t *query, size_t n, NsProtocol proto,
                 const char *name, const uint8_t ip[4], uint8_t *answer,
                 size_t capacity);
bool ns_name_equal(const char *a, const char *b);
bool net_ip_parse(const char *text, uint8_t ip[4]);
void net_ip_text(const uint8_t ip[4], char text[16]);
bool net_mac_parse(const char *text, uint8_t mac[6]);
/* Ethernet ARP and a bounded, one-target IPv4 relay. */
size_t net_arp(uint8_t out[60], bool reply, const uint8_t ethernet_source[6],
               const uint8_t ethernet_dest[6], const uint8_t claimed_mac[6],
               const uint8_t claimed_ip[4], const uint8_t target_mac[6],
               const uint8_t target_ip[4]);
bool net_arp_read(const uint8_t *frame, size_t n, uint8_t sender_mac[6],
                  uint8_t sender_ip[4]);
bool net_relay(uint8_t *frame, size_t n, const uint8_t own_mac[6],
               const uint8_t target_mac[6], const uint8_t gateway_mac[6],
               const uint8_t target_ip[4]);
/* Only records whether an HTTP Authorization header exists, never its value. */
bool net_http_auth_present(const uint8_t *request, size_t n);
size_t ns_query(const char *name, uint16_t id, uint8_t *out, size_t capacity);
unsigned ns_addresses(const uint8_t *packet, size_t length, uint16_t id,
                      uint8_t addresses[][4], unsigned capacity);
size_t net_dns_redirect(const uint8_t *ethernet, size_t length,
                        const uint8_t own_mac[6], const uint8_t target_mac[6],
                        const uint8_t target_ip[4], const char *name,
                        const uint8_t reply_ip[4], uint8_t *out,
                        size_t capacity);
