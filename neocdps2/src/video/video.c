/*******************************************
**** VIDEO.C - Video Hardware Emulation ****
*******************************************/

//-- Include Files -----------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "video.h"
#include "../gs/g2.h"
#include "../gs/gs.h"
#include "../neocd.h"

// bmp resources
#include "resources/splash.h"
#include "resources/insert.h"
#include "resources/loading.h"

//-- Defines -----------------------------------------------------------------
#define VIDEO_TEXT	0
#define VIDEO_NORMAL	1
#define	VIDEO_SCANLINES	2

#define	LINE_BEGIN	mydword = *((int *)fspr)
#define LINE_MID	mydword = *((int *)fspr+1)
#define PIXEL_LAST	col = (mydword&0x0F); if (col) *bm = paldata[col]
#define PIXEL_R		col = (mydword&0x0F); if (col) *bm = paldata[col]; bm--
#define PIXEL_F		col = (mydword&0x0F); if (col) *bm = paldata[col]; bm++
#define SHIFT7		mydword >>= 28
#define SHIFT6		mydword >>= 24
#define SHIFT5		mydword >>= 20
#define SHIFT4		mydword >>= 16
#define SHIFT3		mydword >>= 12
#define SHIFT2		mydword >>= 8
#define SHIFT1		mydword >>= 4


//-- Global Variables --------------------------------------------------------
char *          video_vidram;
unsigned short	*video_paletteram_ng;
unsigned short	video_palette_bank0_ng[4096] __attribute__((aligned(64))); ;
unsigned short	video_palette_bank1_ng[4096] __attribute__((aligned(64))); ;
unsigned short	*video_paletteram_pc;
unsigned short	video_palette_bank0_pc[4096] __attribute__((aligned(64))); ;
unsigned short	video_palette_bank1_pc[4096] __attribute__((aligned(64))); ;
unsigned short	video_color_lut[32768] __attribute__((aligned(64))); ;


short		video_modulo;
unsigned short	video_pointer;
int		video_dos_cx;
int		video_dos_cy;
int		video_mode = 0;

unsigned short 	*video_line_ptr[224] ;//__attribute__((aligned(64))); ;

unsigned char	video_fix_usage[4096] ;//__attribute__((aligned(64))); ;
unsigned char   rom_fix_usage[4096] ;//__attribute__((aligned(64))); ;

unsigned char	video_spr_usage[32768] ;//__attribute__((aligned(64))); ;
unsigned char   rom_spr_usage[32768] ;//__attribute__((aligned(64))); ;


static int dpw = 320;
static int dph = 224;

static int vdph = 256;

static uint16 video_buffer[320*224] __attribute__((aligned(64))); 

unsigned char	video_shrinky[17];// __attribute__((aligned(64))); ;
char		full_y_skip[16]={0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};

unsigned int	neogeo_frame_counter = 0;
unsigned int	neogeo_frame_counter_speed = 4;

unsigned int	video_hide_fps=0;
int		video_scanlines_capable = 1;
double		gamma_correction = 1.0;

int		display_mode=1;
int		frameskip=0;


//-- Function Prototypes -----------------------------------------------------
int	video_init(void);
void	video_shutdown(void);
void	video_draw_line(int);
int	video_set_mode(int);
void	video_mode_toggle(void);
void	incframeskip(void);
void	video_precalc_lut(void);
void	video_flip_pages(void);
void	video_draw_spr(unsigned int code, unsigned int color, int flipx,
			int flipy, int sx, int sy, int zx, int zy);
void	video_draw_screen1(void);
void	video_draw_screen2(void);
void	video_setup(void);
void	normblit(void);



//----------------------------------------------------------------------------
int	video_init(void)
{

	int		y,i;
	int		videomode;

	video_precalc_lut();

	video_vidram = malloc(131072);

	if (video_vidram==NULL) {
		printf("VIDEO: Could not allocate vidram (128k)\n");
		return	0;
	}

	memset(video_palette_bank0_ng, 0, 8192);
	memset(video_palette_bank1_ng, 0, 8192);
	memset(video_palette_bank0_pc, 0, 8192);
	memset(video_palette_bank1_pc, 0, 8192);

	video_paletteram_ng = video_palette_bank0_ng;
	video_paletteram_pc = video_palette_bank0_pc;
	video_modulo = 0;
	video_pointer = 0;

        for(i=0;i<32768;i++)
            video_spr_usage[i]=1;

	if ((videomode=video_set_mode(VIDEO_NORMAL))==-1)
		return -1;

	for(y=0;y<224;y++) {
		video_line_ptr[y] = video_buffer + y * 320;
	}

	return videomode;
}

//----------------------------------------------------------------------------
void	video_shutdown(void)
{
	free(video_vidram);
}

//----------------------------------------------------------------------------
int video_set_mode(int mode)
{
	int vidsys;
	  
	if (gs_is_pal())
	{
		g2_init(PAL_320_256_32);
		vidsys = PAL_MODE;
		vdph = 256;
	} 
   	else
   	{
        	g2_init(NTSC_320_224_32);
        	vidsys = NTSC_MODE;
        	vdph = 224;
   	}

   	// clear the screen
   	clean_buffers();
	
	video_mode = VIDEO_NORMAL;

	return vidsys;
}


void incframeskip(void)
{
    frameskip++;
    frameskip=frameskip%12;
}
//----------------------------------------------------------------------------
void	video_precalc_lut(void)
{
	int	ndx, rr, rg, rb;
	
	for(rr=0;rr<32;rr++) {
		for(rg=0;rg<32;rg++) {
			for(rb=0;rb<32;rb++) {
				ndx = ((rr&1)<<14)|((rg&1)<<13)|((rb&1)<<12)|((rr&30)<<7)
					|((rg&30)<<3)|((rb&30)>>1);
				video_color_lut[ndx] =
				     ((int)( 31 * pow( (double)rb / 31, 1 / gamma_correction ) )<<0)
					|((int)( 63 * pow( (double)rg / 31, 1 / gamma_correction ) )<<5)
					|((int)( 31 * pow( (double)rr / 31, 1 / gamma_correction ) )<<11);
			}
		}
	}
	
}

//----------------------------------------------------------------------------
void video_draw_screen1()
{

	static unsigned int	fc;
	static int	pass1_start;
	int		sx =0,sy =0,oy =0,my =0,zx = 1, rzy = 1;
	int		offs,i,count,y;
	int		tileno,tileatr,t1,t2,t3;
	char		fullmode=0;
	int		ddax=0,dday=0,rzx=15,yskip=0;

	
	for (y=0; y<224; y++) 
  	  memset (video_line_ptr[y],video_paletteram_pc[4095],320*2);
	
	t1 = *((unsigned short *)( &video_vidram[0x10400 + 4] ));

	if ((t1 & 0x40) == 0)
	{
		for (pass1_start=6;pass1_start<0x300;pass1_start+=2)
		{
			t1 = *((unsigned short *)( &video_vidram[0x10400 + pass1_start] ));

			if ((t1 & 0x40) == 0)
				break;
		}
		
		if (pass1_start == 6)
			pass1_start = 0;
	}
	else
		pass1_start = 0;	

	for (count=pass1_start;count<0x300;count+=2) {
		t3 = *((unsigned short *)( &video_vidram[0x10000 + count] ));
		t1 = *((unsigned short *)( &video_vidram[0x10400 + count] ));
		t2 = *((unsigned short *)( &video_vidram[0x10800 + count] ));

		// If this bit is set this new column is placed next to last one
		if (t1 & 0x40) {
			sx += (rzx + 1);
			if ( sx >= 0x1F0 )
				sx -= 0x200;

			// Get new zoom for this column
			zx = (t3 >> 8)&0x0F;

			sy = oy;
		} else {	// nope it is a new block
			// Sprite scaling
			zx = (t3 >> 8)&0x0F;

			rzy = t3 & 0xff;

			sx = (t2 >> 7);
			if ( sx >= 0x1F0 )
				sx -= 0x200;

			// Number of tiles in this strip
			my = t1 & 0x3f;
			if (my == 0x20)
				fullmode = 1;
			else if (my >= 0x21)
				fullmode = 2;	// most games use 0x21, but
			else 
				fullmode = 0;	// Alpha Mission II uses 0x3f

			sy = 0x1F0 - (t1 >> 7);
			if (sy > 0x100) sy -= 0x200;
			
			if (fullmode == 2 || (fullmode == 1 && rzy == 0xff))
			{
				while (sy < -16) sy += 2 * (rzy + 1);
			}
			oy = sy;

		  	if(my==0x21) my=0x20;
			else if(rzy!=0xff && my!=0)
				my=((my*16*256)/(rzy+1) + 15)/16;

            		if(my>0x20) my=0x20;

			ddax=0;	// setup x zoom
		}

		rzx = zx;

		// No point doing anything if tile strip is 0
		if ((my==0)||(sx>311))
			continue;

		// Setup y zoom
		if(rzy==255)
			yskip=16;
		else
			dday=0;	// =256; NS990105 mslug fix

		offs = count<<6;

		// my holds the number of tiles in each vertical multisprite block
		for (y=0; y < my ;y++) {
			tileno  = *((unsigned short *)(&video_vidram[offs]));
			offs+=2;
			tileatr = *((unsigned short *)(&video_vidram[offs]));
			offs+=2;

			if (tileatr&0x8)
				tileno = (tileno&~7)|(neogeo_frame_counter&7);
			else if (tileatr&0x4)
				tileno = (tileno&~3)|(neogeo_frame_counter&3);
				
//			tileno &= 0x7FFF;
			if (tileno>0x7FFF)
				continue;

			if (fullmode == 2 || (fullmode == 1 && rzy == 0xff))
			{
				if (sy >= 224) sy -= 2 * (rzy + 1);
			}
			else if (fullmode == 1)
			{
				if (y == 0x10) sy -= 2 * (rzy + 1);
			}
			else if (sy > 0x100) sy -= 0x200;

			if(rzy!=255)
			{
				yskip=0;
				video_shrinky[0]=0;
				for(i=0;i<16;i++)
				{
					video_shrinky[i+1]=0;
					dday-=rzy+1;
					if(dday<=0)
					{
						dday+=256;
						yskip++;
						video_shrinky[yskip]++;
					}
					else
						video_shrinky[yskip]++;
				}
			}

            if (((tileatr>>8)||(tileno!=0))&&(sy<224))
			{
				video_draw_spr(
					tileno,
					tileatr >> 8,
					tileatr & 0x01,tileatr & 0x02,
					sx,sy,rzx,yskip);
			}

			sy +=yskip;
		}  // for y
	}  // for count

	for (count=0;count<pass1_start;count+=2) {
		t3 = *((unsigned short *)( &video_vidram[0x10000 + count] ));
		t1 = *((unsigned short *)( &video_vidram[0x10400 + count] ));
		t2 = *((unsigned short *)( &video_vidram[0x10800 + count] ));

		// If this bit is set this new column is placed next to last one
		if (t1 & 0x40) {
			sx += (rzx + 1);
			if ( sx >= 0x1F0 )
				sx -= 0x200;

			// Get new zoom for this column
			zx = (t3 >> 8)&0x0F;

			sy = oy;
		} else {	// nope it is a new block
			// Sprite scaling
			zx = (t3 >> 8)&0x0F;

			rzy = t3 & 0xff;

			sx = (t2 >> 7);
			if ( sx >= 0x1F0 )
				sx -= 0x200;

			// Number of tiles in this strip
			my = t1 & 0x3f;
			if (my == 0x20)
				fullmode = 1;
			else if (my >= 0x21)
				fullmode = 2;	// most games use 0x21, but
			else 
				fullmode = 0;	// Alpha Mission II uses 0x3f

			sy = 0x1F0 - (t1 >> 7);
			if (sy > 0x100) sy -= 0x200;
			
			if (fullmode == 2 || (fullmode == 1 && rzy == 0xff))
			{
				while (sy < -16) sy += 2 * (rzy + 1);
			}
			oy = sy;

		  	if(my==0x21) my=0x20;
			else if(rzy!=0xff && my!=0)
				my=((my*16*256)/(rzy+1) + 15)/16;

            		if(my>0x20) my=0x20;

			ddax=0;	// setup x zoom
		}

		rzx = zx;

		// No point doing anything if tile strip is 0
		if ((my==0)||(sx>311))
			continue;

		// Setup y zoom
		if(rzy==255)
			yskip=16;
		else
			dday=0;	// =256; NS990105 mslug fix

		offs = count<<6;

		// my holds the number of tiles in each vertical multisprite block
		for (y=0; y < my ;y++) {
			tileno  = *((unsigned short *)(&video_vidram[offs]));
			offs+=2;
			tileatr = *((unsigned short *)(&video_vidram[offs]));
			offs+=2;

			if (tileatr&0x8)
				tileno = (tileno&~7)|(neogeo_frame_counter&7);
			else if (tileatr&0x4)
				tileno = (tileno&~3)|(neogeo_frame_counter&3);
				
//			tileno &= 0x7FFF;
			if (tileno>0x7FFF)
				continue;

			if (fullmode == 2 || (fullmode == 1 && rzy == 0xff))
			{
				if (sy >= 248) sy -= 2 * (rzy + 1);
			}
			else if (fullmode == 1)
			{
				if (y == 0x10) sy -= 2 * (rzy + 1);
			}
			else if (sy > 0x110) sy -= 0x200;

			if(rzy!=255)
			{
				yskip=0;
				video_shrinky[0]=0;
				for(i=0;i<16;i++)
				{
					video_shrinky[i+1]=0;
					dday-=rzy+1;
					if(dday<=0)
					{
						dday+=256;
						yskip++;
						video_shrinky[yskip]++;
					}
					else
						video_shrinky[yskip]++;
				}
			}

            if (((tileatr>>8)||(tileno!=0))&&(sy<224))
			{
				video_draw_spr(
					tileno,
					tileatr >> 8,
					tileatr & 0x01,tileatr & 0x02,
					sx,sy,rzx,yskip);
			}

			sy +=yskip;
		}  // for y
	}  // for count

	if (fc >= neogeo_frame_counter_speed) {
		neogeo_frame_counter++;
		fc=0;
	}

	fc++;

	video_draw_fix();
	
	// DRAW !!!!!!!
	blitter();

}

//       Without  flip    	    With Flip
// 01: X0000000 00000000	00000000 0000000X
// 02: X0000000 X0000000	0000000X 0000000X
// 03: X0000X00 00X00000	00000X00 00X0000X
// 04: X000X000 X000X000	000X000X 000X000X
// 05: X00X00X0 0X00X000	000X00X0 0X00X00X
// 06: X0X00X00 X0X00X00	00X00X0X 00X00X0X
// 07: X0X0X0X0 0X0X0X00	00X0X0X0 0X0X0X0X
// 08: X0X0X0X0 X0X0X0X0	0X0X0X0X 0X0X0X0X
// 09: XX0X0X0X X0X0X0X0	0X0X0X0X X0X0X0XX
// 10: XX0XX0X0 XX0XX0X0	0X0XX0XX 0X0XX0XX
// 11: XXX0XX0X X0XX0XX0	0XX0XX0X X0XX0XXX
// 12: XXX0XXX0 XXX0XXX0	0XXX0XXX 0XXX0XXX
// 13: XXXXX0XX XX0XXXX0	0XXXX0XX XX0XXXXX
// 14: XXXXXXX0 XXXXXXX0	0XXXXXXX 0XXXXXXX
// 15: XXXXXXXX XXXXXXX0	0XXXXXXX XXXXXXXX
// 16: XXXXXXXX XXXXXXXX	XXXXXXXX XXXXXXXX

void	video_draw_spr(unsigned int code, unsigned int color, int flipx,
			int flipy, int sx, int sy, int zx, int zy)
{

	int		oy, ey, y, dy;
	//unsigned short	*bm;
	uint16		*bm;
	int		col;
	int		l;
	int		mydword;
	unsigned char	*fspr = 0;
	char		*l_y_skip;
	const unsigned short	*paldata;


	// Check for total transparency, no need to draw
	//if (video_spr_usage[code] == 0)
    	//	return;

	if (sx <= -8)
		return;
	

   	if(zy == 16)
		 l_y_skip = full_y_skip;
	else
		 l_y_skip = video_shrinky;

	fspr = neogeo_spr_memory;

	// Mish/AJP - Most clipping is done in main loop
	oy = sy;
  	ey = sy + zy -1; 	// Clip for size of zoomed object

	if (sy < 0)
		sy = 0;
	if (ey >= 224)
		ey = 223;

	if (flipy)	// Y flip
	{
		dy = -8;

	   //   fspr += (code+1)*128 - 8 - (sy-oy)*8; orgin code from pc source base 
         	fspr += ((code+1)<<7) - 8 - ((sy-oy)<<3); // Shift saves 1 to 2 cycles maybe? 
   	} 
	else		// normal
	{
		dy = 8;
  	   //   fspr += code*128 + (sy-oy)*8; //orgin code from pc source base 
         	fspr += ((code)<<7) + ((sy-oy)<<3); 
   	} 

	paldata = &video_paletteram_pc[color*16];
	
	if (flipx)	// X flip
	{
		l=0;
    		switch(zx) {
		case	0:
			for (y = sy;y <= ey;y++)
			{
				fspr += l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx;
				LINE_MID;
				SHIFT7;
				PIXEL_LAST;
				l++;
			}
			break;
		case	1:
			for (y = sy;y <= ey;y++)
			{
				fspr += l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx + 1;
				LINE_BEGIN;
				SHIFT7;
				PIXEL_R;
				LINE_MID;
				SHIFT7;
				PIXEL_LAST;
				l++;
			}
			break;
		case	2:
			for (y = sy;y <= ey;y++)
			{
				fspr += l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx + 2;
				LINE_BEGIN;
				SHIFT5;
				PIXEL_R;
				LINE_MID;
				SHIFT2;
				PIXEL_R;
				SHIFT5;
				PIXEL_LAST;
				l++;
			}
			break;
		case	3:
			for (y = sy;y <= ey;y++)
			{
				fspr += l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx + 3;
				LINE_BEGIN;
				SHIFT3;
				PIXEL_R;
				SHIFT4;
				PIXEL_R;
				LINE_MID;
				SHIFT3;
				PIXEL_R;
				SHIFT4;
				PIXEL_LAST;
				l++;
			}
			break;
		case	4:
			for (y = sy;y <= ey;y++)
			{
				fspr += l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx + 4;
				LINE_BEGIN;
				SHIFT3;
				PIXEL_R;
				SHIFT3;
				PIXEL_R;
				LINE_MID;
				SHIFT1;
				PIXEL_R;
				SHIFT3;
				PIXEL_R;
				SHIFT3;
				PIXEL_LAST;
				l++;
			}
			break;
		case	5:
			for (y = sy;y <= ey;y++)
			{
				fspr += l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx + 5;
				LINE_BEGIN;
				SHIFT2;
				PIXEL_R;
				SHIFT3;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				LINE_MID;
				SHIFT2;
				PIXEL_R;
				SHIFT3;
				PIXEL_R;
				SHIFT2;
				PIXEL_LAST;
				l++;
			}
			break;		
		case	6:
			for (y = sy;y <= ey;y++)
			{
				fspr += l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx + 6;
				LINE_BEGIN;
				SHIFT2;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				LINE_MID;
				SHIFT1;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT2;
				PIXEL_LAST;
				l++;
			}
			break;
		case	7:
			for (y = sy;y <= ey;y++)
			{
				fspr += l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx + 7;
				LINE_BEGIN;
				SHIFT1;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				LINE_MID;
				SHIFT1;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT2;
				PIXEL_LAST;
				l++;
			}
			break;		
		case	8:
			for (y = sy;y <= ey;y++)
			{
				fspr += l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx + 8;
				LINE_BEGIN;
				SHIFT1;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				LINE_MID;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT1;
				PIXEL_LAST;
				l++;
			}
			break;
		case	9:
			for (y = sy;y <= ey;y++)
			{
				fspr += l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx + 9;
				LINE_BEGIN;
				SHIFT1;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				LINE_MID;
				SHIFT1;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT1;
				PIXEL_LAST;
				l++;
			}
			break;		
		case	10:
			for (y = sy;y <= ey;y++)
			{
				fspr += l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx + 10;
				LINE_BEGIN;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				LINE_MID;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_LAST;
				l++;
			}
			break;		
		case	11:
			for (y = sy;y <= ey;y++)
			{
				fspr += l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx + 11;
				LINE_BEGIN;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT2;	
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				LINE_MID;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT2;	
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_LAST;
				l++;
			}
			break;		
		case	12:
			for (y = sy;y <= ey;y++)
			{
				fspr += l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx + 12;
				LINE_BEGIN;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				LINE_MID;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT2;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_LAST;
				l++;
			}
			break;		
		case	13:
			for (y = sy;y <= ey;y++)
			{
				fspr += l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx + 13;
				LINE_BEGIN;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				LINE_MID;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_LAST;
				l++;
			}
			break;		
		case	14:
			for (y = sy;y <= ey;y++)
			{
				fspr += l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx + 14;
				LINE_BEGIN;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				LINE_MID;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_LAST;
				l++;
			}
			break;
		case	15:
			for (y = sy;y <= ey;y++)
			{
				fspr += l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx + 15;
				LINE_BEGIN;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				LINE_MID;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_R;
				SHIFT1;
				PIXEL_LAST;
				l++;
			}
			break;		
		}
	}
	else		// normal
	{
		l=0;
    		switch(zx) {
		case	0:
			for (y = sy ;y <= ey;y++)
			{
				fspr+=l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx;
				LINE_BEGIN;
				PIXEL_LAST;
				l++;
			}
			break;
		case	1:
			for (y = sy ;y <= ey;y++)
			{
				fspr+=l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx;
				LINE_BEGIN;
				PIXEL_F;
				LINE_MID;
				PIXEL_LAST;
				l++;
			}
			break;
		case	2:
			for (y = sy ;y <= ey;y++)
			{
				fspr+=l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx;
				LINE_BEGIN;
				PIXEL_F;
				SHIFT5;
				PIXEL_F;
				LINE_MID;
				SHIFT2;
				PIXEL_LAST;
				l++;
			}
			break;
		case	3:
			for (y = sy ;y <= ey;y++)
			{
				fspr+=l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx;
				LINE_BEGIN;
				PIXEL_F;
				SHIFT4;
				PIXEL_F;
				LINE_MID;
				PIXEL_F;
				SHIFT4;
				PIXEL_LAST;
				l++;
			}
			break;
		case	4:
			for (y = sy ;y <= ey;y++)
			{
				fspr+=l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx;
				LINE_BEGIN;
				PIXEL_F;
				SHIFT3;
				PIXEL_F;
				SHIFT3;
				PIXEL_F;
				LINE_MID;
				SHIFT1;
				PIXEL_F;
				SHIFT3;
				PIXEL_LAST;
				l++;
			}
			break;
		case	5:
			for (y = sy ;y <= ey;y++)
			{
				fspr+=l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx;
				LINE_BEGIN;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT3;
				PIXEL_F;
				LINE_MID;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT3;
				PIXEL_LAST;
				l++;
			}
			break;
		case	6:
			for (y = sy ;y <= ey;y++)
			{
				fspr+=l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx;
				LINE_BEGIN;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				LINE_MID;
				SHIFT1;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT2;
				PIXEL_LAST;
				l++;
			}
			break;
		case	7:
			for (y = sy ;y <= ey;y++)
			{
				fspr+=l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx;
				LINE_BEGIN;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				LINE_MID;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT2;
				PIXEL_LAST;
				l++;
			}
			break;
		case	8:
			for (y = sy ;y <= ey;y++)
			{
				fspr+=l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx;
				LINE_BEGIN;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				LINE_MID;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT2;
				PIXEL_LAST;
				l++;
			}
			break;
		case	9:
			for (y = sy ;y <= ey;y++)
			{
				fspr+=l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx;
				LINE_BEGIN;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				LINE_MID;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT2;
				PIXEL_LAST;
				l++;
			}
			break;
		case	10:
			for (y = sy ;y <= ey;y++)
			{
				fspr+=l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx;
				LINE_BEGIN;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				LINE_MID;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT1;
				PIXEL_LAST;
				l++;
			}
			break;
		case	11:
			for (y = sy ;y <= ey;y++)
			{
				fspr+=l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx;
				LINE_BEGIN;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT2;	
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				LINE_MID;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT2;	
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_LAST;
				l++;
			}
			break;
		case	12:
			for (y = sy ;y <= ey;y++)
			{
				fspr+=l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx;
				LINE_BEGIN;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				LINE_MID;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT2;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_LAST;
				l++;
			}
			break;
		case	13:
			for (y = sy ;y <= ey;y++)
			{
				fspr+=l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx;
				LINE_BEGIN;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				LINE_MID;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_LAST;
				l++;
			}
			break;
		case	14:
			for (y = sy ;y <= ey;y++)
			{
				fspr+=l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx;
				LINE_BEGIN;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				LINE_MID;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_LAST;
				l++;
			}
			break;
		case	15:
			for (y = sy ;y <= ey;y++)
			{
				fspr+=l_y_skip[l]*dy;
				bm  = (video_line_ptr[y]) + sx;
				LINE_BEGIN;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				LINE_MID;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_F;
				SHIFT1;
				PIXEL_LAST;
				l++;
			}
			break;
		}
	}

}

/*------------------------------------------------------------*/
/* blit image to screen */
void blitter(void)
{
	g2_put_image16(0, vdph-224, dpw, dph, video_buffer); 
	g2_wait_vsync();
}
/*------------------------------------------------------------*/
void clean_buffers(void)
{
  g2_set_fill_color(0, 0, 0);
  g2_fill_rect(0, 0, g2_get_max_x(), g2_get_max_y());
}

/*-- UI functions -------------------------------------------------*/
void display_splashscreen(void)
{
	clean_buffers();
	g2_put_image(0, (splash_h-dph)/2, dpw, dph, dpw, dph, splash); 
}
void display_insertscreen(void)
{
	clean_buffers();
	g2_put_image(0, (insert_h-dph)/2, dpw, dph, dpw, dph, insert); 
}
void display_loadingscreen(void)
{
	clean_buffers();
	g2_put_image(0, (loading_h-dph)/2, dpw, dph, dpw, dph, loading); 
}
