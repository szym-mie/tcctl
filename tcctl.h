#ifndef _TCCTL_H_
#define _TCCTL_H_

#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/select.h>

#include <linux/gpio.h>

#define LOG_PATH "./tcctl.log"
#define LOG_MSG_BUF_LEN 16
#define CONF_PATH "/etc/tcctl/tcctl.conf"
#define TEMP_PATH "/sys/class/thermal/thermal_zone0/temp"
#define TEMP_BUF_MAX_LEN 64
#define UNSCK_PATH "af_un_tcctl.serv"
#define UNSCK_SUN_ADDR_LEN 108
#define UNSCK_PATH_MAX_LEN 64

#define ENTRY_NAME_MAX_LEN 64
#define ENTRY_LINE_MAX_LEN 16
#define ARG_SYM_MAX_LEN 32
#define MSG_MAX_LEN 512
#define TIME_BUF_LEN 16
#define TEMP_BUF_LEN 8

#define GPIO_PATH_LEN 40
#define GPIO_BUF_LEN 4

#define ZERO_STR { '\0' }

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
	union tcctl_conf_field low_temp;       // never run below that temperature
	union tcctl_conf_field trig_temp;      // start cooling when reached
	union tcctl_conf_field hyst_dec_temp;  // minimal temp drop to stop

	union tcctl_conf_field update_delay;   // time between updates	
	union tcctl_conf_field output_pin;     // output pin to the switch

	union tcctl_conf_field stay_on;    	// fan on after exit
	union tcctl_conf_field stop;       	// stop the temperature control
	union tcctl_conf_field pin_invert; 	// invert pin (for p-mosfets)
};

enum tcctl_arg_post
{
	POST_NORM,
	POST_EXIT
};

struct tcctl_arg
{
	char *sym;
	char *params;
	char *desc;
	int (*parse)(int argr, char *argv[]);
	enum tcctl_arg_post post;
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

void tcctl_pre_init(void);

int tcctl_arg_help(int, char *[]);
int tcctl_arg_conf(int, char *[]);
int tcctl_arg_log(int, char *[]);
int tcctl_args_parse(int, char *[]);

void tcctl_setup_sig(void);
void tcctl_kill_sig(int);

int tcctl_fd_init(void);

int tcctl_loop(void);
int tcctl_update(void);

int tcctl_gpio_init(void);
int tcctl_gpio_update_conf(unsigned int);
int tcctl_gpio_write(int);

size_t tcctl_rc_addr_len(const char *);
void tcctl_rc_addr_set(struct tcctl_rc_addr *, const char *);
int tcctl_rc_init(const char *);
int tcctl_rc_end(void);
int tcctl_rc_recv_msg(void);
int tcctl_rc_handle_msg(struct tcctl_rc_msg *, struct tcctl_rc_addr *);
int tcctl_rc_send_msg(struct tcctl_rc_msg *, struct tcctl_rc_addr *);

unsigned int tcctl_stat_get(unsigned int);
void tcctl_stat_update(struct tcctl_stat *, struct tcctl_conf *);
int tcctl_temp_read(int, unsigned int *);

void tcctl_conf_reset(struct tcctl_conf *);
void tcctl_conf_apply(struct tcctl_conf *, struct tcctl_conf *);
void tcctl_conf_log_error(const char *msg, const char *entry);
int tcctl_conf_load(int);
const char * tcctl_conf_read_next_line(const char *);

int tcctl_get_uint(union tcctl_conf_field *, const char *);
int tcctl_get_boolean(union tcctl_conf_field *, const char *);

void tcctl_log_writeln(const char **, size_t);
void tcctl_log_info(const char *, const char *, const char *, int);
void tcctl_log_error(const char *, const char *,  const char *, const char *);
void tcctl_stdout_write(const char *);

int str_eq(const char *, const char *, size_t);
size_t str_copy(const char *, char *, size_t);
size_t str_set(char, char *, size_t);
size_t str_len(const char *, size_t);
size_t str_join(char *, char *, size_t);

int uint_read(unsigned int *, const char *);
int uint_write(unsigned int, char *);
int uint_write_pad(unsigned int val, char *str, size_t len);
int boolean_read(int *, const char *);
int boolean_write(int, char *);

struct gpio
{
	char *path;
	int chip_fd;
	struct gpiochip_info info;
};

enum gpio_pull
{
	GPIO_NOPULL,
	GPIO_PULLDOWN,
	GPIO_PULLUP
};

enum gpio_val
{
	GPIO_LOW  = 0,
	GPIO_HIGH = 1
};

struct gpio_pin
{
	unsigned int pin;
	enum gpio_pull pull;
	struct gpiohandle_request hreq;
};

int gpio_open(struct gpio *gpio);
int gpio_close(struct gpio *gpio);
int gpio_pin(struct gpio *gpio, struct gpio_pin *pin);
int gpio_write(struct gpio *gpio, struct gpio_pin *pin, enum gpio_val val);

int time_write(char *);

const char *errno_msg(int);

#define LOW_TEMP_DEFAULT 29
#define TRIG_TEMP_DEFAULT 33
#define HYST_DEC_TEMP_DEFAULT 2

#define UPDATE_DELAY_DEFAULT 1
#define OUTPUT_PIN_DEFAULT -1

#endif//_TCCTL_H_
