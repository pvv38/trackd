#ifdef __GNUC__
#define _GNU_SOURCE             /* for strsignal() */
#endif

#define DEBUG 1

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <syslog.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>

/*************************************************************************/
//
// cwzlwlzfio8yda8fpn00 pvvgps-reader
// b4tt0znyu03sugpjq8e5 pvvgps-writer
//
/* global variables and constants */

volatile sig_atomic_t gGracefulShutdown = 0;
volatile sig_atomic_t gCaughtHupSignal = 0;
volatile sig_atomic_t gNewConnection = 0;

int gLockFileDesc = -1;
int gMasterSocket = -1;

const int gtrackdPort = 8899;

const char *const gLockFilePath = "/var/run/trackd.pid";

int BecomeDaemonProcess ( const char *const lockFileName,
                          const char *const logPrefix,
                          const int logLevel,
                          int *const lockFileDesc, int *const thisPID );
int ConfigureSignalHandlers ( void );
int BindPassiveSocket ( const int portNum, int *const boundSocket );
int AcceptConnections ( const int master );
int HandleConnection ( const int slave );
int ReadLine ( const int sock );
void FatalSigHandler ( int sig );
void TermHandler ( int sig );
void HupHandler ( int sig );
void SIGIOHandler ( int sig );
void Usr1Handler ( int sig );
void TidyUp ( void );
void SaveDataToDB(int day, int month, int year, int sec, int min, int hour, int gsm_lev, int speed, int nsput, int latitude, int longitude);

#define QLEN 10*1024
char qbuff[QLEN];
int bpos = 0, epos = 0;

int getNextRec ( char * buff );
void push ( char *buff, int blen );
void DecodeRec ( char *buff );

int main ( int argc, char *argv[] )
{
  int result;
  pid_t daemonPID;

  if ( argc > 1 ) {
    int fd, len;
    pid_t pid;
    char pid_buf[16];

    if ( ( fd = open ( gLockFilePath, O_RDONLY ) ) < 0 ) {
      perror ( "Lock file not found. May be the server is not running?" );
      exit ( fd );
    }
    len = read ( fd, pid_buf, 16 );
    pid_buf[len] = 0;
    pid = atoi ( pid_buf );
    if ( !strcmp ( argv[1], "stop" ) ) {
      kill ( pid, SIGUSR1 );
      exit ( EXIT_SUCCESS );
    }
    if ( !strcmp ( argv[1], "restart" ) ) {
      kill ( pid, SIGHUP );
      exit ( EXIT_SUCCESS );
    }
    printf ( "usage %s [stop|restart]\n", argv[0] );
    exit ( EXIT_FAILURE );
  }

  if ( ( result = BecomeDaemonProcess ( gLockFilePath, "trackd",
                                        LOG_DEBUG, &gLockFileDesc,
                                        &daemonPID ) ) < 0 ) {
    perror ( "Failed to become daemon process" );
    exit ( result );
  }

  if ( ( result = ConfigureSignalHandlers () ) < 0 ) {
    syslog ( LOG_LOCAL0 | LOG_INFO,
             "ConfigureSignalHandlers failed, errno=%d", errno );
    unlink ( gLockFilePath );
    exit ( result );
  }

  if ( ( result = BindPassiveSocket ( gtrackdPort, &gMasterSocket ) ) < 0 ) {
    syslog ( LOG_LOCAL0 | LOG_INFO, "BindPassiveSocket failed, errno=%d",
             errno );
    unlink ( gLockFilePath );
    exit ( result );
  }

  do {
    if ( AcceptConnections ( gMasterSocket ) < 0 ) {
      syslog ( LOG_LOCAL0 | LOG_INFO, "AcceptConnections failed, errno=%d",
               errno );
      unlink ( gLockFilePath );
      exit ( result );
    }

    if ( ( gGracefulShutdown == 1 ) && ( gCaughtHupSignal == 0 ) )
      break;

    gGracefulShutdown = gCaughtHupSignal = 0;
  } while ( 1 );
  TidyUp ();
  return 0;
}

int BecomeDaemonProcess ( const char *const lockFileName,
                          const char *const logPrefix,
                          const int logLevel,
                          int *const lockFileDesc, pid_t * const thisPID )
{
  int curPID, stdioFD, lockResult, killResult, lockFD, i, numFiles;
  char pidBuf[17], *lfs, pidStr[7];
  FILE *lfp;
  unsigned long lockPID;
  struct flock exclusiveLock;

  chdir ( "/" );

  /* try to grab the lock file */
  lockFD = open ( lockFileName, O_RDWR | O_CREAT | O_EXCL, 0644 );

  if ( lockFD == -1 ) {
    lfp = fopen ( lockFileName, "r" );
    if ( lfp == 0 ) {           /* Game over. Bail out */
      perror ( "Can't get lockfile" );
      return -1;
    }
    lfs = fgets ( pidBuf, 16, lfp );
    if ( lfs != 0 ) {
      if ( pidBuf[strlen ( pidBuf ) - 1] == '\n' )      /* strip linefeed */
        pidBuf[strlen ( pidBuf ) - 1] = 0;

      lockPID = strtoul ( pidBuf, ( char ** ) 0, 10 );
      killResult = kill ( lockPID, 0 );
      if ( killResult == 0 ) {
        printf
          ( "\n\nERROR\n\nA lock file %s has been detected. It appears it is owned\nby the (active) process with PID %ld.\n\n",
            lockFileName, lockPID );
      }
      else {
        if ( errno == ESRCH ) { /* non-existent process */
          printf
            ( "\n\nERROR\n\nA lock file %s has been detected. It appears it is owned\nby the process with PID %ld, which is now defunct. Delete the lock file\nand try again.\n\n",
              lockFileName, lockPID );
        }
        else {
          perror ( "Could not acquire exclusive lock on lock file" );
        }
      }
    }
    else
      perror ( "Could not read lock file" );

    fclose ( lfp );

    return -1;
  }

  exclusiveLock.l_type = F_WRLCK;       /* exclusive write lock */
  exclusiveLock.l_whence = SEEK_SET;    /* use start and len */
  exclusiveLock.l_len = exclusiveLock.l_start = 0;      /* whole file */
  exclusiveLock.l_pid = 0;      /* don't care about this */
  lockResult = fcntl ( lockFD, F_SETLK, &exclusiveLock );

  if ( lockResult < 0 ) {       /* can't get a lock */
    close ( lockFD );
    perror ( "Can't get lockfile" );
    return -1;
  }

  curPID = fork ();
  switch ( curPID ) {
  case 0:                      /* we are the child process */
    break;

  case -1:                     /* error - bail out (fork failing is very bad) */
    fprintf ( stderr, "Error: initial fork failed: %s\n",
              strerror ( errno ) );
    return -1;
    break;

  default:                     /* we are the parent, so exit */
    exit ( 0 );
    break;
  }
  if ( setsid () < 0 )
    return -1;
  if ( ftruncate ( lockFD, 0 ) < 0 )
    return -1;
  sprintf ( pidStr, "%d\n", ( int ) getpid () );
  write ( lockFD, pidStr, strlen ( pidStr ) );
  *lockFileDesc = lockFD;       /* return lock file descriptor to caller */
  numFiles = sysconf ( _SC_OPEN_MAX );  /* how many file descriptors? */
  for ( i = numFiles - 1; i >= 0; --i ) {       /* close all open files except lock */
    if ( i != lockFD )          /* don't close the lock file! */
      close ( i );
  }
  umask ( 0 );                  /* set this to whatever is appropriate for you */
  stdioFD = open ( "/dev/null", O_RDWR);       /* fd 0 = stdin */
  dup ( stdioFD );              /* fd 1 = stdout */
  dup ( stdioFD );              /* fd 2 = stderr */
  openlog ( logPrefix, LOG_PID | LOG_CONS | LOG_NDELAY | LOG_NOWAIT,
            LOG_LOCAL0 );

  ( void ) setlogmask ( LOG_UPTO ( logLevel ) );        /* set logging level */
  setpgrp ();
  return 0;
}

int ConfigureSignalHandlers ( void )
{
  struct sigaction sighupSA, sigusr1SA, sigtermSA, sigsigio;
  signal ( SIGUSR2, SIG_IGN );
  signal ( SIGPIPE, SIG_IGN );
  signal ( SIGALRM, SIG_IGN );
  signal ( SIGTSTP, SIG_IGN );
  signal ( SIGTTIN, SIG_IGN );
  signal ( SIGTTOU, SIG_IGN );
  signal ( SIGURG, SIG_IGN );
  signal ( SIGXCPU, SIG_IGN );
  signal ( SIGXFSZ, SIG_IGN );
  signal ( SIGVTALRM, SIG_IGN );
  signal ( SIGPROF, SIG_IGN );
  signal ( SIGIO, SIG_IGN );
  signal ( SIGCHLD, SIG_IGN );
  signal ( SIGQUIT, FatalSigHandler );
  signal ( SIGILL, FatalSigHandler );
  signal ( SIGTRAP, FatalSigHandler );
  signal ( SIGABRT, FatalSigHandler );
  signal ( SIGIOT, FatalSigHandler );
  signal ( SIGBUS, FatalSigHandler );
#ifdef SIGEMT                   /* this is not defined under Linux */
  signal ( SIGEMT, FatalSigHandler );
#endif
  signal ( SIGFPE, FatalSigHandler );
  signal ( SIGSEGV, FatalSigHandler );
  signal ( SIGSTKFLT, FatalSigHandler );
  signal ( SIGCONT, FatalSigHandler );
  signal ( SIGPWR, FatalSigHandler );
  signal ( SIGSYS, FatalSigHandler );
  sigtermSA.sa_handler = TermHandler;
  sigemptyset ( &sigtermSA.sa_mask );
  sigtermSA.sa_flags = 0;
  sigaction ( SIGTERM, &sigtermSA, NULL );
  sigusr1SA.sa_handler = Usr1Handler;
  sigemptyset ( &sigusr1SA.sa_mask );
  sigusr1SA.sa_flags = 0;
  sigaction ( SIGUSR1, &sigusr1SA, NULL );

  sigsigio.sa_handler = SIGIOHandler;
  sigemptyset ( &sigsigio.sa_mask );
  sigsigio.sa_flags = SA_SIGINFO;
  sigaction ( SIGIO, &sigsigio, NULL );
  sighupSA.sa_handler = HupHandler;
  sigemptyset ( &sighupSA.sa_mask );
  sighupSA.sa_flags = 0;
  sigaction ( SIGHUP, &sighupSA, NULL );

  return 0;
}

int BindPassiveSocket ( const int portNum, int *const boundSocket )
{
  struct sockaddr_in sin;
  int newsock, optval;
  size_t optlen;
  syslog ( LOG_LOCAL0 | LOG_INFO, "Bind-1" );
  memset ( &sin.sin_zero, 0, 8 );
  sin.sin_port = htons ( portNum );
  sin.sin_family = AF_INET;     /* Usage: AF_XXX here, PF_XXX in socket() */
  sin.sin_addr.s_addr = htonl ( INADDR_ANY );

  if ( ( newsock = socket ( PF_INET, SOCK_STREAM, 0 ) ) < 0 )
    return -1;
  optval = 1;
  optlen = sizeof ( int );
  setsockopt ( newsock, SOL_SOCKET, SO_REUSEADDR, &optval, optlen );
  if ( bind
       ( newsock, ( struct sockaddr * ) &sin,
         sizeof ( struct sockaddr_in ) ) < 0 )
    return -1;
  if ( listen ( newsock, SOMAXCONN ) < 0 )
    return -1;
  fcntl(newsock, F_SETOWN, getpid());
  fcntl(newsock,F_SETFL,FASYNC);
  *boundSocket = newsock;
  return 0;
}

int AcceptConnections ( const int master )
{
  int proceed = 1, slave, retval = 0;
  struct sockaddr_in client;
  socklen_t clilen;
  while ( ( proceed == 1 ) && ( gGracefulShutdown == 0 ) ) {
    clilen = sizeof ( client );
    slave = accept ( master, ( struct sockaddr * ) &client, &clilen );
    if ( slave < 0 ) {          /* accept() failed */
      if ( errno == EINTR )
        continue;
      syslog ( LOG_LOCAL0 | LOG_INFO, "accept() failed: %m\n" );
      proceed = 0;
      retval = -1;
    }
    else {
      retval = ReadLine ( slave );      /* process connection */
      if ( retval ) proceed = 0;
    }
    close ( slave );
  }
  return retval;
}

int ReadLine ( const int sock ){
  int done = 0, retval = 0, fd;
  char buff[1024], recBuff[25];
  size_t bytesSoFar = 0;
  ssize_t readResult;
  gNewConnection = 0;
  do {
#ifdef DEBUG      
    syslog ( LOG_LOCAL0 | LOG_INFO, "recv" );
#endif
    readResult = recv ( sock, buff, 1024, 0 );
    if (readResult == -1 ) {
      retval = 0;
      done = 1;
    } else if (readResult == 0 ) {
      retval = 0;
      done = 1;
    } else if (readResult > 0 ) {
#ifdef DEBUG      
    syslog ( LOG_LOCAL0 | LOG_INFO, "read" );
#endif
      int i;
      for ( i = 0; i < readResult; i++ ) {
        if ( *(buff+i) != 0xb )
          *(buff+i) -= 0x20;
      }
      push ( buff, readResult );
      if ( getNextRec ( recBuff ) ) {
        DecodeRec(recBuff);
      }
#ifdef DEBUG      
      fd = open("/tmp/trackd.log", O_WRONLY | O_APPEND | O_CREAT, 0644 );
      if ( fd == -1 ) {
        syslog ( LOG_LOCAL0 | LOG_INFO, "error open file" );
      } else {
        write ( fd, buff, readResult );
        if ( fd == -1 )
          syslog ( LOG_LOCAL0 | LOG_INFO, "error write file" );
        close ( fd );
      }
#endif
    }
  } while ( !done && !gNewConnection);
  gNewConnection = 0;
  return retval;
}

void FatalSigHandler ( int sig )
{
#ifdef _GNU_SOURCE
  syslog ( LOG_LOCAL0 | LOG_INFO, "caught signal: %s - exiting",
           strsignal ( sig ) );
#else
  syslog ( LOG_LOCAL0 | LOG_INFO, "caught signal: %d - exiting", sig );
#endif

  closelog ();
  TidyUp ();
  _exit ( 0 );
}

void TermHandler ( int sig )
{
  TidyUp ();
  _exit ( 0 );
}

void HupHandler ( int sig )
{
  syslog ( LOG_LOCAL0 | LOG_INFO, "caught SIGHUP" );
  gGracefulShutdown = 1;
  gCaughtHupSignal = 1;
}

void SIGIOHandler ( int sig )
{
  syslog ( LOG_LOCAL0 | LOG_INFO, "caught SIGIO" );
  gNewConnection = 1;
}

void Usr1Handler ( int sig )
{
  syslog ( LOG_LOCAL0 | LOG_INFO, "caught SIGUSR1 - soft shutdown" );
  gGracefulShutdown = 1;
}

void TidyUp ( void )
{
  if ( gLockFileDesc != -1 ) {
    close ( gLockFileDesc );
    unlink ( gLockFilePath );
    gLockFileDesc = -1;
  }

  if ( gMasterSocket != -1 ) {
    close ( gMasterSocket );
    gMasterSocket = -1;
  }
}

void push ( char *buff, int blen ){
  int i;
  for ( i = 0; i < blen; i++ ){
    *(qbuff+epos++) = *(buff+i);
    if ( epos == QLEN ){
      epos = QLEN - 1;
      break;
    }
  }
}

int getNextRec ( char * buff ){
  char sb[1024];
  int nb = 0;
  int res = 0;
  while( epos - nb >= 22 ){
    if ( *(qbuff+nb) != 0xb && *(qbuff+nb+22) == 0xb && *(qbuff+nb+21) == 0 && *(qbuff+nb+20) == 0) {
      memcpy(buff,qbuff+nb,22);
      nb += 23;
      res = 1;
      break;
    }
    nb++;
  }
  memmove(qbuff,qbuff+nb,epos-nb);
  epos = 0;
  return res;
}

void DecodeRec ( char *buff ){
//  char sb[1024];
  int day, month, year, sec, min, hour, gsm_lev, speed, nsput, latitude,lat1,lat2,lat3,longitude,long1,long2,long3;
#ifdef DEBUG      
    syslog ( LOG_LOCAL0 | LOG_INFO, "decode" );
#endif
  day = (int)(*(buff) & 0x3f);
  month = (int)(*(buff+1) & 0x3f);
  year =  (int)*(buff+2);
  sec = (int)(*(buff+5) & 0x7f);
  min = (int)(*(buff+4) & 0x7f);
  hour = (int)(*(buff+3) & 0x3f);
  gsm_lev = (int) (((*(buff+5) & 0x80)>>5)|((*(buff+4) & 0xc0)>>6));
  speed =  (int)*(buff+15);
  nsput =  (int)*(buff+16);
  lat1 = (int)*(buff+6);
  lat2 = (int)*(buff+7);
  lat3 = ((int)*(buff+8)*100+(int)*(buff+9));
  latitude = lat1*1000000+lat2*10000+lat3;
  long1 = (int)*(buff+10)*10+(int)*(buff+11);
  long2 = (int)*(buff+12);
  long3 = ((int)*(buff+13)*100+(int)(*buff+14));
  longitude = long1*1000000+long2*10000+long3;
//  sprintf(sb,"%d %d %d - %d %d %d - %d %d",lat1,lat2,lat3,long1,long2,long3,latitude,longitude);
//  syslog ( LOG_LOCAL0 | LOG_INFO, sb );
  SaveDataToDB( day, month, year, sec, min, hour, gsm_lev, speed, nsput, latitude, longitude);
}
