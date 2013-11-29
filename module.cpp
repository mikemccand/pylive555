// License from live555's testRTSPClient:

/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the
Free Software Foundation; either version 2.1 of the License, or (at your
option) any later version. (See <http://www.gnu.org/copyleft/lesser.html>.)

This library is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
more details.

You should have received a copy of the GNU Lesser General Public License
along with this library; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
**********/

/* Basic Python wrapper around live555's APIs for loading RTSP
 * streams */

#include <Python.h>

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"

// Forward function definitions:

// RTSP 'response handlers':
void continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString);
void continueAfterSETUP(RTSPClient* rtspClient, int resultCode, char* resultString);
void continueAfterPLAY(RTSPClient* rtspClient, int resultCode, char* resultString);

// Other event handler functions:
void subsessionAfterPlaying(void* clientData); // called when a stream's subsession (e.g., audio or video substream) ends
void subsessionByeHandler(void* clientData); // called when a RTCP "BYE" is received for a subsession
void streamTimerHandler(void* clientData);
  // called at the end of a stream's expected duration (if the stream has not already signaled its end using a RTCP "BYE")

// Used to iterate through each stream's 'subsessions', setting up each one:
void setupNextSubsession(RTSPClient* rtspClient);

// Used to shut down and close a stream (including its "RTSPClient" object):
void shutdownStream(RTSPClient* rtspClient, int exitCode = 1);

#define RTSP_CLIENT_VERBOSITY_LEVEL 0 // by default, print verbose output from each "RTSPClient"

// If you don't want to see debugging output for each received frame, then comment out the following line:
//#define DEBUG_PRINT_EACH_RECEIVED_FRAME 1

static PyObject *error;

static TaskScheduler* scheduler;
static UsageEnvironment* env;

static PyThreadState *threadState;

// A function that outputs a string that identifies each stream (for debugging output).  Modify this if you wish:
UsageEnvironment& operator<<(UsageEnvironment& env, const RTSPClient& rtspClient) {
  return env << "[URL:\"" << rtspClient.url() << "\"]: ";
}

// A function that outputs a string that identifies each subsession (for debugging output).  Modify this if you wish:
UsageEnvironment& operator<<(UsageEnvironment& env, const MediaSubsession& subsession) {
  return env << subsession.mediumName() << "/" << subsession.codecName();
}

void usage(UsageEnvironment& env, char const* progName) {
  env << "Usage: " << progName << " <rtsp-url-1> ... <rtsp-url-N>\n";
  env << "\t(where each <rtsp-url-i> is a \"rtsp://\" URL)\n";
}

// Define a class to hold per-stream state that we maintain throughout each stream's lifetime:

class StreamClientState {
public:
  StreamClientState();
  virtual ~StreamClientState();

public:
  Boolean useTCP;
  MediaSubsessionIterator* iter;
  MediaSession* session;
  MediaSubsession* subsession;
  PyObject *frameCallback;
  TaskToken streamTimerTask;
  double duration;
};

// If you're streaming just a single stream (i.e., just from a single URL, once), then you can define and use just a single
// "StreamClientState" structure, as a global variable in your application.  However, because - in this demo application - we're
// showing how to play multiple streams, concurrently, we can't do that.  Instead, we have to have a separate "StreamClientState"
// structure for each "RTSPClient".  To do this, we subclass "RTSPClient", and add a "StreamClientState" field to the subclass:

class ourRTSPClient: public RTSPClient {
public:
  static ourRTSPClient* createNew(UsageEnvironment& env,
                                  char const* rtspURL,
                                  PyObject* frameCallback,
				  int verbosityLevel = 0,
				  portNumBits tunnelOverHTTPPortNum = 0);

protected:
  ourRTSPClient(UsageEnvironment& env, char const* rtspURL, PyObject* frameCallback,
		int verbosityLevel, portNumBits tunnelOverHTTPPortNum);
  // called only by createNew();
  virtual ~ourRTSPClient();

public:
  StreamClientState scs;
};

// Define a data sink (a subclass of "MediaSink") to receive the data for each subsession (i.e., each audio or video 'substream').
// In practice, this might be a class (or a chain of classes) that decodes and then renders the incoming audio or video.
// Or it might be a "FileSink", for outputting the received data into a file (as is done by the "openRTSP" application).
// In this example code, however, we define a simple 'dummy' sink that receives incoming data, but does nothing with it.

class DummySink: public MediaSink {
public:
  static DummySink* createNew(UsageEnvironment& env,
			      MediaSubsession& subsession, // identifies the kind of data that's being received
                              PyObject *frameCallback,
			      char const* streamId = NULL, // identifies the stream itself (optional)
                              RTSPClient *rtspClient = NULL);

private:
  DummySink(UsageEnvironment& env, MediaSubsession& subsession, PyObject *frameCallback, char const* streamId, RTSPClient *rtspClient);
    // called only by "createNew()"
  virtual ~DummySink();

  static void afterGettingFrame(void* clientData, unsigned frameSize,
                                unsigned numTruncatedBytes,
				struct timeval presentationTime,
                                unsigned durationInMicroseconds);
  void afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
			 struct timeval presentationTime, unsigned durationInMicroseconds);

private:
  // redefined virtual functions:
  virtual Boolean continuePlaying();

private:
  PyObject *frameCallback;
  u_int8_t* fReceiveBuffer;
  RTSPClient *fRTSPClient;
  MediaSubsession& fSubsession;
  char* fStreamId;
  int first;
};


// Implementation of the RTSP 'response handlers':

void continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString) {
  do {
    UsageEnvironment& env = rtspClient->envir(); // alias
    StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

    if (resultCode != 0) {
      env << *rtspClient << "Failed to get a SDP description: " << resultString << "\n";
      delete[] resultString;
      break;
    }

    char* const sdpDescription = resultString;
    //env << *rtspClient << "Got a SDP description:\n" << sdpDescription << "\n";

    // Create a media session object from this SDP description:
    scs.session = MediaSession::createNew(env, sdpDescription);
    delete[] sdpDescription; // because we don't need it anymore
    if (scs.session == NULL) {
      env << *rtspClient << "Failed to create a MediaSession object from the SDP description: " << env.getResultMsg() << "\n";
      break;
    } else if (!scs.session->hasSubsessions()) {
      env << *rtspClient << "This session has no media subsessions (i.e., no \"m=\" lines)\n";
      break;
    }

    // Then, create and set up our data source objects for the session.  We do this by iterating over the session's 'subsessions',
    // calling "MediaSubsession::initiate()", and then sending a RTSP "SETUP" command, on each one.
    // (Each 'subsession' will have its own data source.)
    scs.iter = new MediaSubsessionIterator(*scs.session);
    setupNextSubsession(rtspClient);
    return;

    // nocommit why have a while loop that runs only once?
    // there is no continue?
  } while (0);

  // An unrecoverable error occurred with this stream.
  shutdownStream(rtspClient);
}

void setupNextSubsession(RTSPClient* rtspClient) {
  UsageEnvironment& env = rtspClient->envir(); // alias
  StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

  scs.subsession = scs.iter->next();
  if (scs.subsession != NULL) {
    // Only tap the video stream (the metadata stream never
    // seems to send anything):
    //if (strcmp(scs.subsession->codecName(), "H264")) {
    //setupNextSubsession(rtspClient);
    //return;
    //}

    if (!scs.subsession->initiate()) {
      env << *rtspClient << "Failed to initiate the \"" << *scs.subsession << "\" subsession: " << env.getResultMsg() << "\n";
      setupNextSubsession(rtspClient); // give up on this subsession; go to the next one
    } else {
      env << *rtspClient << "Initiated the \"" << *scs.subsession
	  << "\" subsession (client ports " << scs.subsession->clientPortNum() << "-" << scs.subsession->clientPortNum()+1 << ")\n";

      // Continue setting up this subsession, by sending a RTSP "SETUP" command:
      rtspClient->sendSetupCommand(*scs.subsession, continueAfterSETUP, False, scs.useTCP);
    }
    return;
  }

  // We've finished setting up all of the subsessions.  Now, send a RTSP "PLAY" command to start the streaming:
  if (scs.session->absStartTime() != NULL) {
    // Special case: The stream is indexed by 'absolute' time, so send an appropriate "PLAY" command:
    rtspClient->sendPlayCommand(*scs.session, continueAfterPLAY, scs.session->absStartTime(), scs.session->absEndTime());
  } else {
    scs.duration = scs.session->playEndTime() - scs.session->playStartTime();
    rtspClient->sendPlayCommand(*scs.session, continueAfterPLAY);
  }
}

void continueAfterSETUP(RTSPClient* rtspClient, int resultCode, char* resultString) {
  do {
    UsageEnvironment& env = rtspClient->envir(); // alias
    StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

    if (resultCode != 0) {
      env << *rtspClient << "Failed to set up the \"" << *scs.subsession << "\" subsession: " << resultString << "\n";
      break;
    }

    env << *rtspClient << "Set up the \"" << *scs.subsession
	<< "\" subsession (client ports " << scs.subsession->clientPortNum() << "-" << scs.subsession->clientPortNum()+1 << ")\n";

    // Having successfully setup the subsession, create a data sink for it, and call "startPlaying()" on it.
    // (This will prepare the data sink to receive data; the actual flow of data from the client won't start happening until later,
    // after we've sent a RTSP "PLAY" command.)

    scs.subsession->sink = DummySink::createNew(env, *scs.subsession, scs.frameCallback, rtspClient->url(), rtspClient);
    // perhaps use your own custom "MediaSink" subclass instead
    if (scs.subsession->sink == NULL) {
      env << *rtspClient << "Failed to create a data sink for the \"" << *scs.subsession
	  << "\" subsession: " << env.getResultMsg() << "\n";
      break;
    }

    env << *rtspClient << "Created a data sink for the \"" << *scs.subsession << "\" subsession\n";
    scs.subsession->miscPtr = rtspClient; // a hack to let subsession handle functions get the "RTSPClient" from the subsession 
    scs.subsession->sink->startPlaying(*(scs.subsession->readSource()),
				       subsessionAfterPlaying, scs.subsession);
    // Also set a handler to be called if a RTCP "BYE" arrives for this subsession:
    if (scs.subsession->rtcpInstance() != NULL) {
      scs.subsession->rtcpInstance()->setByeHandler(subsessionByeHandler, scs.subsession);
    }
  } while (0);
  delete[] resultString;

  // Set up the next subsession, if any:
  setupNextSubsession(rtspClient);
}

void continueAfterPLAY(RTSPClient* rtspClient, int resultCode, char* resultString) {
  Boolean success = False;

  do {
    UsageEnvironment& env = rtspClient->envir(); // alias
    StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

    if (resultCode != 0) {
      env << *rtspClient << "Failed to start playing session: " << resultString << "\n";
      break;
    }

    // Set a timer to be handled at the end of the stream's expected duration (if the stream does not already signal its end
    // using a RTCP "BYE").  This is optional.  If, instead, you want to keep the stream active - e.g., so you can later
    // 'seek' back within it and do another RTSP "PLAY" - then you can omit this code.
    // (Alternatively, if you don't want to receive the entire stream, you could set this timer for some shorter value.)
    if (scs.duration > 0) {
      unsigned const delaySlop = 2; // number of seconds extra to delay, after the stream's expected duration.  (This is optional.)
      scs.duration += delaySlop;
      unsigned uSecsToDelay = (unsigned)(scs.duration*1000000);
      scs.streamTimerTask = env.taskScheduler().scheduleDelayedTask(uSecsToDelay, (TaskFunc*)streamTimerHandler, rtspClient);
    }

    env << *rtspClient << "Started playing session";
    if (scs.duration > 0) {
      env << " (for up to " << scs.duration << " seconds)";
    }
    env << "...\n";

    success = True;
  } while (0);
  delete[] resultString;

  if (!success) {
    // An unrecoverable error occurred with this stream.
    shutdownStream(rtspClient);
  }
}


// Implementation of the other event handlers:

void subsessionAfterPlaying(void* clientData) {
  MediaSubsession* subsession = (MediaSubsession*)clientData;
  RTSPClient* rtspClient = (RTSPClient*)(subsession->miscPtr);

  // Begin by closing this subsession's stream:
  Medium::close(subsession->sink);
  subsession->sink = NULL;

  // Next, check whether *all* subsessions' streams have now been closed:
  MediaSession& session = subsession->parentSession();
  MediaSubsessionIterator iter(session);
  while ((subsession = iter.next()) != NULL) {
    if (subsession->sink != NULL) return; // this subsession is still active
  }

  // All subsessions' streams have now been closed, so shutdown the client:
  shutdownStream(rtspClient);
}

void subsessionByeHandler(void* clientData) {
  MediaSubsession* subsession = (MediaSubsession*)clientData;
  RTSPClient* rtspClient = (RTSPClient*)subsession->miscPtr;
  UsageEnvironment& env = rtspClient->envir(); // alias

  env << *rtspClient << "Received RTCP \"BYE\" on \"" << *subsession << "\" subsession\n";

  // Now act as if the subsession had closed:
  subsessionAfterPlaying(subsession);
}

void streamTimerHandler(void* clientData) {
  ourRTSPClient* rtspClient = (ourRTSPClient*)clientData;
  StreamClientState& scs = rtspClient->scs; // alias

  scs.streamTimerTask = NULL;

  // Shut down the stream:
  shutdownStream(rtspClient);
}

void shutdownStream(RTSPClient* rtspClient, int exitCode) {
  UsageEnvironment& env = rtspClient->envir(); // alias
  StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

  // First, check whether any subsessions have still to be closed:
  if (scs.session != NULL) { 
    Boolean someSubsessionsWereActive = False;
    MediaSubsessionIterator iter(*scs.session);
    MediaSubsession* subsession;

    while ((subsession = iter.next()) != NULL) {
      if (subsession->sink != NULL) {
	Medium::close(subsession->sink);
	subsession->sink = NULL;

	if (subsession->rtcpInstance() != NULL) {
          // nocommit
	  //subsession->rtcpInstance()->setByeHandler(NULL, NULL); // in case the server sends a RTCP "BYE" while handling "TEARDOWN"
	}

	someSubsessionsWereActive = True;
      }
    }

    if (someSubsessionsWereActive) {
      // Send a RTSP "TEARDOWN" command, to tell the server to shutdown the stream.
      // Don't bother handling the response to the
      // "TEARDOWN".
      rtspClient->sendTeardownCommand(*scs.session, NULL);
    }
  }

  env << *rtspClient << "Closing the stream.\n";
  Medium::close(rtspClient);
  // Note that this will also cause this stream's "StreamClientState" structure to get reclaimed.
}


// Implementation of "ourRTSPClient":

ourRTSPClient* ourRTSPClient::createNew(UsageEnvironment& env, char const* rtspURL, PyObject* frameCallback,
					int verbosityLevel, portNumBits tunnelOverHTTPPortNum) {
  return new ourRTSPClient(env, rtspURL, frameCallback, verbosityLevel, tunnelOverHTTPPortNum);
}

ourRTSPClient::ourRTSPClient(UsageEnvironment& env, char const* rtspURL, PyObject* frameCallback,
			     int verbosityLevel, portNumBits tunnelOverHTTPPortNum)
  : RTSPClient(env, rtspURL, verbosityLevel, "", tunnelOverHTTPPortNum, -1) {
  Py_INCREF(frameCallback);
  scs.frameCallback = frameCallback;
}

ourRTSPClient::~ourRTSPClient() {
}


// Implementation of "StreamClientState":

StreamClientState::StreamClientState()
  : iter(NULL), session(NULL), subsession(NULL), streamTimerTask(NULL), duration(0.0) {
}

StreamClientState::~StreamClientState() {
  delete iter;
  if (session != NULL) {
    // We also need to delete "session", and unschedule "streamTimerTask" (if set)
    UsageEnvironment& env = session->envir(); // alias

    env.taskScheduler().unscheduleDelayedTask(streamTimerTask);
    Medium::close(session);
  }
}


// Implementation of "DummySink":

// Define the size of the buffer that we'll use:
#define DUMMY_SINK_RECEIVE_BUFFER_SIZE 1024*1024

DummySink* DummySink::createNew(UsageEnvironment& env, MediaSubsession& subsession, PyObject *frameCallback, char const* streamId, RTSPClient *rtspClient) {
  return new DummySink(env, subsession, frameCallback, streamId, rtspClient);
}

DummySink::DummySink(UsageEnvironment& env, MediaSubsession& subsession, PyObject *frameCallbackIn, char const* streamId, RTSPClient *rtspClient)
  : MediaSink(env),
    fSubsession(subsession) {
  fStreamId = strDup(streamId);
  fReceiveBuffer = new u_int8_t[DUMMY_SINK_RECEIVE_BUFFER_SIZE];
  frameCallback = frameCallbackIn;
  fRTSPClient = rtspClient;
  first = 1;
}

DummySink::~DummySink() {
  delete[] fReceiveBuffer;
  delete[] fStreamId;
  Py_DECREF(frameCallback);
}

void DummySink::afterGettingFrame(void* clientData, unsigned frameSize, unsigned numTruncatedBytes,
				  struct timeval presentationTime, unsigned durationInMicroseconds) {
  DummySink* sink = (DummySink*)clientData;
  sink->afterGettingFrame(frameSize, numTruncatedBytes, presentationTime, durationInMicroseconds);
}

void DummySink::afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
				  struct timeval presentationTime, unsigned durationInUS) {
  PyEval_RestoreThread(threadState);
  if (first == 1) {
    // NOTE: only necessary for H264 I think?
    unsigned numSPropRecords;
    SPropRecord* sPropRecords = parseSPropParameterSets(fSubsession.fmtp_spropparametersets(), numSPropRecords);
    for(unsigned i=0;i<numSPropRecords;i++) {
      PyObject *result = PyEval_CallFunction(frameCallback, "sy#iii", fSubsession.codecName(), sPropRecords[i].sPropBytes, sPropRecords[i].sPropLength, -1, -1, -1);
      if (result == NULL) {
        fprintf(stderr, "Exception in RTSP callback:\n");
        PyErr_PrintEx(1);
        fSource->stopGettingFrames();
        env->taskScheduler().scheduleDelayedTask(0, (TaskFunc*)shutdownStream, fRTSPClient);
        PyEval_SaveThread();
        return;
        //break;
      }
    }
    delete[] sPropRecords;
    first = 0;
  }

  // TODO: can we somehow avoid ... making a full copy here:
  //printf("%d bytes\n", frameSize);fflush(stdout);
  PyObject *result = PyEval_CallFunction(frameCallback, "sy#llI", fSubsession.codecName(), fReceiveBuffer, frameSize, presentationTime.tv_sec, presentationTime.tv_usec, durationInUS);
  if (result == NULL) {
    fprintf(stderr, "Exception in RTSP callback:");
    PyErr_PrintEx(1);
    fSource->stopGettingFrames();
    env->taskScheduler().scheduleDelayedTask(0, (TaskFunc*)shutdownStream, fRTSPClient);
    PyEval_SaveThread();
    return;
  }
  PyEval_SaveThread();

  // We've just received a frame of data.  (Optionally) print out information about it:
#ifdef DEBUG_PRINT_EACH_RECEIVED_FRAME
  if (fStreamId != NULL) envir() << "Stream \"" << fStreamId << "\"; ";
  envir() << fSubsession.mediumName() << "/" << fSubsession.codecName() << ":\tReceived " << frameSize << " bytes";
  if (numTruncatedBytes > 0) envir() << " (with " << numTruncatedBytes << " bytes truncated)";
  char uSecsStr[6+1]; // used to output the 'microseconds' part of the presentation time
  sprintf(uSecsStr, "%06u", (unsigned)presentationTime.tv_usec);
  envir() << ".\tPresentation time: " << (int)presentationTime.tv_sec << "." << uSecsStr;
  if (fSubsession.rtpSource() != NULL && !fSubsession.rtpSource()->hasBeenSynchronizedUsingRTCP()) {
    envir() << "!"; // mark the debugging output to indicate that this presentation time is not RTCP-synchronized
  }
#ifdef DEBUG_PRINT_NPT
  envir() << "\tNPT: " << fSubsession.getNormalPlayTime(presentationTime);
#endif
  envir() << "\n";
#endif
  
  // Then continue, to request the next frame of data:
  continuePlaying();
}

Boolean DummySink::continuePlaying() {
  if (fSource == NULL) return False; // sanity check (should not happen)

  // Request the next frame of data from our input source.  "afterGettingFrame()" will get called later, when it arrives:
  fSource->getNextFrame(fReceiveBuffer, DUMMY_SINK_RECEIVE_BUFFER_SIZE,
                        afterGettingFrame, this,
                        onSourceClosure, this);
  return True;
}

static PyObject *
startRTSP(PyObject *self, PyObject *args)
{
  const char *rtspURL;
  PyObject *frameCallback;
  int useTCP = 1;

  if (!PyArg_ParseTuple(args, "sO|i", &rtspURL, &frameCallback, &useTCP)) {
    return NULL;
  }

  if (!PyCallable_Check(frameCallback)) {
    PyErr_SetString(error, "callback must be a callable");
    return NULL;
  }

  // Begin by creating a "RTSPClient" object.  Note that there is a separate "RTSPClient" object for each stream that we wish
  // to receive (even if more than stream uses the same "rtsp://" URL).
  ourRTSPClient* rtspClient = ourRTSPClient::createNew(*env, rtspURL, frameCallback, RTSP_CLIENT_VERBOSITY_LEVEL);
  if (rtspClient == NULL) {
    PyErr_SetString(error, "failed to create RTSPClient");
    return NULL;
  }
  rtspClient->scs.useTCP = useTCP != 0;

  // Next, send a RTSP "DESCRIBE" command, to get a SDP description for the stream.
  // Note that this command - like all RTSP commands - is sent asynchronously; we do not block, waiting for a response.
  // Instead, the following function call returns immediately, and we handle the RTSP response later, from within the event loop:
  rtspClient->sendDescribeCommand(continueAfterDESCRIBE); 

  Py_INCREF(Py_None);
  return Py_None;
}

static char stopEventLoopFlag = 0;

static PyObject *
runEventLoop(PyObject *self, PyObject *args)
{
  stopEventLoopFlag = 0;

  // All subsequent activity takes place within the event loop:
  threadState = PyEval_SaveThread();
  env->taskScheduler().doEventLoop(&stopEventLoopFlag);
  PyEval_RestoreThread(threadState);

  Py_INCREF(Py_None);
  return Py_None;
}

static PyObject *
stopEventLoop(PyObject *self, PyObject *args)
{
  stopEventLoopFlag = 1;

  Py_INCREF(Py_None);
  return Py_None;
}

static PyMethodDef moduleMethods[] = {
  {"startRTSP",  startRTSP, METH_VARARGS, "Start loading frames from the provided RTSP url.  First argument is the URL string (should be rtsp://username:password@host/...; second argument is a callback function called once per received frame; third agument is False if UDP transport should be used and True if TCP transport should be used."},
  {"runEventLoop",  runEventLoop, METH_NOARGS, "Run the event loop."},
  {"stopEventLoop",  stopEventLoop, METH_NOARGS, "Stop the event loop, which will cause runEventLoop (in another thread) to stop and return."},
  {NULL, NULL, 0, NULL}        /* Sentinel */
};

static struct PyModuleDef module = {
  PyModuleDef_HEAD_INIT,
  "live555",   /* name of module */
  NULL, /* module documentation, may be NULL */
  -1,       /* size of per-interpreter state of the module,
               or -1 if the module keeps state in global variables. */
  moduleMethods
};

PyMODINIT_FUNC
PyInit_live555(void)
{
  PyObject *m;

  m = PyModule_Create(&module);
  if (m == NULL) {
    return NULL;
  }

  error = PyErr_NewException("live555.error", NULL, NULL);
  Py_INCREF(error);
  PyModule_AddObject(m, "error", error);

  // Begin by setting up our usage environment:
  scheduler = BasicTaskScheduler::createNew();
  env = BasicUsageEnvironment::createNew(*scheduler);

  return m;
}
