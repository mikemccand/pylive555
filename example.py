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

def oneFrame(codecName, bytes, sec, usec, durUSec):
  print('frame for %s: %d bytes' % (codecName, len(bytes)))
  fOut.write(b'\0\0\0\1' + bytes)

def oneFrame2(codecName, bytes, sec, usec, durUSec):
  print('frame (handle2) for %s: %d bytes' % (codecName, len(bytes)))
  fOut.write(b'\0\0\0\1' + bytes)

# Starts pulling frames from the URL, with the provided callback:
useTCP = True 
handle = live555.startRTSP(url.format(username=username, password=password, cameraIP=cameraIP, channel=102), oneFrame, useTCP)
print('got handle {}'.format(handle))

handle2 = live555.startRTSP(url.format(username=username, password=password, cameraIP=cameraIP, channel=202), oneFrame2, useTCP)
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

handle3 = live555.startRTSP(url.format(username=username, password=password, cameraIP=cameraIP, channel=302), oneFrame3, useTCP)
print('got handle3 {}'.format(handle3))

live555.stopRTSP(handle)
print('stopped')
endTime = time.time() + seconds
while time.time() < endTime:
  time.sleep(0.1)

live555.stopRTSP(handle2)
print('stopped2')
handle = live555.startRTSP(url.format(username=username, password=password, cameraIP=cameraIP, channel=102), oneFrame, useTCP)
print('got handle {}'.format(handle))

endTime = time.time() + seconds
while time.time() < endTime:
  time.sleep(0.1)

live555.stopRTSP(handle3)
print('stopped3')
time.sleep(0.5)

# Tell Live555's event loop to stop:
live555.stopEventLoop()

# Wait for the background thread to finish:
t.join()

