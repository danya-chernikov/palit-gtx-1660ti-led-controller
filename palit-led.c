/* Turns RGB lighting on/off on the Palit NVIDIA GeForce RTX 1660 Ti */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/ioctl.h>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include <stdbool.h>

#define GPU_BDF		"0000:02:00.0"
#define PALIT_SIG	"PALIT 120"
#define PALIT_ADDR	0x49
#define ID_REGISTER	0xF0

typedef unsigned int u_int;

int i2c_transfer(int fd, struct i2c_msg *msgs, u_int nmsgs)
{
	struct i2c_rdwr_ioctl_data	transfer;

	transfer.msgs = msgs;
	transfer.nmsgs = nmsgs;

	if (ioctl(fd, I2C_RDWR, &transfer) < 0)
		return -1;

	return 0;
}

int read_register(int fd, uint8_t reg, uint8_t *data, uint16_t len)
{
	struct i2c_msg	msgs[2];
	
	msgs[0].addr = PALIT_ADDR;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = &reg;

	msgs[1].addr = PALIT_ADDR;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = data;

	return i2c_transfer(fd, msgs, 2);
}

int write_register(int fd, uint8_t reg, const uint8_t *data, uint16_t len)
{
	struct i2c_msg	msg;
	uint8_t			buffer[32];

	if (len + 1 > sizeof(buffer))
		return -1;

	buffer[0] = reg;
	memcpy(&buffer[1], data, len);

	msg.addr = PALIT_ADDR;
	msg.flags = 0;
	msg.len = len + 1;
	msg.buf = buffer;

	return i2c_transfer(fd, &msg, 1);
}

bool bus_belongs_to_gpu(int bus)
{
	char	link[PATH_MAX];
	char	resolved[PATH_MAX];

	snprintf(link, sizeof(link), "/sys/class/i2c-dev/i2c-%d/device", bus);

	if (realpath(link, resolved) == NULL)
		return false;

	return (strstr(resolved, GPU_BDF) != NULL);
}

bool contains_palit_signature(const uint8_t *data, size_t len)
{
	size_t	sig_len;

	sig_len = sizeof(PALIT_SIG) - 1;

	if (len < sig_len)
		return 0;

	for (size_t i = 0; i <= len - sig_len; ++i)
	{
		if (memcmp(data + i, PALIT_SIG, sig_len) == 0)
			return true;
	}
	return false;
}

void print_buffer(const uint8_t *buf, size_t len)
{
	size_t	i;

	for (i = 0; i < len; ++i)
		printf("%02X ", buf[i]);

	printf(" | ");

	for (i = 0; i < len; ++i)
	{
		if (buf[i] >= 0x20 && buf[i] <= 0x7E)
			putchar(buf[i]);
		else
			putchar('.');
	}
	putchar('\n');
}

bool probe_bus(int bus, int verbose)
{
	char	devname[64];
	uint8_t	data[16];
	int		fd;

	snprintf(devname, sizeof(devname), "/dev/i2c-%d", bus);

	fd = open(devname, O_RDWR);
	if (fd < 0)
	{
		if (verbose)
			fprintf(stderr, "%s: %s\n", devname, strerror(errno));
		return false;
	}

	memset(data, 0, sizeof(data));

	if (read_register(fd, ID_REGISTER, data, sizeof(data)) < 0)
	{
		if (verbose)
			printf("%s: no response from 0x%02X\n", devname, PALIT_ADDR);
		close(fd);
		return false;
	}

	if (verbose)
	{
		printf("%s: ", devname);
		print_buffer(data, sizeof(data));
	}

	close(fd);

	return (contains_palit_signature(data, sizeof(data)));
}

int find_palit_bus()
{
	int	bus;
	int	found;

	found = -1;
	for (bus = 0; bus < 256; ++bus)
	{
		if (!bus_belongs_to_gpu(bus))
			continue;

		if (probe_bus(bus, 0))
		{
			if (found != -1)
			{
				fprintf(stderr, "More than one PALIT controller found\n");
				return -1;
			}
			found = bus;
		}
	}
	return found;
}

/* Returns 0 on success */
int set_led(int bus, int enabled)
{
	char			devname[64];
	uint8_t			enable_command[] = { 0x01 };
	uint8_t			led_on[] = { 0xD1, 0xD1, 0xD1, 0x64 };
	uint8_t			led_off[] = { 0x00, 0x00, 0x00, 0x64 };
	const uint8_t	*state;
	int				fd;

	snprintf(devname, sizeof(devname), "/dev/i2c-%d", bus);

	fd = open(devname, O_RDWR);
	if (fd < 0)
	{
		perror(devname);
		return -1;
	}

	if (write_register(fd, 0x60, enable_command, sizeof(enable_command)) < 0)
	{
		perror("write register 0x60");
		close(fd);
		return -1;
	}

	if (enabled)
		state = led_on;
	else
		state = led_off;

	if (write_register(fd, 0x6C, state, 4) < 0)
	{
		perror("write register 0x60");
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

void probe_all()
{
	int	bus;

	printf("Searching I2C adapters belonging to %s\n\n", GPU_BDF);

	for (bus = 0; bus < 256; ++bus)
	{
		if (!bus_belongs_to_gpu(bus))
			continue;

		printf("Candidate i2c-%d:\n", bus);

		if (probe_bus(bus, 1))
			printf(" *** PALIT controller found ***\n");
		printf("\n");
	}
}

void usage(const char *name)
{
	fprintf(stderr,
		"Usage:\n"
		" %s probe\n"
		" %s off\n"
		" %s on\n",
		name, name, name);
}

int main(int argc, char **argv)
{
	int		bus;
	bool	enabled;

	if (argc != 2)
	{
		usage(argv[0]);
		return 1;
	}

	if (strcmp(argv[1], "probe") == 0)
	{
		probe_all();
		return 0;
	}

	if (strcmp(argv[1], "on") == 0)
		enabled = true;
	else if (strcmp(argv[1], "off") == 0)
		enabled = false;
	else
	{
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	bus = find_palit_bus();

	if (bus < 0)
	{
		fprintf(stderr, "PALIT 120 controller not found\n");
		return EXIT_FAILURE;
	}

	printf("PALIT controller found on /dev/i2c-%d\n", bus);

	if (set_led(bus, enabled) < 0)
		return EXIT_FAILURE;

	printf("LED: %s\n", enabled ? "ON" : "OFF");

	return EXIT_SUCCESS;
}
