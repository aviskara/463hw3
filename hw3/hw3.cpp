// hw3.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include "pch.h"
#include "SenderSocket.h"
#include "checksun.h"

int main(int argc, char **argv)
{
    if (argc != 8)
    {
        printf("Invalid argument count\n");
        exit(-1);
    }

    // parse command-line parameters
    char* targetHost = argv[1];
    int power = atoi(argv[2]);
    int senderWindow = atoi(argv[3]);
    float rtt = atof(argv[4]);
    float forwardLoss = atof(argv[5]);
    float returnLoss = atof(argv[6]);
    float bottleneckSpeed = atof(argv[7]);
    

    printf("Main:\t sender W = %d, RTT %.3f sec, loss %.3e / %.3e, link %.1f Mbps\n", senderWindow, rtt, forwardLoss, returnLoss, bottleneckSpeed);

    // begin creating buffer
    clock_t t = clock();
    printf("Main:\t initializing DWORD array with 2^%d elements... done in ", power);

    UINT64 dwordBufSize = (UINT64) 1 << power;
    DWORD* dwordBuf = new DWORD[dwordBufSize];
    for (UINT64 i = 0; i < dwordBufSize; i++)
    {
        dwordBuf[i] = i;
    }
    printf("%.0f ms\n", (1000) * ((double)clock() - t) / CLOCKS_PER_SEC);

    int status;
    SenderSocket ss;
    LinkProperties lp;
    lp.RTT = rtt;
    lp.speed = bottleneckSpeed * 1e6;
    lp.pLoss[FORWARD_PATH] = forwardLoss;
    lp.pLoss[RETURN_PATH] = returnLoss;
    lp.bufferSize = dwordBufSize + DEFAULT_MAX_ATTEMPTS;
    ss.senderWindow = senderWindow;
    if ((status = ss.Open(targetHost, MAGIC_PORT, senderWindow, &lp)) != STATUS_OK)
    {
        printf("Main:\t connect failed with status %d\n", status);
        return 0;
    }
    clock_t transferStart = clock();
    printf("Main:\t connected to %s in %.3f sec, pkt size %d bytes\n", targetHost, ss.rto / 3, ss.responseSize);

    char* charBuf = (char*)dwordBuf;
    UINT64 byteBufferSize = dwordBufSize << 2;

    UINT64 off = 0;
    while (off < byteBufferSize)
    {
        int bytes = min(byteBufferSize - off, MAX_PKT_SIZE - sizeof(SenderDataHeader));

        if ((status = ss.Send(charBuf + off, bytes)) != STATUS_OK)
        {
            printf("Main:\t send failed with status %d\n", status);
            return 0;
        }
        off += bytes;
    }

    
    if ((status = ss.Close(&lp)) != STATUS_OK) {

    }

    Checksum cs;
    DWORD check = cs.CRC32((unsigned char *)charBuf, byteBufferSize);
    //std::cout << check << std::endl;

    printf("Main:\t transfer finished in %.3f seconds, speed %.2f Kbps, checksum %X\n", ss.transferDuration, (dwordBufSize * 32)/(double)(1000*ss.transferDuration), check);
    printf("Main:\t estRTT %.3f, ideal rate %.2f Kbps\n", ss.est, 8 * (MAX_PKT_SIZE - sizeof(SenderDataHeader)) / (ss.est * 1000));
}
