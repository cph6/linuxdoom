// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: m_bbox.c,v 1.1 1997/02/03 22:45:10 b1 Exp $";

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <time.h>
#include <sys/socket.h>
#include <netipx/ipx.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/ioctl.h>

#include "i_system.h"
#include "d_event.h"
#include "d_net.h"
#include "m_argv.h"

#include "doomstat.h"

#ifdef __GNUG__
#pragma implementation "i_net.h"
#endif
#include "i_net.h"


// setupdata_t is used as doomdata_t during setup
typedef struct
{
     short     gameid;                       // so multiple games can setup at once
     short     drone;
     short     nodesfound;
     short     nodeswanted;
} setupdata_t;


void	NetSend (void);
boolean NetListen (void);


//
// NETWORKING
//

int	DOOMPORT =	0x869b;

int			sendsocket;
int			insocket;

struct	sockaddr	sendaddress[MAXNETNODES+1];
struct  sockaddr	fromaddress;

int	(*netget) (void);
void	(*netsend) (int);

int ipx_socket(void) {
    int s = socket(PF_IPX,SOCK_DGRAM,0);
    struct sockaddr_ipx sa;
    if (s == -1) {
            I_Error("socket(PF_PIPX): %s\n",strerror(errno));
    }
    memset(&sa,0,sizeof(sa));
    sa.sipx_family = AF_IPX;
    memset(sa.sipx_node,0xff,sizeof(sa.sipx_node));
    sa.sipx_port = htons(DOOMPORT);
    memcpy(&sendaddress[MAXNETNODES],&sa,sizeof(sa)); // Save broadcast address
    if (bind(s,(struct sockaddr*)&sa,sizeof(sa)) == -1) {
            I_Error("bind(%d): %s\n",DOOMPORT,strerror(errno));
    }
    return s;
}


//
// PacketSend
//
void PacketSend (int timer)
{
    int		c;

    netbuffer->timer = timer;
    doomcom->datalength = ((doomcom->datalength + 4) +7) & 0xfff8;
    c = sendto (sendsocket, netbuffer, doomcom->datalength
		,0,(void *)&sendaddress[doomcom->remotenode]
		,sizeof(sendaddress[doomcom->remotenode]));
	
    if (c == -1)
    	I_Error ("SendPacket error: %s",strerror(errno));
}

void memdmp(char* p, size_t s) {
	while (s--) { char c = *p++; fprintf(stderr,"%x%x",(c>>4)&0xf,c&0xf); }
	fprintf(stderr,"\n");
}

//
// PacketGet
//
int PacketGet (void)
{
    int			i;
    int			c;
    int			fromlen = sizeof(fromaddress);
				
    c = recvfrom (insocket, netbuffer, sizeof(*netbuffer), 0, &fromaddress, &fromlen );
    ((struct sockaddr_ipx*)&fromaddress)->sipx_zero = 0;
    if (fromlen < sizeof(fromaddress)) memset(&fromaddress,0,sizeof(fromaddress)-fromlen);
    if (c == -1 )
    {
	if (errno != EWOULDBLOCK)
	    I_Error ("GetPacket: %s",strerror(errno));
	doomcom->remotenode = -1;		// no packet
	return 0;
    }

    // find remote node number
    for (i=0 ; i<doomcom->numnodes ; i++) {
//	memdmp(&fromaddress,sizeof fromaddress);
//	memdmp(&sendaddress[i],sizeof fromaddress);
	if (!memcmp(&fromaddress,&sendaddress[i],sizeof fromaddress))
	    break;
    }

    if (i == doomcom->numnodes) {
	i = -1;			// could be a setup packet
    }
	
    {
	static int first=1;
	if (first)
	    printf("len=%d,node=%d:seq=%d,sum=0x%08x,nextword=0%08x] \n", c, i, netbuffer->timer, netbuffer->checksum, *(((int*)netbuffer)+2));
//	first = 0;
    }

    doomcom->remotenode = i;			// good packet from a game player
    doomcom->datalength = c;
	
    return 1;
}


//
// I_ListenPlayers
//
static void I_ListenPlayers (void)
{
    setupdata_t    nodesetup[MAXNETNODES];
    time_t lastsend = 0;

    nodesetup[0].drone = 0;
    nodesetup[0].gameid = 0;
    nodesetup[0].nodesfound = 0;
    nodesetup[0].nodeswanted = doomcom->numnodes;
    netbuffer = &doomcom->data;

    for (;;)  {
	{
	    time_t newtime;
	    newtime = time(NULL);
	    if (newtime != lastsend) {
		lastsend = newtime;
		/* Broadcast our status */
		doomcom->command = CMD_SEND;
		doomcom->remotenode = MAXNETNODES;
		memcpy (&netbuffer->checksum, &nodesetup[0], sizeof(nodesetup[0]));
		doomcom->datalength = sizeof(nodesetup[0]);
		PacketSend(-1);
	    }
	}
	{
	  int i,readynodes;
	  for (i=0, readynodes=0, doomcom->numplayers = 0; i<nodesetup[0].nodesfound ; i++)
	    if (nodesetup[i].nodesfound == nodesetup[i].nodeswanted && nodesetup[i].gameid == nodesetup[0].gameid) {
		readynodes++;
		if (!nodesetup[i].drone) doomcom->numplayers++;
	    }

	  if (readynodes == doomcom->numnodes) break;
	}
	{
	    fd_set fds;
	    struct timeval wt = {1,0};
	    int rc;

	    FD_ZERO(&fds);
	    FD_SET(insocket,&fds);
	    rc = select(insocket+1,&fds,NULL,NULL,&wt);
	    if (rc < 0) I_Error("select: %s",strerror(errno));
	    if (FD_ISSET(insocket,&fds)) {
		if (PacketGet() > 0) {
		    if (doomcom->data.timer == -1) {
			if (doomcom->remotenode == -1) {
			    if (nodesetup[0].nodesfound == nodesetup[0].nodeswanted) continue;
			    // New node!
			    doomcom->remotenode = nodesetup[0].nodesfound++;
			    memcpy(&sendaddress[doomcom->remotenode],&fromaddress,sizeof fromaddress);
			}
			// A setup packet - ignore our own
			if (doomcom->remotenode > 0)
			    memcpy(&nodesetup[doomcom->remotenode],&(doomcom->data.checksum),sizeof(nodesetup[0]));
		    } else if (doomcom->remotenode != -1) {
			// into the game, so must be ready
			nodesetup[doomcom->remotenode].nodesfound = nodesetup[doomcom->remotenode].nodeswanted;
		    }
		}
	    }
	}
    }

    {
	int i;
	doomcom->consoleplayer = doomcom->numplayers = 0;
	for (i=0; i<doomcom->numnodes; i++) {
	    if (nodesetup[i].drone) continue;
	    if (doomcom->numplayers++ > MAXPLAYERS) I_Error("I_ListenPlayers: %d is too many",doomcom->numplayers);
	    if (memcmp(&sendaddress[i],&sendaddress[0],sizeof fromaddress) < 0)
		doomcom->consoleplayer++;
	}
    }
    printf("joined game %d@%d as %d/%d",nodesetup[0].gameid,DOOMPORT,doomcom->consoleplayer+1,doomcom->numplayers);
}

//
// I_InitNetwork
//
void I_InitNetwork (void)
{
    boolean		trueval = true;
    int			i;
    int			p;
	
    doomcom = malloc (sizeof (*doomcom) );
    memset (doomcom, 0, sizeof(*doomcom) );
    
    // set up for network
    i = M_CheckParm ("-dup");
    if (i && i< myargc-1)
    {
	doomcom->ticdup = myargv[i+1][0]-'0';
	if (doomcom->ticdup < 1)
	    doomcom->ticdup = 1;
	if (doomcom->ticdup > 9)
	    doomcom->ticdup = 9;
    }
    else
	doomcom-> ticdup = 1;
	
    if (M_CheckParm ("-extratic"))
	doomcom-> extratics = 1;
    else
	doomcom-> extratics = 0;
		
    p = M_CheckParm ("-port");
    if (p && p<myargc-1)
    {
	DOOMPORT = atoi (myargv[p+1]);
	printf ("using alternate port %i\n",DOOMPORT);
    }
    
    doomcom->id = DOOMCOM_ID;
    doomcom->numplayers = 1;
    
    // parse network game options,
    i = M_CheckParm ("-nodes");
    if (!i)
    {
	// single player game
	netgame = false;
	doomcom->numnodes = 1;
	doomcom->deathmatch = false;
	doomcom->consoleplayer = 0;
	return;
    }

    netsend = PacketSend;
    netget = PacketGet;
    netgame = true;

    // parse player number and host list
    doomcom->numnodes = atoi(myargv[i+1]);	// this node for sure
	
    // build message to receive
    insocket = ipx_socket ();
    ioctl (insocket, FIONBIO, &trueval);

    sendsocket = insocket;

    I_ListenPlayers();
}


void I_NetCmd (void)
{
    if (doomcom->command == CMD_SEND)
    {
	static int counter = 0;
	netsend (counter++);
    }
    else if (doomcom->command == CMD_GET)
    {
	netget ();
    }
    else
	I_Error ("Bad net cmd: %i\n",doomcom->command);
}

