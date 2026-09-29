#include "wardriver_wigle.h"
#include <stdio.h>
#include <string.h>

/* Allows reproducing the old picker configuration against the old source. */
#ifndef WARD_WIGLE_BROWSER_DIR
#define WARD_WIGLE_BROWSER_DIR WARD_WIGLE_DIR
#endif

static unsigned failures;
#define CHECK(c) do { if(!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while(0)

static bool parse(const char* text, size_t chunk) {
    WardWigleCsv csv = {0};
    size_t length = strlen(text);
    for(size_t offset = 0; offset < length; offset += chunk) {
        size_t n = length - offset;
        if(n > chunk) n = chunk;
        if(!wardriver_wigle_csv_feed(&csv, text + offset, n)) return false;
    }
    return wardriver_wigle_csv_finish(&csv) && csv.rows == 1;
}

int main(void) {
    const char* filename = "wardrive_1790510677_d8be95ca_wigle.csv";
    char picked[WARD_WIGLE_PATH_SIZE];
    /* Match file_browser_worker.c's directory + '/' + filename join. */
    snprintf(picked, sizeof(picked), "%s/%s", WARD_WIGLE_BROWSER_DIR, filename);
    CHECK(wardriver_wigle_path_valid(picked));
    CHECK(!strcmp(picked, WARD_WIGLE_DIR "wardrive_1790510677_d8be95ca_wigle.csv"));
    CHECK(wardriver_wigle_path_valid(WARD_WIGLE_DIR "wardrive_1790510677_d8be95ca_wigle.csv"));
    CHECK(!wardriver_wigle_path_valid(WARD_WIGLE_DIR "wardrive_1790510677_d8be95ca_local.csv"));
    CHECK(!wardriver_wigle_path_valid(WARD_WIGLE_DIR "../wardrive_1_wigle.csv"));
    CHECK(!wardriver_wigle_path_valid("/ext/other/wardrive_1_wigle.csv"));
    CHECK(!wardriver_wigle_path_valid(WARD_WIGLE_DIR "wigle.conf"));
    const char* metadata =
        "WigleWifi-1.6,appRelease=1.3,model=T-Embed-CC1101-Plus,release=Sor3nt,"
        "device=ESP32-S3,display=Wardriver,board=T-Embed,brand=LILYGO,star=Sol,body=3,subBody=0\n";
    const char* header =
        "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,"
        "CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n";
    const char* row =
        "AA:BB:CC:DD:EE:FF,\"Cafe, \"\"test\"\"\nSSID\",[WPA2-PSK][ESS],"
        "2026-09-27 12:00:00,6,2437,-67,48.1173000,11.5166667,545,0,,,WIFI\n";
    char text[2048], crlf[4096];
    snprintf(text, sizeof(text), "%s%s%s", metadata, header, row);
    CHECK(parse(text, 1));
    CHECK(parse(text, 17));
    CHECK(parse(text, 4096));
    size_t j = 0;
    for(size_t i = 0; text[i]; ++i) {
        if(text[i] == '\n') crlf[j++] = '\r';
        crlf[j++] = text[i];
    }
    crlf[j] = 0;
    CHECK(parse(crlf, 1));
    text[strlen(text) - 1] = 0;
    CHECK(parse(text, 17));
    snprintf(text, sizeof(text), "%s%s", metadata, header);
    CHECK(!parse(text, 1));
    snprintf(text, sizeof(text), "WigleWifi-1.7,appRelease=1.3\n%s%s", header, row);
    CHECK(!parse(text, 17));
    snprintf(text, sizeof(text), "%s%s%s", metadata, header, row);
    memcpy(strstr(text, "48.1173000"), "99.1173000", 10);
    CHECK(!parse(text, 17));
    if(failures) return 1;
    puts("PASS: picker path, reported filename, path restrictions, generated CSV header, chunking, quoting, CRLF and invalid data");
    return 0;
}
