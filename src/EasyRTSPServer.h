#ifndef _EASYRTSPSERVER_H_
#define _EASYRTSPSERVER_H_

#include <WiFi.h>
#include "OV2640.h"
#include "jpeg.h"

#define LEN_MAX_SUFFIX 16
#define LEN_MAX_IP 16
#define LEN_MAX_URL 64
#define LEN_MAX_AUTH 64
#define MAX_CLIENTS_NUM 3

#define SERVER_RTP_PORT_BASE 57000

#define RTSP_RECV_BUFFER_SIZE 384  // for incoming requests, and outgoing responses
#define RTSP_PARAM_STRING_MAX 200

#define KRtpHeaderSize 12       // size of the RTP header
#define KJpegHeaderSize 8       // size of the special JPEG payload header
#define MAX_FRAGMENT_SIZE 1300  // FIXME, pick more carefully

enum SessionStatus {
  STATUS_UNINIT = 0,
  STATUS_CONNECTING,
  STATUS_STREAMING,
  STATUS_PAUSED,
  STATUS_CLOSED,
  STATUS_ERROR
};

enum RecvStatus {
  hdrStateUnknown,
  hdrStateGotMethod,
  hdrStateInvalid
};

enum RecvResult {
  RECV_BAD_REQUEST,
  RECV_CONTINUE,
  RECV_FULL_REQUEST
};

enum RTSP_CMD_TYPES {
  RTSP_OPTIONS,
  RTSP_DESCRIBE,
  RTSP_SETUP,
  RTSP_PLAY,
  RTSP_TEARDOWN,
  RTSP_UNKNOWN
};

enum RTSP_FRAMERATE{
  FRAMERATE_5HZ,
  FRAMERATE_10HZ,
  FRAMERATE_20HZ,
};

struct StreamInfo {
  char m_suffix[LEN_MAX_SUFFIX] = "mjpeg/1";
  char m_rtspURL[LEN_MAX_URL] = { 0 };
  char m_serverIP[LEN_MAX_IP] = { 0 };
  char m_authStr[LEN_MAX_AUTH] = { 0 };
  int m_width;
  int m_height;
};



class RTSPSession {
public:
  RTSPSession(WiFiClient* client, StreamInfo* streamInfo);
  ~RTSPSession();
  void setIndex(int index) {
    m_index = index;
  }
  SessionStatus Status() {
    return m_status;
  }
  void run();
  void streamFrame(unsigned const char* data, uint32_t dataLen, uint32_t curMsec);

private:
  WiFiClient* m_tcpClient;
  StreamInfo* m_streamInfo;
  int m_index;
  SessionStatus m_status;
  RecvStatus m_recvStatus;
  RTSP_CMD_TYPES m_RtspCmdType;
  unsigned m_CSeq;
  char m_clientIP[LEN_MAX_IP] = { 0 };
  IPAddress m_clientIPAddr;
  WiFiUDP m_rtpSocket;

  uint32_t m_RtspSessionID;  // create a session ID
  bool m_authed;

  bool m_TcpTransport;        /// if Tcp based streaming was activated
  uint16_t m_RtpClientPort;   // RTP receiver port on client (in host byte order!)
  uint16_t m_RtcpClientPort;  // RTCP receiver port on client (in host byte order!)
  uint16_t m_RtpServerPort;   // RTP sender port on server
  uint16_t m_RtcpServerPort;  // RTCP sender port on server

  char buf[RTSP_RECV_BUFFER_SIZE];
  uint32_t m_bufPos = 0;

  uint32_t m_prevMsec = 0;
  uint32_t m_SequenceNumber = 0;
  uint32_t m_Timestamp = 0;
  uint32_t m_SendIdx = 0;

  char RtpBuf[1536];  // Note: we assume single threaded, this large buf we keep off of the tiny stack

  bool checkURL(char* aRequest);
  bool parseCSeq(char* aRequest, unsigned& seq);
  bool ParseOptionRequest(char* aRequest);
  bool ParseDescribeRequest(char* aRequest);
  bool ParseSetupRequest(char* aRequest);
  bool ParsePlayRequest(char* aRequest);
  bool ParseTeardownRequest(char* aRequest);
  RecvResult recv_RTSPRequest();
  RTSP_CMD_TYPES Handle_RtspRequest(char* aRequest, WiFiClient* client);
  void Handle_RtspNotFound(WiFiClient* client);
  void Handle_RtspBadRequest(WiFiClient* client);
  void Handle_RtspTEARDOWN(WiFiClient* client);
  void Handle_RtspPLAY(WiFiClient* client);
  void Handle_RtspSETUP(WiFiClient* client);
  void Handle_RtspDESCRIBE(WiFiClient* client);
  void Handle_RtspOPTION(WiFiClient* client);
  int SendRtpPacket(unsigned const char* jpeg, int jpegLen, int fragmentOffset, BufPtr quant0tbl, BufPtr quant1tbl);
};

class EasyRTSPServer {
public:
  EasyRTSPServer(uint16_t port = 554);
  ~EasyRTSPServer();
  bool setStreamSuffix(char* suffix);
  void setFrameRate(RTSP_FRAMERATE frameRate);
  bool setAuthAccount(char *username, char *pwd);
  void init(OV2640* cam);
  void run();

private:
  uint16_t m_ServerPort;
  WiFiClient rtspClient[MAX_CLIENTS_NUM];
  StreamInfo m_streamInfo;
  OV2640* m_cam;
  WiFiServer m_tcpServer;
  uint32_t m_frameRate;
  uint32_t m_msecPerFrame;
  RTSPSession* m_session[MAX_CLIENTS_NUM] = { NULL };
  void addSession(RTSPSession* session);
  int getStreamingSessionCounts();
};

#endif