/* Copyright 2001 Jeff Dike and others
 * Licensed under the GPL
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <readline/readline.h>
#include <readline/history.h>

static char uml_name[11];
static struct sockaddr_un sun;

static int do_switch(char *name)
{
  struct stat buf;

  if(stat(name, &buf) == -1){
    fprintf(stderr, "Couldn't stat file: %s - ", name);
    perror("");
    return(-1);
  }
  sun.sun_family = AF_UNIX;
  strcpy(sun.sun_path, name);
  if(sscanf(name, "/tmp/uml/%10[^/]/mconsole", uml_name) < 1){
    printf("Couldn't determine UML name from '%s'\n", name);
    strcpy(uml_name, "[Unknown]");
  }
  return(0);
}

static int switch_common(char *name)
{
  char file[MAXPATHLEN];
  int try_file = 1;

  sprintf(file, "/tmp/uml/%s/mconsole", name);
  if(strncmp(name, "/tmp/uml/", strlen("/tmp/uml/"))){
    if(!do_switch(file)) return(0);
    try_file = 0;
  }
  if(!do_switch(name)) return(0);
  if(!try_file) return(-1);
  return(do_switch(file));
}

#define MCONSOLE_MAGIC (0xcafebabe)
#define MCONSOLE_MAX_DATA (512)
#define MCONSOLE_VERSION (1)

#define MIN(a,b) ((a)<(b) ? (a):(b))

struct mconsole_request {
	unsigned long magic;
	int version;
	int len;
	char data[MCONSOLE_MAX_DATA];
};

struct mconsole_reply {
	int err;
	int more;
	int len;
	char data[MCONSOLE_MAX_DATA];
};

static void default_cmd(int fd, char *command)
{
  struct iovec iov;
  struct msghdr msg;
  struct cmsghdr *cmsg;
  struct ucred *credptr;
  struct mconsole_request request;
  struct mconsole_reply reply;
  char anc[CMSG_SPACE(sizeof(*credptr))];
  char name[128];
  int n;

  if((sscanf(command, "%128[^: \f\n\r\t\v]:", name) == 1) && 
     (strchr(command, ':') != NULL)){
    if(switch_common(name)) return;
    command = strchr(command, ':');
    *command++ = '\0';
    while(isspace(*command)) command++;
  }

  request.magic = MCONSOLE_MAGIC;
  request.version = MCONSOLE_VERSION;
  request.len = MIN(strlen(command), sizeof(reply.data) - 1);
  strncpy(request.data, command, request.len);
  request.data[request.len] = '\0';
  iov.iov_base = &request;
  iov.iov_len = sizeof(request);

  msg.msg_control = anc;
  msg.msg_controllen = sizeof(anc);

  cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_CREDENTIALS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(*credptr));
  credptr = (struct ucred *) CMSG_DATA(cmsg);
  credptr->pid = getpid();
  credptr->uid = getuid();
  credptr->gid = getgid();

  msg.msg_name = &sun;
  msg.msg_namelen = sizeof(sun);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_controllen = cmsg->cmsg_len;
  msg.msg_flags = 0;

  if(sendmsg(fd, &msg, 0) < 0){
    fprintf(stderr, "Sending command to '%s' : ", sun.sun_path);
    perror("");
    return;
  }

  iov.iov_base = &reply;
  iov.iov_len = sizeof(reply);

  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = NULL;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;

  do {
    n = recvmsg(fd, &msg, 0);
    if(n < 0){
      perror("recvmsg");
      return;
    }
    if(reply.err) printf("ERR ");
    else printf("OK ");
    printf("%s", reply.data);
  } while(reply.more);
  printf("\n");
}

static void help_cmd(int fd, char *command)
{
  default_cmd(fd, command);
  printf("Additional local mconsole commands:\n");
  printf("    quit - Quit mconsole\n");
  printf("    switch <socket-name> - Switch control to the given machine\n");
}

static void switch_cmd(int fd, char *command)
{
  char *ptr;

  ptr = &command[strlen("switch")];
  while(isspace(*ptr)) ptr++;
  if(switch_common(ptr)) return;
  printf("Switched to '%s'\n", ptr);
}

static void quit_cmd(int fd, char *command)
{
  exit(0);
}

struct cmd {
  char *command;
  void (*proc)(int, char *);
};

static struct cmd cmds[] = {
  { "quit", quit_cmd },
  { "help", help_cmd },
  { "switch", switch_cmd },
  { NULL, default_cmd }
};

/* sends a command */
int issue_command(int fd, char *command)
{
  char *ptr;
  int i;

  /* Trim trailing spaces left by readline's filename completion */
  ptr = &command[strlen(command) - 1];
  while(isspace(*ptr)) *ptr-- = '\0';
    
  for(i = 0; i < sizeof(cmds)/sizeof(cmds[0]); i++){
    if((cmds[i].command == NULL) || 
       !strncmp(cmds[i].command, command, strlen(cmds[i].command))){
      (*cmds[i].proc)(fd, command);
      break;
    }
  }
    
  /* in future, return command status */
  return 0;
}

/* sends a command in argv style array */
int issue_commandv(int fd, char **argv)
{
  char *command;
  int len, i, status;

  len = 1;  /* space for trailing null */
  for(i = 0; argv[i] != NULL; i++)
    len += strlen(argv[i]) + 1;  /* space for space */

  command = malloc(len);
  if(command == NULL){
    perror("issue_command");
    return(-1);
  }
  command[0] = '\0';

  for(i = 0; argv[i] != NULL; i++) {
    strcat(command, argv[i]);
    if(argv[i+1] != NULL) strcat(command, " ");
  }

  status = issue_command(fd, command);

  free(command);

  return status;
}

static void Usage(void)
{
  fprintf(stderr, "Usage : uml_mconsole socket-name [command]\n");
  exit(1);
}

int main(int argc, char **argv)
{
  struct sockaddr_un here;
  char *sock;
  int fd;

  if(argc < 2) Usage();
  strcpy(uml_name, "[None]");
  sock = argv[1];
  switch_common(sock);

  if((fd = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0){
    perror("socket");
    exit(1);
  }
  here.sun_family = AF_UNIX;
  memset(here.sun_path, 0, sizeof(here.sun_path));

  sprintf(&here.sun_path[1], "%5d", getpid());
  if(bind(fd, (struct sockaddr *) &here, sizeof(here)) < 0){
    perror("bind");
    exit(1);
  }

  if(argc>2)
    return issue_commandv(fd, argv+2);

  while(1){
    char *command, prompt[1 + sizeof(uml_name) + 2 + 1];

    sprintf(prompt, "(%s) ", uml_name);
    command = readline(prompt);
    if(command == NULL) break;

    if(*command) add_history(command);

    issue_command(fd,command);
    free(command);
  }
  printf("\n");
  return(0);
}
