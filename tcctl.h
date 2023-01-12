#ifndef _TCCTL_H_
#define _TCCTL_H_

#define CONF_PATH "/etc/tcctl/tcctl.conf"
#define TEMP_PATH "temp_dummy"
#define UNSCK_PATH "af_un_tcctl.serv"
#define UNSCK_PATH_MAX_LEN 64

enum tcctl_phase;
struct tcctl_stat;
struct tcctl_conf;

#define CELSIUS(T) (T*1000)

#define LOW_TEMP_DEFAULT 25
#define TRIG_TEMP_DEFAULT 45
#define HYST_DEC_TEMP_DEFAULT 10

#define UPDATE_DELAY_DEFAULT 0
#define OUTPUT_PIN_DEFAULT -1

#endif //_TCCTL_H_
