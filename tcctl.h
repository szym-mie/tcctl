#ifndef _TCCTL_H_
#define _TCCTL_H_

#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fnctl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>

#include "bcm2835.h"


#define CONF_PATH "/etc/tcctl/tcctl.conf"
#define TEMP_PATH "/sys/class/thermal/thermal_zone0/temp"
#define UNSCK_PATH "af_un_tcctl.serv"
#define UNSCK_PATH_MAX_LEN 64


enum tcctl_phase
{
	LOW_TEMP,  // temperature below threshold (always off)
	IDLE,      // ready for cooling
	RUN,       // cooling
	HIGH_TEMP, // temperature above threshold (always on)
	OVRD_IDLE, // start and stay idle
	OVRD_RUN,  // start and stay running
	FAIL       // failure
};

struct tcctl_stat
{
	unsigned int last_temp;
	unsigned int low_temp;
	unsigned int trig_temp;

	enum tcctl_phase phase;
};

union tcctl_conf_field
{
	unsigned int uint;
	int boolean;
};

struct tcctl_conf_entry
{
	const char *name;
	union tcctl_conf_field *field;
	int (*read_fn)(union tcctl_conf_field *field, const char *val);
};

struct tcctl_conf
{
	union tcctl_conf_field low_temp;       // stop below that temperature
	union tcctl_conf_field trig_temp;      // start cooling when reached
	union tcctl_conf_field hyst_dec_temp;  // minimal temp drop to stop

	union tcctl_conf_field update_delay;   // how much time to sleep min	
	union tcctl_conf_field output_pin;     // output pin to the switch

	union tcctl_conf_field stay_on;    // fan always on
	union tcctl_conf_field stop;       // stop the temperature control
	union tcctl_conf_field pin_invert; // invert pin (for p-mosfets)
};

enum tcctl_rc_cmd
{
	// client commands
	STAT, // get status     | p1 <- parameter id  | p2 <- n/a
	OVRD, // force run/hold | p1 <- on/off        | p2 <- n/a
	AUTO, // automatic mode | p1 <- n/a           | p2 <- n/a
	TRIG, // trigger temps  | p1 <- low temp      | p2 <- trigger temp
	CONF, // reload conf    | p1 <- n/a           | p2 <- n/a
	KILL, // kill service   | p1 <- n/a           | p2 <- n/a
	// service responses
	INFO, // return status  | p1 <- parameter id  | p2 <- return value
	CERR, // conf error     | p1 <- conf entry id | p2 <- conf line
};

union tcctl_rc_param
{
	unsigned int uint;
	int sint;
	int boolean;
};

struct tcctl_rc_msg
{
	enum tcctl_rc_cmd cmd;
	union tcctl_rc_param p1, p2;
};

struct tcctl_rc_addr
{
	struct sockaddr_un *addr;
	socklen_t len;
};


void tcctl_setup_sig(void);
void tcctl_kill_sig(int sig);

int tcctl_init_fds(void);

int tcctl_loop(void);
void tcctl_update(void);

int tcctl_gpio_init(void);
void tcctl_gpio_init_pin(unsigned int);
void tcctl_gpio_write(int);

size_t tcctl_rc_addr_len(const char *);
void tcctl_rc_addr_set(struct tcctl_rc_addr *, const char *);
int tcctl_rc_init(const char *);
int tcctl_rc_recv_msg(void);
int tcctl_rc_handle_msg(struct tcctl_rc_msg *, struct tcctl_rc_addr *);
int tcctl_rc_send_msg(struct tcctl_rc_msg *, struct tcctl_rc_addr *);

unsigned int tcctl_stat_get(unsigned int);
int tcctl_temp_read(unsigned int *);

void tcctl_conf_reset(struct tcctl_conf *);
void tcctl_conf_copy(struct tcctl_conf *, struct tcctl_conf *);
int tcctl_conf_read(void);
const char * tcctl_conf_read_next_line(const char *);

int tcctl_get_uint(union tcctl_conf_field *, const char *);
int tcctl_get_boolean(union tcctl_conf_field *, const char *);

int streq(const char *, const char *);
size_t strlen(const char *);

#define LOW_TEMP_DEFAULT 29
#define TRIG_TEMP_DEFAULT 33
#define HYST_DEC_TEMP_DEFAULT 2

#define UPDATE_DELAY_DEFAULT 0
#define OUTPUT_PIN_DEFAULT -1

#endif //_TCCTL_H_
