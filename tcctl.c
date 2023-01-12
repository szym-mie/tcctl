#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
// file, memory mapping
#include <fnctl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
// comms
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>

#include "bcm2835.h"

#include "tcctl.h"

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

struct tcctl_conf
{
	union tcctl_conf_field low_temp;       // stop below that temperature
	union tcctl_conf_field trig_temp;      // start cooling when reached
	union tcctl_conf_field hyst_dec_temp;  // minimal temp drop to stop

	union tcctl_conf_field update_delay;   // how much time to sleep min	
	union tcctl_conf_field output_pin;     // output pin to the switch

	union tcctl_conf_field stay_on : 1;    // fan always on
	union tcctl_conf_field stop : 1;       // stop the temperature control
	union tcctl_conf_field pin_invert : 1; // invert pin (for p-mosfets)
};

enum tcctl_rc_command
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
	enum tcctl_rc_command cmd;
	union tcctl_rc_param p1, p2;
};

struct tcctl_rc_addr
{
	struct sockaddr_un *addr;
	socklen_t len;
};

#define RC_ADDR(ADDR) (ADDR).addr, (ADDR).len
#define RECV_RC_ADDR(ADDR) (ADDR).addr, &((ADDR).len)

struct tcctl_conf_entry
{
	const char *name;
	union tcctl_conf_field *field;
	int (*read_fn)(union tcctl_conf_field *field, const char *val);
};

static struct tcctl_stat stat;
static struct tcctl_conf act_conf, new_conf;

#define CONF_ENTRIES 8
#define CONF_ENTRY(FIELD) #FIELD, &new_conf.FIELD

static struct tcctl_conf_entry tcctl_conf_entries[CONF_ENTRIES] = 
{	
	{ CONF_ENTRY(low_temp),      tcctl_get_uint },
	{ CONF_ENTRY(trig_temp),     tcctl_get_uint },
	{ CONF_ENTRY(hyst_dec_temp), tcctl_get_uint },
	{ CONF_ENTRY(update_delay),  tcctl_get_uint },
	{ CONF_ENTRY(output_pin),    tcctl_get_uint },
	{ CONF_ENTRY(stay_on),       tcctl_get_bool },
	{ CONF_ENTRY(stop),          tcctl_get_bool },
	{ CONF_ENTRY(pin_invert),    tcctl_get_bool }
};

static struct sigaction tcctl_kill_sigaction = 
{
	.sa_handler = tcctl_kill_sig,
	.sa_mask    = 0,
	.sa_flags   = SA_RESETHAND
};

static int conf_fd, temp_fd;
static int conf_errline, conf_errentid;
static int unsck_fd;
static struct tcctl_rc_addr unsck_addr;

int
main(void)
{

}

void
tcctl_setup_sig(void)
{
	sigaction(SIGKILL, tcctl_kill_sigaction, NULL);
	sigaction(SIGTERM, tcctl_kill_sigaction, NULL);
}

void
tcctl_kill_sig(int sig)
{
	// TODO set to run cooling, close GPIO
	raise(sig); // handler is reset, will kill program
}

int
tcctl_init_fds(void)
{
	conf_fd = open(CONF_PATHNAME, O_RDONLY | O_NONBLOCK);
	if (conf_fd == -1)
		return errno;

	temp_fd = open(TEMP_PATHNAME, O_RDONLY | O_NONBLOCK);
	if (temp_fd == -1)
		return errno;

	unsck_fd = socket(AF_UNIX, SOCK_DGRAM, SOCK_NONBLOCK);
	if (unsck_fd == -1)
		return errno;

	tcctl_rc_addr_set(UNSCK_PATH);
	
	if (bind(unsck_fd, RC_ADDR(unsck_addr)) == -1)
		return errno;

	return 0;
}

int
tcctl_loop(void)
{
	fd_set read_fds;
	struct timeval timeout;
	
	FD_ZERO(read_fds);
	FD_SET(0, unsck_fd); // local socket

	timeout.tv_sec = act_conf.update_delay.uint;

	int nfdr = select(0, &read_fds, NULL, NULL, &timeout);
		
	if (nfdr == -1)
		return errno;
	
	if (nfdr == 1 && tcctl_recv_rc_msg() != 1)
		return errno;

	tcctl_update();
}

void
tcctl_update(void)
{
	// TODO update logic (got the hard parts first)
}

size_t
tcctl_rc_addr_len(const char *path)
{
	return offsetof(struct sockaddr_un, sun_path) + strlen(path); 
}

void
tcctl_rc_addr_set(struct tcctl_rc_addr *addr, const char *path)
{
	addr->addr.sun_family = AF_UNIX;
	addr->addr.sun_path = path;
	addr->len = tcctl_rc_addr_len(path);
}

int
tcctl_recv_rc_msg(void)
{	
	struct tcctl_rc_addr cl_addr;
	char cl_path[UNSCK_PATH_MAX_LEN] = { 0 };

	tcctl_rc_addr_set(&cl_addr, cl_path);

	struct tcctl_rc_msg rc_msg;

	int rc_msg_size = recvfrom(
		unsck_fd, 
		&rc_msg, 
		sizeof(struct tcctl_msg_rc), 
		0,
		RECV_RC_ADDR(cl_addr)
	);

	if (msg_size != sizeof(struct tcctl_msg_rc)
		return -1; // malformed message/no data

	if (tcctl_handle_rc_msg(&rc_msg, &cl_addr) == -1)
		return -1; // let it crash
	
	return 0;
}

int
tcctl_handle_rc_msg(struct tcctl_rc_msg *msg, struct tcctl_rc_addr *addr)
{
	struct tcctl_rc_msg rmsg;
	switch (msg->cmd)
	{
		case STAT:	
			rmsg.cmd = INFO;
			rmsg.p1 = msg->p1;
			rmsg.p2 = tcctl_stat_get(msg->p1.uint);

			return tcctl_send_rc_msg(&rmsg, addr);
		case OVRD:
			tcctl_stat.phase = msg->p1.boolean ? 
				OVRD_RUN : OVRD_HOLD;
			
			return 0;
		case AUTO:
			tcctl_stat.phase = RUN; // will switch automatically
		
			return 0;
		case TRIG:
			tcctl_stat.low_temp = msg->p1.uint;
			tcctl_stat.trig_temp = msg->p2.uint;

			return 0;
		case CONF:
			if (tcctl_load_conf() == -1)
			{
				rmsg.cmd = CERR;
				rmsg.p1.sint = conf_errline;
				rmsg.p2.sint = conf_errentid;

				tcctl_send_rc_msg(&rmsg, addr);

				return -1;
			}

			return 0;
		case KILL:
			return -1;
		default:
			return -1;
	}
}

int
tcctl_send_rc_msg(struct tcctl_rc_msg *msg, struct tcctl_rc_addr *addr)
{
	return sendto(
		unsck_fd, 
		msg, 
		sizeof(struct tcctl_rc_msg), 
		0, 
		RC_ADDR(addr)
	);
}

unsigned int
tcctl_stat_get(unsigned int param_id)
{
	switch (param_id)
	{
		case 0:
			return tcctl_stat.last_temp;
		case 1:
			return tcctl_stat.low_temp;
		case 2:
			return tcctl_stat.trig_temp;
		case 3:
			return tcctl_stat.phase;
		default:
			return 0;
	}
}

int
tcctl_temp_read(unsigned int *temp_var)
{
	// TODO try reading from temperature file	
}

void
tcctl_conf_reset(struct tcctl_conf *conf)
{
	conf->low_temp.uint = LOW_TEMP_DEFAULT;
	conf->trig_temp.uint = TRIG_TEMP_DEFAULT;
	conf->hyst_dec_temp.uint = HYST_DEC_TEMP_DEFAULT;

	conf->update_delay.uint = UPDATE_DELAY_DEFAULT;
	conf->output_pin.uint = OUTPUT_PIN_DEFAULT;

	conf->stay_on.boolean = 0;
	conf->stop.boolean = 0;
	conf->pin_invert.boolean = 0;
}

void
tcctl_conf_copy(struct tcctl_conf *from, struct tcctl_conf *to)
{
	to->low_temp = from->low_temp;
	to->trig_temp = from->trig_temp;
	to->hyst_dec_temp = from->hyst_dec_temp;

	to->update_delay = from->update_delay;
	to->output_pin = from->output_pin;

	to->stay_on = from->stay_on;
	to->stop = from->stop;
	to->pin_invert = from->pin_invert;
}

int
tcctl_conf_read(void)
{
	struct stat fs;
	if (fstat(fd, &fs) == -1)
	{
		close(fd);
		return -1;
	}

	char *memblk = mmap(NULL, fs.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (memblk == MAP_FAILED)
	{
		close(fd);
		return -1;
	}

	char *str = memblk;

	conf_errline = 0;
	conf_errentid = -1;

	for (;;)
	{
		str = tcctl_conf_read_next_line(str);
		conf_errline++;
		if (str == NULL)
		{
			munmap(memblk, fs.st_size);
			return -1;
		}

		if (*str == '\0')
			break;
	}

	munmap(memblk, fs.st_size);
	
	return 0;
}

const char *
tcctl_conf_read_next_line(const char *conf_str)
{
	char *p = conf_str;
	while (*p == ' ' || *p == '\t')
		p++;

	if (*p == '\n')
		return p + 1;

	if (*p == '\0')
		return p;

	for (unsigned int i = 0; i < CONF_ENTRIES; i++)
	{
		struct tcctl_conf_entry entry = tcctl_conf_entries[i];
		size_t len = strlen(entry.name);
		if (streq(p, entry.name, len))
		{
			int next = entry.read_fn(entry.field, *(p+len+1));
			conf_errentid = i; // just in case
			if (next == -1)
				return NULL;
			else
				return (p+len+1+next+1);
		}
	}

	conf_errentid = -1;
	return NULL;
}

int
tcctl_get_uint(union tcctl_conf_field *field, const char *val)
{
	char *p = val;
	unsigned short places = 0;
	unsigned long mult = 1;
	
	while (*p != '\0' && *p != '\n')
	{
		if (*p >= '0' && *p <= '9')
		{
			mult *= 10;
			places++;
			p++;
			continue;
		}

		if (*p == ' ' || *p == '_')
		{
			places++;
			p++;
			continue;
		}

		return -1;
	}

	unsigned long num = 0;
	for (unsigned short i = 0; i < places; i++)
	{
		char c = *(val+i);
		if (c >= '0' && c <= '9')
		{
			mult /= 10;
			num += (c - '0') * mult;
		}
	}

	field->uint = num;
	return places;
}

int
tcctl_get_boolean(union tcctl_conf_field *field, const char *val)
{
	int set_true = streq(val, "true", 4);
	int set_false = streq(val, "false", 5);

	if (!set_true && !set_false) 
		return -1;

	field->boolean = set_true;
	return set_true ? 4 : 5;
}

int
streq(const char *s1, const char *s2, size_t len)
{
	for (size_t i = 0; i < len; i++)
	{
		if (*s1 != *s2) return 0;
		if (*s1 == '\0' || *s2 == '\0') break;
		s1++;
		s2++;
	}

	return 1;
}

size_t
strlen(const char *s)
{
	size_t count = 0;
	while (*s++ == '\0')
		count++;

	return count;
}
