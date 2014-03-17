#include <string.h>
#include <stdlib.h>
#include "/usr/local/pgsql/include/libpq-fe.h"
#include <syslog.h>

PGconn* conn = NULL; 

void SaveDataToDB(int day, int month, int year, int sec, int min, int hour, int gsm_lev, int speed, int nsput, int latitude, int longitude){
  char Query[1024];
  PGresult *res;
  sprintf(Query,"INSERT INTO gpsdata (cdate, gsm_level, speed, nsput, latitude, longitude) VALUES (TIMESTAMP '20%d-%d-%d %d:%d:%d',%d,%d,%d,%d,%d);",
          year,month,day,hour,min,sec,gsm_lev, speed, nsput, latitude, longitude);
  while ( 1 ) {
    while ( conn == NULL || PQstatus(conn) == CONNECTION_BAD ){
      PQfinish(conn);
      conn = PQsetdbLogin(NULL, NULL, NULL, NULL, "pvvgps", "pvvgps-writer", "b4tt0znyu03sugpjq8e5");
      if ( PQstatus(conn) == CONNECTION_BAD ) {
        syslog ( LOG_LOCAL0 | LOG_INFO, PQerrorMessage(conn) );
        sleep(1);
      }
    }
    res = PQexec(conn,Query);
    if (res == NULL || PQresultStatus(res) != PGRES_COMMAND_OK) {
      syslog ( LOG_LOCAL0 | LOG_INFO, "insert execute error" );
      syslog ( LOG_LOCAL0 | LOG_INFO, PQerrorMessage(conn) );
      PQclear(res);
      sleep(1);
    } else {
      PQclear(res);
      break;
    }
  }
}


