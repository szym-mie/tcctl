#include "tcctl.h"

static struct tcctl_stat run_stat;
static struct tcctl_conf run_conf, new_conf;

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

#define LSTR(V) _LSTR(V)
#define _LSTR(V) #V
#define LOG_INFO(MSG, VAL) tcctl_log_info(MSG, VAL, __FUNCTION__, 0)
#define LOG_WARN(MSG, VAL) tcctl_log_info(MSG, VAL, __FUNCTION__, 1)
#define LOG_ERROR(MSG, VAL) tcctl_log_error(MSG, VAL, __FUNCTION__, LSTR(__LINE__))

static int log_fd, conf_fd, temp_fd;
static int conf_errline, conf_errentid;
static int unsck_fd;
static struct sockaddr_un unsck_sun_addr;
static struct tcctl_rc_addr unsck_addr; 

int
main(void)
{
	tcctl_setup_sig();
	if (!tcctl_fd_init())
		return 1;

	tcctl_conf_reset(&new_conf);
	if (!tcctl_conf_load(conf_fd))
		return 2;
	
	tcctl_conf_apply(&new_conf, &run_conf);

	if (!tcctl_gpio_init())
		return 3;

	if (!tcctl_rc_init(UNSCK_PATH))
		return 4;
	
	for (;;)
	{
		if (!tcctl_loop())
			return 0;
	}
}

void
tcctl_setup_sig(void)
{
	sigaction(SIGTERM, &tcctl_kill_sigaction, NULL);
	sigaction(SIGINT, &tcctl_kill_sigaction, NULL);
}

void
tcctl_kill_sig(int sig)
{
	LOG_WARN("received exit signal", NULL);
	LOG_INFO("fan stays: ", run_conf.stay_on.boolean ? "on" : "off");
	tcctl_gpio_write(run_conf.stay_on.boolean);
	LOG_INFO("shutdown remote ctl", NULL);
	tcctl_rc_end();
	char temp_buf[4] = { '\0' };
	uint_write_pad(run_stat.last_temp, temp_buf, 3);
	LOG_INFO("current temperature: ", temp_buf);
	LOG_INFO("exit", NULL);
	raise(sig); // handler is reset, will kill program
}

int
tcctl_fd_init(void)
{
	unlink(LOG_PATH);
	log_fd = open(LOG_PATH, O_WRONLY | O_CREAT, S_IRWXU | S_IRGRP | S_IROTH);
	if (log_fd == -1)
	{
		// can't log
		return 0;
	}
	LOG_INFO("tcctl start", NULL);

	LOG_INFO("conf path: ", CONF_PATH);
	conf_fd = open(CONF_PATH, O_RDONLY | O_NONBLOCK);
	if (conf_fd == -1)
	{
		LOG_ERROR("could not open conf file: ", errno_msg(errno));
		return 0;
	}

	LOG_INFO("temp sensor path: ", TEMP_PATH);
	temp_fd = open(TEMP_PATH, O_RDONLY | O_NONBLOCK);
	if (temp_fd == -1)
	{
		LOG_ERROR("could not open sensor: ", errno_msg(errno));
		return 0;
	}

	LOG_INFO("fds ok", NULL);
	return 1;
}

int
tcctl_loop(void)
{
	fd_set read_fds;
	struct timeval timeout;

	tcctl_conf_apply(&new_conf, &run_conf);
	tcctl_stat_update(&run_stat, &run_conf);

	FD_ZERO(&read_fds);
	FD_SET(unsck_fd, &read_fds); // local socket

	timeout.tv_sec = run_conf.update_delay.uint;
	timeout.tv_usec = 0;

	int nfdr = select(unsck_fd + 1, &read_fds, NULL, NULL, &timeout);
		
	if (nfdr == -1)
	{
		LOG_ERROR("select failed: ", errno_msg(errno));
		return 0;
	}

	if (nfdr == 1 && !tcctl_rc_recv_msg())
	{
		LOG_WARN("loop end", NULL);
		return 0;
	}

	return tcctl_update();
}

int
tcctl_update(void)
{
	int is_on;
	unsigned int temp = run_stat.last_temp;

	switch (run_stat.phase)
	{
		case OVRD_IDLE:
		case OVRD_RUN:
			is_on = run_stat.phase == OVRD_RUN;
			break;
		case LOW_TEMP:
			if (temp >= run_stat.low_temp)
				run_stat.phase = IDLE;
		case IDLE:
			if (temp >= run_stat.trig_temp)
				run_stat.phase = HIGH_TEMP;

			is_on = 0;
			break;
		case HIGH_TEMP:
			if (temp < run_stat.trig_temp)
				run_stat.phase = RUN;
		case RUN:
			// switch to high temp mode
			if (temp >= run_stat.trig_temp)
				run_stat.phase = HIGH_TEMP;
			if (temp <= run_stat.trig_temp - run_conf.hyst_dec_temp.uint)
				run_stat.phase = LOW_TEMP;
			
			is_on = 1;
			break;
		case FAIL:
		default:
			// fail safe
			is_on = 1;
	}

	tcctl_gpio_write(is_on);
	return tcctl_temp_read(temp_fd, &run_stat.last_temp);
}

int
tcctl_gpio_init(void)
{
	if (!bcm2835_init())
	{
		LOG_ERROR("could not init gpio", NULL);	
		return 0;
	}

	tcctl_gpio_init_pin(run_conf.output_pin.uint);
	LOG_INFO("gpio ok", NULL);
	return 1;
}

void
tcctl_gpio_init_pin(unsigned int pin)
{
	bcm2835_gpio_fsel(pin, BCM2835_GPIO_FSEL_OUTP);
	bcm2835_gpio_set_pud(pin, BCM2835_GPIO_PUD_DOWN);
}

void
tcctl_gpio_write(int level)
{
	int true_level = run_conf.pin_invert.boolean ? !level : level;
	bcm2835_gpio_write(run_conf.output_pin.uint, true_level);
}

#define RC_ADDR(ADDR) (struct sockaddr *)(ADDR).addr, (ADDR).len
#define RECV_RC_ADDR(ADDR) (struct sockaddr *)(ADDR).addr, &((ADDR).len)

size_t
tcctl_rc_addr_len(const char *path)
{
	return offsetof(struct sockaddr_un, sun_path) + 
		str_len(path, UNSCK_SUN_ADDR_LEN) + 1; 
}

void
tcctl_rc_addr_set(struct tcctl_rc_addr *addr, const char *path)
{
	addr->addr->sun_family = AF_UNIX;
	str_copy(path, addr->addr->sun_path, UNSCK_SUN_ADDR_LEN);
	addr->len = tcctl_rc_addr_len(path);
}

int
tcctl_rc_init(const char *path)
{
	unsck_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (unsck_fd == -1)
	{
		LOG_ERROR("could not get af_unix socket: ", errno_msg(errno));
		return 0;
	}

	unsck_addr.addr = &unsck_sun_addr;	
	tcctl_rc_addr_set(&unsck_addr, path);
	
	LOG_INFO("bind address: ", path);
	unlink(path);
	if (bind(unsck_fd, RC_ADDR(unsck_addr)) == -1)
	{
		LOG_ERROR("could not bind address: ", errno_msg(errno));
		return 0;
	}

	LOG_INFO("remote ctl ok", NULL);
	return 1;
}

int
tcctl_rc_end(void)
{
	const char *path = unsck_addr.addr->sun_path;
	LOG_INFO("unlink socket: ", path);
	if (unlink(path) == -1)
	{
		LOG_ERROR("could not unlink socket: ", errno_msg(errno));
		return 0;
	}

	return 1;
}

int
tcctl_rc_recv_msg(void)
{	
	struct tcctl_rc_addr cl_addr;
	char cl_path[UNSCK_PATH_MAX_LEN] = { '\0' };

	tcctl_rc_addr_set(&cl_addr, cl_path);

	struct tcctl_rc_msg rc_msg;

	int rc_msg_size = recvfrom(
		unsck_fd, 
		&rc_msg, 
		sizeof(struct tcctl_rc_msg), 
		0,
		RECV_RC_ADDR(cl_addr)
	);

	if (rc_msg_size == -1)
	{
		LOG_ERROR("could not receive message: ", errno_msg(errno));
		return 0;
	}

	if (rc_msg_size != sizeof(struct tcctl_rc_msg))
	{
		LOG_WARN("received malformed message/no data", NULL);
		// TODO send acknowledge
		return 0;
	}

	if (!tcctl_rc_handle_msg(&rc_msg, &cl_addr))
	{
		LOG_WARN("received kill command", NULL);
		return 0;
	}
	
	return 1;
}

int
tcctl_rc_handle_msg(struct tcctl_rc_msg *msg, struct tcctl_rc_addr *addr)
{
	struct tcctl_rc_msg ret_msg;
	union tcctl_rc_param rc_stat;
	switch (msg->cmd)
	{
		case STAT:	
			ret_msg.cmd = INFO;
			ret_msg.p1 = msg->p1;
			rc_stat.uint = tcctl_stat_get(msg->p1.uint);
			ret_msg.p2 = rc_stat;
			return tcctl_rc_send_msg(&ret_msg, addr);
		case OVRD:
			LOG_INFO("override output to ", msg->p1.boolean ? "run" : "idle");
			run_stat.phase = msg->p1.boolean ? 
				OVRD_RUN : OVRD_IDLE;
			return 1;
		case AUTO:
			LOG_INFO("switch to auto mode", NULL);
			run_stat.phase = RUN; // switch to auto mode
			return 1;
		case TRIG:
			LOG_INFO("update trigger temps", NULL);
			run_stat.low_temp = msg->p1.uint;
			run_stat.trig_temp = msg->p2.uint;
			return 1;
		case CONF:
			LOG_INFO("request conf reload", NULL);
			if (!tcctl_conf_load(conf_fd))
			{
				ret_msg.cmd = CERR;
				ret_msg.p1.sint = conf_errline;
				ret_msg.p2.sint = conf_errentid;

				LOG_WARN("could not load conf", NULL);
				tcctl_rc_send_msg(&ret_msg, addr);
				return 0;
			}
			return 1;
		case KILL:
			LOG_INFO("request service kill", NULL);
			return 0;
		default:
			return 0;
	}
}

int
tcctl_rc_send_msg(struct tcctl_rc_msg *msg, struct tcctl_rc_addr *addr)
{
	int rc_msg_size = sendto(
		unsck_fd, 
		msg, 
		sizeof(struct tcctl_rc_msg), 
		0, 
		RC_ADDR(*addr)
	);

	if (rc_msg_size == -1)
	{
		LOG_ERROR("could not send message: ", errno_msg(errno));
		return 0;
	}

	if (rc_msg_size != sizeof(struct tcctl_rc_msg))
	{
		LOG_WARN("sent malformed data/no data", NULL);
		// TODO retry
		return 0;
	}

	return 1;
}

unsigned int
tcctl_stat_get(unsigned int param_id)
{
	switch (param_id)
	{
		case 0:
			return run_stat.last_temp;
		case 1:
			return run_stat.low_temp;
		case 2:
			return run_stat.trig_temp;
		case 3:
			return run_stat.phase;
		default:
			return 0;
	}
}

void
tcctl_stat_update(struct tcctl_stat *stat, struct tcctl_conf *conf)
{
	stat->low_temp = conf->low_temp.uint;
	stat->trig_temp = conf->trig_temp.uint;
}

int
tcctl_temp_read(int fd, unsigned int *val)
{
	char str[TEMP_BUF_MAX_LEN] = { '\0' };
	if (pread(fd, str, TEMP_BUF_MAX_LEN, 0) == -1)
	{
		LOG_ERROR("could not read sensor: ", errno_msg(errno));
		close(fd);
		return 0;
	}
	
	unsigned int temp = 0;

	uint_read(&temp, str);
	*val = temp / 1000;
	return 1;
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
tcctl_conf_apply(struct tcctl_conf *from, struct tcctl_conf *to)
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
		LOG_ERROR("could not read file stats: ", errno_msg(errno));
		close(fd);
		return 0;
	}

	if (fs.st_size == 0)
	{
		LOG_WARN("empty conf file", NULL);
		return 1;
	}

	char *memblk = mmap(NULL, fs.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (memblk == MAP_FAILED)
	{
		LOG_ERROR("mmap failed: ", errno_msg(errno));
		close(fd);
		return 0;
	}

	const char *str = memblk;

	conf_errline = 0;
	conf_errentid = -1;

	for (;;)
	{
		str = tcctl_conf_read_next_line(str);
		conf_errline++;
		if (str == NULL)
		{
			LOG_ERROR("malformed conf entry: ", NULL);
			munmap(memblk, fs.st_size);
			return 0;
		}

		if (*str == '\0') break;
	}

	munmap(memblk, fs.st_size);
	LOG_INFO("load conf ok", NULL);
	return 1;
}

const char *
tcctl_conf_read_next_line(const char *conf_str)
{
	const char *p = conf_str;
	while (*p == ' ' || *p == '\t')
		p++;

	if (*p == '\n')
		return p + 1;

	if (*p == '\0')
		return p;

	for (unsigned int i = 0; i < CONF_ENTRIES; i++)
	{
		struct tcctl_conf_entry entry = tcctl_conf_entries[i];
		size_t len = str_len(entry.name, ENTRY_NAME_MAX_LEN);
		if (str_eq(p, entry.name, len))
		{
			int next = entry.read_fn(entry.field, p+len+1);
			conf_errentid = i; // just in case
			if (next == -1)
				return NULL;
			else
				return p+len+1+next+1;
		}
	}

	conf_errline = 0;
	conf_errentid = CONF_ENTRIES;
	return NULL;
}

int
tcctl_get_uint(union tcctl_conf_field *field, const char *val)
{
	return uint_read(&field->uint, val);
}

int
tcctl_get_boolean(union tcctl_conf_field *field, const char *val)
{
	return boolean_read(&field->boolean, val);
}

void 
tcctl_log_writeln(const char **msgs, size_t cnt)
{
	for (size_t i = 0; i < cnt; i++)
	{
		const char *msg = *(msgs+i);
		if (msg == NULL) continue;
		write(log_fd, msg, str_len(msg, LOG_MSG_MAX_LEN));
	}
	write(log_fd, "\n", 1);
	fsync(log_fd);
}

void 
tcctl_log_info(const char *msg, const char *val, const char *src, int warn)
{
	const char *header = warn ? "warn>" : "info>";
	char time_buf[32] = { '\0' };
	time_write(time_buf);
	const char *msg_buf[] = 
	{ 
		header, " [", time_buf , "] ", msg, val, " (", src, ")" 
	};
	tcctl_log_writeln(msg_buf, 9);
}

void 
tcctl_log_error(const char *msg, const char *val, const char *src, const char *loc)
{
	char time_buf[32] = { '\0' };
	time_write(time_buf);
	const char *msg_buf[] = 
	{ 
		"!err>", " [", time_buf, "] ", msg, val, " (", src, "/:", loc, ")" 
	};
	tcctl_log_writeln(msg_buf, 11);
}

int
str_eq(const char *s1, const char *s2, size_t max_len)
{
	for (size_t i = 0; i < max_len; i++)
	{
		if (*s1 != *s2) return 0;
		if (*s1 == '\0' || *s2 == '\0') break;
		s1++;
		s2++;
	}

	return 1;
}

size_t
str_copy(const char *from, char *to, size_t max_len)
{
	size_t len = 0;
	while (*from != '\0' && len < max_len)
	{
		*to++ = *from++;
		len++;
	}

	return len;
}

size_t 
str_set(char c, char *to, size_t max_len)
{
	size_t len = 0;
	while (*to != '\0' && len < max_len)
	{
		*to++ = c;
		len++;
	}

	return len;
}

size_t
str_len(const char *s, size_t max_len)
{
	size_t len = 0;
	while (*s++ != '\0' && len < max_len)
		len++;

	return len;
}

int
uint_read(unsigned int *val, const char *str)
{
	const char *p = str;
	unsigned short places = 0;
	unsigned int mult = 1;
	
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

		LOG_WARN("read malformed uint: ", str);
		return -1;
	}

	unsigned int num = 0;
	for (unsigned short i = 0; i < places; i++)
	{
		char c = *(str+i);
		if (c >= '0' && c <= '9')
		{
			mult /= 10;
			num += (c - '0') * mult;
		}
	}

	*val = num;
	return places;
}

int
uint_write(unsigned int val, char *str)
{
	unsigned short places = 0;

	unsigned int num = val;
	while (num > 0)
	{
		num /= 10;
		places++;
	}

	uint_write_pad(val, str, places);
	return places;
}

int
uint_write_pad(unsigned int val, char *str, size_t len)
{
	unsigned short places = 0;
	char *p = str+len-1;
	while (p >= str)
	{
		int d = val % 10;
		*p-- = d + '0';
		val /= 10;
		places++;
	}

	return places;
}

int
boolean_read(int *val, const char *str)
{
	int set_true = str_eq(str, "true", 4);
	int set_false = str_eq(str, "false", 5);

	if (!set_true && !set_false)
	{
		LOG_WARN("read malformed boolean: ", str);
		return -1;
	}

	*val = set_true;
	return set_true ? 4 : 5;
}

int
boolean_write(int val, char *str)
{
	return str_copy(val ? "true" : "false", str, 5);
}

int 
time_write(char *str)
{
	struct timeval time;
	gettimeofday(&time, NULL);
	unsigned int s = time.tv_sec % 86400;
	unsigned int tusecs = time.tv_usec / 1000;
	unsigned int tsecs = s % 60;
	unsigned int tmins = s / 60 % 60;
	unsigned int thrs = s / 3600;
	unsigned int ddays = time.tv_sec / 86400;

	char *p = str;

	p+=uint_write_pad(ddays, p, 5);
	*p++ = '/';
	p+=uint_write_pad(thrs, p, 2);
	*p++ = ':';
	p+=uint_write_pad(tmins, p, 2);
	*p++ = ':';
	p+=uint_write_pad(tsecs, p, 2);
	*p++ = '.';
	p+=uint_write_pad(tusecs, p, 3);

	return p-str;
}

static const char *errno_msgs[] = 
{
	"EPERM operation not permitted",
        "ENOENT no such file or directory",
        "ESRCH no such process",
        "EINTR interrupted system call",
        "EIO i/o error",
        "ENXIO no such device or address",
        "E2BIG argument list too long",
        "ENOEXEC exec format error",
        "EBADF bad file descriptor",
        "ECHILD no child process",
        "EAGAIN resource temporarily unavailable",
        "ENOMEM out of memory",
        "EACCES permission denied",
        "EFAULT bad address",
        "ENOTBLK block device required",
        "EBUSY resource busy",
        "EEXIST file exists",
        "EXDEV cross-device link",
        "ENODEV no such device",
        "ENOTDIR not a directory",
        "EISDIR is a directory",
        "EINVAL invalid argument",
        "ENFILE too many open files in system",
        "EMFILE no file descriptors available",
        "ENOTTY not a tty",
        "ETXTBSY text file busy",
        "EFBIG file too large",
        "ENOSPC no space left on device",
        "ESPIPE invalid seek",
        "EROFS read-only file system",
        "EMLINK too many links",
        "EPIPE broken pipe",
        "EDOM domain error",
        "ERANGE result not representable",
        "EDEADLK resource deadlock would occur",
        "ENAMETOOLONG filename too long",
        "ENOLCK no locks available",
        "ENOSYS function not implemented",
        "ENOTEMPTY directory not empty",
        "ELOOP symbolic link loop",
        "EWOULDBLOCK resource temporarily unavailable",
        "ENOMSG no message of desired type",
        "EIDRM identifier removed",
        "ECHRNG no error information",
        "EL2NSYNC no error information",
        "EL3HLT no error information",
        "EL3RST no error information",
        "ELNRNG no error information",
        "EUNATCH no error information",
        "ENOCSI no error information",
        "EL2HLT no error information",
        "EBADE no error information",
        "EBADR no error information",
        "EXFULL no error information",
        "ENOANO no error information",
        "EBADRQC no error information",
        "EBADSLT no error information",
        "EDEADLOCK resource deadlock would occur",
        "EBFONT no error information",
        "ENOSTR device not a stream",
        "ENODATA no data available",
        "ETIME device timeout",
        "ENOSR out of streams resources",
        "ENONET no error information",
        "ENOPKG no error information",
        "EREMOTE no error information",
        "ENOLINK link has been severed",
        "EADV no error information",
        "ESRMNT no error information",
        "ECOMM no error information",
        "EPROTO protocol error",
        "EMULTIHOP multihop attempted",
        "EDOTDOT no error information",
        "EBADMSG bad message",
        "EOVERFLOW value too large for data type",
        "ENOTUNIQ no error information",
        "EBADFD file descriptor in bad state",
        "EREMCHG no error information",
        "ELIBACC no error information",
        "ELIBBAD no error information",
        "ELIBSCN no error information",
        "ELIBMAX no error information",
        "ELIBEXEC no error information",
        "EILSEQ illegal byte sequence",
        "ERESTART no error information",
        "ESTRPIPE no error information",
        "EUSERS no error information",
        "ENOTSOCK not a socket",
        "EDESTADDRREQ destination address required",
        "EMSGSIZE message too large",
        "EPROTOTYPE protocol wrong type for socket",
        "ENOPROTOOPT protocol not available",
        "EPROTONOSUPPORT protocol not supported",
        "ESOCKTNOSUPPORT socket type not supported",
        "EOPNOTSUPP not supported",
        "ENOTSUP not supported",
        "EPFNOSUPPORT protocol family not supported",
        "EAFNOSUPPORT address family not supported by protocol",
        "EADDRINUSE address in use",
        "EADDRNOTAVAIL address not available",
        "ENETDOWN network is down",
        "ENETUNREACH network unreachable",
        "ENETRESET connection reset by network",
        "ECONNABORTED connection aborted",
        "ECONNRESET connection reset by peer",
        "ENOBUFS no buffer space available",
        "EISCONN socket is connected",
        "ENOTCONN socket not connected",
        "ESHUTDOWN cannot send after socket shutdown",
        "ETOOMANYREFS no error information",
        "ETIMEDOUT operation timed out",
        "ECONNREFUSED connection refused",
        "EHOSTDOWN host is down",
        "EHOSTUNREACH host is unreachable",
        "EALREADY operation already in progress",
        "EINPROGRESS operation in progress",
        "ESTALE stale file handle",
        "EUCLEAN no error information",
        "ENOTNAM no error information",
        "ENAVAIL no error information",
        "EISNAM no error information",
        "EREMOTEIO remote i/o error",
        "EDQUOT quota exceeded",
        "ENOMEDIUM no medium found",
        "EMEDIUMTYPE wrong medium type",
        "ECANCELED operation canceled",
        "ENOKEY no error information",
        "EKEYEXPIRED no error information",
        "EKEYREVOKED no error information",
        "EKEYREJECTED no error information",
        "EOWNERDEAD previous owner died",
        "ENOTRECOVERABLE state not recoverable",
        "ERFKILL no error information",
        "EHWPOISON no error information"
};

const char *
errno_msg(int err)
{
	return errno_msgs[err];	
}
