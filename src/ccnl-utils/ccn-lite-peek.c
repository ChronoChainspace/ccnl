/*
 * @f util/ccn-lite-peek.c
 * @b request content: send an interest, wait for reply, output to stdout
 *
 * Copyright (C) 2013-15, Christian Tschudin, University of Basel
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * File history:
 * 2013-04-06  created
 * 2014-06-18  added NDNTLV support
 */

#include "ccnl-common.c"
#include <unistd.h>
#ifndef assert
#define assert(...) do {} while(0)
#endif

// ----------------------------------------------------------------------

unsigned char out[8*CCNL_MAX_PACKET_SIZE];
int outlen;

int
frag_cb(struct ccnl_relay_s *relay, struct ccnl_face_s *from,
        unsigned char **data, int *len)
{
    (void)relay;
    (void)from;
    DEBUGMSG(INFO, "frag_cb\n");

    memcpy(out, *data, *len);
    outlen = *len;
    return 0;
}

int
main(int argc, char *argv[])
{
    int cnt, len, opt, port, sock = 0, socksize, suite = CCNL_SUITE_NDNTLV;
    char *addr = NULL, *udp = NULL, *ux = NULL;
    struct sockaddr sa;
    struct ccnl_prefix_s *prefix;
    float wait = 3.0;
    unsigned int chunknum = UINT_MAX;
    struct ccnl_buf_s *buf = NULL;
#ifdef USE_FRAG
    ccnl_isFragmentFunc isFragment;
#endif

    while ((opt = getopt(argc, argv, "hn:s:u:v:w:x:")) != -1) {
        switch (opt) {
        case 'n':
            chunknum = atoi(optarg);
            break;
        case 's':
            suite = ccnl_str2suite(optarg);
            if (!ccnl_isSuite(suite))
                goto usage;
            break;
        case 'u':
            udp = optarg;
            break;
        case 'v':
#ifdef USE_LOGGING
            if (isdigit(optarg[0]))
                debug_level = atoi(optarg);
            else
                debug_level = ccnl_debug_str2level(optarg);
#endif
            break;
        case 'w':
            wait = atof(optarg);
            break;
        case 'x':
            ux = optarg;
            break;
        case 'h':
        default:
usage:
            fprintf(stderr, "usage: %s [options] URI [NFNexpr]\n"
            "  -n CHUNKNUM      positive integer for chunk interest\n"
            "  -s SUITE         (ccnb, ccnx2015, ndn2013)\n"
            "  -u a.b.c.d/port  UDP destination (default is suite-dependent)\n"
#ifdef USE_LOGGING
            "  -v DEBUG_LEVEL (fatal, error, warning, info, debug, verbose, trace)\n"
#endif
            "  -w timeout       in sec (float)\n"
            "  -x ux_path_name  UNIX IPC: use this instead of UDP\n"
            "Examples:\n"
            "%% peek /ndn/edu/wustl/ping             (classic lookup)\n"
            "%% peek /th/ere  \"lambda expr\"          (lambda expr, in-net)\n"
            "%% peek \"\" \"add 1 1\"                    (lambda expr, local)\n"
            "%% peek /rpc/site \"call 1 /test/data\"   (lambda RPC, directed)\n",
            argv[0]);
            exit(1);
        }
    }

    if (!argv[optind])
        goto usage;

    srandom(time(NULL));

    if (ccnl_parseUdp(udp, suite, &addr, &port) != 0) {
        exit(-1);
    }
    DEBUGMSG(TRACE, "using udp address %s/%d\n", addr, port);

#ifdef USE_FRAG
    isFragment = ccnl_suite2isFragmentFunc(suite);
#endif

    if (ux) { // use UNIX socket
        struct sockaddr_un *su = (struct sockaddr_un*) &sa;
        su->sun_family = AF_UNIX;
        strcpy(su->sun_path, ux);
        sock = ux_open();
    } else { // UDP
        struct sockaddr_in *si = (struct sockaddr_in*) &sa;
        si->sin_family = PF_INET;
        si->sin_addr.s_addr = inet_addr(addr);
        si->sin_port = htons(port);
        sock = udp_open();
    }

    prefix = ccnl_URItoPrefix(argv[optind], suite, argv[optind+1], chunknum == UINT_MAX ? NULL : &chunknum);

    DEBUGMSG(DEBUG, "prefix <%s><%s> became %s\n",
            argv[optind], argv[optind+1], ccnl_prefix_to_path(prefix));

    for (cnt = 0; cnt < 3; cnt++) {
        int nonce = random();
        int rc;
        struct ccnl_face_s dummyFace;
        ccnl_interest_opts_u int_opts;
#ifdef USE_SUITE_NDNTLV
        int_opts.ndntlv.nonce = nonce;
#endif

        DEBUGMSG(TRACE, "sending request, iteration %d\n", cnt);

        memset(&dummyFace, 0, sizeof(dummyFace));

        buf = ccnl_mkSimpleInterest(prefix, &int_opts);

        DEBUGMSG(DEBUG, "interest has %zd bytes\n", buf->datalen);
/*
        {
            int fd = open("outgoing.bin", O_WRONLY|O_CREAT|O_TRUNC);
            write(fd, out, len);
            close(fd);
        }
*/
        if (ux) {
            socksize = sizeof(struct sockaddr_un);
        } else {
            socksize = sizeof(struct sockaddr_in);
        }
        rc = sendto(sock, buf->data, buf->datalen, 0, (struct sockaddr*)&sa, socksize);
        if (rc < 0) {
            perror("sendto");
            myexit(1);
        }
        DEBUGMSG(DEBUG, "sendto returned %d\n", rc);

        for (;;) { // wait for a content pkt (ignore interests)
            unsigned char *cp = out;
            int enc, suite2, len2;
            DEBUGMSG(TRACE, "  waiting for packet\n");

            if (block_on_read(sock, wait) <= 0) // timeout
                break;
            len = recv(sock, out, sizeof(out), 0);

            DEBUGMSG(DEBUG, "received %d bytes\n", len);
/*
            {
                int fd = open("incoming.bin", O_WRONLY|O_CREAT|O_TRUNC, 0700);
                write(fd, out, len);
                close(fd);
            }
*/
            suite2 = -1;
            len2 = len;
            while (!ccnl_switch_dehead(&cp, &len2, &enc))
                suite2 = ccnl_enc2suite(enc);
            if (suite2 != -1 && suite2 != suite) {
                DEBUGMSG(DEBUG, "  unknown suite %d\n", suite);
                continue;
            }

#ifdef USE_FRAG
            if (isFragment && isFragment(cp, len2)) {
                int t;
                int len3;
                DEBUGMSG(DEBUG, "  fragment, %d bytes\n", len2);
                switch(suite) {
                case CCNL_SUITE_CCNTLV: {
                    struct ccnx_tlvhdr_ccnx2015_s *hp;
                    hp = (struct ccnx_tlvhdr_ccnx2015_s *) out;
                    cp = out + sizeof(*hp);
                    len2 -= sizeof(*hp);
                    if (ccnl_ccntlv_dehead(&cp, &len2, (unsigned*)&t, (unsigned*) &len3) < 0 ||
                        t != CCNX_TLV_TL_Fragment) {
                        DEBUGMSG(ERROR, "  error parsing fragment\n");
                        continue;
                    }
                    /*
                    rc = ccnl_frag_RX_Sequenced2015(frag_cb, NULL, &dummyFace,
                                      4096, hp->fill[0] >> 6,
                                      ntohs(*(uint16_t*) hp->fill) & 0x03fff,
                                      &cp, (int*) &len2);
                    */
                    rc = ccnl_frag_RX_BeginEnd2015(frag_cb, NULL, &dummyFace,
                                      4096, hp->fill[0] >> 6,
                                      ntohs(*(uint16_t*) hp->fill) & 0x03fff,
                                      &cp, (int*) &len3);
                    break;
                }
                default:
                    continue;
                }
                if (!outlen)
                    continue;
                len = outlen;
            }
#endif

/*
        {
            int fd = open("incoming.bin", O_WRONLY|O_CREAT|O_TRUNC);
            write(fd, out, len);
            close(fd);
        }
*/
            rc = ccnl_isContent(out, len, suite);
            if (rc < 0) {
                DEBUGMSG(ERROR, "error when checking type of packet\n");
                goto done;
            }
            if (rc == 0) { // it's an interest, ignore it
                DEBUGMSG(WARNING, "skipping non-data packet\n");
                continue;
            }
            write(1, out, len);
            myexit(0);
        }
        if (cnt < 2)
            DEBUGMSG(WARNING, "re-sending interest\n");
    }
    fprintf(stderr, "timeout\n");

done:
    close(sock);
    myexit(-1);
    return 0; // avoid a compiler warning
}

// eof
