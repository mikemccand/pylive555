import time
import sys
import live555
import threading

# Shows how to use live555 module to pull frames from an RTSP/RTP
# source.  Run this (likely first customizing the URL below:

# Example: python3 example.py 10.17.4.118 admin 10 out.264 
if len(sys.argv) != 5:
  print()
  print('Usage: python3 example.py cameraIP username seconds fileOut')
  print()
  sys.exit(1)
  
cameraIP = sys.argv[1]
username = sys.argv[2]
seconds = float(sys.argv[3])
fileOut = sys.argv[4]

password = input("Enter password:")

# NOTE: the username & password, and the URL path, will vary from one
# camera to another!  This URL path works with HikVision/Dahua NVR and pulls a set of DVR channels:
url = 'rtsp://{username}:{password}@{cameraIP}/Streaming/Channels/{channel}'

fOut = open(fileOut, 'wb')

def shutdownCallback():
  print('shutdown callback')

def oneFrame(codecName, bytes, sec, usec, durUSec):
  print('frame for %s: %d bytes' % (codecName, len(bytes)))
  fOut.write(b'\0\0\0\1' + bytes)

def oneFrame2(codecName, bytes, sec, usec, durUSec):
  print('frame (handle2) for %s: %d bytes' % (codecName, len(bytes)))
  fOut.write(b'\0\0\0\1' + bytes)

# Starts pulling frames from the URL, with the provided callback:
useTCP = True 
handle = live555.startRTSP(url.format(username=username, password=password, cameraIP=cameraIP, channel=102), oneFrame, shutdownCallback, useTCP)
print('got handle {}'.format(handle))

handle2 = live555.startRTSP(url.format(username=username, password=password, cameraIP=cameraIP, channel=202), oneFrame2, shutdownCallback, useTCP)
print('got handle2 {}'.format(handle2))

# Run Live555's event loop in a background thread:
t = threading.Thread(target=live555.runEventLoop, args=())
t.setDaemon(True)
t.start()

endTime = time.time() + seconds
while time.time() < endTime:
  time.sleep(0.1)

def oneFrame3(codecName, bytes, sec, usec, durUSec):
  print('frame (handle3) for %s: %d bytes' % (codecName, len(bytes)))
  fOut.write(b'\0\0\0\1' + bytes)

handle3 = live555.startRTSP(url.format(username=username, password=password, cameraIP=cameraIP, channel=302), oneFrame3, shutdownCallback, useTCP)
print('got handle3: {}'.format(handle3))

print('stopping first handle')
try:
  live555.stopRTSP(handle)
except live555.error as e:
  print('live555 error received')
print('stopped first handle')
endTime = time.time() + seconds
while time.time() < endTime:
  time.sleep(0.1)

print('stopping 2nd handle')
try:
  live555.stopRTSP(handle2)
except live555.error as e:
  print('live555 error received')
  pass
print('stopped 2nd handle')
handle = live555.startRTSP(url.format(username=username, password=password, cameraIP=cameraIP, channel=102), oneFrame, shutdownCallback, useTCP)
print('got handle {}'.format(handle))

endTime = time.time() + seconds
while time.time() < endTime:
  time.sleep(0.1)

print('stopping 3rd handle')
try:
  live555.stopRTSP(handle3)
except live555.error as e:
  print('live555 error received')
  pass
print('stopped 3rd handle')
time.sleep(5)
print('slept after 3rd handle')

try:
  live555.stopRTSP(handle)
except live555.error as e:
  print('live555 error received')
  pass

# Tell Live555's event loop to stop:
try:
  live555.stopEventLoop()
except:
  import traceback
  print('exception received')
  traceback.print_exc()

# Wait for the background thread to finish:
t.join()

