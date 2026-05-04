import os, serial, sys, time

port = os.environ.get('ESPPORT', 'COM14')
ser = serial.Serial(port, 115200, timeout=1)

start = time.time()
while time.time() - start < 60:
    data = ser.read(4096)
    if data:
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
ser.close()
