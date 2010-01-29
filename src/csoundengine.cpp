/*
    Copyright (C) 2008, 2009 Andres Cabrera
    mantaraya36@gmail.com

    This file is part of QuteCsound.

    QuteCsound is free software; you can redistribute it
    and/or modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    QuteCsound is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with Csound; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
    02111-1307 USA
*/

#include "csoundengine.h"
#include "widgetlayout.h"
#include "curve.h"
#include "console.h"

CsoundEngine::CsoundEngine()
{
  // Initialize user data pointer passed to Csound
  ud = (CsoundUserData *)malloc(sizeof(CsoundUserData));
  ud->PERF_STATUS = 0;
  ud->cs = this;
  ud->threaded = true;
  ud->csound = NULL;
  //FIXME set widget layout ud->wl =
  pFields = (MYFLT *) calloc(EVENTS_MAX_PFIELDS, sizeof(MYFLT)); // Maximum number of p-fields for events

  m_recording = false;
  bufferSize = 4096;
  recBuffer = (MYFLT *) calloc(bufferSize, sizeof(MYFLT));

  int init = csoundInitialize(0,0,0);
  if (init < 0) {
    qDebug("CsoundEngine::CsoundEngine() Error initializing Csound!\nQutecsound will probably crash if you try to run Csound.");
  }
#ifndef QUTECSOUND_DESTROY_CSOUND
  // Create only once
  csound=csoundCreate(0);
#endif

  ud->perfThread = 0;

  eventQueue.resize(QUTECSOUND_MAX_EVENTS);
  eventTimeStamps.resize(QUTECSOUND_MAX_EVENTS);
  eventQueueSize = 0;

  queueTimer.setSingleShot(true);
  connect(&queueTimer, SIGNAL(timeout()), this, SLOT(dispatchQueues()));
  refreshTime = QCS_QUEUETIMER_DEFAULT_TIME;  // Eventually allow this to be changed
  dispatchQueues(); // starts queue dispatcher timer

  ud->mouseValues.resize(6); // For _MouseX _MouseY _MouseRelX _MouseRelY _MouseBut1 and _MouseBut2 channels

  // FIXME connect(csEngine, SIGNAL(clearMessageQueue()), this, SLOT(clearMessageQueue()));
}

CsoundEngine::~CsoundEngine()
{
  // FIXME make sure this runs!
  qDebug() << "CsoundEngine::~CsoundEngine() ";
#ifndef QUTECSOUND_DESTROY_CSOUND
  csoundDestroy(csound);
#endif
  free(ud);
  free(pFields);
  delete recBuffer;
}

void CsoundEngine::messageCallbackNoThread(CSOUND *csound,
                                          int /*attr*/,
                                          const char *fmt,
                                          va_list args)
{
  CsoundUserData *ud = (CsoundUserData *) csoundGetHostData(csound);
  QString msg;
  msg = msg.vsprintf(fmt, args);
  for (int i = 0; i < ud->cs->consoles.size(); i++) {
    ud->cs->consoles[i]->appendMessage(msg);
    ud->cs->consoles[i]->scrollToEnd();
  }
}

void CsoundEngine::messageCallbackThread(CSOUND *csound,
                                          int /*attr*/,
                                          const char *fmt,
                                          va_list args)
{
  CsoundUserData *ud = (CsoundUserData *) csoundGetHostData(csound);
  QString msg;
  msg = msg.vsprintf(fmt, args);
  ud->cs->queueMessage(msg);
}

void CsoundEngine::outputValueCallbackThread (CSOUND *csound,
                                     const char *channelName,
                                     MYFLT value)
{
  CsoundUserData *ud = (CsoundUserData *) csoundGetHostData(csound);
  if (ud->perfThread->isRunning()) {
    QString name = QString(channelName);
    ud->cs->perfMutex.lock();
    if (name.startsWith('$')) {
      QString channelName = name;
      channelName.chop(name.size() - (int) value + 1);
      QString sValue = name;
      sValue = sValue.right(name.size() - (int) value);
      channelName.remove(0,1);
      ud->cs->queueOutString(channelName, sValue);
    }
    else {
      ud->cs->queueOutValue(name, value);
    }
    ud->cs->perfMutex.unlock();
  }
}

void CsoundEngine::inputValueCallbackThread (CSOUND *csound,
                                     const char *channelName,
                                     MYFLT *value)
{
  // from qutecsound to Csound
  CsoundUserData *ud = (CsoundUserData *) csoundGetHostData(csound);
  if (ud->perfThread->isRunning()) {
    QString name = QString(channelName);
    ud->cs->perfMutex.lock();
    if (name.startsWith('$')) { // channel is a string channel
      int index = ud->channelNames.indexOf(name.mid(1));
      char *string = (char *) value;
      if (index>=0) {
        strcpy(string, ud->stringValues[index].toStdString().c_str());
      }
      else {
        string[0] = '\0'; //empty c string
      }
    }
    else {  // Not a string channel
      int index = ud->channelNames.indexOf(name);
      if (index>=0)
        *value = (MYFLT) ud->values[index];
      else {
        *value = 0;
      }
      //FIXME check if mouse tracking is active
      if (name == "_MouseX") {
        *value = (MYFLT) ud->mouseValues[0];
      }
      else if (name == "_MouseY") {
        *value = (MYFLT) ud->mouseValues[1];
      }
      else if(name == "_MouseRelX") {
        *value = (MYFLT) ud->mouseValues[2];
      }
      else if(name == "_MouseRelY") {
        *value = (MYFLT) ud->mouseValues[3];
      }
      else if(name == "_MouseBut1") {
        *value = (MYFLT) ud->mouseValues[4];
      }
      else if(name == "_MouseBut2") {
        *value = (MYFLT) ud->mouseValues[5];
      }
    }
    ud->cs->perfMutex.unlock();
  }
}

void CsoundEngine::outputValueCallback (CSOUND *csound,
                                     const char *channelName,
                                     MYFLT value)
{
  CsoundUserData *ud = (CsoundUserData *) csoundGetHostData(csound);
  if (ud->PERF_STATUS == 1) {
    QString name = QString(channelName);
    ud->cs->perfMutex.lock();
    if (name.startsWith('$')) {
      QString channelName = name;
      channelName.chop(name.size() - (int) value + 1);
      QString sValue = name;
      sValue = sValue.right(name.size() - (int) value);
      channelName.remove(0,1);
      ud->cs->queueOutString(channelName, sValue);
    }
    else {
      ud->cs->queueOutValue(name, value);
    }
    ud->cs->perfMutex.unlock();
  }
}

void CsoundEngine::inputValueCallback (CSOUND *csound,
                                     const char *channelName,
                                     MYFLT *value)
{
  // from qutecsound to Csound
  CsoundUserData *ud = (CsoundUserData *) csoundGetHostData(csound);
  if (ud->PERF_STATUS == 1) {
    QString name = QString(channelName);
    ud->cs->perfMutex.lock();
    if (name.startsWith('$')) { // channel is a string channel
      int index = ud->channelNames.indexOf(name.mid(1));
      char *string = (char *) value;
      if (index>=0) {
        strcpy(string, ud->stringValues[index].toStdString().c_str());
      }
      else {
        string[0] = '\0'; //empty c string
      }
    }
    else {  // Not a string channel
      int index = ud->channelNames.indexOf(name);
      if (index>=0)
        *value = (MYFLT) ud->values[index];
      else {
        *value = 0;
      }
      //FIXME check if mouse tracking is active
      if (name == "_MouseX") {
        *value = (MYFLT) ud->mouseValues[0];
      }
      else if (name == "_MouseY") {
        *value = (MYFLT) ud->mouseValues[1];
      }
      else if(name == "_MouseRelX") {
        *value = (MYFLT) ud->mouseValues[2];
      }
      else if(name == "_MouseRelY") {
        *value = (MYFLT) ud->mouseValues[3];
      }
      else if(name == "_MouseBut1") {
        *value = (MYFLT) ud->mouseValues[4];
      }
      else if(name == "_MouseBut2") {
        *value = (MYFLT) ud->mouseValues[5];
      }
    }
    ud->cs->perfMutex.unlock();
  }
}

void CsoundEngine::makeGraphCallback(CSOUND *csound, WINDAT *windat, const char *name)
{
//   qDebug("qutecsound::makeGraph()");
  CsoundUserData *ud = (CsoundUserData *) csoundGetHostData(csound);
  windat->caption[CAPSIZE - 1] = 0; // Just in case...
  Polarity polarity;
    // translate polarities and hope the definition in Csound doesn't change.
  switch (windat->polarity) {
    case NEGPOL:
      polarity = POLARITY_NEGPOL;
      break;
    case POSPOL:
      polarity = POLARITY_POSPOL;
      break;
    case BIPOL:
      polarity = POLARITY_BIPOL;
      break;
    default:
      polarity = POLARITY_NOPOL;
  }
  Curve *curve
      = new Curve(windat->fdata,
                  windat->npts,
                  windat->caption,
                  polarity,
                  windat->max,
                  windat->min,
                  windat->absmax,
                  windat->oabsmax,
                  windat->danflag);
  curve->set_id((uintptr_t) curve);
  ud->wl->newCurve(curve);
  windat->windid = (uintptr_t) curve;
//   qDebug("qutecsound::makeGraphCallback %i", windat->windid);
}

void CsoundEngine::drawGraphCallback(CSOUND *csound, WINDAT *windat)
{
  CsoundUserData *udata = (CsoundUserData *) csoundGetHostData(csound);
  // FIXME what is this callback for????
//   qDebug("qutecsound::drawGraph()");
//  udata->qcs->updateCurve(windat);
}

void CsoundEngine::killGraphCallback(CSOUND *csound, WINDAT *windat)
{
//   udata->qcs->killCurve(windat);
  qDebug("qutecsound::killGraph()");
}

int CsoundEngine::exitGraphCallback(CSOUND *csound)
{
//  qDebug("qutecsound::exitGraphCallback()");
  CsoundUserData *udata = (CsoundUserData *) csoundGetHostData(csound);
  return udata->wl->killCurves(csound);
}

int CsoundEngine::keyEventCallback(void *userData,
                                 void *p,
                                 unsigned int type)
{
  if (type != CSOUND_CALLBACK_KBD_EVENT)
    return 1;
  CsoundUserData *ud = (CsoundUserData *) userData;
  WidgetLayout *wl = (WidgetLayout *) ud->wl;
  int *value = (int *) p;
  int key = ud->cs->popKeyPressEvent();
  if (key >= 0) {
    *value = key;
//     qDebug() << "Pressed: " << key;
  }
  else {
    key = ud->cs->popKeyReleaseEvent();
    if (key >= 0) {
      *value = key;
//       qDebug() << "Released: " << key;
    }
  }
  return 0;
}

// void CsoundEngine::ioCallback (CSOUND *csound,
//                              const char *channelName,
//                              MYFLT *value,
//                              int channelType
//                             )
// {
//   qDebug() << "qutecsound::ioCallback";
//   if (channelType & CSOUND_INPUT_CHANNEL) { // is Input Channel
//     if (channelType & CSOUND_CONTROL_CHANNEL) {
//       inputValueCallback(csound, channelName, value);
//     }
//     else if (channelType & CSOUND_AUDIO_CHANNEL) {
//     }
//     else if (channelType & CSOUND_STRING_CHANNEL) {
//     }
//   }
//   else if (channelType & CSOUND_OUTPUT_CHANNEL) { // Is output channel
//     if (channelType & CSOUND_CONTROL_CHANNEL) {
//       outputValueCallback(csound, channelName, *value);
//     }
//     else if (channelType & CSOUND_AUDIO_CHANNEL) {
//     }
//     else if (channelType & CSOUND_STRING_CHANNEL) {
//     }
//   }
// }

void CsoundEngine::csThread(void *data)
{
  CsoundUserData* udata = (CsoundUserData*)data;
  udata->outputBuffer = csoundGetSpout(udata->csound);
  for (int i = 0; i < udata->outputBufferSize*udata->numChnls; i++) {
    udata->audioOutputBuffer.put(udata->outputBuffer[i]/ udata->zerodBFS);
  }
  udata->wl->getValues(&udata->channelNames,
                       &udata->values,
                       &udata->stringValues);
  udata->wl->getMouseValues(&udata->mouseValues);
  //FIXME put back usage of invalue/outvalue
//  if (!udata->qcs->m_options.useInvalue) {
//    writeWidgetValues(udata);
//    readWidgetValues(udata);
//  }
  (udata->ksmpscount)++;
}

void CsoundEngine::readWidgetValues(CsoundUserData *ud)
{
  MYFLT* pvalue;
//   CsoundChannelListEntry **lst;
//   CsoundChannelListEntry *chan;
//   int num = csoundListChannels(ud->csound, lst);
//   for (int i = 0; i < num; i++) {
//     chan = lst[i];
//     if (chan) {  //Not sure why this check is needed here....
//       if (chan->type & (CSOUND_INPUT_CHANNEL | CSOUND_CONTROL_CHANNEL)) {
//         if(csoundGetChannelPtr(ud->csound, &pvalue, chan->name, 0) == 0) {
//           *pvalue = (MYFLT) ud->qcs->values[i];
//         }
//       }
//       else if (chan->type & (CSOUND_INPUT_CHANNEL | CSOUND_STRING_CHANNEL)) {
//         if(csoundGetChannelPtr(ud->csound, &pvalue, chan->name, 0) == 0) {
//           char *string = (char *) pvalue;
//           strcpy(string, ud->qcs->stringValues[i].toStdString().c_str());
//         }
//       }
//     }
//   }
//   csoundDeleteChannelList(ud->csound, *lst);

  for (int i = 0; i < ud->channelNames.size(); i++) {
    if(csoundGetChannelPtr(ud->csound, &pvalue, ud->channelNames[i].toStdString().c_str(),
        CSOUND_INPUT_CHANNEL | CSOUND_CONTROL_CHANNEL) == 0) {
      *pvalue = (MYFLT) ud->values[i];
    }
    if(csoundGetChannelPtr(ud->csound, &pvalue, ud->channelNames[i].toStdString().c_str(),
       CSOUND_INPUT_CHANNEL | CSOUND_STRING_CHANNEL) == 0) {
      char *string = (char *) pvalue;
      strcpy(string, ud->stringValues[i].toStdString().c_str());
    }
  }
  //FIXME check if mouse tracking is active
  if(csoundGetChannelPtr(ud->csound, &pvalue, "_MouseX",
        CSOUND_INPUT_CHANNEL | CSOUND_CONTROL_CHANNEL) == 0) {
      *pvalue = (MYFLT) ud->mouseValues[0];
  }
  else if(csoundGetChannelPtr(ud->csound, &pvalue, "_MouseY",
        CSOUND_INPUT_CHANNEL | CSOUND_CONTROL_CHANNEL) == 0) {
      *pvalue = (MYFLT) ud->mouseValues[1];
  }
  else if(csoundGetChannelPtr(ud->csound, &pvalue, "_MouseRelX",
        CSOUND_INPUT_CHANNEL | CSOUND_CONTROL_CHANNEL) == 0) {
      *pvalue = (MYFLT) ud->mouseValues[2];
  }
  else if(csoundGetChannelPtr(ud->csound, &pvalue, "_MouseRelY",
        CSOUND_INPUT_CHANNEL | CSOUND_CONTROL_CHANNEL) == 0) {
      *pvalue = (MYFLT) ud->mouseValues[3];
  }
  else if(csoundGetChannelPtr(ud->csound, &pvalue, "_MouseBut1",
        CSOUND_INPUT_CHANNEL | CSOUND_CONTROL_CHANNEL) == 0) {
      *pvalue = (MYFLT) ud->mouseValues[4];
  }
  else if(csoundGetChannelPtr(ud->csound, &pvalue, "_MouseBut2",
        CSOUND_INPUT_CHANNEL | CSOUND_CONTROL_CHANNEL) == 0) {
      *pvalue = (MYFLT) ud->mouseValues[5];
  }
}

void CsoundEngine::writeWidgetValues(CsoundUserData *ud)
{
//   qDebug("qutecsound::writeWidgetValues");
   MYFLT* pvalue;
   for (int i = 0; i < ud->channelNames.size(); i++) {
     if (ud->channelNames[i] != "") {
       if(csoundGetChannelPtr(ud->csound, &pvalue, ud->channelNames[i].toStdString().c_str(),
          CSOUND_OUTPUT_CHANNEL | CSOUND_CONTROL_CHANNEL) == 0) {
            ud->wl->setValue(i,*pvalue);
       }
       else if(csoundGetChannelPtr(ud->csound, &pvalue, ud->channelNames[i].toStdString().c_str(),
         CSOUND_OUTPUT_CHANNEL | CSOUND_STRING_CHANNEL) == 0) {
         ud->wl->setValue(i,QString((char *)pvalue));
       }
     }
   }
}

//void CsoundEngine::setThreaded(bool threaded)
//{
//  ud->m_threaded = threaded;
//}
//
//void CsoundEngine::setFiles(QString fileName1, QString fileName2)
//{
//  m_fileName1 = fileName1;
//  m_fileName2 = fileName2;
//}

void CsoundEngine::setOptions(const CsoundOptions &options)
{
  m_options = options;
}

void CsoundEngine::registerConsole(ConsoleWidget *c)
{
  //FIXME register consoles as they are created
  consoles.append(c);

  connect(c, SIGNAL(keyPressed(QString)),
          this, SLOT(keyPressForCsound(QString)));
  connect(c, SIGNAL(keyReleased(QString)),
          this, SLOT(keyReleaseForCsound(QString)));
}

void CsoundEngine::unregisterConsole(ConsoleWidget *c)
{
  //FIXME unregister consoles as they are destroyed
  int index = consoles.indexOf(c);
  if (index >= 0 )
    consoles.remove(index);
}

void CsoundEngine::setConsoleBufferSize(int size)
{
  m_consoleBufferSize = size;
}

void CsoundEngine::keyPressForCsound(QString key)
{
//   qDebug() << "keyPressForCsound " << key;
  keyMutex.lock(); // Is this lock necessary?
  keyPressBuffer << key;
  keyMutex.unlock();
}

void CsoundEngine::keyReleaseForCsound(QString key)
{
//   qDebug() << "keyReleaseForCsound " << key;
  keyMutex.lock(); // Is this lock necessary?
  keyReleaseBuffer << key;
  keyMutex.unlock();
}

int CsoundEngine::popKeyPressEvent()
{
  keyMutex.lock();
  int value = -1;
  if (!keyPressBuffer.isEmpty()) {
    value = (int) keyPressBuffer.takeFirst()[0].toAscii();
  }
  keyMutex.unlock();
  return value;
}

int CsoundEngine::popKeyReleaseEvent()
{
  keyMutex.lock();
  int value = -1;
  if (!keyReleaseBuffer.isEmpty()) {
    value = (int) keyReleaseBuffer.takeFirst()[0].toAscii() + 0x10000;
  }
  keyMutex.unlock();
  return value;
}

void CsoundEngine::processEventQueue()
{
  // This function should only be called when Csound is running
  // TODO would it be good to move the event queue to be a QVector<QStringList> to avoid splitting again here?
  while (eventQueueSize > 0) {
    eventQueueSize--;
    char type = eventQueue[eventQueueSize][0].unicode();
    // FIXME this should process both events from the widget panel and the live event panels!
    QStringList eventElements = eventQueue[eventQueueSize].remove(0,1).split(" ",QString::SkipEmptyParts);
    qDebug("type %c line: %s", type, eventQueue[eventQueueSize].toStdString().c_str());
    // eventElements.size() should never be larger than EVENTS_MAX_PFIELDS
    for (int j = 0; j < eventElements.size(); j++) {
      pFields[j] = (MYFLT) eventElements[j].toDouble();
    }
    if (ud->perfThread != 0) {
      //ScoreEvent is not working
      ud->perfThread->ScoreEvent(0, type, eventElements.size(), pFields);
//       ud->qcs->perfThread->InputMessage(ud->qcs->widgetPanel->eventQueue[ud->qcs->widgetPanel->eventQueueSize].remove(0,1).data());
//       perfThread->lock();
//       csoundScoreEvent(ud->csound,type ,ud->qcs->pFields, eventElements.size());
//       perfThread->unlock();
    }
    else {
      csoundScoreEvent(ud->csound, type, pFields, eventElements.size());
    }
  }
}

void CsoundEngine::queueOutValue(QString channelName, double value)
{
  ud->wl->newValue(QPair<QString, double>(channelName, value));
}

// void qutecsound::queueInValue(QString channelName, double value)
// {
//   inValueQueue.insert(channelName, value);
// }

void CsoundEngine::queueOutString(QString channelName, QString value)
{
//   qDebug() << "qutecsound::queueOutString";
  ud->wl->newValue(QPair<QString, QString>(channelName, value));
}


int CsoundEngine::play()
{
  if (!ud->perfThread->isRunning()) {
    return runCsound(true);
  }
  else {
    ud->perfThread->Play();
    return 0;
  }
}

void CsoundEngine::stop()
{
  stopCsound();
}

void CsoundEngine::pause()
{
  if (ud->perfThread->isRunning())
   ud->perfThread->Pause();
}



void CsoundEngine::startRecording(int sampleformat, QString fileName)
{
  if (isRunning()) {
    //FIXME run act must be checked according to the status of the current document when it changes
//    runAct->setChecked(true);
    play();
  }
  const int channels=ud->numChnls;
  const int sampleRate=ud->sampleRate;
  int format = SF_FORMAT_WAV;
  switch (sampleformat) {
      case 0:
    format |= SF_FORMAT_PCM_16;
    break;
      case 1:
    format |= SF_FORMAT_PCM_24;
    break;
      case 2:
    format |= SF_FORMAT_FLOAT;
    break;
  }
  qDebug("start recording: %s", fileName.toStdString().c_str());
  outfile = new SndfileHandle(fileName.toStdString().c_str(), SFM_WRITE, format, channels, sampleRate);
  // clip instead of wrap when converting floats to ints
  outfile->command(SFC_SET_CLIPPING, NULL, SF_TRUE);
  samplesWritten = 0;
  m_recording = true;

  QTimer::singleShot(20, this, SLOT(recordBuffer()));
}

void CsoundEngine::stopRecording()
{
  m_recording = false;  // Will be processed on next record buffer
}

void CsoundEngine::queueEvent(QString eventLine, int delay)
{
//   qDebug("CsoundEngine::queueEvent %s", eventLine.toStdString().c_str());
  if (eventQueueSize < QUTECSOUND_MAX_EVENTS) {
    eventMutex.lock();
    eventQueue[eventQueueSize] = eventLine;
    eventQueueSize++;
    eventMutex.unlock();
  }
  else
    qDebug("Warning: event queue full, event not processed");
}

int CsoundEngine::runCsound(bool useAPI)
{
  // FIXME change behavior. When play is pressed do pause, not stop
//  if ((ud->m_threaded && ud->perfThread->isRunning() ) ||
//           (!ud->m_threaded && ud->PERF_STATUS == 1)) { //If running, stop
//    stop();
//    return 0;
//  }

  if (useAPI) {
#ifdef MACOSX_PRE_SNOW
//Remember menu bar to set it after FLTK grabs it
    menuBarHandle = GetMenuBar();
#endif
    eventQueueSize = 0; //Flush events gathered while idle
    //   outValueQueue.clear();
//    inValueQueue.clear();
//    outStringQueue.clear();
    ud->audioOutputBuffer.allZero();

    char **argv;
    argv = (char **) calloc(33, sizeof(char*));
    // TODO use: PUBLIC int csoundSetGlobalEnv(const char *name, const char *value);

    // FIXME set realtime and filenames in options before calling this!
    int argc = m_options.generateCmdLine(argv);
#ifdef QUTECSOUND_DESTROY_CSOUND
    ud->csound=csoundCreate(0);
#endif

    // Message Callbacks must be set before compile, otherwise some information is missed
    if (ud->threaded) {
      csoundSetMessageCallback(ud->csound, &CsoundEngine::messageCallbackThread);
    }
    else {
      csoundSetMessageCallback(ud->csound, &CsoundEngine::messageCallbackNoThread);
    }
//    QString oldOpcodeDir = "";
    if (m_options.opcodedirActive) {
      // csoundGetEnv must be called after Compile or Precompile,
      // But I need to set OPCODEDIR before compile....
//      char *name = 0;
//      csoundGetEnv(csound,name);
//      oldOpcodeDir = QString(name);
//      qDebug() << oldOpcodeDir;
      csoundSetGlobalEnv("OPCODEDIR", m_options.opcodedir.toLocal8Bit());
    }
    csoundReset(ud->csound);
    csoundSetHostData(ud->csound, (void *) ud);
    csoundPreCompile(ud->csound);  //Need to run PreCompile to create the FLTK_Flags global variable

    int variable = csoundCreateGlobalVariable(ud->csound, "FLTK_Flags", sizeof(int));
    if (m_options.enableFLTK) {
      // disable FLTK graphs, but allow FLTK widgets
      *((int*) csoundQueryGlobalVariable(ud->csound, "FLTK_Flags")) = 4;
    }
    else {
//       qDebug("play() FLTK Disabled");
      *((int*) csoundQueryGlobalVariable(ud->csound, "FLTK_Flags")) = 3;
    }
//    qDebug("Command Line args:");
//    for (int index=0; index< argc; index++) {
//      qDebug() << argv[index];
//    }

    csoundSetIsGraphable(ud->csound, true);
    csoundSetMakeGraphCallback(ud->csound, &CsoundEngine::makeGraphCallback);
    csoundSetDrawGraphCallback(ud->csound, &CsoundEngine::drawGraphCallback);
    csoundSetKillGraphCallback(ud->csound, &CsoundEngine::killGraphCallback);
    csoundSetExitGraphCallback(ud->csound, &CsoundEngine::exitGraphCallback);

    if (ud->enableWidgets) {
      if (ud->useInvalue) {
        csoundSetInputValueCallback(ud->csound, &CsoundEngine::inputValueCallback);
        csoundSetOutputValueCallback(ud->csound, &CsoundEngine::outputValueCallback);
      }
      else {
        // Not really sure that this is worth the trouble, as it
        // is used only with chnsend and chnrecv which are deprecated
//         qDebug() << "csoundSetChannelIOCallback";
//         csoundSetChannelIOCallback(csound, &qutecsound::ioCallback);
      }
    }
    else {
      csoundSetInputValueCallback(ud->csound, NULL);
      csoundSetOutputValueCallback(ud->csound, NULL);
    }
    csoundSetCallback(ud->csound,
                      &CsoundEngine::keyEventCallback,
                      (void *) ud, CSOUND_CALLBACK_KBD_EVENT);

    ud->result=csoundCompile(ud->csound,argc,argv);
//    qDebug("Csound compiled %i", ud->result);
    if (ud->result!=CSOUND_SUCCESS or variable != CSOUND_SUCCESS) {
      qDebug("Csound compile failed!");
      stop();  // FIXME necessary here any longer?
      free(argv);
      // FIXME mark error lines: documentPages[curPage]->markErrorLines(m_console->errorLines);
      return -3;
    }
    ud->zerodBFS = csoundGet0dBFS(ud->csound);
    ud->sampleRate = csoundGetSr(ud->csound);
    ud->numChnls = csoundGetNchnls(ud->csound);
    ud->outputBufferSize = csoundGetKsmps(ud->csound);
    ud->ksmpscount = 0;

    //TODO is something here necessary to work with doubles?
//     PUBLIC int csoundGetSampleFormat(CSOUND *);
//     PUBLIC int csoundGetSampleSize(CSOUND *);
    unsigned int numWidgets = ud->wl->widgetCount();
    ud->channelNames.resize(numWidgets*2);
    ud->values.resize(numWidgets*2);
    ud->stringValues.resize(numWidgets*2);
    if (ud->threaded) {  // Run threaded
      // First update values from widgets
      if (ud->enableWidgets) {
        ud->wl->getValues(&ud->channelNames,
                          &ud->values,
                          &ud->stringValues);
      }
      ud->perfThread = new CsoundPerformanceThread(ud->csound);
      ud->perfThread->SetProcessCallback(CsoundEngine::csThread, (void*)ud);
//      qDebug() << "qutecsound::runCsound perfThread->Play";
      ud->perfThread->Play();
    } /*if (ud->thread)*/
    else { // Run in the same thread
      ud->PERF_STATUS = 1;
      while(ud->PERF_STATUS == 1 && csoundPerformKsmps(ud->csound)==0) {
        processEventQueue();
        CsoundEngine::csThread(ud);
        qApp->processEvents(); // Must process events last to avoid stopping and calling csThread invalidly
      }
      stop();  // To flush pending queues
#ifdef MACOSX_PRE_SNOW
// Put menu bar back
      SetMenuBar(menuBarHandle);
#endif
    }
    for (int i = 0; i < 33; i++) {
      if (argv[i] != 0)
        free(argv[i]);
    }
    free(argv);
//    if (oldOpcodeDir != "") {
//      csoundSetGlobalEnv("OPCODEDIR", oldOpcodeDir.toLocal8Bit());
//    }
  }
  else {  // Run in external shell

  }
}

void CsoundEngine::stopCsound()
{
  qDebug("qutecsound::stopCsound()");
  if (ud->threaded) {
//    perfThread->ScoreEvent(0, 'e', 0, 0);
    if (ud->perfThread->isRunning()) {
      ud->perfThread->Stop();
      ud->perfThread->Join();
      delete ud->perfThread;
      ud->perfThread = 0;
    }
  } /*if (m_options.threaded)*/
  else {
    csoundStop(ud->csound);
    ud->PERF_STATUS = 0;
    csoundCleanup(ud->csound);
    for (int i = 0; i < ud->cs->consoles.size(); i++) {
      ud->cs->consoles[i]->scrollToEnd();
    }
  }
#ifdef MACOSX_PRE_SNOW
// Put menu bar back
  SetMenuBar(menuBarHandle);
#endif
  foreach (QString msg, messageQueue) { //Flush pending messages
    for (int i = 0; i < ud->cs->consoles.size(); i++) {
      ud->cs->consoles[i]->appendMessage(msg);
    }
    ud->wl->appendMessage(msg);
  }
#ifdef QUTECSOUND_DESTROY_CSOUND
  csoundDestroy(ud->csound);
#endif
}

void CsoundEngine::dispatchQueues()
{
//   qDebug("qutecsound::dispatchQueues()");
  int counter = 0;
  ud->wl->processNewValues();
  if (isRunning()) {
    while ((m_consoleBufferSize <= 0 || counter++ < m_consoleBufferSize) && isRunning()) {
      messageMutex.lock();
      if (messageQueue.isEmpty()) {
        messageMutex.unlock();
        break;
      }
      QString msg = messageQueue.takeFirst();
      messageMutex.unlock();
      for (int i = 0; i < ud->cs->consoles.size(); i++) {
        ud->cs->consoles[i]->appendMessage(msg);
        ud->cs->consoles[i]->scrollToEnd();
      }
      ud->wl->appendMessage(msg);
//      qApp->processEvents(); //FIXME Is this needed here to avoid display problems in the console?
      ud->wl->refreshConsoles();  // Scroll to end of text all console widgets
    }
    messageMutex.lock();
    if (!messageQueue.isEmpty() && m_consoleBufferSize > 0 && counter >= m_consoleBufferSize) {
      messageQueue.clear();
      messageQueue << "\nQUTECSOUND: Message buffer overflow. Messages discarded!\n";
    }
    messageMutex.unlock();
    //   QList<QString> channels = outValueQueue.keys();
    //   foreach (QString channel, channels) {
    //     widgetPanel->setValue(channel, outValueQueue[channel]);
    //   }
    //   outValueQueue.clear();

//    stringValueMutex.lock();
//    QStringList channels = outStringQueue.keys();
//    for  (int i = 0; i < channels.size(); i++) {
//      m_widgetLayout->setValue(channels[i], outStringQueue[channels[i]]);
//    }
//    outStringQueue.clear();
//    stringValueMutex.unlock();
    processEventQueue();
    while (!newCurveBuffer.isEmpty()) {
      Curve * curve = newCurveBuffer.pop();
  // //     qDebug("qutecsound::dispatchQueues() %i-%s", index, curve->get_caption().toStdString().c_str());
        ud->wl->newCurve(curve);
    }
    if (curveBuffer.size() > 32) {
      qDebug("qutecsound::dispatchQueues() WARNING: curve update buffer too large!");
      curveBuffer.resize(32);
    }
    foreach (WINDAT * windat, curveBuffer){
      Curve *curve = ud->wl->getCurveById(windat->windid);
      if (curve != 0) {
  //       qDebug("qutecsound::dispatchQueues() %s -- %s",windat->caption, curve->get_caption().toStdString().c_str());
        curve->set_size(windat->npts);      // number of points
        curve->set_data(windat->fdata);
        curve->set_caption(QString(windat->caption)); // title of curve
    //     curve->set_polarity(windat->polarity); // polarity
        curve->set_max(windat->max);        // curve max
        curve->set_min(windat->min);        // curve min
        curve->set_absmax(windat->absmax);     // abs max of above
    //     curve->set_y_scale(windat->y_scale);    // Y axis scaling factor
        ud->wl->setCurveData(curve);
      }
      curveBuffer.remove(curveBuffer.indexOf(windat));
    }
    qApp->processEvents();
//    if (m_options.thread) {
      // FIXME it's necessary to check this, otherwise even though performance has ended, no one will find out
//      if (m_csEngine->GetStatus() != 0) {
//        stop();
//      }
//    }
  }
  queueTimer.start(refreshTime); //will launch this function again later
}

void CsoundEngine::queueMessage(QString message)
{
  messageMutex.lock();
  messageQueue << message;
  messageMutex.unlock();
}

void CsoundEngine::clearMessageQueue()
{
  messageMutex.lock();
  messageQueue.clear();
  messageMutex.unlock();
}

void CsoundEngine::recordBuffer()
{
  if (m_recording == 1) {
    if (ud->audioOutputBuffer.copyAvailableBuffer(recBuffer, bufferSize)) {
      int samps = outfile->write(recBuffer, bufferSize);
      samplesWritten += samps;
    }
    else {
//       qDebug("qutecsound::recordBuffer() : Empty Buffer!");
    }
    QTimer::singleShot(20, this, SLOT(recordBuffer()));
  }
  else { //Stop recording
    delete outfile;
    qDebug("Recording stopped. Written %li samples", samplesWritten);
  }
}

bool CsoundEngine::isRunning()
{
  if (ud->threaded) {
    return ud->perfThread->isRunning();
  }
  else {
    return ud->PERF_STATUS;
  }
}
