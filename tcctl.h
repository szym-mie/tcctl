#ifndef _TCCTL_H_
#define _TCCTL_H_

#define CONF_PATH "/etc/tcctl/tcctl.conf"
#define TEMP_PATH "/sys/class/thermal/thermal_zone0/temp"
#define UNSCK_PATH "af_un_tcctl.serv"
#define UNSCK_PATH_MAX_LEN 64

struct tcctl_stat;

union tcctl_conf_field;
struct tcctl_conf_entry;
struct tcctl_conf;

union tcctl_rc_param;
struct tcctl_rc_msg;

struct tcctl_rc_addr;


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

#define CELSIUS(T) (T*1000)

#define LOW_TEMP_DEFAULT 25
#define TRIG_TEMP_DEFAULT 45
#define HYST_DEC_TEMP_DEFAULT 10

#define UPDATE_DELAY_DEFAULT 0
#define OUTPUT_PIN_DEFAULT -1

#endif //_TCCTL_H_
