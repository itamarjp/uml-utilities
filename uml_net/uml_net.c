#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <ctype.h>

/* Shouldn't there be some other way to figure out the major and minor
 * number of the tap device other than hard-wiring it here? Does FreeBSD
 * have a 'tap' device? Should we have some kind of #ifdef here?
 */
#define TAP_MAJOR 36
#define TAP_MINOR 16  /* plus whatever tap device it was. */

void fail(int fd)
{
  char c = 0;

  if(write(fd, &c, sizeof(c)) != sizeof(c))
    perror("Writing failure byte");
  exit(1);
}

int do_exec(char **args, int need_zero)
{
  int pid, status;

  if((pid = fork()) == 0){
    execvp(args[0], args);
    fprintf(stderr, "Failed to exec '%s'", args[0]);
    perror("");
    exit(1);
  }
  else if(pid < 0){
    perror("fork failed");
    return(-1);
  }
  if(waitpid(pid, &status, 0) < 0){
    perror("execvp");
    return(-1);
  }
  if(need_zero && (!WIFEXITED(status) || (WEXITSTATUS(status) != 0))){
    printf("'%s' didn't exit with status 0\n", args[0]);
    return(-1);
  }
  return(0);
}

static int maybe_insmod(char *dev)
{
  struct ifreq ifr;
  int fd, unit;
  char unit_buf[sizeof("unit=nnn\0")];
  char ethertap_buf[sizeof("ethertapnnn\0")];
  char *insmod_argv[] = { "insmod", "ethertap", unit_buf, "-o", ethertap_buf,
			  NULL };
  
  if((fd = socket(PF_INET, SOCK_DGRAM, 0)) < 0){
    perror("socket");
    return(-1);
  }
  strcpy(ifr.ifr_name, dev);
  if(ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) return(0);
  if(errno != ENODEV){
    perror("SIOCGIFFLAGS on tap device");
    return(-1);
  }
  if(sscanf(dev, "tap%d", &unit) != 1){
    fprintf(stderr, "failed to get unit number from '%s'\n", dev);
    return(-1);
  }
  sprintf(unit_buf, "unit=%d", unit);
  sprintf(ethertap_buf, "ethertap%d", unit);
  return(do_exec(insmod_argv, 0));
}

/* This is a routine to do a 'mknod' on the /dev/tap<n> if possible:
 * Return: 0 is ok, -1=already open, etc.
  */
static int mk_node(char *devname)
{
  struct stat statval;
  int retval;
  int minor; /* the minor number for the tap device. */

  /* first do a stat on the node to see whether it exists and we
   * had some other reason to fail:
   */
  retval=stat(devname,&statval);
  if(!retval || (errno != ENOENT)){
    /* it does exist. We are just going to return -1, 'cause there
     * was some other problem in the open :-(.
     */
    return -1;
  }
  /* It doesn't exist. We can create it. */

  if(sscanf(devname, "/dev/tap%d", &minor) != 1){
    fprintf(stderr, "failed to get unit number from '%s'\n", devname);
    return(-1);
  }
  minor += TAP_MINOR;

  /* Now to do a mknod on it: */

  return(mknod(devname, S_IFCHR|S_IREAD|S_IWRITE, makedev(TAP_MAJOR,minor)));
}

#define BUF_SIZE 1500

static void ethertap(int argc, char **argv)
{
  char *dev = argv[0];
  int fd = atoi(argv[1]);
  char gate_addr[sizeof("255.255.255.255\0")];
  char remote_addr[sizeof("255.255.255.255\0")];
  char ether_addr[sizeof("ff:ff:ff:ff:ff:ff\0")];
  int ip[4];

  char *ifconfig_argv[] = { "ifconfig", dev, "arp", "mtu", "1500", gate_addr,
			    "up", NULL };
  char *down_argv[] = { "ifconfig", dev, "0.0.0.0", "down", NULL };
  char *route_argv[] = { "route", "add", "-host", remote_addr, "gw", 
			 gate_addr, NULL };
  char *arp_argv[] = { "arp", "-Ds", remote_addr, "eth0", "pub", NULL };
  char *no_arp_argv[] = { "arp", "-i", "eth0", "-d", remote_addr, "pub", 
			  NULL };
  char *forw_argv[] = { "bash",  "-c", 
			"echo 1 > /proc/sys/net/ipv4/ip_forward", NULL };
  char dev_file[sizeof("/dev/tapxxxx\0")], c;
  int tap;

  signal(SIGHUP, SIG_IGN);
  if(argc > 2){
    strncpy(gate_addr, argv[2], sizeof(gate_addr));
    strncpy(remote_addr, argv[3], sizeof(remote_addr));
    setreuid(0, 0);

    sscanf(gate_addr, "%d.%d.%d.%d", &ip[0], &ip[1], &ip[2], &ip[3]);
    sprintf(ether_addr, "fd:fe:%x:%x:%x:%x", ip[0], ip[1], ip[2], ip[3]);
    if(maybe_insmod(dev)) fail(fd);
    if(do_exec(ifconfig_argv, 1)) fail(fd);
    if(do_exec(route_argv, 0)) fail(fd);
    do_exec(arp_argv, 0);
    do_exec(forw_argv, 0);

    /* On UML : route add -net 192.168.0.0 gw 192.168.0.4 netmask 255.255.255.0
     * route add default gw 192.168.0.4 - to reach the rest of the net
     */
  }
  sprintf(dev_file, "/dev/%s", dev);

  mk_node(dev_file); /* do a mknod on it if it doesn't exist. */

  if((tap = open(dev_file, O_RDWR | O_NONBLOCK)) < 0){
    perror("open");
    fail(fd);
  }

  c = 1;
  if(write(fd, &c, sizeof(c)) != sizeof(c)){
    perror("write");
    fail(fd);
  }

  while(1){
    fd_set fds, except;
    char buf[BUF_SIZE];
    int n, max;

    FD_ZERO(&fds);
    FD_SET(tap, &fds);
    FD_SET(fd, &fds);
    except = fds;
    max = ((tap > fd) ? tap : fd) + 1;
    if(select(max, &fds, NULL, &except, NULL) < 0){
      perror("select");
      continue;
    }
    if(FD_ISSET(tap, &fds)){
      n = read(tap, buf, sizeof(buf));
      if(n == 0) break;
      else if(n < 0) perror("read");
      n = send(fd, buf, n, 0);
      if((n < 0) && (errno != EAGAIN)){
	perror("send");
	break;
      }
    }
    else if(FD_ISSET(fd, &fds)){
      n = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
      if(n == 0) break;
      else if(n < 0) perror("recvfrom");
      n = write(tap, buf, n);
      if(n < 0) perror("write");      
    }
    else continue;
  }
  do_exec(down_argv, 0);
  do_exec(no_arp_argv, 0);
}

static void slip_up(char **argv)
{
  int fd = atoi(argv[0]);
  char *gate_addr = argv[1];
  char *remote_addr = argv[2];
  char slip_name[sizeof("slxxxx\0")];
  char *up_argv[] = { "ifconfig", slip_name, gate_addr, "pointopoint", 
		      remote_addr, "mtu", "1500", "up", NULL };
  char *arp_argv[] = { "arp", "-Ds", remote_addr, "eth0", "pub", NULL };
  char *forw_argv[] = { "bash",  "-c", 
			"echo 1 > /proc/sys/net/ipv4/ip_forward", NULL };
  int disc, sencap, n;
  
  disc = N_SLIP;
  if((n = ioctl(fd, TIOCSETD, &disc)) < 0){
    perror("Setting slip line discipline");
    exit(1);
  }
  sencap = 0;
  if(ioctl(fd, SIOCSIFENCAP, &sencap) < 0){
    perror("Setting slip encapsulation");
    exit(1);
  }
  sprintf(slip_name, "sl%d", n);
  if(do_exec(up_argv, 1)) exit(1);
  do_exec(arp_argv, 1);
  do_exec(forw_argv, 0);
}

static void slip_down(char **argv)
{
  int fd = atoi(argv[0]);
  char *remote_addr = argv[1];
  char slip_name[sizeof("slxxxx\0")];
  char *down_argv[] = { "ifconfig", slip_name, "0.0.0.0", "down", NULL };
  char *no_arp_argv[] = { "arp", "-i", "eth0", "-d", remote_addr, "pub", 
			  NULL };
  int n, disc;

  if((n = ioctl(fd, TIOCGETD, &disc)) < 0){
    perror("Getting slip line discipline");
    exit(1);
  }
  sprintf(slip_name, "sl%d", n);  
  if(do_exec(down_argv, 1)) exit(1);
  do_exec(no_arp_argv, 1);
}

static void slip(int argc, char **argv)
{
  char *op = argv[0];

  if(!strcmp(argv[0], "up")) slip_up(&argv[1]);
  else if(!strcmp(argv[0], "down")) slip_down(&argv[1]);
  else {
    printf("slip - Unknown op '%s'\n", op);
    exit(1);
  }
}

int main(int argc, char **argv)
{
  char *transport = argv[1];

  setenv("PATH", "/bin:/usr/bin:/sbin:/usr/sbin", 1);
  if(!strcmp(transport, "ethertap")) ethertap(argc - 2, &argv[2]);
  else if(!strcmp(transport, "slip")) slip(argc - 2, &argv[2]);
  else {
    printf("Unknown transport : '%s'\n", transport);
    exit(1);
  }
  return(0);
}
