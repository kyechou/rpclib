#ifndef _TIMESTAMPING_HPP
#define _TIMESTAMPING_HPP

#include <iostream>
#include <cstring>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/net_tstamp.h>
#include <linux/errqueue.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/sockios.h>
#include <sys/types.h>
#include <ifaddrs.h>

#define CTRL_SZ 1024

static inline void hwtstamp_setup(int sock, const char *interface)
{
    struct ifreq hwtstamp;
    struct hwtstamp_config hwconfig, hwconfig_req;

    std::memset(&hwtstamp, 0, sizeof(hwtstamp));
    std::strncpy(hwtstamp.ifr_name, interface, sizeof(hwtstamp.ifr_name) - 1);
    hwtstamp.ifr_name[sizeof(hwtstamp.ifr_name) - 1] = 0;
    hwtstamp.ifr_data = (char *)&hwconfig;
    std::memset(&hwconfig, 0, sizeof(hwconfig));
    hwconfig.tx_type = HWTSTAMP_TX_ON;
    hwconfig.rx_filter = HWTSTAMP_FILTER_ALL; // HWTSTAMP_FILTER_PTP_V1_L4_SYNC
    hwconfig_req = hwconfig;
    if (ioctl(sock, SIOCSHWTSTAMP, &hwtstamp) < 0) {
        std::cerr << "SIOCSHWTSTAMP [" << interface << "] failed" << std::endl;
        return;
    }
    std::cerr << "SIOCSHWTSTAMP: [" << interface << "]\n"
              << "               tx_type " << hwconfig_req.tx_type
              << " requested, got " << hwconfig.tx_type << "\n"
              << "               rx_filter " << hwconfig_req.rx_filter
              << " requested, got " << hwconfig.rx_filter << "\n";
}

static inline void ts_setup(int sock) {
    int tsflags;
    struct ifaddrs *ifap, *ifa;

    /* set hardware timestamp options for all interfaces */
    if (getifaddrs(&ifap) < 0) {
        std::cerr << "getifaddrs failed" << std::endl;
        exit(1);
    }
    for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_PACKET &&
                std::strcmp(ifa->ifa_name, "lo") != 0)
            hwtstamp_setup(sock, ifa->ifa_name);
    }
    freeifaddrs(ifap);

    /* set socket options for time stamping */
    tsflags = // request timestamps
            SOF_TIMESTAMPING_RX_HARDWARE
            | SOF_TIMESTAMPING_RX_SOFTWARE
            | SOF_TIMESTAMPING_TX_HARDWARE
            | SOF_TIMESTAMPING_TX_SOFTWARE
            | SOF_TIMESTAMPING_TX_SCHED
            | SOF_TIMESTAMPING_TX_ACK
            // timestamp reporting
            | SOF_TIMESTAMPING_SOFTWARE
            | SOF_TIMESTAMPING_RAW_HARDWARE
            // timestamp options
            | SOF_TIMESTAMPING_OPT_ID
            | SOF_TIMESTAMPING_OPT_TSONLY
            | SOF_TIMESTAMPING_OPT_TX_SWHW
            ;
    if (setsockopt(sock, SOL_SOCKET, SO_TIMESTAMPING, &tsflags,
            sizeof(tsflags)) < 0) {
        std::cerr << "setsockopt failed" << std::endl;
        exit(1);
    }
}

static inline void __print_ts(struct scm_timestamping *tss,
                              struct sock_extended_err *serr)
{
    int type, id;
    char name[64];
    struct timespec *ts;

    if (!tss)
        return;
    if (serr) {
        type = serr->ee_info;
        id = serr->ee_data;
    } else {
        type = id = -1;
    }
    switch (type) {
    case -1:
        strncpy(name, "RX    ", sizeof(name));
        break;
    case SCM_TSTAMP_SND:
        strncpy(name, "TX    ", sizeof(name));
        break;
    case SCM_TSTAMP_SCHED:
        strncpy(name, "SCHED ", sizeof(name));
        break;
    case SCM_TSTAMP_ACK:
        strncpy(name, "ACK   ", sizeof(name));
        break;
    default:
        strncpy(name, "??    ", sizeof(name));
    }

    for (int i = 0; i < 3; i += 2) {
        switch (i) {
        case 0:
            strncpy(name + 6, "SW", sizeof(name) - 6 - 1);
            break;
        case 2:
            strncpy(name + 6, "HW", sizeof(name) - 6 - 1);
            break;
        }

        ts = &tss->ts[i];
        if (ts->tv_sec == 0 && ts->tv_nsec == 0)
            continue;
        fprintf(stderr, "%s: [%3d] %lu.%09lu\n", name, id, ts->tv_sec,
                ts->tv_nsec);
    }
}

static inline void printstamps(struct msghdr *msg)
{
    struct sock_extended_err *serr = NULL;
    struct scm_timestamping *tss = NULL;
    struct cmsghdr *cm;

    for (cm = CMSG_FIRSTHDR(msg); cm; cm = CMSG_NXTHDR(msg, cm)) {
        if (cm->cmsg_level == SOL_SOCKET &&
                        cm->cmsg_type == SCM_TIMESTAMPING) {
            tss = (struct scm_timestamping *) CMSG_DATA(cm);
        } else if ((cm->cmsg_level == SOL_IP &&
                        cm->cmsg_type == IP_RECVERR) ||
                        (cm->cmsg_level == SOL_IPV6 &&
                         cm->cmsg_type == IPV6_RECVERR)) {
            serr = (struct sock_extended_err *) CMSG_DATA(cm);
            if (serr->ee_errno != ENOMSG || serr->ee_origin !=
                            SO_EE_ORIGIN_TIMESTAMPING) {
                fprintf(stderr, "unsupported ip error: %d, "
                        "%d\n", serr->ee_errno,
                        serr->ee_origin);
                serr = NULL;
            }
        } else {
            fprintf(stderr, "unsupported cmsg level, type: %d, "
                    "%d\n", cm->cmsg_level, cm->cmsg_type);
        }
        if (tss && serr) {
            __print_ts(tss, serr);
            tss = NULL;
            serr = NULL;
        }
    }

    /* print the timestamps */
    __print_ts(tss, serr);
}

#endif
