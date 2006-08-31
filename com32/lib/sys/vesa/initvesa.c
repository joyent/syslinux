/* ----------------------------------------------------------------------- *
 *
 *   Copyright 1999-2006 H. Peter Anvin - All Rights Reserved
 *
 *   Permission is hereby granted, free of charge, to any person
 *   obtaining a copy of this software and associated documentation
 *   files (the "Software"), to deal in the Software without
 *   restriction, including without limitation the rights to use,
 *   copy, modify, merge, publish, distribute, sublicense, and/or
 *   sell copies of the Software, and to permit persons to whom
 *   the Software is furnished to do so, subject to the following
 *   conditions:
 *
 *   The above copyright notice and this permission notice shall
 *   be included in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 *   OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 *   HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 *   WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 *   OTHER DEALINGS IN THE SOFTWARE.
 *
 * ----------------------------------------------------------------------- */

/*
 * initvesa.c
 *
 * Query the VESA BIOS and select a 640x480x32 mode with local mapping
 * support, if one exists.
 */

#include <inttypes.h>
#include <com32.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include "vesa.h"
#include "video.h"

struct vesa_info __vesa_info;

struct vesa_char *__vesacon_text_display;

int __vesacon_font_height, __vesacon_text_rows;
uint8_t __vesacon_graphics_font[FONT_MAX_CHARS][FONT_MAX_HEIGHT];

uint32_t __vesacon_background[VIDEO_Y_SIZE][VIDEO_X_SIZE];

ssize_t __serial_write(void *fp, const void *buf, size_t count);

static void debug(const char *str, ...)
{
  va_list va;
  char buf[65536];
  size_t len;

  va_start(va, str);
  len = vsnprintf(buf, sizeof buf, str, va);
  va_end(va);

  if (len >= sizeof buf)
    len = sizeof buf - 1;

  __serial_write(NULL, buf, len);
}

static void unpack_font(uint8_t *dst, uint8_t *src, int height)
{
  int i;

  for (i = 0; i < FONT_MAX_CHARS; i++) {
    memcpy(dst, src, height);
    memset(dst+height, 0, FONT_MAX_HEIGHT-height);

    dst += FONT_MAX_HEIGHT;
    src += height;
  }
}

static int vesacon_set_mode(void)
{
  com32sys_t rm;
  uint8_t *rom_font;
  uint16_t mode, *mode_ptr;
  struct vesa_general_info *gi;
  struct vesa_mode_info *mi;

  /* Allocate space in the bounce buffer for these structures */
  gi = &((struct vesa_info *)__com32.cs_bounce)->gi;
  mi = &((struct vesa_info *)__com32.cs_bounce)->mi;

  debug("Hello, World!\r\n");

  memset(&rm, 0, sizeof rm);

  gi->signature = VBE2_MAGIC;	/* Get VBE2 extended data */
  rm.eax.w[0] = 0x4F00;		/* Get SVGA general information */
  rm.edi.w[0] = OFFS(gi);
  rm.es      = SEG(gi);
  __intcall(0x10, &rm, &rm);

  if ( rm.eax.w[0] != 0x004F )
    return 1;			/* Function call failed */
  if ( gi->signature != VESA_MAGIC )
    return 2;			/* No magic */
  if ( gi->version < 0x0200 ) {
    return 3;			/* VESA 2.0 not supported */
  }

  /* Search for a 640x480 32-bit linear frame buffer mode */
  mode_ptr = GET_PTR(gi->video_mode_ptr);

  for(;;) {
    if ((mode = *mode_ptr++) == 0xFFFF)
      return 4;			/* No mode found */

    debug("Found mode: 0x%04x\r\n", mode);

    rm.eax.w[0] = 0x4F01;	/* Get SVGA mode information */
    rm.ecx.w[0] = mode;
    rm.edi.w[0] = OFFS(mi);
    rm.es  = SEG(mi);
    __intcall(0x10, &rm, &rm);

    /* Must be a supported mode */
    if ( rm.eax.w[0] != 0x004f )
      continue;

    debug("mode_attr 0x%04x, h_res = %4d, v_res = %4d, bpp = %2d, layout = %d (%d,%d,%d)\r\n",
	  mi->mode_attr, mi->h_res, mi->v_res, mi->bpp, mi->memory_layout,
	  mi->rpos, mi->gpos, mi->bpos);

    /* Must be an LFB color graphics mode supported by the hardware */
    if ( (mi->mode_attr & 0x0099) != 0x0099 )
      continue;
    /* Must be 640x480, 32 bpp */
    if ( mi->h_res != VIDEO_X_SIZE || mi->v_res != VIDEO_Y_SIZE ||
	 mi->bpp != 32 )
      continue;
    /* Must either be a packed-pixel mode or a direct color mode
       (depending on VESA version ) */
    if ( mi->memory_layout != 4 && /* Packed pixel */
	 (mi->memory_layout != 6 || mi->rpos != 16 ||
	  mi->gpos != 8 || mi->bpos != 0) )
      continue;

    /* Hey, looks like we found something we can live with */
    break;
  }

  /* Download the BIOS-provided font */
  rm.eax.w[0] = 0x1130;		/* Get Font Information */
  rm.ebx.w[0] = 0x0600;		/* Get 8x16 ROM font */
  __intcall(0x10, &rm, &rm);
  rom_font = MK_PTR(rm.es, rm.ebp.w[0]);
  __vesacon_font_height = 16;
  unpack_font((uint8_t *)__vesacon_graphics_font, rom_font, 16);
  __vesacon_text_rows = (VIDEO_Y_SIZE-2*VIDEO_BORDER)/__vesacon_font_height;

  /* Now set video mode */
  rm.eax.w[0] = 0x4F02;		/* Set SVGA video mode */
  rm.ebx.w[0] = mode | 0xC000;	/* Don't clear video RAM, use linear fb */
  __intcall(0x10, &rm, &rm);
  if ( rm.eax.w[0] != 0x004F ) {
    rm.eax.w[0] = 0x0003;	/* Set regular text mode */
    __intcall(0x10, &rm, NULL);
    return 9;			/* Failed to set mode */
  }

  /* Copy established state out of the bounce buffer */
  memcpy(&__vesa_info, __com32.cs_bounce, sizeof __vesa_info);

  return 0;
}


static int init_text_display(void)
{
  size_t nchars;
  struct vesa_char *ptr;

  nchars = (TEXT_PIXEL_ROWS/__vesacon_font_height+2)*
    (TEXT_PIXEL_COLS/FONT_WIDTH+2);

  __vesacon_text_display = ptr = malloc(nchars*sizeof(struct vesa_char));

  if (!ptr)
    return -1;

  /* I really which C had a memset() for larger-than-bytes objects... */
  asm volatile("cld; rep; stosl"
	       : "+D" (ptr), "+c" (nchars)
	       : "a" (' '+(0x07 << 8)+(SHADOW_NORMAL << 16))
	       : "memory");

  return 0;
}

int __vesacon_init(void)
{
  int rv;

  rv = vesacon_set_mode();
  if (rv)
    return rv;

  init_text_display();

  debug("Mode set, now drawing at %#p\n", __vesa_info.mi.lfb_ptr);

  __vesacon_init_background();

  debug("Ready!\r\n");
  return 0;
}
