/*
    Copyright (c) 2012 Martin Sustrik.  All rights reserved.
    Copyright (c) 2014 Wirebird Labs LLC.  All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "../src/nn.h"
#include "../src/pair.h"
#include "../src/ws.h"

#include "../src/utils/int.h"
#include "../src/utils/thread.c"

#include "testutil.h"

#include <string.h>

/*****************************************************************************/
/*  Stress tests the WebSocket transport using Autobahn Testsuite.           */
/*  To run this test, Autobahn Testsuite must be installed via:              */
/*  http://autobahn.ws/testsuite/installation.html                           */
/*****************************************************************************/

/*  Skips this WebSocket stress test entirely. */
#define NN_WS_ENABLE_AUTOBAHN_TEST 1

/*  Control whether performances tests are run, which may add an additional
    minute or longer to the test. */
#define NN_WS_STRESS_SKIP_PERF 1

#define NN_WS_DEBUG_AUTOBAHN 0

#define FUZZING_SERVER_ADDRESS "ws://127.0.0.1:9002"

/*  The longest intentional delay in a test as of Autobahn Testsuite v0.7.2
    is nominally 2sec, so a 5000msec timeout gives a bit of headroom. With
    performance tests enabled, some of those tests take 30sec or longer,
    depending on platform. */
#if NN_WS_STRESS_SKIP_PERF
    #define NN_WS_EXCLUDE_CASES "[\"9.*\", \"12.*\", \"13.*\"]"
    #define NN_WS_TEST_CASE_TIMEO 5000
#else
    #define NN_WS_EXCLUDE_CASES "[\"12.*\", \"13.*\"]"
    #define NN_WS_TEST_CASE_TIMEO 60000
#endif

#if NN_WS_DEBUG_AUTOBAHN
    #define NN_WS_DEBUG_AUTOBAHN_FLAG " --debug"
#else
    #define NN_WS_DEBUG_AUTOBAHN_FLAG ""
#endif

#define NN_WS_OPCODE_CLOSE 0x08
#define NN_WS_OPCODE_PING 0x09
#define NN_WS_OPCODE_PONG 0x0A

static int nn_ws_send (int s, const void *msg, size_t len, uint8_t msg_type, int flags)
{
    int rc;
    struct nn_iovec iov;
    struct nn_msghdr hdr;
    struct nn_cmsghdr *cmsg;
    size_t cmsgsz;

    iov.iov_base = (void*) msg;
    iov.iov_len = len;
    
    cmsgsz = NN_CMSG_SPACE (sizeof (msg_type));
    cmsg = nn_allocmsg (cmsgsz, 0);
    if (cmsg == NULL)
        return -1;

    cmsg->cmsg_level = NN_WS;
    cmsg->cmsg_type = NN_WS_MSG_TYPE;
    cmsg->cmsg_len = NN_CMSG_LEN (sizeof (msg_type));
    memcpy (NN_CMSG_DATA (cmsg), &msg_type, sizeof (msg_type));

    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = &cmsg;
    hdr.msg_controllen = NN_MSG;

    rc = nn_sendmsg (s, &hdr, flags);

    return rc;
}

static int nn_ws_recv (int s, void *msg, size_t len, uint8_t *msg_type, int flags)
{
    struct nn_iovec iov;
    struct nn_msghdr hdr;
    struct nn_cmsghdr *cmsg;
    void *cmsg_buf;
    int rc;

    iov.iov_base = msg;
    iov.iov_len = len;

    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = &cmsg_buf;
    hdr.msg_controllen = NN_MSG;

    rc = nn_recvmsg (s, &hdr, flags);
    if (rc < 0)
        return rc;

    /* Find WebSocket opcode ancillary property. */
    cmsg = NN_CMSG_FIRSTHDR (&hdr);
    while (cmsg) {
        if (cmsg->cmsg_level == NN_WS && cmsg->cmsg_type == NN_WS_MSG_TYPE) {
            *msg_type = *(uint8_t *) NN_CMSG_DATA (cmsg);
            break;
        }
        cmsg = NN_CMSG_NXTHDR (&hdr, cmsg);
    }

    /*  WebSocket transport should always report this header. */
    nn_assert (cmsg);

    /*  WebSocket transport should always reassemble fragmented messages. */
    nn_assert (*msg_type & 0x80);

   *msg_type &= 0x0F;

    nn_freemsg (cmsg_buf);

    return rc;
}

static void nn_ws_launch_fuzzing_client (NN_UNUSED void)
{
    FILE *fd;
    int rc;

    /*  Create an Autobahn json config file in the same working directory
        as where the call to wstest will be. */
    fd = fopen ("fuzzingclient.json", "w+");
    errno_assert (fd != NULL);
    rc = fprintf (fd,
        "{\n"
        "    \"servers\": [\n"
        "                  {\n"
        "                    \"agent\": \"nanomsg\",\n"
        "                    \"url\" : \"%s\",\n"
        "                    \"protocols\" : [\"pair.sp.nanomsg.org\"]\n"
        "                  }\n"
        "               ],\n"
        "    \"outdir\" : \"./reports/client\",\n"
        "    \"cases\" : [\"*\"],\n"
        "    \"exclude-cases\" : %s,\n"
        "    \"exclude-agent-cases\" : {}\n"
        "}\n",
        FUZZING_SERVER_ADDRESS, NN_WS_EXCLUDE_CASES);

    errno_assert (rc > 0);
    rc = fclose (fd);
    errno_assert (rc == 0);

#if defined NN_HAVE_WINDOWS
    rc = system (
        "start wstest"
        NN_WS_DEBUG_AUTOBAHN_FLAG
        " --mode=fuzzingclient "
        " --spec=fuzzingclient.json");
#else
    rc = system (
        "wstest"
        NN_WS_DEBUG_AUTOBAHN_FLAG
        " --mode=fuzzingclient"
        " --spec=fuzzingclient.json &");
#endif
    errno_assert (rc == 0);

    return;
}

static void nn_ws_launch_fuzzing_server (NN_UNUSED void)
{
    FILE *fd;
    int rc;

    /*  Create an Autobahn json config file in the same working directory
        as where the call to wstest will be. */
    fd = fopen ("fuzzingserver.json", "w+");
    errno_assert (fd != NULL);
    rc = fprintf (fd,
        "{\n"
        "    \"url\": \"%s\",\n"
        "    \"protocols\" : [\"pair.sp.nanomsg.org\"],\n"
        "    \"outdir\" : \"./reports/server\",\n"
        "    \"cases\" : [\"*\"],\n"
        "    \"exclude-cases\" : %s,\n"
        "    \"exclude-agent-cases\" : {}\n"
        "}\n",
        FUZZING_SERVER_ADDRESS, NN_WS_EXCLUDE_CASES);

    errno_assert (rc > 0);
    rc = fclose (fd);
    errno_assert (rc == 0);

    /*  The following call launches a fuzzing server in an async
        process, assuming Autobahn Testsuite is fully installed
        as per http://autobahn.ws/testsuite/installation.html */

#if defined NN_HAVE_WINDOWS
    rc = system (
        "start wstest"
        NN_WS_DEBUG_AUTOBAHN_FLAG
        " --mode=fuzzingserver"
        " --spec=fuzzingserver.json"
        " --webport=0");
#else
    rc = system (
        "wstest"
        NN_WS_DEBUG_AUTOBAHN_FLAG
        " --mode=fuzzingserver"
        " --spec=fuzzingserver.json"
        " --webport=0 &");
#endif
    errno_assert (rc == 0);

    /*  Allow the server some time to initialize; else, the initial
        connections to it will fail. */
    nn_sleep (5000);

    return;
}

static void nn_ws_kill_autobahn (NN_UNUSED void)
{
    int rc;

#if defined NN_HAVE_WINDOWS
    rc = system ("taskkill /IM wstest.exe");
#else
    rc = system ("pkill Python");
#endif
    nn_assert (rc == 0);
}

static int nn_autobahn_conn (int s, const char *method, int case_number)
{
    char addr [128];
    int ep;

    memset (addr, 0, sizeof (addr));

    if (case_number > 0) {
        sprintf (addr, "%s/%s?agent=nanomsg&case=%d", FUZZING_SERVER_ADDRESS,
            method, case_number);
    }
    else {
        sprintf (addr, "%s/%s?agent=nanomsg", FUZZING_SERVER_ADDRESS,
            method);
    }

    ep = test_connect (s, addr);

    return ep;
}

static void nn_ws_test_agent (void *arg)
{
    int s;
    int rc;
    uint8_t ws_msg_type;
    void *recv_buf;

    nn_assert (arg);

    s = *((int *) arg);

    /*  Remain active until remote endpoint either initiates a Close
        Handshake, or if this endpoint fails the connection based on
        invalid input from the remote peer. */
    while (1) {

        rc = nn_ws_recv (s, &recv_buf, NN_MSG, &ws_msg_type, 0);
        if (rc < 0) {
            errno_assert (errno == EBADF || errno == EINTR);
            return;
        }
        
        errno_assert (rc >= 0);

        switch (ws_msg_type) {
        case NN_WS_MSG_TYPE_TEXT:
            /*  Echo text message verbatim. */
            rc = nn_ws_send (s, &recv_buf, NN_MSG, ws_msg_type, 0);
            break;
        case NN_WS_MSG_TYPE_BINARY:
            /*  Echo binary message verbatim. */
            rc = nn_ws_send (s, &recv_buf, NN_MSG, ws_msg_type, 0);
            break;
        case NN_WS_OPCODE_PING:
            /*  As per RFC 6455 5.5.3, echo PING data payload as a PONG. */
            rc = nn_ws_send (s, &recv_buf, NN_MSG,
                NN_WS_OPCODE_PONG, 0);
            break;
        case NN_WS_OPCODE_PONG:
            /*  Silently ignore PONGs in this echo server. */
            break;
        //case NN_WS_OPCODE_CLOSE:
        //    /*  As per RFC 6455 5.5.1, repeat Close Code in message body. */
        //    rc = nn_ws_send (s, &recv_buf, NN_MSG, ws_msg_type, 0);
        //    return;
        default:
            /*  The library delivered an unexpected message type. */
            nn_assert (0);
            break;
        }
    }
}

int nn_ws_check_result (int case_num, const char *result, size_t len)
{
    /*  This is currently the exhaustive dictonary of responses potentially
        returned by Autobahn Testsuite v0.7.2. It is intentionally hard-coded,
        such that if the Autobahn dependency ever changes in any way, this
        test will likewise require re-evaluation. */
    const char *OK = "{\"behavior\": \"OK\"}";
    const char *NON = "{\"behavior\": \"NON-STRICT\"}";
    const char *INFO = "{\"behavior\": \"INFORMATIONAL\"}";
    const char *UNIMP = "{\"behavior\": \"UNIMPLEMENTED\"}";
    const char *FAILED = "{\"behavior\": \"FAILED\"}";
    int rc;

    if (strncmp (result, OK, len))
        rc = 0;
    else if (strncmp (result, NON, len))
        rc = 0;
    else if (strncmp (result, INFO, len))
        rc = 0;
    else if (strncmp (result, UNIMP, len))
        rc = -1;
    else if (strncmp (result, FAILED, len))
        rc = -1;
    else
        nn_assert (0);

    return rc;
}

void nn_autobahn_disconnect (int s, int ep)
{
    uint8_t *recv_buf = NULL;
    uint8_t ws_msg_type;
    int rc;

    /*  Autobahn sends a close code after all API calls. */
    rc = nn_ws_recv (s, &recv_buf, NN_MSG, &ws_msg_type, 0);
    errno_assert (rc >= 0);
    nn_assert (ws_msg_type == NN_WS_OPCODE_CLOSE);

    /*  As per RFC 6455 5.5.1, repeat Close Code in message body. */
    rc = nn_ws_send (s, &recv_buf, NN_MSG, ws_msg_type, 0);
    errno_assert (rc == 0);

    test_shutdown (s, ep);

    return;
}

int main ()
{
    int client_under_test;
    int client_under_test_ep;
    int test_executive;
    int test_executive_ep;
    int msg_type;
    int rc;
    int i;
    int cases;
    int timeo;
    int passes;
    int failures;
    uint8_t ws_msg_type;
    uint8_t *recv_buf = NULL;
    struct nn_thread echo_agent;

    if (!NN_WS_ENABLE_AUTOBAHN_TEST)
        return 0;

    test_executive = test_socket (AF_SP, NN_PAIR);

    /*  Autobahn TestSuite always sends UTF-8. */
    msg_type = NN_WS_MSG_TYPE_TEXT;
    test_setsockopt (test_executive, NN_WS, NN_WS_MSG_TYPE, &msg_type,
        sizeof (msg_type));

    /*  The first receive could take a few seconds while Autobahn loads. */
    nn_ws_launch_fuzzing_server ();
    timeo = 10000;
    test_setsockopt (test_executive, NN_SOL_SOCKET, NN_RCVTIMEO, &timeo,
        sizeof (timeo));

    /*  We expect nominally three ASCII digits [0-9] representing total
        number of cases to run as of Autobahn TestSuite v0.7.2, but anything
        between 1-4 digits is accepted. */
    printf ("Fetching cases...\n");
    test_executive_ep = nn_autobahn_conn (test_executive, "getCaseCount", -1);
    rc = nn_ws_recv (test_executive, &recv_buf, NN_MSG, &ws_msg_type, 0);
    errno_assert (1 <= rc && rc <= 4);
    nn_assert (ws_msg_type == NN_WS_MSG_TYPE_TEXT);

    /*  Parse ASCII response. */
    cases = 0;
    for (i = 0; i < rc; ++i) {
        nn_assert ('0' <= recv_buf [i] && recv_buf [i] <= '9');
        cases *= 10;
        cases += recv_buf [i] - '0';
    }

    rc = nn_freemsg (recv_buf);
    errno_assert (rc == 0);

    /*  Acknowledge close handshake that follows number of test cases. */
    //nn_autobahn_disconnect (test_executive, test_executive_ep);

    timeo = NN_WS_TEST_CASE_TIMEO;
    test_setsockopt (test_executive, NN_SOL_SOCKET, NN_RCVTIMEO, &timeo,
        sizeof (timeo));

    passes = 0;
    failures = 0;

    /*  Autobahn test cases are 1-indexed, not 0-indexed. */
    for (i = 1; i <= cases; i++) {
        /*  Register the Test Executive to listen for result from Autobahn. */
        test_executive_ep = nn_autobahn_conn (test_executive, "getCaseStatus", i);

        /*  Prepare the echo client for Autobahn Fuzzing Server test case. */
        client_under_test = test_socket (AF_SP, NN_PAIR);
        nn_thread_init (&echo_agent, nn_ws_test_agent, &client_under_test);

        /*  Launch test case on Autobahn Fuzzing Server. */
        client_under_test_ep = nn_autobahn_conn (client_under_test, "runCase", i);

        /*  Wait for Autobahn Server to notify test case is complete. */
        rc = nn_ws_recv (test_executive, &recv_buf, NN_MSG, &ws_msg_type, 0);
        errno_assert (rc > 0);
        nn_assert (ws_msg_type == NN_WS_MSG_TYPE_TEXT);

        switch (nn_ws_check_result (i, recv_buf, rc)) {
        case 0:
            passes++;
            break;
        case -1:
            failures++;
            break;
        default:
            nn_assert (0);
            break;
        }

        rc = nn_freemsg (recv_buf);
        errno_assert (rc == 0);

        /*  Shut down echo client. */
        nn_autobahn_disconnect (test_executive, test_executive_ep);
        test_close (client_under_test);
        nn_thread_term (&echo_agent);
    }
    
    printf ("Server test complete:\n"
            "Passes: %d\n"
            "Failures: %d\n",
            passes, failures);

    /*  Notify Autobahn Fuzzer it's time to create reports. */
    timeo = 10000;
    test_setsockopt (test_executive, NN_SOL_SOCKET, NN_RCVTIMEO, &timeo,
        sizeof (timeo));
    test_executive_ep = nn_autobahn_conn (test_executive, "updateReports", -1);
    nn_autobahn_disconnect (test_executive, test_executive_ep);
    test_close (test_executive);
    
    nn_ws_kill_autobahn ();

    /*  libnanomsg WebSocket Server testing by the Autobahn Client Fuzzer is
        disabled for now until a strategy is devised for communicating with it
        programmatically. */
    /*  nn_ws_launch_fuzzing_client (); */

    return 0;
}
