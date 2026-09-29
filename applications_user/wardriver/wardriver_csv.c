#include "wardriver_csv.h"
#include <stdio.h>
/* RFC 4180 quoting. Returning zero means the destination was too short. */
size_t wardriver_csv_quote(char* out,size_t size,const char* in) {
    if(size<3) return 0;
    size_t n=0; out[n++]='"';
    for(;*in;++in) {
        size_t needed=*in=='"'?2:1;
        if(n+needed+2>size) { out[0]=0; return 0; }
        if(*in=='"') out[n++]='"';
        out[n++]=*in;
    }
    out[n++]='"'; out[n]=0; return n;
}
/* wifi_auth_mode_t values from the target's ESP-IDF 5.4.1. Capabilities do
 * not fabricate a pairwise cipher or claim an authentication was attempted. */
const char* wardriver_security(uint8_t auth) {
    switch(auth) {
    case 0: return "[ESS]";
    case 1: return "[WEP][ESS]";
    case 2: return "[WPA-PSK][ESS]";
    case 3: return "[WPA2-PSK][ESS]";
    case 4: return "[WPA-PSK][WPA2-PSK][ESS]";
    case 5: return "[WPA2-EAP][ESS]";
    case 6: return "[WPA3-SAE][ESS]";
    case 7: return "[WPA2-PSK][WPA3-SAE][ESS]";
    case 8: return "[WAPI-PSK][ESS]";
    case 9: return "[OWE][ESS]";
    case 10: return "[WPA3-EAP-192][ESS]";
    case 11: case 12: return "[WPA3-SAE][ESS]";
    case 13: return "[DPP][ESS]";
    case 14: return "[WPA3-EAP][ESS]";
    case 15: return "[WPA2-EAP][WPA3-EAP][ESS]";
    default: return "[UNKNOWN]";
    }
}
