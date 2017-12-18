/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2011-2012 Stefan Tauner
 * Copyright (C) 2014 T-platforms JSC
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

/*
 *   History of changes:
 *	05/01/2015  Add compliance to JESD216B standard and SFDP revision 1.6
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "flash.h"
#include "spi.h"
#include "spi4ba.h"
#include "chipdrivers.h"

/* Default four bytes addressing behavior:
   1) 4-Bytes Addressing Mode (FBA_USE_EXT_ADDR_REG_BY_DEFAULT not defined)
   2) 3-bytes mode with Ext.Addr.Register (FBA_USE_EXT_ADDR_REG_BY_DEFAULT defined) */
/* #define FBA_USE_EXT_ADDR_REG_BY_DEFAULT 1 */

static int spi_sfdp_read_sfdp_chunk(struct flashctx *flash, uint32_t address, uint8_t *buf, int len)
{
	int i, ret;
	uint8_t *newbuf;
	const unsigned char cmd[JEDEC_SFDP_OUTSIZE] = {
		JEDEC_SFDP,
		(address >> 16) & 0xff,
		(address >> 8) & 0xff,
		(address >> 0) & 0xff,
		/* FIXME: the following dummy byte explodes on some programmers.
		 * One workaround is to read the dummy byte
		 * instead and discard its value.
		 */
		0
	};
	msg_cspew("%s: addr=0x%x, len=%d, data:\n", __func__, address, len);
	newbuf = malloc(len + 1);
	if (!newbuf)
		return SPI_PROGRAMMER_ERROR;
	ret = spi_send_command(flash, sizeof(cmd) - 1, len + 1, cmd, newbuf);
	memmove(buf, newbuf + 1, len);
	free(newbuf);
	if (ret)
		return ret;
	for (i = 0; i < len; i++)
		msg_cspew(" 0x%02x", buf[i]);
	msg_cspew("\n");
	return 0;
}

static int spi_sfdp_read_sfdp(struct flashctx *flash, uint32_t address, uint8_t *buf, int len)
{
	/* FIXME: There are different upper bounds for the number of bytes to
	 * read on the various programmers (even depending on the rest of the
	 * structure of the transaction). 2 is a safe bet. */
	int maxstep = 2;
	int ret = 0;
	while (len > 0) {
		int step = min(len, maxstep);
		ret = spi_sfdp_read_sfdp_chunk(flash, address, buf, step);
		if (ret)
			return ret;
		address += step;
		buf += step;
		len -= step;
	}
	return ret;
}

struct sfdp_tbl_hdr {
	uint16_t id;
	uint8_t v_minor;
	uint8_t v_major;
	uint8_t len;
	uint32_t ptp; /* 24b pointer */
};

static int sfdp_add_uniform_eraser(struct flashchip *chip, int eraser_type, uint8_t opcode, uint32_t block_size)
{
	int i;
	uint32_t total_size = chip->total_size * 1024;

	/* choosing different eraser functions for 3-bytes and 4-bytes addressing */
	erasefunc_t *erasefn = (chip->feature_bits & FEATURE_4BA_SUPPORT) ?
		spi_get_erasefn_from_opcode_4ba(opcode) : spi_get_erasefn_from_opcode(opcode);

	if (erasefn == NULL || total_size == 0 || block_size == 0 ||
	    total_size % block_size != 0) {
		msg_cdbg("%s: invalid input, please report to "
			 "flashrom@flashrom.org\n", __func__);
		return 1;
	}

	for (i = 0; i < NUM_ERASEFUNCTIONS; i++) {
		struct block_eraser *eraser = &chip->block_erasers[i];
		/* Check for duplicates (including (some) non-uniform ones). */
		if (eraser->eraseblocks[0].size == block_size &&
		    eraser->block_erase == erasefn) {
			msg_cdbg2("  Tried to add a duplicate block eraser: "
				  "%d x %d B with opcode 0x%02x.\n",
				  total_size/block_size, block_size, opcode);
			return 1;
		}
		if (eraser->eraseblocks[0].size != 0 ||
		    eraser->block_erase != NULL) {
			msg_cspew("  Block Eraser %d is already occupied.\n",
				  i);
			continue;
		}

		eraser->type = eraser_type;
		eraser->block_erase = erasefn;
		eraser->eraseblocks[0].size = block_size;
		eraser->eraseblocks[0].count = total_size/block_size;
		msg_cdbg2("  Block eraser %d: %d x %d B with opcode "
			  "0x%02x\n", i, total_size/block_size, block_size,
			  opcode);
		return 0;
	}
	msg_cinfo("%s: Not enough space to store another eraser (i=%d)."
		  " Please report this at flashrom@flashrom.org\n",
		  __func__, i);
	return 1;
}

/* Try of replace exist erasers to new direct 4-bytes addressing erasers
   which can be called from ANY addressing mode: 3-byte or 4-bytes.
   These erasers opcodes defines in SFDP 4-byte address instruction table
   from SFDP revision 1.6 that is defined by JESD216B standard. */
static int sfdp_change_uniform_eraser_4ba_direct(struct flashchip *chip, int eraser_type, uint8_t opcode)
{
	int i;
	erasefunc_t *erasefn = spi_get_erasefn_from_opcode_4ba_direct(opcode);

	if (erasefn == NULL) {
		msg_cdbg("%s: invalid input, please report to "
			 "flashrom@flashrom.org\n", __func__);
		return 1;
	}

	for (i = 0; i < NUM_ERASEFUNCTIONS; i++) {
		struct block_eraser *eraser = &chip->block_erasers[i];
		if (eraser->eraseblocks[0].size == 0)
			break;
		if (eraser->type != eraser_type)
			continue;

		eraser->block_erase = erasefn;
		msg_cdbg2("  Block eraser %d (type %d) changed to opcode "
			  "0x%02x\n", i, eraser_type, opcode);
		return 0;
	}

	msg_cspew("%s: Block Eraser type %d isn't found."
		  " Please report this at flashrom@flashrom.org\n",
		  __func__, eraser_type);
	return 1;
}

/* Parse of JEDEC SFDP Basic Flash Parameter Table */
static int sfdp_fill_flash(struct flashchip *chip, uint8_t *buf, uint16_t len, int sfdp_rev_16)
{
	uint8_t opcode_4k_erase = 0xFF;
	uint32_t tmp32;
	uint8_t tmp8;
	uint32_t total_size; /* in bytes */
	uint32_t block_size;
	int j;

	msg_cdbg("Parsing JEDEC flash parameter table... ");
	if (len != 16 * 4 && len != 9 * 4 && len != 4 * 4) {
		msg_cdbg("%s: len out of spec\n", __func__);
		return 1;
	}
	msg_cdbg2("\n");

	/* 1. double word */
	tmp32 =  ((unsigned int)buf[(4 * 0) + 0]);
	tmp32 |= ((unsigned int)buf[(4 * 0) + 1]) << 8;
	tmp32 |= ((unsigned int)buf[(4 * 0) + 2]) << 16;
	tmp32 |= ((unsigned int)buf[(4 * 0) + 3]) << 24;

	chip->feature_bits = 0;

	tmp8 = (tmp32 >> 17) & 0x3;
	switch (tmp8) {
	case 0x0:
		msg_cdbg2("  3-Byte only addressing.\n");
		break;
	case 0x1:
		msg_cdbg2("  3-Byte (and optionally 4-Byte) addressing.\n");
#ifndef FBA_USE_EXT_ADDR_REG_BY_DEFAULT
		/* assuming that 4-bytes addressing mode can be entered
		   by CMD B7h preceded with WREN and all read, write and
		   erase commands will be able to receive 4-bytes address */
		chip->feature_bits |= FEATURE_4BA_SUPPORT;
		chip->four_bytes_addr_funcs.enter_4ba = spi_enter_4ba_b7_we;
		chip->four_bytes_addr_funcs.program_byte = spi_byte_program_4ba;
		chip->four_bytes_addr_funcs.program_nbyte = spi_nbyte_program_4ba;
		chip->four_bytes_addr_funcs.read_nbyte = spi_nbyte_read_4ba;
#else /* if FBA_USE_EXT_ADDR_REG_BY_DEFAULT defined */
		/* assuming that 4-bytes addressing is working using
		   extended address register which can be assigned
		   throught CMD C5h and then all commands will use
		   3-bytes address as usual */
		chip->feature_bits |= ( FEATURE_4BA_SUPPORT |
					FEATURE_4BA_EXTENDED_ADDR_REG );
		chip->four_bytes_addr_funcs.enter_4ba = NULL;
		chip->four_bytes_addr_funcs.program_byte = spi_byte_program_4ba_ereg;
		chip->four_bytes_addr_funcs.program_nbyte = spi_nbyte_program_4ba_ereg;
		chip->four_bytes_addr_funcs.read_nbyte = spi_nbyte_read_4ba_ereg;
#endif /* FBA_USE_EXT_ADDR_REG_BY_DEFAULT */
		break;
	case 0x2:
		msg_cdbg2("  4-Byte only addressing.\n");
		chip->feature_bits |= ( FEATURE_4BA_SUPPORT |
					FEATURE_4BA_ONLY );
		chip->four_bytes_addr_funcs.enter_4ba = NULL;
		chip->four_bytes_addr_funcs.program_byte = spi_byte_program_4ba;
		chip->four_bytes_addr_funcs.program_nbyte = spi_nbyte_program_4ba;
		chip->four_bytes_addr_funcs.read_nbyte = spi_nbyte_read_4ba;
		break;
	default:
		msg_cdbg("  Required addressing mode (0x%x) not supported.\n",
			 tmp8);
		return 1;
	}

	msg_cdbg2("  Status register is ");
	if (tmp32 & (1 << 3)) {
		msg_cdbg2("volatile and writes to the status register have to "
			  "be enabled with ");
		if (tmp32 & (1 << 4)) {
			chip->feature_bits |= FEATURE_WRSR_WREN;
			msg_cdbg2("WREN (0x06).\n");
		} else {
			chip->feature_bits |= FEATURE_WRSR_EWSR;
			msg_cdbg2("EWSR (0x50).\n");
		}
	} else {
		msg_cdbg2("non-volatile and the standard does not allow "
			  "vendors to tell us whether EWSR/WREN is needed for "
			  "status register writes - assuming EWSR.\n");
			chip->feature_bits |= FEATURE_WRSR_EWSR;
		}

	msg_cdbg2("  Write chunk size is ");
	if (tmp32 & (1 << 2)) {
		msg_cdbg2("at least 64 B.\n");
		chip->page_size = 64;
		chip->write = spi_chip_write_256;
	} else {
		msg_cdbg2("1 B only.\n");
		chip->page_size = 256;
		chip->write = spi_chip_write_1;
	}

	if ((tmp32 & 0x3) == 0x1) {
		opcode_4k_erase = (tmp32 >> 8) & 0xFF;
		msg_cspew("  4kB erase opcode is 0x%02x.\n", opcode_4k_erase);
		/* add the eraser later, because we don't know total_size yet */
	} else
		msg_cspew("  4kB erase opcode is not defined.\n");

	/* 2. double word */
	tmp32 =  ((unsigned int)buf[(4 * 1) + 0]);
	tmp32 |= ((unsigned int)buf[(4 * 1) + 1]) << 8;
	tmp32 |= ((unsigned int)buf[(4 * 1) + 2]) << 16;
	tmp32 |= ((unsigned int)buf[(4 * 1) + 3]) << 24;

	if (tmp32 & (1 << 31)) {
		msg_cdbg("Flash chip size >= 4 Gb/512 MB not supported.\n");
		return 1;
	}
	total_size = ((tmp32 & 0x7FFFFFFF) + 1) / 8;
	chip->total_size = total_size / 1024;
	msg_cdbg2("  Flash chip size is %d kB.\n", chip->total_size);

	if (total_size > (1 << 24)) {
		if(!sfdp_rev_16) {
			msg_cdbg("Flash chip size is bigger than what 3-Byte addressing "
				 "can access but chip's SFDP revision is lower than 1.6 "
				 "\nConsequently 4-bytes addressing can NOT be "
				 "properly configured using current SFDP information.\n");
#ifndef FBA_USE_EXT_ADDR_REG_BY_DEFAULT
			msg_cdbg("Assuming that 4-bytes addressing mode can be "
				 "entered by CMD B7h with WREN.\n");
#else
			msg_cdbg("Assuming that 4-bytes addressing is working via "
				 "an Extended Address Register which can be written "
				 "by CMD C5h.\n");
#endif
		}
	}

	/* FIXME: double words 3-7 contain unused fast read information */

	if (len < 9 * 4) {
		msg_cdbg("  It seems like this chip supports the preliminary "
			 "Intel version of SFDP, skipping processing of double "
			 "words 3-9.\n");

		/* in the case if BFPT erasers array is not present
		   trying to add default 4k-eraser */
		if (opcode_4k_erase != 0xFF)
			sfdp_add_uniform_eraser(chip, 0, opcode_4k_erase, 4 * 1024);

		goto done;
	}

	/* 8. double word & 9. double word */
	/* for by block eraser types, from Type 1 to Type 4 */
	for (j = 0; j < 4; j++) {
		/* 7 double words from the start + 2 bytes for every eraser */
		tmp8 = buf[(4 * 7) + (j * 2)];
		msg_cspew("   Erase Sector (Type %d) Size: 0x%02x\n", j + 1, tmp8);
		if (tmp8 == 0) {
			msg_cspew("  Erase Sector (Type %d) is unused.\n", j + 1);
			continue;
		}
		if (tmp8 >= 31) {
			msg_cdbg2("  Block size of erase Sector (Type %d): 2^%d "
				 "is too big for flashrom.\n", j + 1, tmp8);
			continue;
		}
		block_size = 1 << (tmp8); /* block_size = 2 ^ field */

		tmp8 = buf[(4 * 7) + (j * 2) + 1];
		msg_cspew("   Erase Sector (Type %d) Opcode: 0x%02x\n", j + 1, tmp8);
		sfdp_add_uniform_eraser(chip, j + 1, tmp8, block_size);
	}

	/* Trying to add the default 4k eraser after parsing erasers info.
	   In most cases this eraser has already been added before. */
	if (opcode_4k_erase != 0xFF)
		sfdp_add_uniform_eraser(chip, 0, opcode_4k_erase, 4 * 1024);

	/* Trying to read the exact page size if it's available */
	if (len >= 11 * 4) {
		/* 11. double word */
		tmp8 = buf[(4*10) + 0] >> 4; /* get upper nibble of LSB of 11th dword */
		chip->page_size = 1 << tmp8; /* page_size = 2 ^ N */
		msg_cdbg2("  Page size is %d B.\n", chip->page_size);
	}

	/* If the chip doesn't support 4-bytes addressing mode we don't have
	   to read and analyze 16th DWORD of Basic Flash Parameter Table */
	if (!(chip->feature_bits & FEATURE_4BA_SUPPORT))
		goto done;

	/* In the case if the chip is working in 4-bytes addressing mode ONLY we
	   don't have to read and analyze 16th DWORD of Basic Flash Parameter Table
	   because we don't have to know how to switch to 4-bytes mode and back
	   when we are already in 4-bytes mode permanently. */
	if (chip->feature_bits & FEATURE_4BA_ONLY)
		goto done;

	/* If the SFDP revision supported by the chip is lower that 1.6
	   we can not read and analyze 16th DWORD of Basic Flash Parameter Table.
	   Using defaults by FBA_USE_EXT_ADDR_REG_BY_DEFAULT define. */
	if(!sfdp_rev_16)
		goto done;

	if (len < 16 * 4) {
		msg_cdbg("%s: len of BFPT is out of spec\n", __func__);
		msg_cerr("ERROR: Unable read 4-bytes addressing parameters.\n");
		return 1;
	}

	/* 16. double word */
	tmp32 =  ((unsigned int)buf[(4 * 15) + 0]);
	tmp32 |= ((unsigned int)buf[(4 * 15) + 1]) << 8;
	tmp32 |= ((unsigned int)buf[(4 * 15) + 2]) << 16;
	tmp32 |= ((unsigned int)buf[(4 * 15) + 3]) << 24;

	/* Parsing 16th DWORD of Basic Flash Parameter Table according to JESD216B */

	if(tmp32 & JEDEC_BFPT_DW16_ENTER_B7) {
		msg_cdbg2("  Enter 4-bytes addressing mode by CMD B7h\n");
		chip->four_bytes_addr_funcs.enter_4ba = spi_enter_4ba_b7;
		chip->four_bytes_addr_funcs.program_byte = spi_byte_program_4ba;
		chip->four_bytes_addr_funcs.program_nbyte = spi_nbyte_program_4ba;
		chip->four_bytes_addr_funcs.read_nbyte = spi_nbyte_read_4ba;
		/* if can go to 4BA-mode -> not need to use Ext.Addr.Reg */
		chip->feature_bits &= ~FEATURE_4BA_EXTENDED_ADDR_REG;
	}
	else if(tmp32 & JEDEC_BFPT_DW16_ENTER_B7_WE) {
		msg_cdbg2("  Enter 4-bytes addressing mode by CMD B7h with WREN\n");
		chip->four_bytes_addr_funcs.enter_4ba = spi_enter_4ba_b7_we;
		chip->four_bytes_addr_funcs.program_byte = spi_byte_program_4ba;
		chip->four_bytes_addr_funcs.program_nbyte = spi_nbyte_program_4ba;
		chip->four_bytes_addr_funcs.read_nbyte = spi_nbyte_read_4ba;
		/* if can go to 4BA-mode -> not need to use Ext.Addr.Reg */
		chip->feature_bits &= ~FEATURE_4BA_EXTENDED_ADDR_REG;
	}
	else if(tmp32 & JEDEC_BFPT_DW16_ENTER_EXTENDED_ADDR_REG) {
		msg_cdbg2("  Extended Address Register used for 4-bytes addressing\n");
		chip->four_bytes_addr_funcs.enter_4ba = NULL;
		chip->four_bytes_addr_funcs.program_byte = spi_byte_program_4ba_ereg;
		chip->four_bytes_addr_funcs.program_nbyte = spi_nbyte_program_4ba_ereg;
		chip->four_bytes_addr_funcs.read_nbyte = spi_nbyte_read_4ba_ereg;
		/* this flag signals to all '*_selector' functions
		   to use Ext.Addr.Reg while erase operations */
		chip->feature_bits |= FEATURE_4BA_EXTENDED_ADDR_REG;
	}
	else {
		msg_cerr("ERROR: Unable to use 4-bytes addressing for this chip.\n"
			 " Please report this at flashrom@flashrom.org\n\n");
		return 1;
	}

done:
	msg_cdbg("done.\n");
	return 0;
}

/* Parse of JEDEC SFDP 4-byte address instruction table. From SFDP revision 1.6 only.
   This parsing shoukd be called after basic flash parameter table is parsed. */
static int sfdp_parse_4ba_table(struct flashchip *chip, uint8_t *buf, uint16_t len)
{
	uint32_t tmp32;
	uint8_t tmp8;
	int j, direct_erasers;
	int direct_count;

	msg_cdbg("Parsing JEDEC 4-byte address instuction table... ");
	if (len != 2 * 4) {
		msg_cdbg("%s: len out of spec\n", __func__);
		return 1;
	}
	msg_cdbg2("\n");

	/* 1. double word */
	tmp32 =  ((unsigned int)buf[(4 * 0) + 0]);
	tmp32 |= ((unsigned int)buf[(4 * 0) + 1]) << 8;
	tmp32 |= ((unsigned int)buf[(4 * 0) + 2]) << 16;
	tmp32 |= ((unsigned int)buf[(4 * 0) + 3]) << 24;

	direct_count = 0;

	if(tmp32 & JEDEC_4BAIT_READ_SUPPORT) {
		msg_cdbg2("  Found Read CMD 13h with 4-bytes address\n");
		chip->four_bytes_addr_funcs.read_nbyte = spi_nbyte_read_4ba_direct;
		/* read function has changed to direct 4-bytes function,
		   so entering 4-bytes mode isn't required for reading bytes */
		chip->feature_bits |= FEATURE_4BA_DIRECT_READ;
		direct_count++;
	}

	if(tmp32 & JEDEC_4BAIT_PROGRAM_SUPPORT) {
		msg_cdbg2("  Found Write CMD 12h with 4-bytes address\n");
		chip->four_bytes_addr_funcs.program_byte = spi_byte_program_4ba_direct;
		chip->four_bytes_addr_funcs.program_nbyte = spi_nbyte_program_4ba_direct;
		/* write (program) functions have changed to direct 4-bytes functions,
		   so entering 4-bytes mode isn't required for writing bytes */
		chip->feature_bits |= FEATURE_4BA_DIRECT_WRITE;
		direct_count++;
	}

	direct_erasers = 0;

	/* 2. double word */
	for (j = 0; j < 4; j++) {
		if(!(tmp32 & (JEDEC_4BAIT_ERASE_TYPE_1_SUPPORT << j)))
			continue;

		tmp8 = buf[(4 * 1) + j];

		msg_cdbg2("  Found Erase (type %d) CMD %02Xh with 4-bytes address\n", j + 1, tmp8);

		if(tmp8 == 0xFF) {
			msg_cdbg("%s: Eraser (type %d) is supported, but opcode = 0xFF\n"
				 "  Please report to flashrom@flashrom.org\n\n", __func__, j + 1);
			continue;
		}

		/* try of replacing the eraser with direct 4-bytes eraser */
		if(!sfdp_change_uniform_eraser_4ba_direct(chip, j + 1, tmp8))
			direct_erasers++;
	}

	for (j = 0; j < NUM_ERASEFUNCTIONS; j++) {
		if (chip->block_erasers[j].eraseblocks[0].size == 0)
			break;
	}

	if( j == direct_erasers ) {
		/* if all erasers have been changed to direct 4-bytes ones,
		   then we don't have to enter 4-bytes mode for erase */
		chip->feature_bits |= FEATURE_4BA_ALL_ERASERS_DIRECT;
		direct_count++;
		msg_cspew("All erasers have changed to direct ones.\n");
	}

	if( direct_count == 3 ) {
		/* if all read/write/erase functions are direct 4-bytes now,
		   then we don't have to use extended address register */
		chip->feature_bits &= ~FEATURE_4BA_EXTENDED_ADDR_REG;
		msg_cspew("All read/write/erase functions have changed to direct ones.\n");
	}

	msg_cdbg("done.\n");
	return 0;
}

int probe_spi_sfdp(struct flashctx *flash)
{
	int ret = 0;
	uint8_t buf[8];
	uint32_t tmp32;
	uint8_t nph;
	/* need to limit the table loop by comparing i to uint8_t nph hence: */
	uint16_t i;
	struct sfdp_tbl_hdr *hdrs;
	uint8_t *hbuf;
	uint8_t *tbuf;
	int sfdp_rev_16 = 0;

	if (spi_sfdp_read_sfdp(flash, 0x00, buf, 4)) {
		msg_cdbg("Receiving SFDP signature failed.\n");
		return 0;
	}
	tmp32 = buf[0];
	tmp32 |= ((unsigned int)buf[1]) << 8;
	tmp32 |= ((unsigned int)buf[2]) << 16;
	tmp32 |= ((unsigned int)buf[3]) << 24;

	if (tmp32 != 0x50444653) {
		msg_cdbg2("Signature = 0x%08x (should be 0x50444653)\n", tmp32);
		msg_cdbg("No SFDP signature found.\n");
		return 0;
	}

	if (spi_sfdp_read_sfdp(flash, 0x04, buf, 3)) {
		msg_cdbg("Receiving SFDP revision and number of parameter "
			 "headers (NPH) failed. ");
		return 0;
	}
	msg_cdbg2("SFDP revision = %d.%d\n", buf[1], buf[0]);
	if (buf[1] != 0x01) {
		msg_cdbg("The chip supports an unknown version of SFDP. "
			  "Aborting SFDP probe!\n");
		return 0;
	}

	/* JEDEC JESD216B defines SFDP revision 1.6 and includes:
	   1) 16 dwords in Basic Flash Parameter Table
	   2) 16th dword has information how to enter
		and exit 4-bytes addressing mode
	   3) 4-Bytes Address Instruction Table with ID 0xFF84
	*/
	sfdp_rev_16 = (buf[1] == 1 && buf[0] >= 6) || buf[1] > 1;

	nph = buf[2];
	msg_cdbg2("SFDP number of parameter headers is %d (NPH = %d).\n",
		  nph + 1, nph);

	/* Fetch all parameter headers, even if we don't use them all (yet). */
	hbuf = malloc((nph + 1) * 8);
	hdrs = malloc((nph + 1) * sizeof(struct sfdp_tbl_hdr));
	if (hbuf == NULL || hdrs == NULL ) {
		msg_gerr("Out of memory!\n");
		goto cleanup_hdrs;
	}
	if (spi_sfdp_read_sfdp(flash, 0x08, hbuf, (nph + 1) * 8)) {
		msg_cdbg("Receiving SFDP parameter table headers failed.\n");
		goto cleanup_hdrs;
	}

	for (i = 0; i <= nph; i++) {
		uint16_t len;
		hdrs[i].id = hbuf[(8 * i) + 0]; /* ID LSB read */
		hdrs[i].v_minor = hbuf[(8 * i) + 1];
		hdrs[i].v_major = hbuf[(8 * i) + 2];
		hdrs[i].len = hbuf[(8 * i) + 3];
		hdrs[i].ptp = hbuf[(8 * i) + 4];
		hdrs[i].ptp |= ((unsigned int)hbuf[(8 * i) + 5]) << 8;
		hdrs[i].ptp |= ((unsigned int)hbuf[(8 * i) + 6]) << 16;
		hdrs[i].id |= ((uint16_t)hbuf[(8 * i) + 7]) << 8; /* ID MSB read */
		msg_cdbg2("\nSFDP parameter table header %d/%d:\n", i+1, nph+1);
		msg_cdbg2("  ID 0x%02x, version %d.%d\n", hdrs[i].id,
			  hdrs[i].v_major, hdrs[i].v_minor);
		len = hdrs[i].len * 4;
		tmp32 = hdrs[i].ptp;
		msg_cdbg2("  Length %d B, Parameter Table Pointer 0x%06x\n",
			  len, tmp32);

		if (tmp32 + len >= (1 << 24)) {
			msg_cdbg("SFDP Parameter Table %d supposedly overflows "
				  "addressable SFDP area. This most\nprobably "
				  "indicates a corrupt SFDP parameter table "
				  "header. Skipping it.\n", i);
			continue;
		}

		tbuf = malloc(len);
		if (tbuf == NULL) {
			msg_gerr("Out of memory!\n");
			goto cleanup_hdrs;
		}
		if (spi_sfdp_read_sfdp(flash, tmp32, tbuf, len)){
			msg_cdbg("Fetching SFDP parameter table %d failed.\n",
				 i);
			free(tbuf);
			continue;
		}
		msg_cspew("  Parameter table contents:\n");
		for (tmp32 = 0; tmp32 < len; tmp32++) {
			if ((tmp32 % 8) == 0) {
				msg_cspew("    0x%04x: ", tmp32);
			}
			msg_cspew(" %02x", tbuf[tmp32]);
			if ((tmp32 % 8) == 7) {
				msg_cspew("\n");
				continue;
			}
			if ((tmp32 % 8) == 3) {
				msg_cspew(" ");
				continue;
			}
		}
		msg_cspew("\n");

		if (i == 0) { /* Mandatory JEDEC SFDP parameter table */
			if (hdrs[i].id != JEDEC_BFPT_ID)
				msg_cdbg("ID of the mandatory JEDEC SFDP "
					 "parameter table is not 0xFF00 as"
					 "demanded by JESD216 (warning only)."
					 "\n");
			if (hdrs[i].v_major != 0x01) {
				msg_cdbg("The chip contains an unknown "
					  "version of the JEDEC flash "
					  "parameters table, skipping it.\n");
			} else if (len != 16 * 4 && len != 9 * 4 && len != 4 * 4) {
				msg_cdbg("Length of the mandatory JEDEC SFDP "
					 "parameter table is wrong (%d B), "
					 "skipping it.\n", len);
			} else if (sfdp_fill_flash(flash->chip, tbuf, len, sfdp_rev_16) == 0)
				ret = 1;
		}
		/* JEDEC SFDP 4-byte address instruction table. From SFDP revision 1.6 only.
		   This parsing shoukd be called after basic flash parameter table is parsed. */
		else if(sfdp_rev_16 && hdrs[i].id == JEDEC_4BAIT_ID && ret == 1) {
			if (hdrs[i].v_major != 0x01) {
				msg_cdbg("The chip contains an unknown "
					  "version of the JEDEC 4-bytes "
					  "address instruction table, "
					  "skipping it.\n");
			}
			else {  /* no result check because this table is optional */
				sfdp_parse_4ba_table(flash->chip, tbuf, len);
			}
		}
		free(tbuf);
	}

cleanup_hdrs:
	free(hdrs);
	free(hbuf);
	return ret;
}
