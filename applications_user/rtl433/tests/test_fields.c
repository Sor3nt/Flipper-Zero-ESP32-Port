#include "rtl_engine.h"
#include "rtl_capture.h"
#include "rtl_text.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void save_test(const char *name, const char *s) {
  const char *dir = getenv("RTL_TEST_OUT");
  assert(dir);
  char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE *f = fopen(path, "wb"); assert(f);
  assert(fwrite(s, 1, strlen(s), f) == strlen(s)); assert(!fclose(f));
}
void test_fields(void) {
  int ints[] = {-123, 0, 2147483647};
  double doubles[] = {23.4, 1.2345678901234567, -0.00000012345678901234567};
  const char *strings[] = {"quote\" slash\\", "degree \xc2\xb0 C", "line\nnext", "emoji \xf0\x9f\x8c\xa1"};
  data_t *objects[] = {data_make("last_nested", "", DATA_INT, 42, NULL)};
  data_array_t *arrays[] = {data_array(3, DATA_INT, ints), data_array(0, DATA_INT, NULL)};
  char long_id[300], long_text[7000]; memset(long_id, 'i', sizeof long_id - 1); long_id[299] = 0;
  memset(long_text, 'a', sizeof long_text - 1); long_text[6999] = 0;
  data_t *d = data_make(
      "model", "", DATA_STRING, "Field audit",
      "id", "", DATA_STRING, long_id,
      "integer", "", DATA_INT, -123,
      "pressure_PSI", "", DATA_DOUBLE, 35.5,
      "temperature_C", "", DATA_FORMAT, "%.1f C", DATA_DOUBLE, 23.4,
      "precise", "", DATA_DOUBLE, 1.2345678901234567,
      "not_finite", "", DATA_DOUBLE, NAN,
      "ints", "", DATA_ARRAY, data_array(3, DATA_INT, ints),
      "doubles", "", DATA_ARRAY, data_array(3, DATA_DOUBLE, doubles),
      "strings", "", DATA_ARRAY, data_array(4, DATA_STRING, strings),
      "objects", "", DATA_ARRAY, data_array(1, DATA_DATA, objects),
      "arrays", "", DATA_ARRAY, data_array(2, DATA_ARRAY, arrays),
      "nested", "", DATA_DATA, data_make("battery_ok", "", DATA_INT, 1, NULL),
      "conditional", "", DATA_COND, 1, DATA_STRING, "present",
      "absent", "", DATA_COND, 0, DATA_INT, 7,
      "long_text", "", DATA_STRING, long_text,
      "final_field", "", DATA_STRING, "END-OF-RECORD", NULL);
  assert(d);
  r_device device = {.name = "All supported data types", .protocol_num = 7};
  RtlMeta meta = {.frequency = 433920000, .rssi = -50, .time_ms = 123};
  char *json = rtl_json(&device, d, &meta); assert(json);
  char *pretty = rtl_pretty_json(json); assert(pretty);
  char *csv = rtl_csv_record("quoted\"model\nsecond line", "ID", "1", 2, &meta, json); assert(csv);
  save_test("fields.json", json); save_test("fields-pretty.json", pretty); save_test("fields.csv", csv);
  unsigned lines = rtl_text_page(pretty, 52, 0, 0, NULL, 0);
  assert(lines > 130);
  char screen[13][53];
  bool final = false;
  for (unsigned start = 0; start < lines; start += 13) {
    rtl_text_page(pretty, 52, 13, start, &screen[0][0], 53);
    for (unsigned i = 0; i < 13; i++) final |= strstr(screen[i], "END-OF-RECORD") != NULL;
  }
  assert(final); /* last sensor field remains reachable ahead of capture/evidence metadata */
  char *key1 = rtl_identity(&device, d); assert(key1);
  ((char *)rtl_field(d, "id")->value.v_ptr)[298] = 'z';
  char *key2 = rtl_identity(&device, d); assert(key2 && strcmp(key1, key2));
  char exact[55]; memset(exact, 'x', 52); memcpy(exact + 52, "\nY", 3);
  assert(rtl_text_page(exact, 52, 13, 0, &screen[0][0], 53) == 2);
  assert(strlen(screen[0]) == 52 && !strcmp(screen[1], "Y"));
  free(key1); free(key2); free(json); free(pretty); free(csv); data_free(d);
  const char *checks[]={"CRC","CHECKSUM","PARITY","unknown",""};
  for (unsigned i=0;i<5;i++) {
    d=data_make("model","",DATA_STRING,"Candidate sensor","mic","",DATA_STRING,checks[i],NULL);
    assert(d && !rtl_encoding_family(&device,d));
    assert(!strcmp(rtl_evidence(&device,d),i<3?checks[i]:"CANDIDATE"));
    json=rtl_json(&device,d,&meta);assert(json && strstr(json,"\"identification_status\":\"model_candidate\""));
    free(json);data_free(d);
  }
  puts("PASS: every data type, nested arrays/objects, optional fields, exact float values, long identities, Unicode and final-field pagination");
}

/* Synthetic, independently assembled Toyota TPMS packet: ID 12345678,
 * 35.5 PSI, 24 C, status 0. Actual upstream slicer/CRC/decoder are exercised.
 * No TPMS encoder is included in the app; this fixture is host-test only. */
void test_tpms(RtlEngine *e, RtlFrame *f) {
  uint8_t payload[9] = {0x12, 0x34, 0x56, 0x78, 0x55, 0x20, 0x00, 0x55, 0};
  unsigned crc = 0x80;
  for (unsigned i = 0; i < 8; i++) {
    crc ^= payload[i];
    for (unsigned k = 0; k < 8; k++) crc = ((crc << 1) ^ ((crc & 0x80) ? 7 : 0)) & 255;
  }
  payload[8] = crc;
  bool bits[200]; unsigned count = 0;
  const uint8_t sync[] = {0xa9, 0xe0};
  for (unsigned i = 0; i < 11; i++) bits[count++] = !!(sync[i / 8] & (0x80 >> (i % 8)));
  bool previous = true;
  for (unsigned i = 0; i < 72; i++) {
    bool bit = !!(payload[i / 8] & (0x80 >> (i % 8)));
    bool first = !previous, second = bit ? first : !first;
    bits[count++] = first; bits[count++] = second; previous = second;
  }
  bits[count++] = !previous; bits[count++] = !previous;
  RtlCapture c; rtl_capture_reset(&c, f);
  bool level = bits[0]; unsigned duration = 0;
  for (unsigned i = 0; i < count; i++) {
    if (bits[i] != level) { rtl_capture_feed(&c, f, level, duration); level = bits[i]; duration = 0; }
    duration += 52;
  }
  rtl_capture_feed(&c, f, level, duration);
  assert(rtl_capture_finish(&c, f, 100000));
  f->meta = (RtlMeta){.frequency = 315000000, .fsk = true, .profile = 4, .profile_known = true};
  for (unsigned i = 0; i < e->count; i++) e->enabled[i] = !strcmp(e->devices[i]->name, "Toyota TPMS");
  assert(rtl_engine_decode(e, &f->pulses, &f->meta) == 1);
}
