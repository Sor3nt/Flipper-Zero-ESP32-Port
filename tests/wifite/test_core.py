#!/usr/bin/env python3
"""Independent Python WSC peer vectors, native C crypto/parsing tests.
Requires cryptography, gcc, and an installed/built Mbed TLS 3.x crypto library.
Set MBEDTLS_ROOT and MBEDTLS_LIB to the checkout and libmbedcrypto.a paths.
This does not validate the ESP32 radio or physical display.
"""
import hashlib, hmac, os, pathlib, subprocess, tempfile, ctypes
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
ROOT=pathlib.Path(__file__).resolve().parents[2]
PRIME=int('FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA237327FFFFFFFFFFFFFFFF',16)
def tlv(t,b):return t.to_bytes(2,'big')+len(b).to_bytes(2,'big')+b
def u8(t,v):return tlv(t,bytes([v]))
def u16(t,v):return tlv(t,v.to_bytes(2,'big'))
def mac(k,b):return hmac.new(k,b,hashlib.sha256).digest()
def base(t):return u8(0x104a,0x10)+u8(0x1022,t)
x=bytes(range(1,33));y=bytes(range(32,64));rn=bytes(range(16));en=bytes(range(16,32));ap=bytes.fromhex('021122334455')
pke=pow(2,int.from_bytes(y,'big'),PRIME).to_bytes(192,'big');pkr=pow(2,int.from_bytes(x,'big'),PRIME).to_bytes(192,'big')
shared=pow(int.from_bytes(pke,'big'),int.from_bytes(x,'big'),PRIME).to_bytes(192,'big')
kdk=mac(hashlib.sha256(shared).digest(),en+ap+rn)
keys=b''.join(mac(kdk,i.to_bytes(4,'big')+b'Wi-Fi Easy and Secure Key Derivation'+(640).to_bytes(4,'big')) for i in range(1,4))[:80]
auth,wrap=keys[:32],keys[32:48]
uuid=bytearray(rn);uuid[6]=(uuid[6]&15)|0x40;uuid[8]=(uuid[8]&63)|0x80
m1=base(4)+tlv(0x101a,en)+tlv(0x1032,pke)+tlv(0x1020,ap)+u16(0x1004,0x23)+u16(0x1010,0x0d)
m2=base(5)+tlv(0x101a,en)+tlv(0x1039,rn)+tlv(0x1048,uuid)+tlv(0x1032,pkr)+u16(0x1004,0x23)+u16(0x1010,0x0d)+u8(0x100d,1)+u16(0x1008,0x010c)+tlv(0x1021,b'WIFITE')+tlv(0x1023,b'T-Embed')+tlv(0x1024,b'0.1')+tlv(0x1042,b'1')+tlv(0x1054,bytes.fromhex('00010050f2040001'))+tlv(0x1011,b'WIFITE')+u8(0x103c,1)+u16(0x1002,0)+u16(0x1009,0)+u16(0x1012,0)+tlv(0x102d,bytes.fromhex('80000001'))
def signed(previous,msg):return msg+tlv(0x1005,mac(auth,previous+msg)[:8])
m2=signed(m1,m2)
pin=b'12345670';zero=bytes(16)
def ph(nonce,half):return mac(auth,nonce+mac(auth,half)[:16]+pke+pkr)
m3=signed(m2,base(7)+tlv(0x1039,rn)+tlv(0x1014,ph(zero,pin[:4]))+tlv(0x1015,ph(zero,pin[4:])))
def encrypted(payload,iv):
 payload+=tlv(0x101e,mac(auth,payload)[:8]);pad=16-len(payload)%16;payload+=bytes([pad])*pad
 e=Cipher(algorithms.AES(wrap),modes.CBC(iv)).encryptor();return tlv(0x1018,iv+e.update(payload)+e.finalize())
random48=bytes(range(48));iv=bytes(range(48,64))
m4=signed(m3,base(8)+tlv(0x101a,en)+tlv(0x103d,ph(random48[:16],pin[:4]))+tlv(0x103e,ph(random48[16:32],pin[4:]))+encrypted(tlv(0x103f,random48[:16]),random48[32:]))
m5=signed(m4,base(9)+tlv(0x1039,rn)+encrypted(tlv(0x1016,zero),iv))
m6=signed(m5,base(10)+tlv(0x101a,en)+encrypted(tlv(0x1040,random48[16:32]),iv))
m7=signed(m6,base(11)+tlv(0x1039,rn)+encrypted(tlv(0x1017,zero)+tlv(0x1045,b'LAB')+tlv(0x1027,b'fixture-password'),iv))

def lfsr(seed):
 while True:
  if not seed:seed=1
  byte=0
  for _ in range(8):
   bit=seed&1;seed=((seed^(0x80000057 if bit else 0))>>1)|(bit<<31);byte=(byte<<1)|bit
  yield byte
def ecos(seed,kind):
 while True:
  if kind==2:
   seed=(seed*1103515245+12345)&0xffffffff;v=seed&0xffe00000
   seed=(seed*1103515245+12345)&0xffffffff;v+=(seed&0xfffc0000)>>11
   seed=(seed*1103515245+12345)&0xffffffff;v+=(seed&0xfe000000)>>25
  elif kind==4:seed=(seed*1103515245+12345)&0xffffffff;v=seed
  else:
   seed=(48271*(seed%44488)-3399*(seed//44488))&0xffffffff
   if seed&0x80000000:seed=(seed+2147483647)&0xffffffff
   v=seed
  yield v&255
def take(g):return bytes(next(g) for _ in range(16))
libc=ctypes.CDLL(None);libc.srand.argtypes=[ctypes.c_uint]
def rtl(seed):
 libc.srand(seed);return b''.join(libc.rand().to_bytes(4,'big') for _ in range(4))
fixtures=[]
g=lfsr(12345);a,b,n=take(g),take(g),take(g);fixtures.append((1,n,a,b))
for mode in (2,4,5):
 g=ecos(1,mode);n,a,b=take(g),take(g),take(g);fixtures.append((mode,n,a,b))
fixtures.append((3,rtl(100000),rtl(100002),rtl(100004)))

with tempfile.TemporaryDirectory() as td:
 td=pathlib.Path(td)
 def array(name,b):return 'static const unsigned char '+name+'[]={'+','.join(str(x) for x in b)+'};\n'
 header=''.join(array(name,v) for name,v in dict(x=x,rn=rn,auth=auth,wrap=wrap,pke=pke,pkr=pkr,m1=m1,m2=m2,m3=m3,m4=m4,m5=m5,m6=m6,m7=m7,random48=random48,iv=iv).items())
 for mode,n,a,b in fixtures:
  header+=array(f'nonce{mode}',n)+array(f'hash1_{mode}',ph(a,pin[:4]))+array(f'hash2_{mode}',ph(b,pin[4:]))
 (td/'golden.h').write_text(header)
 mb=pathlib.Path(os.environ['MBEDTLS_ROOT']);lib=os.environ['MBEDTLS_LIB']
 exe=td/'test';src=ROOT/'components/wifi/wifite'
 subprocess.run(['gcc','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(td),'-I'+str(src),'-I'+str(mb/'include'),str(pathlib.Path(__file__).with_name('test_core.c')),str(src/'core.c'),str(src/'pixie.c'),lib,'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
