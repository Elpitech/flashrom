/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2009 Paul Fox <pgf@laptop.org>
 * Copyright (C) 2009, 2010 Carl-Daniel Hailfinger
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#if CONFIG_FT2232_SPI == 1

#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "flash.h"
#include "programmer.h"
#include "spi.h"
#include <ftdi.h>
#include "chipdrivers.h"

/* This is not defined in libftdi.h <0.20 (c7e4c09e68cfa6f5e112334aa1b3bb23401c8dc7 to be exact).
 * Some tests indicate that his is the only change that it is needed to support the FT232H in flashrom. */
#if !defined(HAVE_FT232H)
#define TYPE_232H	6
#endif

/* Please keep sorted by vendor ID, then device ID. */

#define FTDI_VID		0x0403
#define FTDI_FT2232H_PID	0x6010
#define FTDI_FT4232H_PID	0x6011
#define FTDI_FT232H_PID		0x6014
#define TIAO_TUMPA_PID		0x8a98
#define TIAO_TUMPA_LITE_PID	0x8a99
#define AMONTEC_JTAGKEY_PID	0xCFF8

#define GOEPEL_VID		0x096C
#define GOEPEL_PICOTAP_PID	0x1449

#define FIC_VID			0x1457
#define OPENMOKO_DBGBOARD_PID	0x5118

#define OLIMEX_VID		0x15BA
#define OLIMEX_ARM_OCD_PID	0x0003
#define OLIMEX_ARM_TINY_PID	0x0004
#define OLIMEX_ARM_OCD_H_PID	0x002B
#define OLIMEX_ARM_TINY_H_PID	0x002A

#define GOOGLE_VID		0x18D1
#define GOOGLE_SERVO_PID	0x5001
#define GOOGLE_SERVO_V2_PID0	0x5002
#define GOOGLE_SERVO_V2_PID1	0x5003

const struct dev_entry devs_ft2232spi[] = {
	{FTDI_VID, FTDI_FT2232H_PID, OK, "FTDI", "FT2232H"},
	{FTDI_VID, FTDI_FT4232H_PID, OK, "FTDI", "FT4232H"},
	{FTDI_VID, FTDI_FT232H_PID, OK, "FTDI", "FT232H"},
	{FTDI_VID, TIAO_TUMPA_PID, OK, "TIAO", "USB Multi-Protocol Adapter"},
	{FTDI_VID, TIAO_TUMPA_LITE_PID, OK, "TIAO", "USB Multi-Protocol Adapter Lite"},
	{FTDI_VID, AMONTEC_JTAGKEY_PID, OK, "Amontec", "JTAGkey"},
	{GOEPEL_VID, GOEPEL_PICOTAP_PID, OK, "GOEPEL", "PicoTAP"},
	{GOOGLE_VID, GOOGLE_SERVO_PID, OK, "Google", "Servo"},
	{GOOGLE_VID, GOOGLE_SERVO_V2_PID0, OK, "Google", "Servo V2 Legacy"},
	{GOOGLE_VID, GOOGLE_SERVO_V2_PID1, OK, "Google", "Servo V2"},
	{FIC_VID, OPENMOKO_DBGBOARD_PID, OK, "FIC", "OpenMoko Neo1973 Debug board (V2+)"},
	{OLIMEX_VID, OLIMEX_ARM_OCD_PID, OK, "Olimex", "ARM-USB-OCD"},
	{OLIMEX_VID, OLIMEX_ARM_TINY_PID, OK, "Olimex", "ARM-USB-TINY"},
	{OLIMEX_VID, OLIMEX_ARM_OCD_H_PID, OK, "Olimex", "ARM-USB-OCD-H"},
	{OLIMEX_VID, OLIMEX_ARM_TINY_H_PID, OK, "Olimex", "ARM-USB-TINY-H"},

	{0},
};

#define DEFAULT_DIVISOR 2

#define BITMODE_BITBANG_RESET	0
#define BITMODE_BITBANG_NORMAL	1
#define BITMODE_BITBANG_SPI	2

/* The variables cs_bits and pindir store the values for the "set data bits low byte" MPSSE command that
 * sets the initial state and the direction of the I/O pins. The pin offsets are as follows:
 * SCK is bit 0.
 * DO  is bit 1.
 * DI  is bit 2.
 * CS  is bit 3.
 *
 * The default values (set below) are used for most devices:
 *  value: 0x08  CS=high, DI=low, DO=low, SK=low
 *    dir: 0x0b  CS=output, DI=input, DO=output, SK=output
 */
static uint8_t hold_bits = 1 << 0;
static uint8_t wp_bits = 1 << 7; // 7-th pin
static uint8_t cs_bits = 0x08;
static uint8_t pindir = 0x0b;
static struct ftdi_context ftdic_context;

static const char *get_ft2232_devicename(int ft2232_vid, int ft2232_type)
{
	int i;
	for (i = 0; devs_ft2232spi[i].vendor_name != NULL; i++) {
		if ((devs_ft2232spi[i].device_id == ft2232_type) && (devs_ft2232spi[i].vendor_id == ft2232_vid))
				return devs_ft2232spi[i].device_name;
	}
	return "unknown device";
}

static const char *get_ft2232_vendorname(int ft2232_vid, int ft2232_type)
{
	int i;
	for (i = 0; devs_ft2232spi[i].vendor_name != NULL; i++) {
		if ((devs_ft2232spi[i].device_id == ft2232_type) && (devs_ft2232spi[i].vendor_id == ft2232_vid))
				return devs_ft2232spi[i].vendor_name;
	}
	return "unknown vendor";
}

static int send_buf(struct ftdi_context *ftdic, const unsigned char *buf,
		    int size)
{
	int r;
	r = ftdi_write_data(ftdic, (unsigned char *) buf, size);
	if (r < 0) {
		msg_perr("ftdi_write_data: %d, %s\n", r, ftdi_get_error_string(ftdic));
		return 1;
	}
	return 0;
}

static int get_buf(struct ftdi_context *ftdic, const unsigned char *buf,
		   int size)
{
	int r;

	while (size > 0) {
		r = ftdi_read_data(ftdic, (unsigned char *) buf, size);
		if (r < 0) {
			msg_perr("ftdi_read_data: %d, %s\n", r, ftdi_get_error_string(ftdic));
			return 1;
		}
		buf += r;
		size -= r;
	}
	return 0;
}

static int ft2232_spi_send_command(struct flashctx *flash,
				   unsigned int writecnt, unsigned int readcnt,
				   const unsigned char *writearr,
				   unsigned char *readarr);

static int ft2232_spi_send_multicommand(struct flashctx *flash,
				 struct spi_command *cmds);

static int ft2232_spi_read(struct flashctx *flash, uint8_t *buf,
			    unsigned int start, unsigned int len);

static const struct spi_master spi_master_ft2232 = {
	.type		= SPI_CONTROLLER_FT2232,
	.max_data_read	= 64 * 1024,
	.max_data_write	= 256,
	.command	= ft2232_spi_send_command,
	.multicommand	= ft2232_spi_send_multicommand,
	.read		= ft2232_spi_read,
	.write_256	= default_spi_write_256,
	.write_aai	= default_spi_write_aai,
};

static int ft2232_spi_shutdown(void *data)
{
	static unsigned char buf[] = { SET_BITS_HIGH, 0, 0, SET_BITS_LOW, 0, 0 };
	struct ftdi_context *ftdic = &ftdic_context;
	int ret = 0;

	ret = send_buf(ftdic, buf, sizeof(buf));
	if (ret)
		msg_perr("ft2232_spi_shutdown: send_buf failed at end: %i\n", ret);

	return 0;
}

/* Returns 0 upon success, a negative number upon errors. */
int ft2232_spi_init(void)
{
	int ret = 0;
	struct ftdi_context *ftdic = &ftdic_context;
	unsigned char buf[512];
	int ft2232_vid = FTDI_VID;
	int ft2232_type = FTDI_FT4232H_PID;
	int channel_count = 4; /* Stores the number of channels of the device. */
	enum ftdi_interface ft2232_interface = INTERFACE_A;
	/*
	 * The 'H' chips can run with an internal clock of either 12 MHz or 60 MHz,
	 * but the non-H chips can only run at 12 MHz. We enable the divide-by-5
	 * prescaler on the former to run on the same speed.
	 */
	uint8_t clock_5x = 1;
	/* In addition to the prescaler mentioned above there is also another
	 * configurable one on all versions of the chips. Its divisor div can be
	 * set by a 16 bit value x according to the following formula:
	 * div = (1 + x) * 2 <-> x = div / 2 - 1
	 * Hence the expressible divisors are all even numbers between 2 and
	 * 2^17 (=131072) resulting in SCK frequencies of 6 MHz down to about
	 * 92 Hz for 12 MHz inputs.
	 */
	uint32_t divisor = DEFAULT_DIVISOR;
	int f;
	char *arg;
	double mpsse_clk;

	arg = extract_programmer_param("type");
	if (arg) {
		if (!strcasecmp(arg, "2232H")) {
			ft2232_type = FTDI_FT2232H_PID;
			channel_count = 2;
		} else if (!strcasecmp(arg, "4232H")) {
			ft2232_type = FTDI_FT4232H_PID;
			channel_count = 4;
		} else if (!strcasecmp(arg, "232H")) {
			ft2232_type = FTDI_FT232H_PID;
			channel_count = 1;
		} else if (!strcasecmp(arg, "jtagkey")) {
			ft2232_type = AMONTEC_JTAGKEY_PID;
			channel_count = 2;
			/* JTAGkey(2) needs to enable its output via Bit4 / GPIOL0
			*  value: 0x18  OE=high, CS=high, DI=low, DO=low, SK=low
			*    dir: 0x1b  OE=output, CS=output, DI=input, DO=output, SK=output */
			cs_bits = 0x18;
			pindir = 0x1b;
		} else if (!strcasecmp(arg, "picotap")) {
			ft2232_vid = GOEPEL_VID;
			ft2232_type = GOEPEL_PICOTAP_PID;
			channel_count = 2;
		} else if (!strcasecmp(arg, "tumpa")) {
			/* Interface A is SPI1, B is SPI2. */
			ft2232_type = TIAO_TUMPA_PID;
			channel_count = 2;
		} else if (!strcasecmp(arg, "tumpalite")) {
			/* Only one channel is used on lite edition */
			ft2232_type = TIAO_TUMPA_LITE_PID;
			channel_count = 1;
		} else if (!strcasecmp(arg, "busblaster")) {
			/* In its default configuration it is a jtagkey clone */
			ft2232_type = FTDI_FT2232H_PID;
			channel_count = 2;
			cs_bits = 0x18;
			pindir = 0x1b;
		} else if (!strcasecmp(arg, "openmoko")) {
			ft2232_vid = FIC_VID;
			ft2232_type = OPENMOKO_DBGBOARD_PID;
			channel_count = 2;
		} else if (!strcasecmp(arg, "arm-usb-ocd")) {
			ft2232_vid = OLIMEX_VID;
			ft2232_type = OLIMEX_ARM_OCD_PID;
			channel_count = 2;
			/* arm-usb-ocd(-h) has an output buffer that needs to be enabled by pulling ADBUS4 low.
			*  value: 0x08  #OE=low, CS=high, DI=low, DO=low, SK=low
			*    dir: 0x1b  #OE=output, CS=output, DI=input, DO=output, SK=output */
			cs_bits = 0x08;
			pindir = 0x1b;
		} else if (!strcasecmp(arg, "arm-usb-tiny")) {
			ft2232_vid = OLIMEX_VID;
			ft2232_type = OLIMEX_ARM_TINY_PID;
			channel_count = 2;
		} else if (!strcasecmp(arg, "arm-usb-ocd-h")) {
			ft2232_vid = OLIMEX_VID;
			ft2232_type = OLIMEX_ARM_OCD_H_PID;
			channel_count = 2;
			/* See arm-usb-ocd */
			cs_bits = 0x08;
			pindir = 0x1b;
		} else if (!strcasecmp(arg, "arm-usb-tiny-h")) {
			ft2232_vid = OLIMEX_VID;
			ft2232_type = OLIMEX_ARM_TINY_H_PID;
			channel_count = 2;
		} else if (!strcasecmp(arg, "google-servo")) {
			ft2232_vid = GOOGLE_VID;
			ft2232_type = GOOGLE_SERVO_PID;
		} else if (!strcasecmp(arg, "google-servo-v2")) {
			ft2232_vid = GOOGLE_VID;
			ft2232_type = GOOGLE_SERVO_V2_PID1;
			/* Default divisor is too fast, and chip ID fails */
			divisor = 6;
		} else if (!strcasecmp(arg, "google-servo-v2-legacy")) {
			ft2232_vid = GOOGLE_VID;
			ft2232_type = GOOGLE_SERVO_V2_PID0;
		} else {
			msg_perr("Error: Invalid device type specified.\n");
			free(arg);
			return -1;
		}
	}
	free(arg);

	arg = extract_programmer_param("port");
	if (arg) {
		switch (toupper((unsigned char)*arg)) {
		case 'A':
			ft2232_interface = INTERFACE_A;
			break;
		case 'B':
			ft2232_interface = INTERFACE_B;
			if (channel_count < 2)
				channel_count = -1;
			break;
		case 'C':
			ft2232_interface = INTERFACE_C;
			if (channel_count < 3)
				channel_count = -1;
			break;
		case 'D':
			ft2232_interface = INTERFACE_D;
			if (channel_count < 4)
				channel_count = -1;
			break;
		default:
			channel_count = -1;
			break;
		}
		if (channel_count < 0 || strlen(arg) != 1) {
			msg_perr("Error: Invalid channel/port/interface specified: \"%s\".\n", arg);
			free(arg);
			return -2;
		}
	}
	free(arg);

	arg = extract_programmer_param("divisor");
	if (arg && strlen(arg)) {
		unsigned int temp = 0;
		char *endptr;
		temp = strtoul(arg, &endptr, 10);
		if (*endptr || temp < 2 || temp > 131072 || temp & 0x1) {
			msg_perr("Error: Invalid SPI frequency divisor specified: \"%s\".\n"
				 "Valid are even values between 2 and 131072.\n", arg);
			free(arg);
			return -2;
		} else {
			divisor = (uint32_t)temp;
		}
	}
	free(arg);

	msg_pdbg("Using device type %s %s ",
		 get_ft2232_vendorname(ft2232_vid, ft2232_type),
		 get_ft2232_devicename(ft2232_vid, ft2232_type));
	msg_pdbg("channel %s.\n",
		 (ft2232_interface == INTERFACE_A) ? "A" :
		 (ft2232_interface == INTERFACE_B) ? "B" :
		 (ft2232_interface == INTERFACE_C) ? "C" : "D");

	if (ftdi_init(ftdic) < 0) {
		msg_perr("ftdi_init failed.\n");
		return -3;
	}

	if (ftdi_set_interface(ftdic, ft2232_interface) < 0) {
		msg_perr("Unable to select channel (%s).\n", ftdi_get_error_string(ftdic));
	}

	arg = extract_programmer_param("serial");
	f = ftdi_usb_open_desc(ftdic, ft2232_vid, ft2232_type, NULL, arg);
	free(arg);

	if (f < 0 && f != -5) {
		msg_perr("Unable to open FTDI device: %d (%s).\n", f, ftdi_get_error_string(ftdic));
		return -4;
	}

	if (ftdic->type != TYPE_2232H && ftdic->type != TYPE_4232H && ftdic->type != TYPE_232H) {
		msg_pdbg("FTDI chip type %d is not high-speed.\n", ftdic->type);
		clock_5x = 0;
	}

	if (ftdi_usb_reset(ftdic) < 0) {
		msg_perr("Unable to reset FTDI device (%s).\n", ftdi_get_error_string(ftdic));
	}

	if (ftdi_set_latency_timer(ftdic, 2) < 0) {
		msg_perr("Unable to set latency timer (%s).\n", ftdi_get_error_string(ftdic));
	}

	if (ftdi_write_data_set_chunksize(ftdic, 480)) {
		msg_perr("Unable to set chunk size (%s).\n", ftdi_get_error_string(ftdic));
	}

	if (ftdi_set_bitmode(ftdic, 0x00, BITMODE_BITBANG_RESET) < 0) {
		msg_perr("Unable to reset MPSSE controller (%s).\n", ftdi_get_error_string(ftdic));
	}

	if (ftdi_set_bitmode(ftdic, 0x00, BITMODE_BITBANG_SPI) < 0) {
		msg_perr("Unable to set bitmode to SPI (%s).\n", ftdi_get_error_string(ftdic));
	}

	if (clock_5x) {
		msg_pdbg("Disable divide-by-5 front stage\n");
		buf[0] = 0x8a; /* Disable divide-by-5. DIS_DIV_5 in newer libftdi */
		if (send_buf(ftdic, buf, 1)) {
			ret = -5;
			goto ftdi_err;
		}
		mpsse_clk = 60.0;
	} else {
		mpsse_clk = 12.0;
	}

	msg_pdbg("Set clock divisor\n");
	buf[0] = TCK_DIVISOR;
	buf[1] = (divisor / 2 - 1) & 0xff;
	buf[2] = ((divisor / 2 - 1) >> 8) & 0xff;
	if (send_buf(ftdic, buf, 3)) {
		ret = -6;
		goto ftdi_err;
	}

	msg_pdbg("MPSSE clock: %f MHz, divisor: %u, SPI clock: %f MHz\n",
		 mpsse_clk, divisor, (double)(mpsse_clk / divisor));

	/* Disconnect TDI/DO to TDO/DI for loopback. */
	msg_pdbg("No loopback of TDI/DO TDO/DI\n");
	buf[0] = LOOPBACK_END;
	if (send_buf(ftdic, buf, 1)) {
		ret = -7;
		goto ftdi_err;
	}

	msg_pdbg("Set data bits\n");
	buf[0] = SET_BITS_LOW;
	buf[1] = cs_bits;
	buf[2] = pindir;
	if (send_buf(ftdic, buf, 3)) {
		ret = -8;
		goto ftdi_err;
	}

	register_shutdown(ft2232_spi_shutdown, NULL);
	register_spi_master(&spi_master_ft2232);

	return 0;

ftdi_err:
	if ((f = ftdi_usb_close(ftdic)) < 0) {
		msg_perr("Unable to close FTDI device: %d (%s)\n", f, ftdi_get_error_string(ftdic));
	}
	return ret;
}

/* Returns 0 upon success, a negative number upon errors. */
static int ft2232_spi_send_command(struct flashctx *flash,
				   unsigned int writecnt, unsigned int readcnt,
				   const unsigned char *writearr,
				   unsigned char *readarr)
{
	struct ftdi_context *ftdic = &ftdic_context;
	static unsigned char *buf = NULL;
	/* failed is special. We use bitwise ops, but it is essentially bool. */
	int i = 0, ret = 0, failed = 0;
	int bufsize;
	static int oldbufsize = 0;

	if (writecnt > 65536 || readcnt > 65536)
		return SPI_INVALID_LENGTH;

	/* buf is not used for the response from the chip. */
	bufsize = max(writecnt + 9 + 6, 260 + 9 + 6);
	/* Never shrink. realloc() calls are expensive. */
	if (bufsize > oldbufsize) {
		buf = realloc(buf, bufsize);
		if (!buf) {
			msg_perr("Out of memory!\n");
			/* TODO: What to do with buf? */
			return SPI_GENERIC_ERROR;
		}
		oldbufsize = bufsize;
	}

	/*
	 * Minimize USB transfers by packing as many commands as possible
	 * together. If we're not expecting to read, we can assert CS#, write,
	 * and deassert CS# all in one shot. If reading, we do three separate
	 * operations.
	 */
	msg_pspew("Assert CS#\n");

	buf[i++] = SET_BITS_HIGH;
	buf[i++] = hold_bits; /* assertive */
	buf[i++] = hold_bits;

	buf[i++] = SET_BITS_LOW;
	buf[i++] = 0 | wp_bits; /* & ~cs_bits; assertive */
	buf[i++] = pindir | wp_bits;

	if (writecnt) {
		buf[i++] = MPSSE_DO_WRITE | MPSSE_WRITE_NEG;
		buf[i++] = (writecnt - 1) & 0xff;
		buf[i++] = ((writecnt - 1) >> 8) & 0xff;
		memcpy(buf + i, writearr, writecnt);
		i += writecnt;
	}

	/*
	 * Optionally terminate this batch of commands with a
	 * read command, then do the fetch of the results.
	 */
	if (readcnt) {
		buf[i++] = MPSSE_DO_READ;
		buf[i++] = (readcnt - 1) & 0xff;
		buf[i++] = ((readcnt - 1) >> 8) & 0xff;
		ret = send_buf(ftdic, buf, i);
		failed = ret;
		/* We can't abort here, we still have to deassert CS#. */
		if (ret)
			msg_perr("send_buf failed before read: %i\n", ret);
		i = 0;
		if (ret == 0) {
			/*
			 * FIXME: This is unreliable. There's no guarantee that
			 * we read the response directly after sending the read
			 * command. We may be scheduled out etc.
			 */
			ret = get_buf(ftdic, readarr, readcnt);
			failed |= ret;
			/* We can't abort here either. */
			if (ret)
				msg_perr("get_buf failed: %i\n", ret);
		}
	}

	msg_pspew("De-assert CS#\n");
	buf[i++] = SET_BITS_LOW;
	buf[i++] = cs_bits;
	buf[i++] = pindir;

	buf[i++] = SET_BITS_HIGH;
	buf[i++] = 0; /* assertive */
	buf[i++] = 0;

	ret = send_buf(ftdic, buf, i);
	failed |= ret;
	if (ret)
		msg_perr("send_buf failed at end: %i\n", ret);

	return failed ? -1 : 0;
}

static int ft2232_spi_send_multicommand(struct flashctx *flash,
				 struct spi_command *cmds)
{
	struct ftdi_context *ftdic = &ftdic_context;

	/* static buffer */
	static unsigned char *buf = NULL;
	static int oldbufsize = 0;

	/* failed is special. We use bitwise ops, but it is essentially bool. */
	int i = 0, ret, failed = 0;

	for (; (cmds->writecnt || cmds->readcnt) && !failed; cmds++) {

		int bufsize;

		if (cmds->writecnt > 65536 || cmds->readcnt > 65536)
			return SPI_INVALID_LENGTH;

		/* buf is not used for the response from the chip. */
		bufsize = max(i + cmds->writecnt + 9 + 6, 260 + 9 + 6);
		/* Never shrink. realloc() calls are expensive. */
		if (bufsize > oldbufsize) {
			buf = realloc(buf, bufsize);
			if (!buf) {
				msg_perr("Out of memory!\n");
				/* TODO: What to do with buf? */
				return SPI_GENERIC_ERROR;
			}
			oldbufsize = bufsize;
		}

		/*
		* Minimize USB transfers by packing as many commands as possible
		* together. If we're not expecting to read, we can assert CS#, write,
		* and deassert CS# all in one shot. If reading, we do three separate
		* operations.
		*/
		msg_pspew("Assert CS#\n");

		buf[i++] = SET_BITS_HIGH;
		buf[i++] = hold_bits; /* assertive */
		buf[i++] = hold_bits;

		buf[i++] = SET_BITS_LOW;
		buf[i++] = 0 | wp_bits; /* & ~cs_bits; assertive */
		buf[i++] = pindir | wp_bits;

		if (cmds->writecnt) {
			buf[i++] = MPSSE_DO_WRITE | MPSSE_WRITE_NEG;
			buf[i++] = (cmds->writecnt - 1) & 0xff;
			buf[i++] = ((cmds->writecnt - 1) >> 8) & 0xff;
			memcpy(buf + i, cmds->writearr, cmds->writecnt);
			i += cmds->writecnt;
		}

		/*
		* Optionally terminate this batch of commands with a
		* read command, then do the fetch of the results.
		*/
		if (cmds->readcnt) {
			buf[i++] = MPSSE_DO_READ;
			buf[i++] = (cmds->readcnt - 1) & 0xff;
			buf[i++] = ((cmds->readcnt - 1) >> 8) & 0xff;
			ret = send_buf(ftdic, buf, i);
			failed = ret;
			/* We can't abort here, we still have to deassert CS#. */
			if (ret)
				msg_perr("send_buf failed before read: %i\n", ret);
			i = 0;
			if (ret == 0) {
				/*
				* FIXME: This is unreliable. There's no guarantee
				* that we read the response directly after sending
				* the read command. We may be scheduled out etc.
				*/
				ret = get_buf(ftdic, cmds->readarr, cmds->readcnt);
				failed |= ret;
				/* We can't abort here either. */
				if (ret) msg_perr("get_buf failed: %i\n", ret);
			}
		}

		msg_pspew("De-assert CS#\n");
		buf[i++] = SET_BITS_LOW;
		buf[i++] = cs_bits;
		buf[i++] = pindir;

		buf[i++] = SET_BITS_HIGH;
		buf[i++] = 0; /* assertive */
		buf[i++] = 0;
	}

	if(i) {
		ret = send_buf(ftdic, buf, i);
		failed |= ret;
		if (ret) msg_perr("send_buf failed at end: %i\n", ret);
	}

	return failed ? -1 : 0;
}

/* FIXME: This function is optimized so that it does not split each transaction
 * into chip page_size long blocks unnecessarily like spi_read_chunked. This has
 * the advantage that it is much faster for most chips, but breaks those with
 * non-continuous reads. When spi_read_chunked is fixed this method can be removed. */
static int ft2232_spi_read(struct flashctx *flash, uint8_t *buf,
			    unsigned int start, unsigned int len)
{
	int ret;
	unsigned int i, cur_len;
	const unsigned int max_read = spi_master_ft2232.max_data_read;
	unsigned int percent_last = 0, percent_current = 0;
	int show_progress = 0;

	/* progress visualizaion init */
	if(len >= MIN_LENGTH_TO_SHOW_READ_PROGRESS) {
		msg_cinfo(" "); /* only this space will go to logfile but
				 all strings with \b wont. */
		msg_cinfo("\b 0%%");
		show_progress = 1; /* enable progress visualizaion */
	}

	for (i = 0; i < len; i += cur_len) {
		cur_len = min(max_read, (len - i));
		ret = (flash->chip->feature_bits & FEATURE_4BA_SUPPORT) == 0
			? spi_nbyte_read(flash, start + i, buf + i, cur_len)
			: flash->chip->four_bytes_addr_funcs.read_nbyte(flash,
						 start + i, buf + i, cur_len);
		if (ret)
			break;

		if(show_progress) {
			percent_current = (unsigned int)
			    (((unsigned long long)(i + cur_len)) * 100 / len);
			if(percent_current != percent_last) {
				msg_cinfo("\b\b\b%2d%%", percent_current);
				percent_last = percent_current;
			}
		}
	}

	if(show_progress && !ret)
		msg_cinfo("\b\b\b\b"); /* remove progress percents from the screen */

	return ret;
}

#endif
