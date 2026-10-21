from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[2]
s = (root / "applications_user/walkie_talkie/walkie_talkie.c").read_text()
def func(name):
 start=s.index('static '+('int wrap' if name=='wrap' else 'void '+name)+'(');brace=s.index('{',start);depth=1;i=brace+1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[start:i]
h='''#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#define LOAD(x) (x)
#define STORE(x,v) ((x)=(v))
typedef enum {InputKeyUp,InputKeyDown,InputKeyLeft,InputKeyRight,InputKeyOk,InputKeyBack} Key;
typedef enum {InputTypeShort,InputTypeLong,InputTypeRepeat} Type;
typedef struct {Key key;Type type;} InputEvent;
typedef struct {bool running,begun,edit,scan,held,mute,squelch;int page,cursor,channel,label,threshold,direction;uint32_t volume;} App;
static int tunes;static void tune(App*a){(void)a;tunes++;}
'''+func('wrap')+'\n'+func('change_setting')+'\n'+func('handle')+'''
int main(void){
 App a={.running=true,.begun=true,.direction=1,.threshold=-68,.squelch=true,.volume=3};
 handle(&a,(InputEvent){InputKeyUp,InputTypeShort});assert(a.channel==21 && tunes==1);
 handle(&a,(InputEvent){InputKeyDown,InputTypeShort});assert(a.channel==0);
 handle(&a,(InputEvent){InputKeyOk,InputTypeLong});assert(a.scan);
 a.held=true;handle(&a,(InputEvent){InputKeyOk,InputTypeLong});assert(a.scan&&!a.held&&a.channel==1);
 handle(&a,(InputEvent){InputKeyOk,InputTypeLong});assert(!a.scan);
 handle(&a,(InputEvent){InputKeyBack,InputTypeShort});assert(a.page==1);
 handle(&a,(InputEvent){InputKeyDown,InputTypeShort});handle(&a,(InputEvent){InputKeyOk,InputTypeShort});assert(a.page==2);
 a.cursor=1;handle(&a,(InputEvent){InputKeyOk,InputTypeShort});assert(a.edit);
 for(int i=0;i<200;i++)handle(&a,(InputEvent){InputKeyDown,InputTypeShort});assert(a.threshold==-30);
 for(int i=0;i<200;i++)handle(&a,(InputEvent){InputKeyUp,InputTypeShort});assert(a.threshold==-120);
 handle(&a,(InputEvent){InputKeyBack,InputTypeShort});assert(!a.edit&&a.page==2);
 a.cursor=3;change_setting(&a,-1);assert(a.label==38);change_setting(&a,1);assert(a.label==0);
 a.cursor=4;for(int i=0;i<20;i++)change_setting(&a,1);assert(a.volume==10);
 a.cursor=5;handle(&a,(InputEvent){InputKeyOk,InputTypeShort});assert(a.volume==3&&a.threshold==-68&&a.squelch);
 handle(&a,(InputEvent){InputKeyBack,InputTypeLong});assert(!a.running);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t)/'test.c';p.write_text(h);subprocess.run(['cc','-std=c11','-fsanitize=undefined',str(p),'-o',t+'/test'],check=True);subprocess.run([t+'/test'],check=True)
print('PASS: actual control handlers: channel wrap, scan/skip/stop, rotary settings, bounds, reset, exit')
