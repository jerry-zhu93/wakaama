/*******************************************************************************
 *
 * Copyright (c) 2013, 2014, 2015 Intel Corporation and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v10.html
 * The Eclipse Distribution License is available at
 *    http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    David Navarro, Intel Corporation - initial API and implementation
 *    Benjamin Cabé - Please refer to git log
 *    Fabien Fleutot - Please refer to git log
 *    Simon Bernard - Please refer to git log
 *    Julien Vermillard - Please refer to git log
 *    Axel Lorente - Please refer to git log
 *    Toby Jaffey - Please refer to git log
 *    Bosch Software Innovations GmbH - Please refer to git log
 *    Pascal Rieux - Please refer to git log
 *    
 *******************************************************************************/

/*
 Copyright (c) 2013, 2014 Intel Corporation

 Redistribution and use in source and binary forms, with or without modification,
 are permitted provided that the following conditions are met:

     * Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.
     * Redistributions in binary form must reproduce the above copyright notice,
       this list of conditions and the following disclaimer in the documentation
       and/or other materials provided with the distribution.
     * Neither the name of Intel Corporation nor the names of its contributors
       may be used to endorse or promote products derived from this software
       without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 THE POSSIBILITY OF SUCH DAMAGE.

 David Navarro <david.navarro@intel.com>
 Bosch Software Innovations GmbH - Please refer to git log

*/

#include "liblwm2m.h"
#include "connection.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>

extern lwm2m_object_t * get_object_device(void);
extern void free_object_device(lwm2m_object_t * objectP);
extern lwm2m_object_t * get_server_object(void);
extern void free_server_object(lwm2m_object_t * object);
extern lwm2m_object_t * get_security_object(void);
extern void free_security_object(lwm2m_object_t * objectP);
extern char * get_server_uri(lwm2m_object_t * objectP, uint16_t secObjInstID);
extern lwm2m_object_t * get_test_object(void);
extern void free_test_object(lwm2m_object_t * object);

#define MAX_PACKET_SIZE 1024

int g_reboot = 0;
static int g_quit = 0;

typedef struct
{
    lwm2m_object_t * securityObjP;
    int sock;
    connection_t * connList;
} client_data_t;


void handle_sigint(int signum)
{
    g_quit = 1;
}

static void * prv_connect_server(uint16_t secObjInstID,
                                 void * userData)
{
    client_data_t * dataP;
    char * uri;
    char * host;
    char * port;
    connection_t * newConnP = NULL;

    dataP = (client_data_t *)userData;

    uri = get_server_uri(dataP->securityObjP, secObjInstID);

    if (uri == NULL) return NULL;

    fprintf(stdout, "Connecting to %s\r\n", uri);

    // parse uri in the form "coaps://[host]:[port]"
    if (0 == strncmp(uri, "coaps://", strlen("coaps://")))
    {
        host = uri+strlen("coaps://");
    }
    else if (0 == strncmp(uri, "coap://", strlen("coap://")))
    {
        host = uri+strlen("coap://");
    }
    else
    {
        goto exit;
    }
    port = strrchr(host, ':');
    if (port == NULL) goto exit;
    // remove brackets
    if (host[0] == '[')
    {
        host++;
        if (*(port - 1) == ']')
        {
            *(port - 1) = 0;
        }
        else goto exit;
    }
    // split strings
    *port = 0;
    port++;

    newConnP = connection_create(dataP->connList, dataP->sock, host, port);
    if (newConnP == NULL) {
        fprintf(stderr, "Connection creation failed.\r\n");
    }
    else {
        dataP->connList = newConnP;
    }

exit:
    lwm2m_free(uri);
    return (void *)newConnP;
}

static uint8_t prv_buffer_send(void * sessionH,
                               uint8_t * buffer,
                               size_t length,
                               void * userdata)
{
    connection_t * connP = (connection_t*) sessionH;

    if (connP == NULL)
    {
        fprintf(stderr, "#> failed sending %lu bytes, missing connection\r\n", length);
        return COAP_500_INTERNAL_SERVER_ERROR ;
    }

    if (-1 == connection_send(connP, buffer, length))
    {
        fprintf(stderr, "#> failed sending %lu bytes\r\n", length);
        return COAP_500_INTERNAL_SERVER_ERROR ;
    }

    return COAP_NO_ERROR;
}

void print_usage(void)
{
    fprintf(stdout, "Usage: lwm2mclient [OPTION]\r\n");
    fprintf(stdout, "Launch a LWM2M client.\r\n");
    fprintf(stdout, "Options:\r\n");
    fprintf(stdout, "  -n NAME\tSet the endpoint name of the Client. Default: testlightclient\r\n");
    fprintf(stdout, "  -l PORT\tSet the local UDP port of the Client. Default: 56830\r\n");
    fprintf(stdout, "\r\n");
}

void print_state(lwm2m_context_t * lwm2mH)
{
    fprintf(stdout, "State: ");
    switch(lwm2mH->state)
    {
    case STATE_INITIAL:
        fprintf(stdout, "STATE_INITIAL");
        break;
    case STATE_BOOTSTRAP_REQUIRED:
        fprintf(stdout, "STATE_BOOTSTRAP_REQUIRED");
        break;
    case STATE_HOLD_OFF:
        fprintf(stdout, "STATE_HOLD_OFF");
        break;
    case STATE_CLIENT_INITIATED:
        fprintf(stdout, "STATE_CLIENT_INITIATED");
        break;
    case STATE_BOOTSTRAPPING:
        fprintf(stdout, "STATE_BOOTSTRAPPING");
        break;
    case STATE_REGISTERING:
        fprintf(stdout, "STATE_REGISTERING");
        break;
    case STATE_READY:
        fprintf(stdout, "STATE_READY");
        break;
    default:
        fprintf(stdout, "Unknown !");
        break;
    }
    fprintf(stdout, "\r\n");
}

#define OBJ_COUNT 4

int main(int argc, char *argv[])
{
    client_data_t data;
    lwm2m_context_t * lwm2mH = NULL;
    lwm2m_object_t * objArray[OBJ_COUNT];

    const char * localPort = "56830";
    char * name = "testlwm2mclient";

    int result;
    int opt;

    memset(&data, 0, sizeof(client_data_t));

    while ((opt = getopt(argc, argv, "l:n:t:")) != -1)
    {
        switch (opt)
        {
        case 'n':
            name = optarg;
            break;
        case 'l':
            localPort = optarg;
            break;
        default:
            print_usage();
            return 0;
        }
    }

    /*
     *This call an internal function that create an IPv6 socket on the port 5683.
     */
    fprintf(stderr, "Trying to bind LWM2M Client to port %s\r\n", localPort);
    data.sock = create_socket(localPort);
    if (data.sock < 0)
    {
        fprintf(stderr, "Failed to open socket: %d %s\r\n", errno, strerror(errno));
        return -1;
    }

    /*
     * Now the main function fill an array with each object, this list will be later passed to liblwm2m.
     * Those functions are located in their respective object file.
     */
    objArray[0] = get_security_object();
    if (NULL == objArray[0])
    {
        fprintf(stderr, "Failed to create security object\r\n");
        return -1;
    }
    data.securityObjP = objArray[0];

    objArray[1] = get_server_object();
    if (NULL == objArray[1])
    {
        fprintf(stderr, "Failed to create server object\r\n");
        return -1;
    }

    objArray[2] = get_object_device();
    if (NULL == objArray[2])
    {
        fprintf(stderr, "Failed to create Device object\r\n");
        return -1;
    }

    objArray[3] = get_test_object();
    if (NULL == objArray[3])
    {
        fprintf(stderr, "Failed to create Test object\r\n");
        return -1;
    }

    /*
     * The liblwm2m library is now initialized with the functions that will be in
     * charge of communication
     */
    lwm2mH = lwm2m_init(prv_connect_server, prv_buffer_send, &data);
    if (NULL == lwm2mH)
    {
        fprintf(stderr, "lwm2m_init() failed\r\n");
        return -1;
    }

    /*
     * We configure the liblwm2m library with the name of the client - which shall be unique for each client -
     * the number of objects we will be passing through and the objects array
     */
    result = lwm2m_configure(lwm2mH, name, NULL, NULL, OBJ_COUNT, objArray);
    if (result != 0)
    {
        fprintf(stderr, "lwm2m_configure() failed: 0x%X\r\n", result);
        return -1;
    }

    /*
     * We catch Ctrl-C signal for a clean exit
     */
    signal(SIGINT, handle_sigint);

    fprintf(stdout, "LWM2M Client \"%s\" started on port %s.\r\nUse Ctrl-C to exit.\r\n\n", name, localPort);

    /*
     * We now enter in a while loop that will handle the communications from the server
     */
    while (0 == g_quit)
    {
        struct timeval tv;
        fd_set readfds;

        tv.tv_sec = 60;
        tv.tv_usec = 0;

        FD_ZERO(&readfds);
        FD_SET(data.sock, &readfds);

        print_state(lwm2mH);

        /*
         * This function does two things:
         *  - first it does the work needed by liblwm2m (eg. (re)sending some packets).
         *  - Secondly it adjusts the timeout value (default 60s) depending on the state of the transaction
         *    (eg. retransmission) and the time before the next operation
         */
        result = lwm2m_step(lwm2mH, &(tv.tv_sec));
        if (result != 0)
        {
            fprintf(stderr, "lwm2m_step() failed: 0x%X\r\n", result);
            return -1;
        }

        /*
         * This part wait for an event on the socket until "tv" timed out (set
         * with the precedent function)
         */
        result = select(FD_SETSIZE, &readfds, NULL, NULL, &tv);

        if (result < 0)
        {
            if (errno != EINTR)
            {
              fprintf(stderr, "Error in select(): %d %s\r\n", errno, strerror(errno));
            }
        }
        else if (result > 0)
        {
            uint8_t buffer[MAX_PACKET_SIZE];
            int numBytes;

            /*
             * If an event happens on the socket
             */
            if (FD_ISSET(data.sock, &readfds))
            {
                struct sockaddr_storage addr;
                socklen_t addrLen;

                addrLen = sizeof(addr);

                /*
                 * We retrieve the data received
                 */
                numBytes = recvfrom(data.sock, buffer, MAX_PACKET_SIZE, 0, (struct sockaddr *)&addr, &addrLen);

                if (0 > numBytes)
                {
                    fprintf(stderr, "Error in recvfrom(): %d %s\r\n", errno, strerror(errno));
                }
                else if (0 < numBytes)
                {
                    connection_t * connP;

                    connP = connection_find(data.connList, &addr, addrLen);
                    if (connP != NULL)
                    {
                        /*
                         * Let liblwm2m respond to the query depending on the context
                         */
                        lwm2m_handle_packet(lwm2mH, buffer, numBytes, connP);
                    }
                    else
                    {
                        /*
                         * This packet comes from an unknown peer
                         */
                        fprintf(stderr, "received bytes ignored!\r\n");
                    }
                }
            }
        }
    }

    /*
     * Finally when the loop is left, we unregister our client from it
     */
    lwm2m_close(lwm2mH);
    close(data.sock);
    connection_free(data.connList);

    free_security_object(objArray[0]);
    free_server_object(objArray[1]);
    free_object_device(objArray[2]);
    free_test_object(objArray[3]);

    fprintf(stdout, "\r\n\n");

    return 0;
}
