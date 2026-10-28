from pathlib import Path
import os,re,sys,tempfile
R=Path(__file__).resolve().parents[2];A=R/'applications_user/protopirate'
sys.path.insert(0,str(R/'tests/host'))
from host_build import run
if os.name=='nt':os.environ.setdefault('ZIG_CC',str(R/'.backups/bw16-evidence/native/ziglang/zig.exe'))
def strip(s):return '\n'.join(l for l in s.splitlines() if not re.match(r'\s*#(?:include|pragma)\b',l))
def section(path,start,end):
 s=path.read_text();return s[s.index(start):s.index(end,s.index(start))]
with tempfile.TemporaryDirectory(prefix='pp362_') as tmp:
 t=Path(tmp)
 (t/'types.inc').write_text(strip((A/'helpers/protopirate_types.h').read_text()))
 (t/'manager.inc').write_text('\n'.join(strip((R/'components/gui'/p).read_text()) for p in ['scene_manager.h','scene_manager_i.h','scene_manager.c']))
 (t/'actions.inc').write_text(strip((A/'helpers/protopirate_actions.h').read_text()))
 (t/'start.inc').write_text(strip((A/'scenes/protopirate_scene_start.c').read_text()))
 (t/'navigation.inc').write_text(section(A/'protopirate_app.c','static void protopirate_actions_menu_callback','static void protopirate_app_tick_event_callback'))
 (t/'variable_input.inc').write_text(section(A/'helpers/variable_item_list.c','static bool variable_item_list_input_callback','void variable_item_list_process_up(VariableItemList* variable_item_list) {'))
 (t/'receiver.inc').write_text(section(A/'views/protopirate_receiver.c','bool protopirate_view_receiver_input','void protopirate_view_receiver_enter'))
 (t/'emulate.inc').write_text(section(A/'scenes/plugins/protopirate_emulate_plugin.c','static bool emulate_input_callback','static bool\n    get_tx_preset_byte'))
 chrysler=(A/'protocols/chrysler_v0.c').read_text()
 prefix=chrysler[chrysler.index('#define TAG'):chrysler.index('#if PROTOPIRATE_WITH_ENCODER')]
 prefix=re.sub(r'struct SubGhzProtocolEncoderChrysler_V0 \{.*?\};','',prefix,flags=re.S)
 decoder=section(A/'protocols/chrysler_v0.c','void* subghz_protocol_decoder_chrysler_v0_alloc','SubGhzProtocolStatus subghz_protocol_decoder_chrysler_v0_serialize')
 output=chrysler[chrysler.index('void subghz_protocol_decoder_chrysler_v0_get_string'):]
 structs=''
 for file,name in [('decoder.h','SubGhzBlockDecoder'),('generic.h','SubGhzBlockGeneric')]:
  text=(R/'lib/subghz/blocks'/file).read_text()
  structs += 'typedef struct '+name+' '+name+';\n'+re.search(r'struct '+name+r' \{.*?\};',text,re.S).group()+'\n'
 (t/'chrysler.inc').write_text(structs+prefix+decoder+output)
 for test in ('controls','chrysler'):
  exe=t/(test+'.exe' if os.name=='nt' else test)
  run(['gcc','-std=gnu17','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-O1','-g','-fsanitize=address,undefined','-I'+str(t),'-I'+str(R/'components/mlib'),R/('tests/protopirate362/'+test+'_test.c'),'-o',exe],check=True)
  run([exe],check=True,timeout=30)
print('PASS ProtoPirate 3.6.2 control regressions')
