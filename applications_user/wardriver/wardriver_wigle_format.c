#include "wardriver_wigle.h"
#include <string.h>
#include <stdlib.h>

void wardriver_wigle_erase(void* p,size_t n) { volatile unsigned char* b=p; while(n--) *b++=0; }
bool wardriver_wigle_value_valid(const char* text,bool name) {
    size_t limit=name?WARD_WIGLE_NAME_SIZE:WARD_WIGLE_TOKEN_SIZE;
    for(size_t i=0;i<limit;++i) {
        unsigned char c=text[i];
        if(!c) return true; /* Blank field is permitted while configuring. */
        if(c<33 || c>126 || (name && c==':')) return false;
    }
    return false;
}
bool wardriver_wigle_path_valid(const char* path) {
    const char* dir=WARD_WIGLE_DIR;
    size_t n=strlen(path),d=strlen(dir),suffix=strlen("_wigle.csv");
    if(n>=WARD_WIGLE_PATH_SIZE || n<=d+9+suffix || strncmp(path,dir,d)) return false;
    const char* name=path+d;
    if(strncmp(name,"wardrive_",9) || strcmp(path+n-suffix,"_wigle.csv")) return false;
    for(const char* p=name;p<path+n-suffix;++p)
        if(!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||*p=='_')) return false;
    return true;
}
static bool coordinate(const char* s,double limit) {
    if(!*s) return false;
    const char* p=s;
    if(*p=='-') ++p;
    unsigned whole=0,fraction=0;
    while(*p>='0' && *p<='9') { ++p; ++whole; }
    if(!whole || whole>3) return false;
    if(*p=='.') {
        ++p;
        while(*p>='0' && *p<='9') { ++p; ++fraction; }
        if(!fraction || fraction>9) return false;
    }
    if(*p) return false; /* Bounded decimal: excludes NaN/Inf/overflow. */
    char* end; double n=strtod(s,&end);
    return end!=s && !*end && n>=-limit && n<=limit;
}
static bool field(WardWigleCsv* c,bool row_end) {
    c->cell[c->length]=0;
    if(c->column==0) {
        if(c->length!=17) return false;
        for(unsigned i=0;i<17;++i) {
            char v=c->cell[i];
            if(i%3==2) { if(v!=':') return false; }
            else if(!((v>='0'&&v<='9')||(v>='A'&&v<='F')||(v>='a'&&v<='f'))) return false;
        }
    }
    if(c->column==3 && (c->length!=19 || c->cell[4]!='-' || c->cell[7]!='-' || c->cell[10]!=' ')) return false;
    if(c->column==7 && !coordinate(c->cell,90)) return false;
    if(c->column==8 && !coordinate(c->cell,180)) return false;
    if(c->column==13 && strcmp(c->cell,"WIFI")) return false;
    if(row_end) { if(c->column!=13) return false; ++c->rows; c->column=0; }
    else if(++c->column>=14) return false;
    c->length=0; c->after_quote=false; return true;
}
bool wardriver_wigle_csv_feed(WardWigleCsv* c,const char* data,size_t n) {
    static const char header[]="MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type";
    if(c->error) return false;
    for(size_t i=0;i<n;++i) {
        char ch=data[i]; if(!ch) goto bad;
        if(c->header<2) {
            if(ch=='\n') {
                if(c->length && c->cell[c->length-1]=='\r') --c->length;
                c->cell[c->length]=0;
                if(!c->header?strncmp(c->cell,"WigleWifi-1.6,",14)!=0:strcmp(c->cell,header)!=0) goto bad;
                ++c->header; c->length=0; continue;
            }
        } else if(c->quoted) {
            if(ch=='"') { c->quoted=false; c->after_quote=true; continue; }
        } else {
            if(c->cr && ch!='\n') goto bad;
            if(ch=='\r') { c->cr=true; continue; }
            c->cr=false;
            if(ch==',' || ch=='\n') { if(!field(c,ch=='\n')) goto bad; continue; }
            if(ch=='"') {
                if(c->after_quote) { c->quoted=true; c->after_quote=false; }
                else { if(c->length) goto bad; c->quoted=true; continue; }
            } else if(c->after_quote) goto bad;
        }
        if(c->length>=sizeof(c->cell)-1) goto bad;
        c->cell[c->length++]=ch;
    }
    return true;
bad: c->error=true; return false;
}
bool wardriver_wigle_csv_finish(WardWigleCsv* c) {
    if(c->error || c->header!=2 || c->quoted) return false;
    if(c->length || c->column || c->after_quote || c->cr)
        if(!wardriver_wigle_csv_feed(c,"\n",1)) return false;
    return c->rows>0;
}
/* Strict JSON parsing: never accept a substring such as a quoted \"success\".
 * Ignore response strings rather than displaying untrusted server text/secrets. */
typedef struct { const char *p,*end; unsigned depth; } Json;
static void ws(Json* j) { while(j->p<j->end && strchr(" \r\n\t",*j->p)) ++j->p; }
static bool string(Json* j,char* out,size_t cap) {
    if(j->p==j->end || *j->p++!='"') return false;
    size_t n=0;
    while(j->p<j->end) {
        unsigned char c=*j->p++;
        if(c=='"') { if(out) out[n<cap?n:cap-1]=0; return true; }
        if(c<32) return false;
        if(c=='\\') {
            if(j->p==j->end) return false;
            c=*j->p++;
            if(c=='u') {
                for(unsigned i=0;i<4;++i) {
                    if(j->p==j->end || !strchr("0123456789abcdefABCDEF",*j->p++)) return false;
                }
                c='?';
            } else if(!strchr("\"\\/bfnrt",c)) return false;
        }
        if(out && n+1<cap) out[n]=c;
        ++n;
    }
    return false;
}
static bool literal(Json* j,const char* s) {
    size_t n=strlen(s); if((size_t)(j->end-j->p)<n || memcmp(j->p,s,n)) return false;
    j->p+=n; return true;
}
static bool value(Json* j) {
    ws(j); if(j->p==j->end || j->depth>=8) return false;
    if(*j->p=='"') return string(j,NULL,0);
    if(*j->p=='{' || *j->p=='[') {
        bool object=*j->p++=='{'; char end=object?'}':']'; ++j->depth; ws(j);
        if(j->p<j->end && *j->p==end) { ++j->p; --j->depth; return true; }
        for(;;) {
            if(object) { if(!string(j,NULL,0)) return false; ws(j); if(j->p==j->end || *j->p++!=':') return false; }
            if(!value(j)) return false;
            ws(j); if(j->p==j->end) return false;
            char ch=*j->p++; if(ch==end) { --j->depth; return true; }
            if(ch!=',') return false;
            ws(j);
        }
    }
    if(*j->p=='t') return literal(j,"true");
    if(*j->p=='f') return literal(j,"false");
    if(*j->p=='n') return literal(j,"null");
    if(*j->p=='-') ++j->p;
    if(j->p==j->end || *j->p<'0' || *j->p>'9') return false;
    if(*j->p++!='0') while(j->p<j->end && *j->p>='0' && *j->p<='9') ++j->p;
    if(j->p<j->end && *j->p=='.') {
        ++j->p; const char* begin=j->p;
        while(j->p<j->end && *j->p>='0' && *j->p<='9') ++j->p;
        if(begin==j->p) return false;
    }
    if(j->p<j->end && (*j->p=='e'||*j->p=='E')) {
        ++j->p; if(j->p<j->end && (*j->p=='+'||*j->p=='-')) ++j->p;
        const char* begin=j->p;
        while(j->p<j->end && *j->p>='0' && *j->p<='9') ++j->p;
        if(begin==j->p) return false;
    }
    return true;
}
bool wardriver_wigle_response(const char* data,size_t n,bool* success,bool* warning) {
    *success=false; *warning=false;
    if(!data || !n || n>4096 || memchr(data,0,n)) return false;
    Json j={.p=data,.end=data+n}; ws(&j);
    if(j.p==j.end || *j.p++!='{') return false;
    bool found=false; ws(&j);
    while(j.p<j.end && *j.p!='}') {
        char key[24]; if(!string(&j,key,sizeof(key))) return false;
        ws(&j); if(j.p==j.end || *j.p++!=':') return false; ws(&j);
        if(!strcmp(key,"success")) {
            if(found) return false;
            found=true;
            if(literal(&j,"true")) *success=true;
            else if(!literal(&j,"false")) return false;
        } else {
            if(!strcmp(key,"warning") && j.p<j.end && *j.p=='"') {
                char message[2]; if(!string(&j,message,sizeof(message))) return false;
                *warning=message[0]!=0;
            } else if(!value(&j)) return false;
        }
        ws(&j); if(j.p==j.end) return false;
        if(*j.p=='}') break;
        if(*j.p++!=',') return false;
        ws(&j); if(j.p==j.end || *j.p=='}') return false;
    }
    if(j.p==j.end || *j.p++!='}') return false;
    ws(&j); return found && j.p==j.end;
}
