/// DOOM macOS Port - UDP Networking
///
/// Peer-to-peer lockstep multiplayer. Each machine sends only its
/// ticcmd (8 bytes/tic) to all peers. d_net.c handles synchronization,
/// retransmission, and handshake — this file just moves UDP packets.
///
/// Usage: ./macdoom -net 1 192.168.1.20    (player 1, peer at .20)
///        ./macdoom -net 2 192.168.1.10    (player 2, peer at .10)

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/ioctl.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_event.h"
#include "d_net.h"
#include "m_argv.h"
#include "i_system.h"
#include "i_net.h"

static int DOOMPORT = (IPPORT_USERRESERVED + 0x1d);
static int DOOMPORT_REMOTE = 0;

static int                 sendsocket;
static int                 insocket;
static struct sockaddr_in  sendaddress[MAXNETNODES];

static void (*netsend)(void);
static void (*netget)(void);


static int UDPsocket(void)
{
    int s = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0)
        I_Error("can't create socket: %s", strerror(errno));
    return s;
}


static void BindToLocalPort(int s, int port)
{
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = port;

    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    if (bind(s, (struct sockaddr *)&address, sizeof(address)) == -1)
        I_Error("BindToPort: %s", strerror(errno));
}


static void PacketSend(void)
{
    int c;
    doomdata_t sw;

    sw.checksum = htonl(netbuffer->checksum);
    sw.player = netbuffer->player;
    sw.retransmitfrom = netbuffer->retransmitfrom;
    sw.starttic = netbuffer->starttic;
    sw.numtics = netbuffer->numtics;

    for (c = 0; c < netbuffer->numtics; c++)
    {
        sw.cmds[c].forwardmove  = netbuffer->cmds[c].forwardmove;
        sw.cmds[c].sidemove     = netbuffer->cmds[c].sidemove;
        sw.cmds[c].angleturn    = htons(netbuffer->cmds[c].angleturn);
        sw.cmds[c].consistancy  = htons(netbuffer->cmds[c].consistancy);
        sw.cmds[c].chatchar     = netbuffer->cmds[c].chatchar;
        sw.cmds[c].buttons      = netbuffer->cmds[c].buttons;
    }

    sendto(sendsocket, &sw, doomcom->datalength, 0,
           (struct sockaddr *)&sendaddress[doomcom->remotenode],
           sizeof(sendaddress[doomcom->remotenode]));
}


static void PacketGet(void)
{
    int i, c;
    struct sockaddr_in fromaddress;
    socklen_t fromlen = sizeof(fromaddress);
    doomdata_t sw;

    c = recvfrom(insocket, &sw, sizeof(sw), 0,
                 (struct sockaddr *)&fromaddress, &fromlen);

    if (c == -1)
    {
        if (errno != EWOULDBLOCK)
            I_Error("GetPacket: %s", strerror(errno));
        doomcom->remotenode = -1;
        return;
    }

    for (i = 0; i < doomcom->numnodes; i++)
        if (fromaddress.sin_addr.s_addr == sendaddress[i].sin_addr.s_addr)
            break;

    if (i == doomcom->numnodes)
    {
        doomcom->remotenode = -1;
        return;
    }

    doomcom->remotenode = i;
    doomcom->datalength = c;

    netbuffer->checksum = ntohl(sw.checksum);
    netbuffer->player = sw.player;
    netbuffer->retransmitfrom = sw.retransmitfrom;
    netbuffer->starttic = sw.starttic;
    netbuffer->numtics = sw.numtics;

    for (c = 0; c < netbuffer->numtics; c++)
    {
        netbuffer->cmds[c].forwardmove  = sw.cmds[c].forwardmove;
        netbuffer->cmds[c].sidemove     = sw.cmds[c].sidemove;
        netbuffer->cmds[c].angleturn    = ntohs(sw.cmds[c].angleturn);
        netbuffer->cmds[c].consistancy  = ntohs(sw.cmds[c].consistancy);
        netbuffer->cmds[c].chatchar     = sw.cmds[c].chatchar;
        netbuffer->cmds[c].buttons      = sw.cmds[c].buttons;
    }
}


void I_InitNetwork(void)
{
    int i, p;
    struct hostent *hostentry;

    doomcom = malloc(sizeof(*doomcom));
    memset(doomcom, 0, sizeof(*doomcom));

    i = M_CheckParm("-dup");
    if (i && i < myargc - 1)
    {
        doomcom->ticdup = myargv[i+1][0] - '0';
        if (doomcom->ticdup < 1) doomcom->ticdup = 1;
        if (doomcom->ticdup > 9) doomcom->ticdup = 9;
    }
    else
        doomcom->ticdup = 1;

    doomcom->extratics = M_CheckParm("-extratic") ? 1 : 0;

    p = M_CheckParm("-port");
    if (p && p < myargc - 1)
    {
        DOOMPORT = atoi(myargv[p+1]);
        printf("using local port %i\n", DOOMPORT);
    }

    /// -port2: remote peer port (for same-machine testing).
    /// Without this, remote port equals local port (normal LAN behavior).
    p = M_CheckParm("-port2");
    if (p && p < myargc - 1)
    {
        DOOMPORT_REMOTE = atoi(myargv[p+1]);
        printf("using remote port %i\n", DOOMPORT_REMOTE);
    }

    /// -net <consoleplayer> <host> <host> ...
    /// consoleplayer is 1-based. Hosts are peer IPs.
    i = M_CheckParm("-net");
    if (!i)
    {
        netgame = false;
        doomcom->id = DOOMCOM_ID;
        doomcom->numplayers = doomcom->numnodes = 1;
        doomcom->deathmatch = false;
        doomcom->consoleplayer = 0;
        return;
    }

    netsend = PacketSend;
    netget = PacketGet;
    netgame = true;

    doomcom->consoleplayer = myargv[i+1][0] - '1';
    doomcom->numnodes = 1;

    i++;
    while (++i < myargc && myargv[i][0] != '-')
    {
        sendaddress[doomcom->numnodes].sin_family = AF_INET;
        int remote = DOOMPORT_REMOTE ? DOOMPORT_REMOTE : DOOMPORT;
        sendaddress[doomcom->numnodes].sin_port = htons(remote);

        if (myargv[i][0] == '.')
        {
            sendaddress[doomcom->numnodes].sin_addr.s_addr =
                inet_addr(myargv[i] + 1);
        }
        else
        {
            hostentry = gethostbyname(myargv[i]);
            if (!hostentry)
                I_Error("gethostbyname: couldn't find %s", myargv[i]);
            memcpy(&sendaddress[doomcom->numnodes].sin_addr,
                   hostentry->h_addr_list[0], hostentry->h_length);
        }
        doomcom->numnodes++;
    }

    doomcom->id = DOOMCOM_ID;
    doomcom->numplayers = doomcom->numnodes;

    insocket = UDPsocket();
    BindToLocalPort(insocket, htons(DOOMPORT));

    int trueval = 1;
    ioctl(insocket, FIONBIO, &trueval);

    sendsocket = UDPsocket();
}


void I_NetCmd(void)
{
    if (doomcom->command == CMD_SEND)
        netsend();
    else if (doomcom->command == CMD_GET)
        netget();
    else
        I_Error("Bad net cmd: %i\n", doomcom->command);
}
