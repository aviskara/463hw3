#include "pch.h"
#include "SenderSocket.h"

SenderSocket::~SenderSocket()
{
    DWORD workerStatus;
    GetExitCodeThread(workerThread, &workerStatus);

    if (workerStatus == STILL_ACTIVE)
    {
        SetEvent(eventQuit);
        WaitForSingleObject(workerThread, INFINITE);
        CloseHandle(workerThread);
    }

    DWORD statStatus;
    GetExitCodeThread(statThread, &statStatus);
    if (statStatus == STILL_ACTIVE)
    {
        SetEvent(complete);
        WaitForSingleObject(statThread, INFINITE);
        CloseHandle(statThread);
    }

    if (pending_pkts != NULL)
    {
        delete pending_pkts;
        pending_pkts = NULL;
    }
}

int SenderSocket::Open(char* _host, int _portNo, int _senderWindow, LinkProperties *_lp)
{
    startTimer = clock();

    // create handles and semaphores necessary for processing
    eventQuit = CreateEvent(NULL, true, false, NULL);
    socketReceiveReady = CreateEvent(NULL, false, false, NULL);
    complete = CreateEvent(NULL, true, false, NULL);
    empty = CreateSemaphore(NULL, 0, senderWindow, NULL);
    full = CreateSemaphore(NULL, 0, senderWindow, NULL);

    pending_pkts = new Packet[_senderWindow];

    // start wsa to bind to localhost
    WSADATA wsaData;

    WORD wVersionRequested = MAKEWORD(2, 2);
    if (WSAStartup(wVersionRequested, &wsaData) != 0) {
        printf("WSAStartup error %d\n", WSAGetLastError());
        WSACleanup();
        return -1;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        printf("socket() generated error %d\n", WSAGetLastError());
        WSACleanup();
        return -1;
    }

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(0);
    if (bind(sock, (struct sockaddr*)&local, sizeof(local)) == SOCKET_ERROR)
    {
        printf("bind() generated error %d\n", WSAGetLastError());
        WSACleanup();
        return -1;
    }

    
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    DWORD IP = inet_addr(_host);
    if (IP == INADDR_NONE)
    {
        struct hostent* r;
        if ((r = gethostbyname(_host)) == NULL)
        {
            printf("[%.3f] --> target %s is invalid\n", ((double)clock() - startTimer) / CLOCKS_PER_SEC, host);
            return INVALID_NAME;
        }
        else
        {
            memcpy((char*)&(remote.sin_addr), r->h_addr, r->h_length);
        }
    }
    else
    {
        remote.sin_addr.S_un.S_addr = IP;
    }
    remote.sin_port = htons(_portNo);

    // attempt connections
    int attempt = 0;
    while (attempt++ < SYN_MAX_ATTEMPTS)
    {
        //printf("[%.3f] --> SYN %d (attempt %d of 3, RTO 1.000) to %s\n", ((double)clock() - startTimer) / CLOCKS_PER_SEC, 0, attempt, _host);
        
        int status = 0;
        // send SYN packet for handshake
        if ((status = SendSYN(_lp)) != STATUS_OK)
        {
            printf("[%.3f] --> failed sendto with %d\n", ((double)clock() - startTimer) / CLOCKS_PER_SEC, WSAGetLastError());
            return FAILED_SEND;
        }
        // recieve packet and parse
        if ((status = RecvSYN()) != STATUS_OK)
        {
            if ((status == TIMEOUT) && (attempt < 3)) {
                continue;
            }
            printf("[%.3f] --> failed recvfrom with %d\n", ((double)clock() - startTimer) / CLOCKS_PER_SEC, WSAGetLastError());
            return FAILED_SEND;
        }

        

        statThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&Status, this, 0, NULL);
        workerThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&WorkerRun, this, 0, NULL);

        break;
    }

    return 0;
}

int SenderSocket::Send(char* data, int size)
{
    HANDLE arr[] = { eventQuit, empty };
    WaitForMultipleObjects(2, arr, false, INFINITE);

    slot = nextSeq % senderWindow;
    Packet* p = pending_pkts + slot;
    p->sdh.seq = nextSeq;
    p->type = DATA_TYPE;
    p->size = sizeof(SenderDataHeader) + size;
    memcpy(p->pkt, data, size);
    nextSeq++;
    ReleaseSemaphore(full, 1, NULL);
    return 0;
}

int SenderSocket::Close(LinkProperties* _lp)
{
    SetEvent(eventQuit);
    WaitForSingleObject(workerThread, INFINITE);
    CloseHandle(workerThread);

    SetEvent(complete);
    WaitForSingleObject(statThread, INFINITE);
    CloseHandle(statThread);

    int attempt = 0;
    while (attempt++ < DEFAULT_MAX_ATTEMPTS)
    {
        //printf("[%.3f] --> FIN %d (attempt %d of 5, RTO %.3f)\n", ((double)clock() - startTimer) / CLOCKS_PER_SEC, 0, attempt, packetRTT);

        int status = 0;
        // send SYN packet for handshake
        if ((status = SendFIN(_lp)) != STATUS_OK)
        {
            printf("[%.3f] --> failed sendto with %d\n", ((double)clock() - startTimer) / CLOCKS_PER_SEC, WSAGetLastError());
            return FAILED_SEND;
        }
        // recieve packet and parse
        if ((status = RecvFIN()) != STATUS_OK)
        {
            if ((status == TIMEOUT) && (attempt < 3)) {
                continue;
            }
            printf("[%.3f] --> failed recvfrom with %d\n", ((double)clock() - startTimer) / CLOCKS_PER_SEC, WSAGetLastError());
            return FAILED_SEND;
        }
        break;
    }
    return 0;
}

int SenderSocket::SendSYN(LinkProperties* _lp)
{
    // create header to send
    struct SenderSynHeader* hdr = new SenderSynHeader();
    hdr->lp.RTT = _lp->RTT;
    hdr->lp.speed = _lp->speed;
    hdr->lp.pLoss[FORWARD_PATH] = _lp->pLoss[FORWARD_PATH];
    hdr->lp.pLoss[RETURN_PATH] = _lp->pLoss[RETURN_PATH];
    hdr->lp.bufferSize = 100;
    hdr->sdh.flags.SYN = 1;
    hdr->sdh.seq = 0;

    //printf("RTT = %f Speed = %f Forward Loss = %f Backwards Loss = %f Buffer = %u\n", 
        //hdr->lp.RTT, hdr->lp.speed, hdr->lp.pLoss[FORWARD_PATH], hdr->lp.pLoss[RETURN_PATH], hdr->lp.bufferSize);

    rttTimer = clock();
    
    // send the data
    if (sendto(sock, (char*)hdr, sizeof(SenderSynHeader), 0, (struct sockaddr*)&remote, sizeof(remote)) == SOCKET_ERROR)
    {
        return FAILED_SEND;
    }
    return 0;
}

int SenderSocket::RecvSYN()
{
    timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    clock_t t = clock();

    // create buffer for incoming data
    char* responseBuf = new char[sizeof(SenderSynHeader)];
    memset(responseBuf, 0, sizeof(SenderSynHeader));

    struct sockaddr_in response;
    int responseAddrSize = sizeof(response);
    

    fd_set fd;
    FD_ZERO(&fd);
    FD_SET(sock, &fd);
    int available = select(0, &fd, NULL, NULL, &timeout);

    //std::cout << "start\n";
    if (available > 0)
    {
        if ((responseSize = recvfrom(sock, responseBuf, sizeof(SenderSynHeader), 0, (struct sockaddr*)&response, &responseAddrSize)) < 0)
        {
            //std::cout << "less than 0\n";
            return FAILED_RECV;
        }
        //std::cout << "available\n";

        struct ReceiverHeader* resp = (struct ReceiverHeader*)responseBuf;

        if ((resp->flags.SYN == 1) && (resp->flags.ACK == 1))
        {
            rto = 3* ((double)clock() - rttTimer) / CLOCKS_PER_SEC;
            est = rto / 3;
            //printf("[%.3f] <-- SYN-ACK %d window %d; setting initial RTO to %.3f\n", ((double)clock() - startTimer) / CLOCKS_PER_SEC, resp->ackSeq, resp->recvWnd, rto);
            transferDuration = (double)clock() / CLOCKS_PER_SEC;
            delete [] responseBuf;

            lastReleased = min(senderWindow, resp->recvWnd);
            ReleaseSemaphore(empty, lastReleased, NULL);
            mutex = CreateMutex(NULL, 0, NULL);

            return STATUS_OK;
        }
    }
    else if (available == 0)
    {
        //std::cout << "timeout\n";
        delete responseBuf;
        return TIMEOUT;
    }
    else
    {
        //std::cout << "else\n";
        delete [] responseBuf;
        return -1;
    }
    //std::cout << "outside\n";
    delete [] responseBuf;
    return -1;
}

int SenderSocket::SendFIN(LinkProperties* _lp)
{
    // create header to send
    struct SenderSynHeader* hdr = new SenderSynHeader();
    hdr->lp.RTT = packetRTT;
    hdr->lp.speed = _lp->speed;
    hdr->lp.pLoss[FORWARD_PATH] = _lp->pLoss[FORWARD_PATH];
    hdr->lp.pLoss[RETURN_PATH] = _lp->pLoss[RETURN_PATH];
    hdr->lp.bufferSize = 100;
    hdr->sdh.flags.FIN = 1;
    hdr->sdh.seq = base;

    //printf("RTT = %f Speed = %f Forward Loss = %f Backwards Loss = %f Buffer = %u\n", 
        //hdr->lp.RTT, hdr->lp.speed, hdr->lp.pLoss[FORWARD_PATH], hdr->lp.pLoss[RETURN_PATH], hdr->lp.bufferSize);

    transferDuration = ((double)clock() / CLOCKS_PER_SEC) - transferDuration;
    // send the data
    if (sendto(sock, (char*)hdr, sizeof(SenderSynHeader), 0, (struct sockaddr*)&remote, sizeof(remote)) == SOCKET_ERROR)
    {
        return FAILED_SEND;
    }
    return 0;
}

int SenderSocket::RecvFIN()
{
    timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    clock_t t = clock();

    // create buffer for incoming data
    char* responseBuf = new char[sizeof(SenderSynHeader)];
    memset(responseBuf, 0, sizeof(SenderSynHeader));

    struct sockaddr_in response;
    int responseAddrSize = sizeof(response);


    fd_set fd;
    FD_ZERO(&fd);
    FD_SET(sock, &fd);
    int available = select(0, &fd, NULL, NULL, &timeout);

    //std::cout << "start\n";
    if (available > 0)
    {
        if ((responseSize = recvfrom(sock, responseBuf, sizeof(SenderSynHeader), 0, (struct sockaddr*)&response, &responseAddrSize)) < 0)
        {
            //std::cout << "less than 0\n";
            return FAILED_RECV;
        }
        //std::cout << "available\n";

        struct ReceiverHeader* resp = (struct ReceiverHeader*)responseBuf;

        if ((resp->flags.ACK == 1) && (resp->flags.FIN == 1))
        {
            packetRTT = 3 * ((double)clock() - rttTimer) / CLOCKS_PER_SEC;
            //std::cout << resp->recvWnd << std::endl;
            printf("[%.3f] <-- FIN-ACK %d window %X\n", ((double)clock() - startTimer) / CLOCKS_PER_SEC, resp->ackSeq, resp->recvWnd);
            rto = packetRTT;
            return STATUS_OK;
        }
    }
    else if (available == 0)
    {
        //std::cout << "timeout\n";
        return TIMEOUT;
    }
    else
    {
        //std::cout << "else\n";
        return -1;
    }
    return -1;
}

int SenderSocket::RecvACK()
{
    timeval timeout;
    timeout.tv_sec = floor(rto);
    timeout.tv_usec = (rto-floor(rto)) * 1e6;

    

    char* responseBuf = new char[sizeof(SenderSynHeader)];
    memset(responseBuf, 0, sizeof(SenderSynHeader));

    struct sockaddr_in response;
    int responseAddrSize = sizeof(response);

    if ((responseSize = recvfrom(sock, responseBuf, sizeof(SenderSynHeader), 0, (struct sockaddr*)&response, &responseAddrSize)) < 0)
    {
        delete[] responseBuf;
        return FAILED_RECV;
    }

    struct ReceiverHeader* resp = (struct ReceiverHeader*)responseBuf;

    int y = resp->ackSeq;
    printf("<-- RECV BASE: %d, NextTo Send %d, ACK %d\n", base, nextToSend, y);
    if (base != resp->ackSeq)
    {
        //printf("base = %d \tgot ackSeq %d \t next to send %d\n", base, y, nextToSend);
    }
    
    if (y > base)
    {
        if (dupCount == 0)
        {
            double tempRTT = (clock() - (double)pending_pkts[(y - 1) % senderWindow].txTime) / CLOCKS_PER_SEC;
            rto = CalculateRTO(tempRTT);
        }
        WaitForSingleObject(mutex, INFINITE);
        int releaseAmount = y - base;
        base = y;
        dupCount = 0;
        ReleaseMutex(mutex);

        if (base != nextToSend)
        {
            pending_pkts[base % senderWindow].txTime = clock();
        }
        WaitForSingleObject(mutex, INFINITE);
        //printf("release amount %d\n", releaseAmount);
        //ReleaseSemaphore(empty, releaseAmount, NULL);
        
        int effectiveWindow = min(senderWindow, resp->recvWnd);
        int newReleased = base + effectiveWindow - lastReleased;
        
        ReleaseSemaphore(empty, newReleased, NULL);
        lastReleased += newReleased;

        ReleaseMutex(mutex);
        
    }
    else if (y == base) {
        dupCount++;
        if (dupCount == 3) {
            pending_pkts[base % senderWindow].txTime = clock();
            printf("--> RTX BASE: %d, NextTo Send %d\n", base, nextToSend);
            sendto(sock, (char*)&(pending_pkts[base % senderWindow].sdh), pending_pkts[base % senderWindow].size,
                0, (sockaddr*)&(remote), sizeof(remote));
            WaitForSingleObject(mutex, INFINITE);
            rtxCount++;
            
            ReleaseMutex(mutex);
        }
    }
}

float SenderSocket::CalculateRTO(float _newTime)
{
    rto = CalculateEstRTT(_newTime) + (4 * max(CalculateDevRTT(_newTime), 0.010));
    return rto;
}

float SenderSocket::CalculateEstRTT(float _newTime)
{
    float a = 0.125;
    est = (((1 - a) * est) + (a * _newTime));
    return est;
}

float SenderSocket::CalculateDevRTT(float _newTime)
{
    float b = 0.25;
    dev = ((1 - b) * dev) + (b * abs(_newTime - est));
    return dev;
}

void SenderSocket::Status(LPVOID _param)
{
    SenderSocket* s = (SenderSocket*)_param;

    clock_t start = clock();
    clock_t cur = 0;
    int prevSize = 0;

    while (WaitForSingleObject(s->complete, 2000) == WAIT_TIMEOUT)
    {
        cur = clock();
        s->speed = ((double)(s->base - prevSize) * 8 * (MAX_PKT_SIZE - sizeof(SenderDataHeader)) / ((double)1e6*2));

        printf("[%3.0f] B %6d (%5.1f MB) N %6d T %d F %d W %d S %.3f Mbps RTT %.3f\n",
            (double)(cur - start) / CLOCKS_PER_SEC, s->base,
            s->base * MAX_PKT_SIZE / 1e6,
            s->nextSeq, s->timoutCount, s->rtxCount, s->senderWindow,
            s->speed , s->est);

        prevSize = s->base;
    }
}

void SenderSocket::WorkerRun(LPVOID _param)
{
    SenderSocket* s = (SenderSocket*)_param;

    int kernelBuffer = 20e6; // 20 meg
    if (setsockopt(s->sock, SOL_SOCKET, SO_RCVBUF, (char*)&kernelBuffer, sizeof(int)) == SOCKET_ERROR)
    {
        return;
    }
    kernelBuffer = 20e6; // 20 meg
    if (setsockopt(s->sock, SOL_SOCKET, SO_SNDBUF, (char*)&kernelBuffer, sizeof(int)) == SOCKET_ERROR)
    {
        return;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    WSAEventSelect(s->sock, s->socketReceiveReady, FD_READ);

    s->nextToSend = 0;
    s->dupCount = 0;
    bool lastPkt = false;

    //printf("started\n");
    HANDLE events[] = { s->socketReceiveReady, s->full, s->eventQuit };
    while (!lastPkt || (lastPkt && (s->base < s->nextToSend)))
    {
        if (s->base < s->nextToSend)
        {
            s->timeout = ( s->rto) * 3000;
        }
        else
        {
            s->timeout = INFINITE;
        }

        int ret = WaitForMultipleObjects(3, events, false, s->timeout);
        switch (ret) 
        {
        case WAIT_TIMEOUT:
        {
            
            s->timoutCount++;
            printf("--> TIMEOUT BASE: %d, NextTo Send %d\n", s->base, s->nextToSend);
            sendto(s->sock, (char*)&(s->pending_pkts[s->base % s->senderWindow].sdh), s->pending_pkts[s->base % s->senderWindow].size,
                0, (sockaddr*)&(s->remote), sizeof(s->remote));

            s->pending_pkts[s->base % s->senderWindow].txTime = clock();
            break;
        }
        case 0:
        {
            //printf("got data\n");
            s->RecvACK();
            break;
        }
        case 1:
        {
            printf("--> SEND BASE: %d, NextTo Send %d\n", s->base, s->nextToSend);
            sendto(s->sock, (char*)&(s->pending_pkts[s->nextToSend % s->senderWindow].sdh), s->pending_pkts[s->nextToSend % s->senderWindow].size,
                0, (sockaddr*)&(s->remote), sizeof(s->remote));
            //printf("sent %d\n", s->nextToSend);
            s->pending_pkts[s->base % s->senderWindow].txTime = clock();
            WaitForSingleObject(s->mutex, INFINITE);
            s->nextToSend++;
            ReleaseMutex(s->mutex);
            break;
        }
        case 2:
        {
            lastPkt = true;
            break;
        }
        default:
        {
            break;
        }
        }
    }
    
    return;
}
