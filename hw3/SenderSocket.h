#pragma once

#include "pch.h"

// SenderSocket.h
#define MAGIC_PORT 22345 // receiver listens on this port
#define MAX_PKT_SIZE (1500-28) // maximum UDP packet size accepted by receiver

#define FORWARD_PATH 0
#define RETURN_PATH 1 
#define MAGIC_PROTOCOL 0x8311AA

// possible status codes from ss.Open, ss.Send, ss.Close
#define STATUS_OK 0 // no error
#define ALREADY_CONNECTED 1 // second call to ss.Open() without closing connection
#define NOT_CONNECTED 2 // call to ss.Send()/Close() without ss.Open()
#define INVALID_NAME 3 // ss.Open() with targetHost that has no DNS entry
#define FAILED_SEND 4 // sendto() failed in kernel
#define TIMEOUT 5 // timeout after all retx attempts are exhausted
#define FAILED_RECV 6 // recvfrom() failed in kernel

#define SYN_MAX_ATTEMPTS 3
#define DEFAULT_MAX_ATTEMPTS 5

#pragma pack(push, 1)
class LinkProperties {
public:
	// transfer parameters
	float RTT; // propagation RTT (in sec)
	float speed; // bottleneck bandwidth (in bits/sec)
	float pLoss[2]; // probability of loss in each direction
	DWORD bufferSize; // buffer size of emulated routers (in packets)
	LinkProperties() { memset(this, 0, sizeof(*this)); }
};

class Flags {
public:
	DWORD reserved : 5; // must be zero
	DWORD SYN : 1;
	DWORD ACK : 1;
	DWORD FIN : 1;
	DWORD magic : 24;
	Flags() { memset(this, 0, sizeof(*this)); magic = MAGIC_PROTOCOL; }
};

class SenderDataHeader {
public:
	Flags flags;
	DWORD seq; // must begin from 0
};

class ReceiverHeader {
public:
	Flags flags;
	DWORD recvWnd; // receiver window for flow control (in pkts)
	DWORD ackSeq; // ack value = next expected sequence
};

class SenderSynHeader {
public:
	SenderDataHeader sdh;
	LinkProperties lp;
};

class SenderPacket {
public:
	SenderDataHeader sdh;
	char data[MAX_PKT_SIZE];
};

class Packet {
public:
	int type; // SYN, FIN, data
	int size; // bytes in packet data
	clock_t txTime; // transmission time
	SenderDataHeader sdh;
	char pkt[MAX_PKT_SIZE]; // packet with header
};
#pragma pop


class SenderSocket {
	int SendSYN(LinkProperties* _lp);
	int RecvSYN();
	int SendFIN(LinkProperties* _lp);
	int RecvFIN();
	int RecvACK();

	float CalculateRTO(float _newTime);
	float CalculateEstRTT(float _newTime);
	float CalculateDevRTT(float _newTime);
public:
	char* host;
	int portNo;
	int senderWindow;
	float packetRTT;
	float forwardLoss;
	float backwardLoss;
	float linkSpeed;
	UINT64 bufSize;
	int responseSize;
	float speed;

	SOCKET sock;
	struct sockaddr_in remote;

	clock_t startTimer;
	clock_t rttTimer;
	float transferDuration;

	float est = 0;
	float dev = 0;
	double rto;

	int base = 0;
	int nextSeq = 1;
	int lastReleased;
	int timoutCount;
	int rtxCount;
	Packet* pending_pkts = NULL;

	HANDLE workerThread;
	HANDLE statThread;

	HANDLE eventQuit;
	HANDLE empty;
	HANDLE full;
	HANDLE complete;
	HANDLE socketReceiveReady;

	int Open(char* _host, int _portNo, int _senderWindow, LinkProperties* _lp);
	int Send(char *data, int size);
	int Close(LinkProperties* _lp);

	static void Status(LPVOID _param);
};