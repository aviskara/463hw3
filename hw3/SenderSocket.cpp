#include "pch.h"
#include "SenderSocket.h"

int SenderSocket::Open(char* _host, int _portNo, int _senderWindow, LinkProperties *_lp)
{
    startTimer = clock();

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
        printf("[%.3f] --> SYN %d (attempt %d of 3, RTO 1.000) to %s\n", ((double)clock() - startTimer) / CLOCKS_PER_SEC, 0, attempt, _host);
        
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
    hdr->lp.bufferSize = _lp->bufferSize;
    hdr->sdh.flags.SYN = 1;
    hdr->sdh.seq = 0;

    printf("RTT = %f Speed = %f Forward Loss = %f Backwards Loss = %f Buffer = %u\n", 
        hdr->lp.RTT, hdr->lp.speed, hdr->lp.pLoss[FORWARD_PATH], hdr->lp.pLoss[RETURN_PATH], hdr->lp.bufferSize);

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
    int responseSize = 0;

    fd_set fd;
    FD_ZERO(&fd);
    FD_SET(sock, &fd);
    int available = select(0, &fd, NULL, NULL, &timeout);

    //std::cout << "start\n";
    if (available > 0)
    {
        if ((responseSize = recvfrom(sock, responseBuf, sizeof(SenderSynHeader), 0, (struct sockaddr*)&response, &responseAddrSize)) < 0)
        {
            std::cout << "less than 0\n";
            return FAILED_RECV;
        }
        std::cout << "available\n";

        struct ReceiverHeader* resp = (struct ReceiverHeader*)responseBuf;

        if ((resp->flags.SYN == 1) && (resp->flags.ACK == 1))
        {
            printf("[%.3f] <-- SYN-ACK %D window %d; setting initial RTO to %.3f\n", ((double)clock() - startTimer) / CLOCKS_PER_SEC, resp->recvWnd, ((double)clock() - rttTimer) / CLOCKS_PER_SEC);
            return STATUS_OK;
        }
    }
    else if (available == 0)
    {
        std::cout << "timeout\n";
        return TIMEOUT;
    }
    else
    {
        std::cout << "else\n";
        return -1;
    }
    std::cout << "outside\n";
}
