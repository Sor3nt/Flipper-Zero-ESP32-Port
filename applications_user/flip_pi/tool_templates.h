#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    const char *name, *format;
    const char* fields[3];
    const char* defaults[3];
    unsigned count;
} PiTool;
/* {0}..{2} are shell-quoted data; never raw shell interpolation. */
static const PiTool pi_tools[] = {
    {"Nmap", "nmap -sT -sV --top-ports 100 {0}", {"Target IP / hostname"}, {"127.0.0.1"}, 1},
    {"Hydra (SSH)", "hydra -l {1} -P {2} {0} ssh", {"Target IP / hostname", "Login username", "Password wordlist on Pi"}, {"127.0.0.1", "kali", "/home/kali/passwords.txt"}, 3},
    {"John the Ripper", "john --wordlist={1} {0}", {"Hash file on Pi", "Wordlist on Pi"}, {"/home/kali/hashes.txt", "/home/kali/passwords.txt"}, 2},
    {"SQLmap", "sqlmap -u {0}", {"Target URL with parameter"}, {"http://127.0.0.1/?id=1"}, 1},
    {"Netcat (connect)", "nc -v {0} {1}", {"Target IP / hostname", "TCP port"}, {"127.0.0.1", "22"}, 2},
    {"Nikto", "nikto -h {0}", {"Target URL"}, {"http://127.0.0.1"}, 1},
    {"Gobuster (directories)", "gobuster dir -u {0} -w {1}", {"Target URL", "Wordlist on Pi"}, {"http://127.0.0.1", "/usr/share/wordlists/dirb/common.txt"}, 2},
    {"Bettercap console", "sudo bettercap -iface {0}", {"Network interface"}, {"wlan0"}, 1},
    {"Aircrack-ng", "aircrack-ng -w {1} {0}", {"Capture file on Pi", "Wordlist on Pi"}, {"/home/kali/capture.cap", "/home/kali/passwords.txt"}, 2},
    {"Tcpdump", "sudo tcpdump -nn -i {0} -c 100", {"Network interface"}, {"wlan0"}, 1},
    {"Metasploit console", "msfconsole -q", {NULL}, {NULL}, 0},
    {"Tor status", "systemctl --no-pager status tor", {NULL}, {NULL}, 0},
    {"Tor start", "sudo systemctl start tor", {NULL}, {NULL}, 0},
    {"Tor stop", "sudo systemctl stop tor", {NULL}, {NULL}, 0},
    {"Curl via Tor", "curl --socks5-hostname 127.0.0.1:9050 {0}", {"Target URL"}, {"https://example.com"}, 1},
    {"DNS lookup", "dig {0}", {"Hostname"}, {"example.com"}, 1},
};
#define PI_TOOL_COUNT (sizeof(pi_tools)/sizeof(pi_tools[0]))

static bool pi_append(char* out, size_t cap, size_t* used, char c) {
    if(*used + 1 >= cap) return false;
    out[(*used)++] = c; out[*used] = 0; return true;
}
static bool pi_quote(char* out, size_t cap, size_t* used, const char* value) {
    if(!*value || *value == '-') return false;
    if(!pi_append(out, cap, used, '\'')) return false;
    for(; *value; value++) {
        if((unsigned char)*value < 32 || (unsigned char)*value > 126) return false;
        if(*value == '\'') {
            const char* escape = "'\\''";
            for(; *escape; escape++) if(!pi_append(out, cap, used, *escape)) return false;
        } else if(!pi_append(out, cap, used, *value)) return false;
    }
    return pi_append(out, cap, used, '\'');
}
static bool pi_tool_command(const PiTool* tool, char fields[3][96], char* out, size_t cap) {
    size_t used = 0; if(!cap) return false; out[0] = 0;
    for(const char* p = tool->format; *p; p++) {
        if(p[0]=='{' && p[1]>='0' && p[1]<='2' && p[2]=='}') {
            if(!pi_quote(out, cap, &used, fields[p[1]-'0'])) {out[0]=0; return false;}
            p += 2;
        } else if(!pi_append(out, cap, &used, *p)) {out[0]=0; return false;}
    }
    return true;
}
