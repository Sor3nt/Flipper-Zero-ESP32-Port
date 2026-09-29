#include "bw16_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    Bw16Ap ap;
    assert(bw16_parse("Lab,36,001122AABBCC,-42", &ap) == Bw16Record);
    assert(strcmp(ap.ssid,"Lab")==0 && ap.channel==36 && ap.rssi==-42);
    assert(strcmp(ap.mac,"001122AABBCC")==0);
    assert(bw16_parse("Lab, east,149,00:11:22:AA:BB:CC,-67", &ap) == Bw16Record);
    assert(strcmp(ap.ssid,"Lab, east")==0);
    assert(bw16_parse("list-station: Lab,6,-90", &ap) == Bw16Record);
    assert(bw16_parse("list-station: Lab,6,001122334455,-90", &ap) == Bw16Record);
    assert(bw16_parse(",1,001122334455,-80", &ap) == Bw16Record && !ap.ssid[0]);
    assert(bw16_parse("A_START_DONE,6,001122334455,-44", &ap) == Bw16Record);
    assert(bw16_parse("SCAN_DONE", &ap) == Bw16Done);
    assert(bw16_parse("NO SCAN RESULTS", &ap) == Bw16Empty);
    const char* ignored[] = {"Booting...", "Serial1: SCAN_DONE", "SCAN_DONE junk",
        "LIST_STATION_DONE", "LIST_STATION_START", "SCAN", "log,foo,bar,baz",
        "noise"};
    for(unsigned i=0;i<sizeof(ignored)/sizeof(*ignored);++i)
        assert(bw16_parse(ignored[i],&ap)==Bw16Ignore);
    const char* bad[] = {"Lab,6,NOT-A-MAC,-30", "list-station:", "list-station: A,0,-5",
        "list-station: A,15,-5", "list-station: A,99999999999999,-5",
        "list-station: A,6,-999", "list-station: A,6,30", "list-station: A,6,-5tail",
        "list-station: A,6,", "list-station: A,+6,-5",
        "123456789012345678901234567890123,6,001122334455,-5",
        "bad\rname,6,001122334455,-5"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i)
        assert(bw16_parse(bad[i],&ap)==Bw16Malformed);
    Bw16Line line={0};
    const char* text="SCAN_DONE\r\n";
    for(size_t i=0;i<strlen(text)-1;++i) assert(bw16_frame(&line,text[i])==Bw16More);
    assert(bw16_frame(&line,'\n')==Bw16LineReady && strcmp(line.text,"SCAN_DONE")==0);
    assert(bw16_frame(&line,'\n')==Bw16More);
    for(unsigned i=0;i<BW16_LINE_SIZE+50;++i) bw16_frame(&line,'x');
    assert(bw16_frame(&line,'\n')==Bw16LineDropped);
    bw16_frame(&line,0);
    for(size_t i=0;i<strlen("SCAN_DONE");++i) bw16_frame(&line,"SCAN_DONE"[i]);
    assert(bw16_frame(&line,'\n')==Bw16LineDropped); /* no truncated success */
    for(size_t i=0;i<strlen(text);++i) bw16_frame(&line,text[i]);
    assert(strcmp(line.text,"SCAN_DONE")==0); /* resynchronized */
    assert(!bw16_scan_expired(20999,1000));
    assert(bw16_scan_expired(21000,1000));
    assert(bw16_scan_expired(19999,UINT32_MAX)); /* timer wraps */
    puts("protocol: records, noise, malformed, framing, limits, timeout PASS");
}
