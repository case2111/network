/* 
 * Use Linux AF_PACKET/SOCK_RAW with PACKET_RX_RING for rx.
 * 
 * see packet(7)
 *
 * also 
 *  sudo apt-get install linux-doc
 *  zcat /usr/share/doc/linux-doc/networking/packet_mmap.txt.gz
 *
 */

#include <errno.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>

struct {
  int verbose;
  char *prog;
  char *dev;
  int ticks;
  int bufsz;
  int snaplen;
  int rx_fd;
  int signal_fd;
  int epoll_fd;
} cfg = {
  .snaplen = 65535,
  .dev = "eth0",
  .bufsz = 1024*1024*100, /* 100 mb */
  .rx_fd = -1,
  .signal_fd = -1,
  .epoll_fd = -1,
};

void usage() {
  fprintf(stderr,"usage: %s [-v] [options]\n"
                 " options:      -i <eth>        (interface name)\n"
                 "\n",
          cfg.prog);
  exit(-1);
}

/* signals that we'll accept via signalfd in epoll */
int sigs[] = {SIGHUP,SIGTERM,SIGINT,SIGQUIT,SIGALRM};

int setup_rx(void) {
  int rc=-1, ec;

  /* any link layer protocol packets (linux/if_ether.h) */
  int protocol = htons(ETH_P_ALL);

  /* create the packet socket */
  cfg.rx_fd = socket(AF_PACKET, SOCK_RAW, protocol);
  if (cfg.rx_fd == -1) {
    fprintf(stderr,"socket: %s\n", strerror(errno));
    goto done;
  }

  /* convert interface name to index (in ifr.ifr_ifindex) */
  struct ifreq ifr; 
  strncpy(ifr.ifr_name, cfg.dev, sizeof(ifr.ifr_name));
  ec = ioctl(cfg.rx_fd, SIOCGIFINDEX, &ifr);
  if (ec < 0) {
    fprintf(stderr,"failed to find interface %s\n", cfg.dev);
    goto done;
  }

  /* bind to receive the packets from just one interface */
  struct sockaddr_ll sl;
  memset(&sl, 0, sizeof(sl));
  sl.sll_family = AF_PACKET;
  sl.sll_protocol = protocol;
  sl.sll_ifindex = ifr.ifr_ifindex;
  ec = bind(cfg.rx_fd, (struct sockaddr*)&sl, sizeof(sl));
  if (ec < 0) {
    fprintf(stderr,"socket: %s\n", strerror(errno));
    goto done;
  }

  /* at this point we have a raw packet socket. it could be
   * used to transmit or receive packets coming on the nic.
   * we still need to set promiscuous mode to get all packets. */
  struct packet_mreq m;
  memset(&m, 0, sizeof(m));
  m.mr_ifindex = ifr.ifr_ifindex;
  m.mr_type = PACKET_MR_PROMISC;
  ec = setsockopt(cfg.rx_fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &m, sizeof(m));
  if (ec < 0) {
    fprintf(stderr,"setsockopt PACKET_ADD_MEMBERSHIP: %s\n", strerror(errno));
    goto done;
  }

  /* enable ancillary data, providing packet length and snaplen, etc */
  int on = 1;
  ec = setsockopt(cfg.rx_fd, SOL_PACKET, PACKET_AUXDATA, &on, sizeof(on));
  if (ec < 0) {
    fprintf(stderr,"setsockopt PACKET_AUXDATA: %s\n", strerror(errno));
    goto done;
  }

  rc = 0;

 done:
  return rc;
}

void periodic_work() {
}

int new_epoll(int events, int fd) {
  int rc;
  struct epoll_event ev;
  memset(&ev,0,sizeof(ev)); // placate valgrind
  ev.events = events;
  ev.data.fd= fd;
  if (cfg.verbose) fprintf(stderr,"adding fd %d to epoll\n", fd);
  rc = epoll_ctl(cfg.epoll_fd, EPOLL_CTL_ADD, fd, &ev);
  if (rc == -1) {
    fprintf(stderr,"epoll_ctl: %s\n", strerror(errno));
  }
  return rc;
}

int handle_signal(void) {
  int rc=-1;
  struct signalfd_siginfo info;
  
  if (read(cfg.signal_fd, &info, sizeof(info)) != sizeof(info)) {
    fprintf(stderr,"failed to read signal fd buffer\n");
    goto done;
  }

  switch(info.ssi_signo) {
    case SIGALRM: 
      cfg.ticks++;
      periodic_work();
      alarm(1); 
      break;
    default: 
      fprintf(stderr,"got signal %d\n", info.ssi_signo);  
      goto done;
      break;
  }

 rc = 0;

 done:
  return rc;
}

int handle_packet(void) {
  int rc=-1;
  ssize_t nr;
  struct tpacket_auxdata *pa; /* for PACKET_AUXDATA; see packet(7) */
  struct cmsghdr *cmsg;
  union {
    struct cmsghdr h;
    char data[sizeof(struct cmsghdr) + sizeof(struct tpacket_auxdata)];
  } u;

  /* we get the packet and metadata via recvmsg */
  struct msghdr msgh;
  memset(&msgh, 0, sizeof(msgh));

  /* 'name' is source address. TODO redundant if parsing outselves? */
  char name[100];
  msgh.msg_name = name; 
  msgh.msg_namelen = sizeof(name); 

  /* ancillary data; we requested packet metadata (PACKET_AUXDATA) */
  msgh.msg_control = &u;
  msgh.msg_controllen = sizeof(u);

  /* buffer for packet content itself. */
  char buf[8000];
  struct iovec iov;
  iov.iov_base = buf;
  iov.iov_len = sizeof(buf);
  msgh.msg_iov = &iov;
  msgh.msg_iovlen = 1;

  nr = recvmsg(cfg.rx_fd, &msgh, 0);
  if (nr <= 0) {
    fprintf(stderr,"recvmsg: %s\n", nr ? strerror(errno) : "eof");
    goto done;
  }

  fprintf(stderr,"received %lu bytes of message data\n", (long)nr);
  fprintf(stderr,"received %lu bytes of control data\n", (long)msgh.msg_controllen);
  cmsg = CMSG_FIRSTHDR(&msgh);
  if (cmsg == NULL) {
    fprintf(stderr,"ancillary data missing from packet\n");
    goto done;
  }
  pa = (struct tpacket_auxdata*)CMSG_DATA(cmsg);
  fprintf(stderr, " packet length  %u\n", pa->tp_len);
  fprintf(stderr, " packet snaplen %u\n", pa->tp_snaplen);

  rc = 0;

 done:
  return rc;
}

int main(int argc, char *argv[]) {
  struct epoll_event ev;
  cfg.prog = argv[0];
  int n,opt;

  while ( (opt=getopt(argc,argv,"vi:h")) != -1) {
    switch(opt) {
      case 'v': cfg.verbose++; break;
      case 'i': cfg.dev=strdup(optarg); break; 
      case 'h': default: usage(); break;
    }
  }

  /* block all signals. we take signals synchronously via signalfd */
  sigset_t all;
  sigfillset(&all);
  sigprocmask(SIG_SETMASK,&all,NULL);

  /* a few signals we'll accept via our signalfd */
  sigset_t sw;
  sigemptyset(&sw);
  for(n=0; n < sizeof(sigs)/sizeof(*sigs); n++) sigaddset(&sw, sigs[n]);

  /* create the signalfd for receiving signals */
  cfg.signal_fd = signalfd(-1, &sw, 0);
  if (cfg.signal_fd == -1) {
    fprintf(stderr,"signalfd: %s\n", strerror(errno));
    goto done;
  }

  /* set up the raw socket */
  if (setup_rx() < 0) goto done;

  /* set up the epoll instance */
  cfg.epoll_fd = epoll_create(1); 
  if (cfg.epoll_fd == -1) {
    fprintf(stderr,"epoll: %s\n", strerror(errno));
    goto done;
  }

  /* add descriptors of interest */
  if (new_epoll(EPOLLIN, cfg.signal_fd)) goto done; // signals
  if (new_epoll(EPOLLIN, cfg.rx_fd)) goto done;     // packets

  alarm(1);

  while (epoll_wait(cfg.epoll_fd, &ev, 1, -1) > 0) {
    if (cfg.verbose > 1)  fprintf(stderr,"epoll reports fd %d\n", ev.data.fd);
    if      (ev.data.fd == cfg.signal_fd)   { if (handle_signal() < 0) goto done; }
    else if (ev.data.fd == cfg.rx_fd)       { if (handle_packet() < 0) goto done; }
  }

done:
  if (cfg.rx_fd != -1) close(cfg.rx_fd);
  if (cfg.signal_fd != -1) close(cfg.signal_fd);
  if (cfg.epoll_fd != -1) close(cfg.epoll_fd);
  return 0;
}
