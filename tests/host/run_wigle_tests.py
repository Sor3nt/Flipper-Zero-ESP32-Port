from pathlib import Path
from host_build import run as run_native
import subprocess,tempfile,email,email.policy
root=Path(__file__).resolve().parents[2]
app=root/'applications_user/wardriver'; host=root/'tests/host/wardriver_host'
out=root/'build_t_embed/host_tests/wardriver_wigle_test'
run_native(['gcc','-std=c17','-Wall','-Wextra','-Werror','-g','-O1','-fsanitize=address,undefined',
 '-I'+str(host),'-I'+str(app),str(Path(__file__).with_name('wardriver_wigle_test.c')),
 *[str(app/n) for n in ['wardriver_wigle.c','wardriver_wigle_credentials.c','wardriver_wigle_format.c']],'-lm','-o',str(out)],check=True)
with tempfile.TemporaryDirectory() as d:
 run_native([str(out),d],check=True)
 p=Path(d)
 message=email.message_from_bytes(b'Content-Type: '+(p/'multipart-type.txt').read_bytes()+b'\r\nMIME-Version: 1.0\r\n\r\n'+(p/'multipart-request.txt').read_bytes(),policy=email.policy.default)
 parts=list(message.iter_parts())
 assert len(parts)==1 and parts[0].get_param('name',header='content-disposition')=='file'
 assert parts[0].get_filename()=='wardrive_1790164800_abcd1234_wigle.csv'
 assert parts[0].get_payload(decode=True).startswith(b'WigleWifi-1.6,')
 print('PASS: multipart request parsed independently with Python email parser')
