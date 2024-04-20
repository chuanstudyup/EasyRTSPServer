#include "EasyRTSPServer.h"
#include "base64.h"

char const* DateHeader() {
  static char buf[128] = { 0 };
  time_t tt = time(NULL);
  strftime(buf, sizeof(buf), "Date: %a, %b %d %Y %H:%M:%S GMT", gmtime(&tt));
  return buf;
}

void RTPPacket::setRtpHeader(uint32_t seq, uint32_t timestamp) {
  m_rtpBuf[7] = seq & 0x0FF;               // each packet is counted with a sequence counter
  m_rtpBuf[6] = seq >> 8;
  
  m_rtpBuf[8] = (timestamp & 0xFF000000) >> 24;  // each image gets a timestamp
  m_rtpBuf[9] = (timestamp & 0x00FF0000) >> 16;
  m_rtpBuf[10] = (timestamp & 0x0000FF00) >> 8;
  m_rtpBuf[11] = (timestamp & 0x000000FF);
}

int RTPPacket::packRtpPack(unsigned const char* jpeg, uint32_t jpegLen, int fragmentOffset, BufPtr quant0tbl, BufPtr quant1tbl, StreamInfo* streamInfo) {
  int fragmentLen = MAX_FRAGMENT_SIZE;
  if (fragmentLen + fragmentOffset > jpegLen)  // Shrink last fragment if needed
    fragmentLen = jpegLen - fragmentOffset;

  m_isLastFragment = (fragmentOffset + fragmentLen) == jpegLen;

  // Do we have custom quant tables? If so include them per RFC

  bool includeQuantTbl = quant0tbl && quant1tbl && fragmentOffset == 0;
  uint8_t q = includeQuantTbl ? 128 : 0x5e;

  m_RtpPacketSize = fragmentLen + KRtpHeaderSize + KJpegHeaderSize + (includeQuantTbl ? (4 + 64 * 2) : 0);

  memset(m_rtpBuf, 0x00, sizeof(m_rtpBuf));
  // Prepare the first 4 byte of the packet. This is the Rtp over Rtsp header in case of TCP based transport
  m_rtpBuf[0] = '$';  // magic number
  m_rtpBuf[1] = 0;    // number of multiplexed subchannel on RTPS connection - here the RTP channel
  m_rtpBuf[2] = (m_RtpPacketSize & 0x0000FF00) >> 8;
  m_rtpBuf[3] = (m_RtpPacketSize & 0x000000FF);
  // Prepare the 12 byte RTP header
  m_rtpBuf[4] = 0x80;                                   // RTP version
  m_rtpBuf[5] = 0x1a | (m_isLastFragment ? 0x80 : 0x00);  // JPEG payload (26) and marker bit
  m_rtpBuf[12] = 0x13;  // 4 byte SSRC (sychronization source identifier)
  m_rtpBuf[13] = 0xf9;  // we just an arbitrary number here to keep it simple
  m_rtpBuf[14] = 0x7e;
  m_rtpBuf[15] = 0x67;

  // Prepare the 8 byte payload JPEG header
  m_rtpBuf[16] = 0x00;                                 // type specific
  m_rtpBuf[17] = (fragmentOffset & 0x00FF0000) >> 16;  // 3 byte fragmentation offset for fragmented images
  m_rtpBuf[18] = (fragmentOffset & 0x0000FF00) >> 8;
  m_rtpBuf[19] = (fragmentOffset & 0x000000FF);

  /*    These sampling factors indicate that the chrominance components of
       type 0 video is downsampled horizontally by 2 (often called 4:2:2)
       while the chrominance components of type 1 video are downsampled both
       horizontally and vertically by 2 (often called 4:2:0). */
  m_rtpBuf[20] = 0x00;                        // type (fixme might be wrong for camera data) https://tools.ietf.org/html/rfc2435
  m_rtpBuf[21] = q;                           // quality scale factor was 0x5e
  m_rtpBuf[22] = streamInfo->m_width / 8;   // width  / 8
  m_rtpBuf[23] = streamInfo->m_height / 8;  // height / 8

  int headerLen = 24;     // Inlcuding jpeg header but not qant table header
  if (includeQuantTbl) {  // we need a quant header - but only in first packet of the frame
    //if ( debug ) printf("inserting quanttbl\n");
    m_rtpBuf[24] = 0;  // MBZ
    m_rtpBuf[25] = 0;  // 8 bit precision
    m_rtpBuf[26] = 0;  // MSB of lentgh

    int numQantBytes = 64;          // Two 64 byte tables
    m_rtpBuf[27] = 2 * numQantBytes;  // LSB of length

    headerLen += 4;

    memcpy(m_rtpBuf + headerLen, quant0tbl, numQantBytes);
    headerLen += numQantBytes;

    memcpy(m_rtpBuf + headerLen, quant1tbl, numQantBytes);
    headerLen += numQantBytes;
  }
  // if ( debug ) printf("Sending timestamp %d, seq %d, fragoff %d, fraglen %d, jpegLen %d\n", m_Timestamp, m_SequenceNumber, fragmentOffset, fragmentLen, jpegLen);

  // append the JPEG scan data to the RTP buffer
  memcpy(m_rtpBuf + headerLen, jpeg + fragmentOffset, fragmentLen);
  fragmentOffset += fragmentLen;

  return m_isLastFragment ? 0 : fragmentOffset;
}

RTSPSession::RTSPSession(WiFiClient* client, StreamInfo* streamInfo) {
  m_tcpClient = client;
  m_streamInfo = streamInfo;
  m_status = SessionStatus::STATUS_UNINIT;
  sprintf(m_clientIP, "%s", m_tcpClient->remoteIP().toString());
  m_clientIPAddr = m_tcpClient->remoteIP();
  if (strlen(m_streamInfo->m_authStr) == 0) {
    m_authed = true;
  } else {
    m_authed = false;
  }
}

RTSPSession::~RTSPSession() {
  m_rtpSocket.stop();
  m_tcpClient->stop();
}

void RTSPSession::Handle_RtspOPTION(WiFiClient* client) {
  int l = snprintf(buf, sizeof(buf),
                   "RTSP/1.0 200 OK\r\nCSeq: %u\r\n"
                   "Public: DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE\r\n\r\n",
                   m_CSeq);
  client->write(buf, l);
}

void RTSPSession::Handle_RtspDESCRIBE(WiFiClient* client) {
  char SDPBuf[256] = { 0 };
  int l;

  if (!m_authed) {
    l = snprintf(buf, sizeof(buf),
                 "RTSP/1.0 401 Unauthorized\r\nCSeq: %u\r\n"
                 "WWW-Authenticate: Basic realm=\"EasyRTSPServer\"\r\n"
                 "%s\r\n\r\n",
                 m_CSeq,
                 DateHeader());
  } else {
    snprintf(SDPBuf, sizeof(SDPBuf),
             "v=0\r\n"
             "o=- %d 1 IN IP4 %s\r\n"
             "s=\r\n"
             "t=0 0\r\n"                 // start / stop - 0 -> unbounded and permanent session
             "m=video 0 RTP/AVP 26\r\n"  // currently we just handle UDP sessions
             "a=x-dimensions: 640,480\r\n"
             "a=x-control: trackID=1\r\n"
             "c=IN IP4 0.0.0.0\r\n",
             rand(),
             m_streamInfo->m_serverIP);

    l = snprintf(buf, sizeof(buf),
                     "RTSP/1.0 200 OK\r\nCSeq: %u\r\n"
                     "Content-Base: %s\r\n"
                     "Content-Type: application/sdp\r\n"
                     "Content-Length: %d\r\n\r\n"
                     "%s",
                     m_CSeq,
                     m_streamInfo->m_rtspURL,
                     (int)strlen(SDPBuf),
                     SDPBuf);
  }

  client->write(buf, l);
}

void RTSPSession::Handle_RtspSETUP(WiFiClient* client) {
  char Transport[256] = { 0 };

  m_RtspSessionID = abs(rand());  // create a session ID
  m_RtpServerPort = SERVER_RTP_PORT_BASE + m_index * 2 + 0;
  m_RtcpServerPort = SERVER_RTP_PORT_BASE + m_index * 2 + 1;

  // simulate SETUP server response
  if (m_TcpTransport) {
    snprintf(Transport, sizeof(Transport), "RTP/AVP/TCP;unicast;interleaved=0-1");
  } else {
    snprintf(Transport, sizeof(Transport),
             "RTP/AVP;unicast;destination=%s;source=%s;client_port=%i-%i;server_port=%i-%i",
             m_clientIP,
             m_streamInfo->m_serverIP,
             m_RtpClientPort,
             m_RtcpClientPort,
             m_RtpServerPort,
             m_RtcpServerPort);
    m_rtpSocket.begin(m_RtpServerPort);
  }

  int l = snprintf(buf, sizeof(buf),
                   "RTSP/1.0 200 OK\r\nCSeq: %u\r\n"
                   "Session: %i;timeout=60\r\n"
                   "Transport: %s\r\n"
                   "%s\r\n\r\n",
                   m_CSeq,
                   m_RtspSessionID,
                   Transport,
                   DateHeader());

  client->write(buf, l);
}

void RTSPSession::Handle_RtspPLAY(WiFiClient* client) {
  int l = snprintf(buf, sizeof(buf),
                   "RTSP/1.0 200 OK\r\nCSeq: %u\r\n"
                   "%s\r\n"
                   "Range: npt=0.000-\r\n"
                   "Session: %i;timeout=60\r\n"
                   "RTP-Info: url=%s/trackID=1;seq=0;rtptime=0\r\n\r\n",  // FIXME
                   m_CSeq,
                   m_streamInfo->m_rtspURL,
                   m_RtspSessionID,
                   DateHeader());

  client->write(buf, l);
}

void RTSPSession::Handle_RtspTEARDOWN(WiFiClient* client) {
  int l = snprintf(buf, sizeof(buf),
                   "RTSP/1.0 200 OK\r\nCSeq: %u\r\n\r\n",
                   m_CSeq);

  client->write(buf, l);
}

bool RTSPSession::checkURL(char* aRequest) {
  if (strstr(aRequest, m_streamInfo->m_rtspURL)) {
    return true;
  } else {
    return false;
  }
}

bool RTSPSession::parseCSeq(char* aRequest, unsigned& seq) {
  char* ptr = strstr(aRequest, "CSeq:");
  if (!ptr) {
    return false;
  }
  ptr += 5;
  while (!isDigit(*ptr)) {
    ptr++;
    if (*ptr == '\r') {
      return false;
    }
  }

  seq = atoi(ptr);
  return true;
}

bool RTSPSession::ParseOptionRequest(char* aRequest) {
  /*
  OPTIONS rtsp://192.168.1.102:8554/mjpeg/1 RTSP/1.0\r\n
  CSeq: 0\r\n
  User-Agent: LibVLC/2.2.8 (LIVE555 Streaming Media v2016.02.22)\r\n
  \r\n
  */
  return true;
}

bool RTSPSession::ParseDescribeRequest(char* aRequest) {
  /*
  DESCRIBE rtsp://192.168.1.102:8554/mjpeg/1 RTSP/1.0\r\n
  CSeq: 1\r\n
  User-Agent: LibVLC/2.2.8 (LIVE555 Streaming Media v2016.02.22)\r\n
  Accept: application/sdp\r\n
  \r\n
  */
  if (!strstr(aRequest, "application/sdp")) {
    return false;
  }

  if (strstr(aRequest, m_streamInfo->m_authStr)) {
    m_authed = true;
  } else {
    m_authed = false;
  }
  return true;
}

bool RTSPSession::ParseSetupRequest(char* aRequest) {
  /*
  SETUP rtsp://192.168.1.102:8554/mjpeg/1 RTSP/1.0\r\n
  CSeq: 3\r\n
  User-Agent: LibVLC/2.2.8 (LIVE555 Streaming Media v2016.02.22)\r\n
  Transport: RTP/AVP;unicast;client_port=57844-57845\r\n
  \r\n

  Or

  SETUP rtsp://192.168.1.102:8554/mjpeg/1 RTSP/1.0\r\n
  CSeq: 3\r\n
  User-Agent: LibVLC/2.2.8 (LIVE555 Streaming Media v2016.02.22)\r\n
  Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n
  \r\n
  */

  char* ptr = strstr(aRequest, "Transport:");
  if (!ptr) {
    return false;
  }
  ptr += 10;

  if (strstr(ptr, "RTP/AVP/TCP")) {
    m_TcpTransport = true;
  } else {
    m_TcpTransport = false;
    ptr = strstr(ptr, "client_port=");
    if (!ptr) {
      return false;
    }
    ptr += 12;
    while (!isDigit(*ptr)) {
      ptr++;
      if (*ptr == '\r') {
        return false;
      }
    }
    m_RtpClientPort = atoi(ptr);
    m_RtcpClientPort = m_RtpClientPort + 1;
  }

  return true;
}

bool RTSPSession::ParsePlayRequest(char* aRequest) {
  /*
  PLAY rtsp://192.168.1.102:8554/mjpeg/1 RTSP/1.0\r\n
  CSeq: 4\r\n
  Session: 66334873\r\n
  Range: npt=0.000-\r\n
  \r\n
  */
  return true;
}

bool RTSPSession::ParseTeardownRequest(char* aRequest) {
  /*
  TEARDOWN rtsp://192.168.1.102:8554/mjpeg/1 RTSP/1.0\r\n
  CSeq: 5\r\n
  Session: 66334873\r\n
  \r\n
  */
  return true;
}

RecvResult RTSPSession::recv_RTSPRequest() {
  int len = m_tcpClient->available();
  if (len) {
    if (m_bufPos == 0 || m_bufPos >= sizeof(buf) - 256)  // in case of bad client
    {
      memset(buf, 0x00, sizeof(buf));
      m_bufPos = 0;
      m_recvStatus = hdrStateUnknown;
    }
  }
  while (len) {
    len = m_tcpClient->readBytes(&buf[m_bufPos], len);
    m_bufPos += len;
    len = m_tcpClient->available();
  }

  if (m_bufPos > 0) {
    Serial.printf("Read %d bytes: %s\n", m_bufPos, buf);
    if (m_recvStatus == hdrStateUnknown && m_bufPos >= 6)  // we need at least 4-letter at the line start with optional heading CRLF
    {
      if (NULL != strstr(buf, "\r\n"))  // got a full line
      {
        char* s = buf;
        if (*s == '\r' && *(s + 1) == '\n')  // skip allowed empty line at front
          s += 2;

        // find out the command type
        m_RtspCmdType = RTSP_UNKNOWN;

        if (strncmp(s, "OPTIONS ", 8) == 0) m_RtspCmdType = RTSP_OPTIONS;
        else if (strncmp(s, "DESCRIBE ", 9) == 0) m_RtspCmdType = RTSP_DESCRIBE;
        else if (strncmp(s, "SETUP ", 6) == 0) m_RtspCmdType = RTSP_SETUP;
        else if (strncmp(s, "PLAY ", 5) == 0) m_RtspCmdType = RTSP_PLAY;
        else if (strncmp(s, "TEARDOWN ", 9) == 0) m_RtspCmdType = RTSP_TEARDOWN;

        if (m_RtspCmdType != RTSP_UNKNOWN)  // got some
          m_recvStatus = hdrStateGotMethod;
        else
          m_recvStatus = hdrStateInvalid;
      }
    }

    if (m_recvStatus == hdrStateUnknown) {  // if state == hdrStateUnknown when bufPos<6 or find on \r\n, need continue read
      return RecvResult::RECV_CONTINUE;
    }

    if (m_recvStatus == hdrStateInvalid)  // read a line but find no RTSP method, bad request
    {
      m_bufPos = 0;
      return RecvResult::RECV_BAD_REQUEST;
    }

    // per https://tools.ietf.org/html/rfc2326 we need to look for an empty line
    // to be sure that we got the correctly formed header. Also starting CRLF should be ignored.
    char* s = strstr(m_bufPos > 4 ? buf + m_bufPos - 4 : buf, "\r\n\r\n");  // try to save cycles by searching in the new data only
    if (s == NULL) {                                                        // no end of header seen yet, need continue read
      return RecvResult::RECV_CONTINUE;
    } else {
      return RecvResult::RECV_FULL_REQUEST;
    }
  } else {
    return RecvResult::RECV_CONTINUE;
  }
}

void RTSPSession::Handle_RtspNotFound(WiFiClient* client) {
  int l = snprintf(buf, sizeof(buf),
                   "RTSP/1.0 404 Stream Not Found\r\nCSeq: %u\r\n%s\r\n\r\n",
                   m_CSeq,
                   DateHeader());
  client->write(buf, l);
}

void RTSPSession::Handle_RtspBadRequest(WiFiClient* client) {
  int l = snprintf(buf, sizeof(buf), "RTSP/1.0 400 Bad Request\r\nCSeq: %u\r\n\r\n", m_CSeq);
  client->write(buf, l);
}

RTSP_CMD_TYPES RTSPSession::Handle_RtspRequest(char* aRequest, WiFiClient* client) {
  /* check URL */
  if (!checkURL(aRequest)) {
    Handle_RtspNotFound(client);
    return RTSP_UNKNOWN;
  }
  if (!parseCSeq(aRequest, m_CSeq)) {
    Handle_RtspBadRequest(client);
    return RTSP_UNKNOWN;
  }

  switch (m_RtspCmdType) {
    case RTSP_OPTIONS:
      Handle_RtspOPTION(client);
      break;
    case RTSP_DESCRIBE:
      if (ParseDescribeRequest(aRequest)) {
        Handle_RtspDESCRIBE(client);
      } else {
        Handle_RtspBadRequest(client);
        return RTSP_UNKNOWN;
      }
      break;
    case RTSP_SETUP:
      if (ParseSetupRequest(aRequest)) {
        Handle_RtspSETUP(client);
      } else {
        Handle_RtspBadRequest(client);
        return RTSP_UNKNOWN;
      }
      break;
    case RTSP_PLAY:
      Handle_RtspPLAY(client);
      break;
    case RTSP_TEARDOWN:
      Handle_RtspTEARDOWN(client);
      break;
    default:
      Handle_RtspBadRequest(client);
      return RTSP_UNKNOWN;
      break;
  }

  return m_RtspCmdType;
}

int RTSPSession::SendRtpPacket(RTPPacket* rtpPcaket) {
  char* rtpBuf = rtpPcaket->getRtpBufHead();
  int rtpButLen = rtpPcaket->getRtpPacketSize();
  int sendlen = 0;
  if (m_TcpTransport) {
    sendlen = m_tcpClient->write(rtpBuf, rtpButLen + 4);
  } else {
    m_rtpSocket.beginPacket(m_clientIPAddr, m_RtpClientPort);
    sendlen = m_rtpSocket.write((const unsigned char*)&rtpBuf[4], rtpButLen);
    m_rtpSocket.endPacket();
  }
  return sendlen;
}

void RTSPSession::streamRTP(RTPPacket* rtpPcaket, uint32_t curMsec) {

  //Serial.printf("curMsec = %d, m_prevMsec = %d\n", curMsec, m_prevMsec);

  rtpPcaket->setRtpHeader(m_SequenceNumber, m_Timestamp);
  
  SendRtpPacket(rtpPcaket);

  m_SequenceNumber++;
  // Increment ONLY after a full frame
  //Serial.printf("deltams = %d, m_Timestamp = %d\n", deltams, m_Timestamp);
  if (rtpPcaket->isLastFragment()) {
    // compute deltat (being careful to handle clock rollover with a little lie)
    uint32_t deltams = (curMsec >= m_prevMsec) ? curMsec - m_prevMsec : 100;
    m_prevMsec = curMsec;
    m_Timestamp += (90000 * deltams / 1000);  // fixed timestamp increment for a frame rate of 25fps
  }
  
  m_SendIdx++;
  if (m_SendIdx > 1)
    m_SendIdx = 0;
}

void RTSPSession::run() {
  if (m_tcpClient->connected()) {
    RecvResult result = recv_RTSPRequest();

    if (result == RecvResult::RECV_FULL_REQUEST) {
      // got full header, parse
      RTSP_CMD_TYPES C = Handle_RtspRequest(buf, m_tcpClient);

      if (C == RTSP_PLAY)
        m_status = SessionStatus::STATUS_STREAMING;

      else if (C == RTSP_TEARDOWN)
        m_status = SessionStatus::STATUS_CLOSED;

      //cleaning up
      m_recvStatus = hdrStateUnknown;
      m_bufPos = 0;
    } else if (result == RecvResult::RECV_BAD_REQUEST) {
      Handle_RtspBadRequest(m_tcpClient);
      m_status = SessionStatus::STATUS_ERROR;
    }
  } else {
    m_status = SessionStatus::STATUS_CLOSED;
  }
}

EasyRTSPServer::EasyRTSPServer(uint16_t port) {
  m_ServerPort = port;
  memset(&m_streamInfo, 0, sizeof(m_streamInfo));
  m_msecPerFrame = 100;
}

EasyRTSPServer::~EasyRTSPServer() {
}

bool EasyRTSPServer::setStreamSuffix(char* suffix) {
  memset(m_streamInfo.m_suffix, 0, LEN_MAX_SUFFIX);
  if (strlen(suffix) < LEN_MAX_SUFFIX) {
    strcpy(m_streamInfo.m_suffix, suffix);
    return true;
  } else {
    return false;
  }
}

void EasyRTSPServer::setFrameRate(RTSP_FRAMERATE frameRate) {
  switch (frameRate) {
    case FRAMERATE_5HZ: m_msecPerFrame = 200; break;
    case FRAMERATE_10HZ: m_msecPerFrame = 100; break;
    case FRAMERATE_20HZ: m_msecPerFrame = 50; break;
    default: m_msecPerFrame = 100; break;
  }
}

bool EasyRTSPServer::setAuthAccount(char* username, char* pwd) {
  char ori_str[32] = { 0 };
  if (strlen(username) + strlen(pwd) < 32) {
    int l = sprintf(ori_str, "%s:%s", username, pwd);
    String str = base64::encode(reinterpret_cast<const uint8_t*>(ori_str), l);
    memcpy(m_streamInfo.m_authStr, str.c_str(), str.length());
    Serial.printf("Auth string: %s\n", m_streamInfo.m_authStr);
    return true;
  } else {
    return false;
  }
}

void EasyRTSPServer::init(OV2640* cam) {
  m_cam = cam;
  IPAddress ip = WiFi.localIP();
  sprintf(m_streamInfo.m_serverIP, "%s", ip.toString());
  sprintf(m_streamInfo.m_rtspURL, "rtsp://%s:%u/%s", m_streamInfo.m_serverIP, m_ServerPort, m_streamInfo.m_suffix);
  m_streamInfo.m_width = m_cam->getWidth();
  m_streamInfo.m_height = m_cam->getHeight();
  m_tcpServer.begin(m_ServerPort);
  Serial.printf("RTSP URL: %s\n", m_streamInfo.m_rtspURL);
  Serial.printf("Resolution: %dx%d\n", m_streamInfo.m_width, m_streamInfo.m_height);
}

int EasyRTSPServer::getStreamingSessionCounts() {
  int count = 0;
  for (int i = 0; i < MAX_CLIENTS_NUM; i++) {
    if (m_session[i] && m_session[i]->Status() == SessionStatus::STATUS_STREAMING) {
      count++;
    }
  }
  return count;
}

void EasyRTSPServer::run() {
  int i = 0;
  if (m_tcpServer.hasClient()) {
    for (i = 0; i < MAX_CLIENTS_NUM; i++) {
      if (!rtspClient[i].connected()) {
        rtspClient[i] = m_tcpServer.accept();
        Serial.printf("Accept Client %s:%d\n", rtspClient[i].remoteIP().toString(), rtspClient[i].remotePort());
        RTSPSession* session = new RTSPSession(&rtspClient[i], &m_streamInfo);
        m_session[i] = session;
        m_session[i]->setIndex(i);
        break;
      }
    }
    if (i == MAX_CLIENTS_NUM) {
      m_tcpServer.accept().stop();
    }
  }

  for (i = 0; i < MAX_CLIENTS_NUM; i++) {
    if (m_session[i]) {
      m_session[i]->run();
    }
    if (m_session[i] && m_session[i]->Status() >= SessionStatus::STATUS_CLOSED) {
      delete m_session[i];
      m_session[i] = NULL;
    }
  }

  static uint32_t lastimage = millis();
  uint32_t now = millis();
  int streamingCounts = getStreamingSessionCounts();
  if (streamingCounts > 0) {
    if (now > lastimage + m_msecPerFrame || now < lastimage) {  // handle clock rollover
      m_cam->run();                                             // queue up a read for next time
      BufPtr bytes = m_cam->getfb();
      uint32_t frameSize = m_cam->getSize();
      lastimage = now;

      // locate quant tables if possible
      BufPtr qtable0 = NULL;
      BufPtr qtable1 = NULL;

      // if (!decodeJPEGfile(&bytes, &frameSize, &qtable0, &qtable1)) {
      //   Serial.printf("can't decode jpeg data\n");
      //   return;
      // }
      int offset = 0;
      do {
        offset = m_rtpPacket.packRtpPack(bytes, frameSize, offset, qtable0, qtable1, &m_streamInfo);
        for (i = 0; i < MAX_CLIENTS_NUM; i++) {
          if (m_session[i] && m_session[i]->Status() == SessionStatus::STATUS_STREAMING) {
            m_session[i]->streamRTP(&m_rtpPacket, now);
          }
        }
      } while (offset != 0);

      m_cam->done();

      now = millis();  // check if we are overrunning our max frame rate
      if (now > lastimage + m_msecPerFrame) {
        Serial.printf("warning exceeding max frame rate of %d ms\n", now - lastimage);
      }
    }
  }
}