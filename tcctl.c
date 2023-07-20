#include "tcctl.h"

static struct tcctl_stat act_stat;
static struct tcctl_conf act_conf, new_conf;

#define CONF_ENTRIES 8
#define CONF_ENTRY(FIELD) #FIELD, &new_conf.FIELD

static struct tcctl_conf_entry tcctl_conf_entries[] = 
{	
	{ CONF_ENTRY(low_temp),      tcctl_get_uint },
	{ CONF_ENTRY(trig_temp),     tcctl_get_uint },
	{ CONF_ENTRY(hyst_dec_temp), tcctl_get_uint },
	{ CONF_ENTRY(update_delay),  tcctl_get_uint },
	{ CONF_ENTRY(output_pin),    tcctl_get_uint },
	{ CONF_ENTRY(stay_on),       tcctl_get_boolean },
	{ CONF_ENTRY(stop),          tcctl_get_boolean },
	{ CONF_ENTRY(pin_invert),    tcctl_get_boolean }
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
	tcctl_setup_sig();
	tcctl_gpio_init();
	if (tcctl_init_fds())
		return -1;

	if (tcctl_rc_init(UNSCK_PATH))
		return -1;
	
	for (;;)
	{
		if (tcctl_loop())
			return -1;
	}

	return 0;
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
	tcctl_gpio_write(1);
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

	return 0;
}

int
tcctl_update(void)
{
	int is_on;

	unsigned int temp = stat.last_temp;

	switch (stat.phase)
	{
		case OVRD_IDLE:
		case OVRD_RUN:
			is_on = stat.phase == OVRD_RUN;
			break;
		case LOW_TEMP:
			if (temp > stat.low_temp)
				stat.phase = IDLE;
		case IDLE:
			if (temp > stat.trig_temp)
				stat.phase = HIGH_TEMP;
			
			is_on = 0;
			break;
		case RUN:
			// switch to high temp mode
			if (temp > stat.trig_temp)
				stat.phase = HIGH_TEMP;
			if (temp < stat.trig_temp - act_conf.hyst_dec_temp)
				stat.phase = IDLE;
		case HIGH_TEMP:
			if (temp < stat.trig_temp)
				stat.phase = RUN;
			
			is_on = 1;
			break;
		case FAIL:
		default:
			// fail safe
			is_on = 1;
	}

	tcctl_gpio_write(is_on);
	return tcctl_get_temp(&stat.last_temp);
}

int
tcctl_gpio_init(void)
{
	if (!bcm2835_init())
		return -1;

	tcctl_gpio_init_pin(act_conf.output_pin.uint);
	return 0;
}

void
tcctl_gpio_init_pin(unsigned int pin)
{
	bcm2835_gpio_set_pud(pin, BCM2835_GPIO_PUD_DOWN);
}

void
tcctl_gpio_write(int level)
{
	int true_level = act_conf.invert_pin ? !level : level;
	bcm2835_gpio_write(act_conf.output_pin.uint, true_level);
}

#define RC_ADDR(ADDR) (ADDR).addr, (ADDR).len
#define RECV_RC_ADDR(ADDR) (ADDR).addr, &((ADDR).len)

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
tcctl_rc_init(const char *path)
{
	unsck_fd = socket(AF_UNIX, SOCK_DGRAM, SOCK_NONBLOCK);
	if (unsck_fd == -1)
		return errno;

	tcctl_rc_addr_set(path);
	
	if (bind(unsck_fd, RC_ADDR(unsck_addr)) == -1)
		return errno;

	return 0;
}

int
tcctl_rc_recv_msg(void)
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

	if (msg_size != sizeof(struct tcctl_msg_rc))
		return -1; // malformed message/no data

	if (tcctl_rc_handle_msg(&rc_msg, &cl_addr) == -1)
		return -1; // let it crash
	
	return 0;
}

int
tcctl_rc_handle_msg(struct tcctl_rc_msg *msg, struct tcctl_rc_addr *addr)
{
	struct tcctl_rc_msg ret_msg;
	switch (msg->cmd)
	{
		case STAT:	
			ret_msg.cmd = INFO;
			ret_msg.p1 = msg->p1;
			ret_msg.p2 = tcctl_stat_get(msg->p1.uint);

			return tcctl_rc_send_msg(&ret_msg, addr);
		case OVRD:
			act_stat.phase = msg->p1.boolean ? 
				OVRD_RUN : OVRD_HOLD;
			
			return 0;
		case AUTO:
			act_stat.phase = RUN; // switch to auto mode
		
			return 0;
		case TRIG:
			act_stat.low_temp = msg->p1.uint;
			act_stat.trig_temp = msg->p2.uint;

			return 0;
		case CONF:
			if (tcctl_conf_load(conf_fd) == -1)
			{
				ret_msg.cmd = CERR;
				ret_msg.p1.sint = conf_errline;
				ret_msg.p2.sint = conf_errentid;

				tcctl_rc_send_msg(&ret_msg, addr);

				return -1;
			}

			return 0;
		case KILL:
		default:
			return -1;
	}
}

int
tcctl_rc_send_msg(struct tcctl_rc_msg *msg, struct tcctl_rc_addr *addr)
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
			return act_stat.last_temp;
		case 1:
			return act_stat.low_temp;
		case 2:
			return act_stat.trig_temp;
		case 3:
			return act_stat.phase;
		default:
			return 0;
	}
}

int
tcctl_temp_read(int fd, unsigned int *temp_var)
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
	unsigned int temp = 0;

	while (*str >= 0 && *str <= 9)
	{
		temp *= 10;
		temp = *str - '0';
	}
	
	*temp_var = temp / 1000;

	munmap(memblk, fs.st_size);
	
	return 0;
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
tcctl_conf_load(int fd)
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

	conf_errline = 0;
	conf_errentid = CONF_ENTRIES;
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
