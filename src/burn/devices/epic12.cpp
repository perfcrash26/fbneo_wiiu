/* ported from mame 0.155 */
/*
Copyright (c) 1997-2015 Nicola Salmoria and the MAME team
Redistributions may not be sold, nor may they be used in a commercial product or activity.
*/
/* emulation of Altera Cyclone EPIC12 FPGA programmed as a blitter */
/* code by David Haywood, Luca Elia, MetalliC */

#include "burnint.h"
#include "tiles_generic.h"
#include "sh4_intf.h"
#include "thready.h"
#include "rectangle.h"
#include <math.h> // floor()

/* VRAM row stride in pixels — the emulator's own layout, not the hardware's. A power-of-two stride (0x2000,
 * 32 KB rows) makes every sprite row map to the same cache sets on the Wii U; padding each row by 1 KB (8448)
 * avoids that. Only addressing changes: video, audio and savestates are bit-identical.
 * -DEP1C_PITCH overrides it for A/B builds. */
#ifndef EP1C_PITCH
#define EP1C_PITCH 8448
#endif

static const int EP1C_VRAM_CLK_NANOSEC = 13;
static const int EP1C_SRAM_CLK_NANOSEC = 20;
static const int EP1C_VRAM_H_LINE_PERIOD_NANOSEC = 63600;
static const int EP1C_VRAM_H_LINE_DURATION_NANOSEC = 2160;
static const int EP1C_FRAME_DURATION_NANOSEC = 16666666;
static const int EP1C_DRAW_OPERATION_SIZE_BYTES = 20;
static const int EP1C_CLIP_OPERATION_SIZE_BYTES = 2;

// When looking at VRAM viewer in Special mode in Muchi Muchi Pork, draws 32 pixels outside of
// the "clip area" is visible. This is likely why the frame buffers will have at least a 32 pixel offset
// from the VRAM borders or other buffers in all games.
static int EP1C_CLIP_MARGIN = 32;
// note: configurable via dipsetting  -dink may 2023

// Number of bytes that are read each time Blitter fetches operations from SRAM.
static const int OPERATION_CHUNK_SIZE_BYTES = 64;

// Approximate time it takes to fetch a chunk of operations.
// This is composed of the time that the Blitter holds the Bus Request (BREQ) signal
// of the SH-3, as well as the overhead between requests.
static const int OPERATION_READ_CHUNK_INTERVAL_NS = 700;

static UINT64 m_blit_delay_ns = 0;
static UINT16 m_blit_idle_op_bytes = 0;

static void idle_blitter(UINT8 operation_size_bytes)
{
	m_blit_idle_op_bytes += operation_size_bytes;
	if (m_blit_idle_op_bytes >= OPERATION_CHUNK_SIZE_BYTES)
	{
		m_blit_idle_op_bytes -= OPERATION_CHUNK_SIZE_BYTES;
		m_blit_delay_ns += OPERATION_READ_CHUNK_INTERVAL_NS;
	}
}

static UINT16* m_ram16;
static UINT16* m_ram16_copy;
static UINT32 m_gfx_addr;
static UINT32 m_gfx_addr_shadowcopy;
static UINT32 m_gfx_scroll_x, m_gfx_scroll_y;
static UINT32 m_gfx_clip_x, m_gfx_clip_y;
static UINT32 m_gfx_clip_x_shadowcopy, m_gfx_clip_y_shadowcopy;

static int m_gfx_size;
static UINT32 *m_bitmaps;
static rectangle m_clip;


static UINT64 epic12_device_blit_delay;
static int m_blitter_busy;

static int m_main_ramsize; // type D has double the main ram
static int m_main_rammask;

static int m_delay_method;
static int m_delay_scale;
static int m_burn_cycles;

static INT32 sleep_on_busy = 1;

static UINT8 epic12_device_colrtable[0x20][0x40];
static UINT8 epic12_device_colrtable_rev[0x20][0x40];
static UINT8 epic12_device_colrtable_add[0x20][0x20];

static UINT16 *pal16 = NULL; // palette lut for 16bpp video emulation

#include "epic12.h"

struct _clr_t
{
	UINT8 b,g,r,t;
};

typedef struct _clr_t clr_t;

union colour_t
{
	clr_t trgb;
	UINT32 u32;
};

typedef const void (*epic12_device_blitfunction)(
						const rectangle *,
						UINT32 *, /* gfx */
						int , /* src_x */
						int , /* src_y */
						const int , /* dst_x_start */
						const int , /* dst_y_start */
						int , /* dimx */
						int , /* dimy */
						const int , /* flipy */
						const UINT8 , /* s_alpha */
						const UINT8 , /* d_alpha */
						//int , /* tint */
						const clr_t * );

#define BLIT_PARAMS const rectangle *clip, UINT32 *gfx, int src_x, int src_y, const int dst_x_start, const int dst_y_start, int dimx, int dimy, const int flipy, const UINT8 s_alpha, const UINT8 d_alpha, const clr_t *tint_clr

	static inline void pen_to_clr(UINT32 pen, clr_t *clr)
	{
	// --t- ---- rrrr r--- gggg g--- bbbb b---  format
		clr->r = (pen >> (16+3));// & 0x1f;
		clr->g = (pen >>  (8+3));// & 0x1f;
		clr->b = (pen >>   3);// & 0x1f;

	// --t- ---- ---r rrrr ---g gggg ---b bbbb  format
	//  clr->r = (pen >> 16) & 0x1f;
	//  clr->g = (pen >> 8) & 0x1f;
	//  clr->b = (pen >> 0) & 0x1f;

	}


	// convert separate r,g,b biases (0..80..ff) to clr_t (-1f..0..1f)
	static inline void tint_to_clr(UINT8 r, UINT8 g, UINT8 b, clr_t *clr)
	{
		clr->r  =   r>>2;
		clr->g  =   g>>2;
		clr->b  =   b>>2;
	}

	// clr_t to r5g5b5
	static inline UINT32 clr_to_pen(const clr_t *clr)
	{
	// --t- ---- rrrr r--- gggg g--- bbbb b---  format
		return (clr->r << (16+3)) | (clr->g << (8+3)) | (clr->b << 3);

	// --t- ---- ---r rrrr ---g gggg ---b bbbb  format
	//  return (clr->r << (16)) | (clr->g << (8)) | (clr->b);
	}


	static inline void clr_add_with_clr_mul_fixed(clr_t *clr, const clr_t *clr0, const UINT8 mulfixed_val, const clr_t *mulfixed_clr0)
	{
		clr->r = epic12_device_colrtable_add[clr0->r][epic12_device_colrtable[(mulfixed_clr0->r)][mulfixed_val]];
		clr->g = epic12_device_colrtable_add[clr0->g][epic12_device_colrtable[(mulfixed_clr0->g)][mulfixed_val]];
		clr->b = epic12_device_colrtable_add[clr0->b][epic12_device_colrtable[(mulfixed_clr0->b)][mulfixed_val]];
	}

	static inline  void clr_add_with_clr_mul_3param(clr_t *clr, const clr_t *clr0, const clr_t *clr1, const clr_t *clr2)
	{
		clr->r = epic12_device_colrtable_add[clr0->r][epic12_device_colrtable[(clr2->r)][(clr1->r)]];
		clr->g = epic12_device_colrtable_add[clr0->g][epic12_device_colrtable[(clr2->g)][(clr1->g)]];
		clr->b = epic12_device_colrtable_add[clr0->b][epic12_device_colrtable[(clr2->b)][(clr1->b)]];
	}

	static inline  void clr_add_with_clr_square(clr_t *clr, const clr_t *clr0, const clr_t *clr1)
	{
		clr->r = epic12_device_colrtable_add[clr0->r][epic12_device_colrtable[(clr1->r)][(clr1->r)]];
		clr->g = epic12_device_colrtable_add[clr0->r][epic12_device_colrtable[(clr1->g)][(clr1->g)]];
		clr->b = epic12_device_colrtable_add[clr0->r][epic12_device_colrtable[(clr1->b)][(clr1->b)]];
	}

	static inline  void clr_add_with_clr_mul_fixed_rev(clr_t *clr, const clr_t *clr0, const UINT8 val, const clr_t *clr1)
	{
		clr->r =  epic12_device_colrtable_add[clr0->r][epic12_device_colrtable_rev[val][(clr1->r)]];
		clr->g =  epic12_device_colrtable_add[clr0->g][epic12_device_colrtable_rev[val][(clr1->g)]];
		clr->b =  epic12_device_colrtable_add[clr0->b][epic12_device_colrtable_rev[val][(clr1->b)]];
	}

	static inline  void clr_add_with_clr_mul_rev_3param(clr_t *clr, const clr_t *clr0, const clr_t *clr1, const clr_t *clr2)
	{
		clr->r =  epic12_device_colrtable_add[clr0->r][epic12_device_colrtable_rev[(clr2->r)][(clr1->r)]];
		clr->g =  epic12_device_colrtable_add[clr0->g][epic12_device_colrtable_rev[(clr2->g)][(clr1->g)]];
		clr->b =  epic12_device_colrtable_add[clr0->b][epic12_device_colrtable_rev[(clr2->b)][(clr1->b)]];
	}

	static inline  void clr_add_with_clr_mul_rev_square(clr_t *clr, const clr_t *clr0, const clr_t *clr1)
	{
		clr->r =  epic12_device_colrtable_add[clr0->r][epic12_device_colrtable_rev[(clr1->r)][(clr1->r)]];
		clr->g =  epic12_device_colrtable_add[clr0->g][epic12_device_colrtable_rev[(clr1->g)][(clr1->g)]];
		clr->b =  epic12_device_colrtable_add[clr0->b][epic12_device_colrtable_rev[(clr1->b)][(clr1->b)]];
	}


	static inline  void clr_add(clr_t *clr, const clr_t *clr0, const clr_t *clr1)
	{
	/*
	    clr->r = clr0->r + clr1->r;
	    clr->g = clr0->g + clr1->g;
	    clr->b = clr0->b + clr1->b;
	*/
		// use pre-clamped lookup table
		clr->r =  epic12_device_colrtable_add[clr0->r][clr1->r];
		clr->g =  epic12_device_colrtable_add[clr0->g][clr1->g];
		clr->b =  epic12_device_colrtable_add[clr0->b][clr1->b];

	}


	static inline void clr_mul(clr_t *clr0, const clr_t *clr1)
	{
		clr0->r = epic12_device_colrtable[(clr0->r)][(clr1->r)];
		clr0->g = epic12_device_colrtable[(clr0->g)][(clr1->g)];
		clr0->b = epic12_device_colrtable[(clr0->b)][(clr1->b)];
	}

	static inline void clr_square(clr_t *clr0, const clr_t *clr1)
	{
		clr0->r = epic12_device_colrtable[(clr1->r)][(clr1->r)];
		clr0->g = epic12_device_colrtable[(clr1->g)][(clr1->g)];
		clr0->b = epic12_device_colrtable[(clr1->b)][(clr1->b)];
	}

	static inline void clr_mul_3param(clr_t *clr0, const clr_t *clr1, const clr_t *clr2)
	{
		clr0->r = epic12_device_colrtable[(clr2->r)][(clr1->r)];
		clr0->g = epic12_device_colrtable[(clr2->g)][(clr1->g)];
		clr0->b = epic12_device_colrtable[(clr2->b)][(clr1->b)];
	}

	static inline void clr_mul_rev(clr_t *clr0, const clr_t *clr1)
	{
		clr0->r = epic12_device_colrtable_rev[(clr0->r)][(clr1->r)];
		clr0->g = epic12_device_colrtable_rev[(clr0->g)][(clr1->g)];
		clr0->b = epic12_device_colrtable_rev[(clr0->b)][(clr1->b)];
	}

	static inline void clr_mul_rev_square(clr_t *clr0, const clr_t *clr1)
	{
		clr0->r = epic12_device_colrtable_rev[(clr1->r)][(clr1->r)];
		clr0->g = epic12_device_colrtable_rev[(clr1->g)][(clr1->g)];
		clr0->b = epic12_device_colrtable_rev[(clr1->b)][(clr1->b)];
	}


	static inline void clr_mul_rev_3param(clr_t *clr0, const clr_t *clr1, const clr_t *clr2)
	{
		clr0->r = epic12_device_colrtable_rev[(clr2->r)][(clr1->r)];
		clr0->g = epic12_device_colrtable_rev[(clr2->g)][(clr1->g)];
		clr0->b = epic12_device_colrtable_rev[(clr2->b)][(clr1->b)];
	}

	static inline void clr_mul_fixed(clr_t *clr, const UINT8 val, const clr_t *clr0)
	{
		clr->r = epic12_device_colrtable[val][(clr0->r)];
		clr->g = epic12_device_colrtable[val][(clr0->g)];
		clr->b = epic12_device_colrtable[val][(clr0->b)];
	}

	static inline void clr_mul_fixed_rev(clr_t *clr, const UINT8 val, const clr_t *clr0)
	{
		clr->r = epic12_device_colrtable_rev[val][(clr0->r)];
		clr->g = epic12_device_colrtable_rev[val][(clr0->g)];
		clr->b = epic12_device_colrtable_rev[val][(clr0->b)];
	}

	static inline void clr_copy(clr_t *clr, const clr_t *clr0)
	{
		clr->r = clr0->r;
		clr->g = clr0->g;
		clr->b = clr0->b;
	}



	// (1|s|d) * s_factor * s + (1|s|d) * d_factor * d
	// 0: +alpha
	// 1: +source
	// 2: +dest
	// 3: *
	// 4: -alpha
	// 5: -source
	// 6: -dest
	// 7: *

#include "epic12_blit0.inc"
#include "epic12_blit1.inc"
#include "epic12_blit2.inc"
#include "epic12_blit3.inc"
#include "epic12_blit4.inc"
#include "epic12_blit5.inc"
#include "epic12_blit6.inc"
#include "epic12_blit7.inc"
#include "epic12_blit8.inc"

static UINT8 *dips; // pointer to cv1k's dips

static void blitter_delay_callback(int)
{
	m_blitter_busy = 0;
	//bprintf(0, _T("cyc @ blitdelay callback: %d\n"), Sh3TotalCycles());
}

enum { BAND_ALL = 0, BAND_UPLOADS = 1, BAND_DRAWS = 2 };   // gfx_exec_band phase
static void gfx_exec_band(rectangle *clip, int ylo, int yhi, int phase,
                          UINT32 a0 = 0, UINT32 a1 = 0, int ct = 1); // forward (a0 = 0: list start; a1 = 0: list end)

extern "C" { int g_blit_split = 1; }    // runtime switch: split the blit across core 2 and core 0
extern "C" { int g_comp_split = 1; }    // runtime switch: 32-bit composite on the blit cores

#if (THREADY == THREADY_WIIU)
#define EP1C_SPLIT 1
#else
#define EP1C_SPLIT 0
#endif

#if EP1C_SPLIT
/* ---- blitter split: the 240-tall display is cut into two Y-bands; the blitter thread (core 2) draws the top,
 * a core 0 helper the bottom, each walking the list clipped to its band. Uploads run first, on one core. ---- */
static int g_blit_mid = 120;   // split row within the display
#define BLIT_ADAPTIVE 1
#define BLIT_SAFE     1
#define SS_SEGS       8
#define BLIT0_STACK (64 * 1024)
static OSThread  *blit0_thread;
static UINT8      *blit0_stack;
static OSSemaphore blit0_start, blit0_done;
static rectangle  blit0_clip;
static int         blit0_ylo, blit0_yhi;
static UINT32      blit0_a0, blit0_a1;   // list segment for the helper (0 = whole list)
static int         blit0_ct = 1;         // clip type at the segment start
static volatile int blit0_ready, blit0_quit;
static int         blit0_failed;         // thread creation failed: stay on one core until the next init
static volatile long long blit0_ticks;   // time core0 spent on its band (for adaptive balance)
/* Split safety: a frame whose list has a cross-band dependency (one band reads VRAM the other band writes, or an
 * upload lands on VRAM an earlier draw used, which the uploads pre-pass would reorder) is cut into segments with a
 * sync point between them, or runs on one core. */
static volatile int last_split_mid = -1;   /* VRAM split row of the last blit, -1 = it ran on one core */
static bool split_safe(int mid);   // defined after READ_NEXT_WORD
/* plan from split_safe: ss_nseg list segments starting at ss_seg_addr[i] with clip type ss_seg_ct[i];
 * ss_tail_addr != 0: from there on, one core draws all rows */
#define SS_MAXSEG 64   /* array size; the cap in use is SS_SEGS */
static UINT32 ss_seg_addr[SS_MAXSEG];
static int ss_seg_ct[SS_MAXSEG], ss_nseg = 1;
static UINT32 ss_tail_addr;
static int ss_tail_ct;

static int blit0_main(int, const char **)
{
	for (;;) {
		OSWaitSemaphore(&blit0_start);                           // parent set band + signalled
		if (blit0_quit) return 0;
		long long t = OSGetSystemTime();
		gfx_exec_band(&blit0_clip, blit0_ylo, blit0_yhi, BAND_DRAWS, blit0_a0, blit0_a1, blit0_ct); // bottom band
		long long t1 = OSGetSystemTime();
		blit0_ticks = t1 - t;
		OSSignalSemaphore(&blit0_done);
	}
	return 0;
}

static void blit_split_init()
{
	if (blit0_ready || blit0_failed) return;
	OSInitSemaphore(&blit0_start, 0);
	OSInitSemaphore(&blit0_done, 0);
	blit0_quit = 0;
	blit0_stack = (UINT8 *)memalign(16, BLIT0_STACK);
	blit0_thread = (OSThread *)memalign(16, sizeof(OSThread));
	if (blit0_stack && blit0_thread &&
	    OSCreateThread(blit0_thread, blit0_main, 0, NULL, blit0_stack + BLIT0_STACK, BLIT0_STACK, 16,
	                   OS_THREAD_ATTRIB_AFFINITY_CPU0)) {
		OSResumeThread(blit0_thread);
		blit0_ready = 1;
	} else {
		free(blit0_thread);
		free(blit0_stack);
		blit0_thread = NULL;
		blit0_stack = NULL;
		blit0_failed = 1;
	}
}

static void blit_split_exit()   // the blitter thread is stopped first, so the helper is idle
{
	if (blit0_ready) {
		blit0_quit = 1;
		OSSignalSemaphore(&blit0_start);
		OSJoinThread(blit0_thread, NULL);
		free(blit0_thread);
		free(blit0_stack);
		blit0_thread = NULL;
		blit0_stack = NULL;
		blit0_ready = 0;
	}
	blit0_failed = 0;
	last_split_mid = -1;
}
#endif // EP1C_SPLIT

static void run_blitter_cb()
{
	epic12_device_blit_delay = 0;
#if EP1C_SPLIT
	bool split = g_blit_split && blit0_ready;
	if (split && BLIT_SAFE) {
		split = split_safe(m_gfx_clip_y_shadowcopy + g_blit_mid);
	}
	last_split_mid = -1;
	if (split) {
		/* uploads first, on this core, so the parallel draws never race an in-flight upload */
		rectangle upclip;
		gfx_exec_band(&upclip, 0, 0x1000 - 1, BAND_UPLOADS);

		const int mid = m_gfx_clip_y_shadowcopy + g_blit_mid;   // split row
		last_split_mid = mid;
		/* list segments from the safety check: both cores draw segment i (each its band), meet, then segment i+1;
		 * a tail (if any) runs on this core alone. Without a check: one segment = the whole list. */
		const int nseg = BLIT_SAFE == 1 ? ss_nseg : 1;
		rectangle topclip;
		long long top = 0, bot = 0;
		for (int i = 0; i < nseg; i++) {
			const UINT32 a0 = nseg > 1 || BLIT_SAFE == 1 ? ss_seg_addr[i] : 0;
			const UINT32 a1 = (i + 1 < nseg) ? ss_seg_addr[i + 1] : (BLIT_SAFE == 1 ? ss_tail_addr : 0);
			const int ct = BLIT_SAFE == 1 ? ss_seg_ct[i] : 1;
			blit0_ylo = mid; blit0_yhi = 0x1000 - 1;     // bottom band -> core0
			blit0_a0 = a0; blit0_a1 = a1; blit0_ct = ct;
			OSSignalSemaphore(&blit0_start);
			long long t = OSGetSystemTime();
			gfx_exec_band(&topclip, 0, mid - 1, BAND_DRAWS, a0, a1, ct);   // top band -> this thread
			top += OSGetSystemTime() - t;
			OSWaitSemaphore(&blit0_done);
			bot += blit0_ticks;
		}
		if (BLIT_SAFE == 1 && ss_tail_addr) {   // too many sync points: the rest on one core, all rows
			long long t = OSGetSystemTime();
			gfx_exec_band(&topclip, 0, 0x1000 - 1, BAND_DRAWS, ss_tail_addr, 0, ss_tail_ct);
			top += OSGetSystemTime() - t;
		}
		/* load balance: move the split row so both bands take about the same time (1/16 deadband) */
		long long sum = top + bot;
		if (sum > 0 && BLIT_ADAPTIVE) {
			long long thresh = sum / 16;
			if (top - bot > thresh && g_blit_mid > 48)  g_blit_mid -= 3;  // top slower -> shrink top
			else if (bot - top > thresh && g_blit_mid < 200) g_blit_mid += 3;  // bottom slower -> grow top
		}
	} else
#endif // EP1C_SPLIT
	{
		rectangle clip;
		gfx_exec_band(&clip, 0, 0x1000 - 1, BAND_ALL);    // one core, all rows, all ops
	}
}

#if EP1C_SPLIT
static void comp_exit();
#endif

void epic12_exit()
{
	thready.exit();
#if EP1C_SPLIT
	blit_split_exit();
	comp_exit();
#endif

	BurnFree(m_bitmaps);
	BurnFree(m_ram16_copy);

	if (pal16) {
		BurnFree(pal16);
		pal16 = NULL;
	}
}

void epic12_init(INT32 ram_size, UINT16 *ram, UINT8 *dippy)
{
	m_main_ramsize = ram_size;
	m_main_rammask = ram_size - 1;

	m_ram16 = ram;

	m_ram16_copy = (UINT16*)BurnMalloc(ram_size);

	dips = dippy;

	m_gfx_size = 0x2000 * 0x1000;
	m_bitmaps = (UINT32*)BurnMalloc ((size_t)EP1C_PITCH * 0x1000 * 4);

	m_clip.set(0, 0x2000-1, 0, 0x1000-1);

	m_delay_method = 0; // accurate
	m_delay_scale = 50;
	m_blitter_busy = 0;
	m_gfx_addr = 0;
	m_gfx_addr_shadowcopy = 0;
	m_gfx_scroll_x = 0;
	m_gfx_scroll_y = 0;
	m_gfx_clip_x = 0;
	m_gfx_clip_y = 0;
	m_gfx_clip_x_shadowcopy = 0;
	m_gfx_clip_y_shadowcopy = 0;
	epic12_device_blit_delay = 0;

	m_blit_delay_ns = 0;
	m_blit_idle_op_bytes = 0;

	epic12_set_blitter_clipping_margin(1);
	epic12_set_blitter_sleep_on_busy(1);

	thready.init(run_blitter_cb);
#if EP1C_SPLIT
	blit_split_init();
#endif

	sh4_set_cave_blitter_delay_func(blitter_delay_callback);
}

void epic12_set_blitterthreading(INT32 value)
{
	thready.set_threading(value);
}

void epic12_set_blitterdelay(INT32 delay, INT32 burn_cycles)
{
	m_delay_scale = delay;
	m_burn_cycles = burn_cycles;
}

void epic12_set_blitterdelay_method(INT32 delay_method) // 0 = accurate, !0 = ancient
{
	m_delay_method = delay_method;
}

void epic12_set_blitter_clipping_margin(INT32 c_margin_on) // ??? not sure.
{
	EP1C_CLIP_MARGIN = (c_margin_on) ? 32 : 0;
}

void epic12_set_blitter_sleep_on_busy(INT32 busysleep_on)
{
	sleep_on_busy = (busysleep_on) ? 1 : 0;
}

void epic12_reset()
{
	// cache table to avoid divides in blit code, also pre-clamped
	int x,y;
	for (y=0;y<0x40;y++)
	{
		for (x=0;x<0x20;x++)
		{
			epic12_device_colrtable[x][y] = (x*y) / 0x1f;
			if (epic12_device_colrtable[x][y]>0x1f) epic12_device_colrtable[x][y] = 0x1f;

			epic12_device_colrtable_rev[x^0x1f][y] = (x*y) / 0x1f;
			if (epic12_device_colrtable_rev[x^0x1f][y]>0x1f) epic12_device_colrtable_rev[x^0x1f][y] = 0x1f;
		}
	}

	// preclamped add table
	for (y=0;y<0x20;y++)
	{
		for (x=0;x<0x20;x++)
		{
			epic12_device_colrtable_add[x][y] = (x+y);
			if (epic12_device_colrtable_add[x][y]>0x1f) epic12_device_colrtable_add[x][y] = 0x1f;
		}
	}

	m_blitter_busy = 0;
	m_gfx_addr = 0;
	m_gfx_addr_shadowcopy = 0;
	m_gfx_scroll_x = 0;
	m_gfx_scroll_y = 0;
	m_gfx_clip_x = 0;
	m_gfx_clip_y = 0;
	m_gfx_clip_x_shadowcopy = 0;
	m_gfx_clip_y_shadowcopy = 0;
	epic12_device_blit_delay = 0;
	m_blit_delay_ns = 0;
	m_blit_idle_op_bytes = 0;

	thready.reset();
}

static inline UINT16 READ_NEXT_WORD(UINT32 *addr)
{
	const UINT16 data = m_ram16_copy[((*addr & m_main_rammask) >> 1)];

	*addr += 2;

	return data;
}

#if EP1C_SPLIT
/* ---- split safety check (see run_blitter_cb) ----
 * One walk of the list, no drawing. VRAM is tracked in 32x32-pixel cells: per band, the cells it has written and
 * the cells it has read so far. Cells are coarser than pixels, so the check can only err towards falling back. */
#define SS_CW 256   /* 0x2000 / 32 */
#define SS_CH 128   /* 0x1000 / 32 */
static UINT32 ss_map[4][SS_CH][SS_CW / 32];   /* [0]=written band0, [1]=written band1, [2]=read band0, [3]=read band1 */

static inline void ss_clamp(int &x0, int &x1, int &y0, int &y1)
{
	if (x0 < 0 || x1 > 0x1fff) { x0 = 0; x1 = 0x1fff; y0--; y1++; }   /* off the row ends: whole rows, +-1 row */
	if (y0 < 0) y0 = 0;
	if (y1 > 0xfff) y1 = 0xfff;
}

/* mark (set=true) or test the cell rectangle of a pixel rect in map m */
static bool ss_cells(int m, int x0, int x1, int y0, int y1, bool set)
{
	ss_clamp(x0, x1, y0, y1);
	if (x0 > x1 || y0 > y1) return false;
	const int c0 = x0 >> 5, c1 = x1 >> 5;
	for (int r = y0 >> 5; r <= (y1 >> 5); r++) {
		UINT32 *row = ss_map[m][r];
		for (int w = c0 >> 5; w <= (c1 >> 5); w++) {
			const int lo = (w == (c0 >> 5)) ? (c0 & 31) : 0, hi = (w == (c1 >> 5)) ? (c1 & 31) : 31;
			const UINT32 mask = (hi == 31 ? 0xffffffffu : ((1u << (hi + 1)) - 1)) & ~((1u << lo) - 1);
			if (set) row[w] |= mask;
			else if (row[w] & mask) return true;
		}
	}
	return false;
}

/* exact confirmation: rectangle lists next to the cell maps. A cell hit is a conflict only if a rectangle overlaps too
 * (cells alone give many false conflicts). A full list falls back to the cell answer. */
struct ss_rect { int x0, x1, y0, y1; };
#define SS_RCAP 2048
static ss_rect ss_r[4][SS_RCAP];   /* same indexing as ss_map */
static int ss_n[4];
static UINT8 ss_over[4];
static UINT32 ss_all[SS_CH][SS_CW / 32];   /* every cell any draw read or wrote this list (never reset): upload check */
static bool ss_rhit(int m, int x0, int x1, int y0, int y1)
{
	if (ss_over[m]) return true;
	for (int i = 0; i < ss_n[m]; i++) {
		const ss_rect &r = ss_r[m][i];
		if (r.x0 <= x1 && x0 <= r.x1 && r.y0 <= y1 && y0 <= r.y1) return true;
	}
	return false;
}
static void ss_radd(int m, int x0, int x1, int y0, int y1)
{
	if (ss_n[m] >= SS_RCAP) { ss_over[m] = 1; return; }
	ss_rect r = { x0, x1, y0, y1 }; ss_r[m][ss_n[m]++] = r;
}
/* the 0xfff source-row wrap, for both structures: calls fn on one or two row ranges */
#define SS_ROWS(y0, y1, CALL) do { int _a = (y0), _b = (y1); \
	if (_b - _a >= 0xfff) { _a = 0; _b = 0xfff; } else { _a &= 0xfff; _b &= 0xfff; } \
	if (_a <= _b) { CALL(_a, _b); } else { CALL(_a, 0xfff); CALL(0, _b); } } while (0)

static bool ss_hit_rect(int m, int x0, int x1, int y0, int y1)   /* cells first, rects to confirm */
{
	return ss_cells(m, x0, x1, y0, y1, false) && ss_rhit(m, x0, x1, y0, y1);
}
static void ss_mark(int m, int x0, int x1, int y0, int y1)
{
	ss_cells(m, x0, x1, y0, y1, true);
	ss_radd(m, x0, x1, y0, y1);
}
static void ss_reset()
{
	memset(ss_map, 0, sizeof(ss_map));
	ss_n[0] = ss_n[1] = ss_n[2] = ss_n[3] = 0;
	ss_over[0] = ss_over[1] = ss_over[2] = ss_over[3] = 0;
}
static void ss_all_mark(int x0, int x1, int y0, int y1)
{
	ss_clamp(x0, x1, y0, y1);
	if (x0 > x1 || y0 > y1) return;
	for (int r = y0 >> 5; r <= (y1 >> 5); r++)
		for (int c = x0 >> 5; c <= (x1 >> 5); c++) ss_all[r][c >> 5] |= 1u << (c & 31);
}
static bool ss_all_hit(int x0, int x1, int y0, int y1)
{
	ss_clamp(x0, x1, y0, y1);
	if (x0 > x1 || y0 > y1) return false;
	for (int r = y0 >> 5; r <= (y1 >> 5); r++)
		for (int c = x0 >> 5; c <= (x1 >> 5); c++) if (ss_all[r][c >> 5] & (1u << (c & 31))) return true;
	return false;
}

/* Returns false = run the whole frame on one core. A cross-band conflict at a draw becomes a sync point (segment
 * boundary, maps reset: everything before it is finished on both cores); after SS_SEGS segments the rest of the list
 * becomes a single-core tail. (BLIT_SAFE == 2, prof builds only: any conflict = whole frame on one core.) */
static bool split_safe(int mid)
{
	const bool seg = BLIT_SAFE == 1;
	UINT32 addr = m_gfx_addr_shadowcopy & 0x1fffffff;
	const int cx0 = m_gfx_clip_x_shadowcopy - EP1C_CLIP_MARGIN, cx1 = m_gfx_clip_x_shadowcopy + 320 - 1 + EP1C_CLIP_MARGIN;
	const int cy0 = m_gfx_clip_y_shadowcopy - EP1C_CLIP_MARGIN, cy1 = m_gfx_clip_y_shadowcopy + 240 - 1 + EP1C_CLIP_MARGIN;
	int k0 = cx0, k1 = cx1, l0 = cy0, l1 = cy1, ct = 1;   /* current clip, as gfx_exec_band sets it */
	bool tail = false;                                    /* past the tail start: only the upload check continues */
	ss_reset();
	memset(ss_all, 0, sizeof(ss_all));
	ss_nseg = 1; ss_seg_addr[0] = addr; ss_seg_ct[0] = 1; ss_tail_addr = 0;
	for (;;) {
		const UINT32 opaddr = addr;
		const UINT16 data = READ_NEXT_WORD(&addr);
		switch (data & 0xf000) {
		case 0x0000: case 0xf000:
			return true;
		case 0xc000:
			ct = READ_NEXT_WORD(&addr) ? 1 : 0;
			if (ct) { k0 = cx0; k1 = cx1; l0 = cy0; l1 = cy1; }
			else { k0 = 0; k1 = 0x2000 - 1; l0 = 0; l1 = 0x1000 - 1; }
			break;
		case 0x2000: {
			READ_NEXT_WORD(&addr); READ_NEXT_WORD(&addr); READ_NEXT_WORD(&addr);
			const int ux = READ_NEXT_WORD(&addr) & 0x1fff, uy = READ_NEXT_WORD(&addr) & 0x0fff;
			const int uw = (READ_NEXT_WORD(&addr) & 0x1fff) + 1, uh = (READ_NEXT_WORD(&addr) & 0x0fff) + 1;
			addr += (UINT32)uw * uh * 2;
			/* the uploads pre-pass moves this ahead of every draw: it must not touch anything a draw used */
			int x0 = ux, x1 = ux + uw - 1;
			if (x1 > 0x1fff) { x0 = 0; x1 = 0x1fff; }   /* wraps: whole rows */
			bool hit = false;
#define SS_UPCHK(a, b) hit = hit || ss_all_hit(x0, x1, a, b)
			SS_ROWS(uy, uy + uh - 1, SS_UPCHK);
#undef SS_UPCHK
			if (hit) {
				return false;
			}
			break;
		}
		case 0x1000: {
			const UINT16 attr = data;
			READ_NEXT_WORD(&addr);   /* alpha */
			const int sx = READ_NEXT_WORD(&addr) & 0x1fff, sy = READ_NEXT_WORD(&addr) & 0x0fff;
			const UINT16 dxw = READ_NEXT_WORD(&addr), dyw = READ_NEXT_WORD(&addr);
			const int w = (READ_NEXT_WORD(&addr) & 0x1fff) + 1, h = (READ_NEXT_WORD(&addr) & 0x0fff) + 1;
			READ_NEXT_WORD(&addr); READ_NEXT_WORD(&addr);   /* tint */
			const int x = (dxw & 0x7fff) - (dxw & 0x8000), y = (dyw & 0x7fff) - (dyw & 0x8000);
			const int flipy = attr & 0x0400, flipx = attr & 0x0800;
			const int dx0 = x > k0 ? x : k0;
			const int dx1 = (x + w - 1) < k1 ? (x + w - 1) : k1;
			if (dx0 > dx1) break;
			if (sx + w - 1 > 0x1fff) break;   /* the blitter skips draws whose source wraps in x */
			/* source columns actually read for the clipped destination columns */
			const int sc0 = flipx ? sx + (w - 1) - (dx1 - x) : sx + (dx0 - x);
			const int sc1 = flipx ? sx + (w - 1) - (dx0 - x) : sx + (dx1 - x);
			int pr[2][4]; bool part[2];
			for (int b = 0; b < 2; b++) {   /* each band's part: clipped dest rows r0..r1, source rows s0..s1 */
				int r0 = y > l0 ? y : l0, r1 = (y + h - 1) < l1 ? (y + h - 1) : l1;
				if (b) { if (r0 < mid) r0 = mid; } else if (r1 > mid - 1) r1 = mid - 1;
				part[b] = r0 <= r1;
				pr[b][0] = r0; pr[b][1] = r1;
				pr[b][2] = flipy ? sy + (h - 1) - (r1 - y) : sy + (r0 - y);
				pr[b][3] = flipy ? sy + (h - 1) - (r0 - y) : sy + (r1 - y);
			}
			if (!tail) {
				int why = -1;
				for (int b = 0; b < 2 && why < 0; b++) {
					if (!part[b]) continue;
					bool rd = false;
#define SS_RDCHK(a, c) rd = rd || ss_hit_rect(1 - b, sc0, sc1, a, c)
					SS_ROWS(pr[b][2], pr[b][3], SS_RDCHK);
#undef SS_RDCHK
					if (rd) why = 0;   /* reads the other band's output */
					else {
						int ex0 = dx0, ex1 = dx1, ey0 = pr[b][0], ey1 = pr[b][1];
						ss_clamp(ex0, ex1, ey0, ey1);
						if (ss_hit_rect(3 - b, ex0, ex1, ey0, ey1)) why = 1;   /* overwrites the other band's input */
					}
				}
				if (why >= 0) {
					if (!seg) return false;
					/* sync point before this draw: everything earlier is finished on both cores */
					ss_reset();
					if (ss_nseg < (SS_SEGS < SS_MAXSEG ? SS_SEGS : SS_MAXSEG)) { ss_seg_addr[ss_nseg] = opaddr; ss_seg_ct[ss_nseg] = ct; ss_nseg++; }
					else { tail = true; ss_tail_addr = opaddr; ss_tail_ct = ct; }
				}
				if (!tail) for (int b = 0; b < 2; b++) {
					if (!part[b]) continue;
					int ex0 = dx0, ex1 = dx1, ey0 = pr[b][0], ey1 = pr[b][1];
					ss_clamp(ex0, ex1, ey0, ey1);
					ss_mark(b, ex0, ex1, ey0, ey1);
#define SS_RDMARK(a, c) ss_mark(2 + b, sc0, sc1, a, c)
					SS_ROWS(pr[b][2], pr[b][3], SS_RDMARK);
#undef SS_RDMARK
				}
			}
			for (int b = 0; b < 2; b++) {   /* union map for the upload check */
				if (!part[b]) continue;
				ss_all_mark(dx0, dx1, pr[b][0], pr[b][1]);
#define SS_ALLMARK(a, c) ss_all_mark(sc0, sc1, a, c)
				SS_ROWS(pr[b][2], pr[b][3], SS_ALLMARK);
#undef SS_ALLMARK
			}
			break;
		}
		default:
			return true;
		}
	}
}
#endif // EP1C_SPLIT

static inline UINT16 COPY_NEXT_WORD(UINT32 *addr)
{
	const UINT16 data = m_ram16[((*addr & m_main_rammask) >> 1)];
	m_ram16_copy[((*addr & m_main_rammask) >> 1)] = data;

	*addr += 2;

	return data;
}

static void gfx_upload_shadow_copy(UINT32 *addr)
{
	COPY_NEXT_WORD(addr);
	COPY_NEXT_WORD(addr);
	COPY_NEXT_WORD(addr);
	COPY_NEXT_WORD(addr);
	COPY_NEXT_WORD(addr);
	COPY_NEXT_WORD(addr);

	const UINT32 dimx = (COPY_NEXT_WORD(addr) & 0x1fff) + 1;
	const UINT32 dimy = (COPY_NEXT_WORD(addr) & 0x0fff) + 1;

	for (UINT32 y = 0; y < dimy; y++)
	{
		for (UINT32 x = 0; x < dimx; x++)
		{
			COPY_NEXT_WORD(addr);
		}
	}

	// Time spent on uploads is mostly due to Main RAM accesses.
	// The Blitter will send BREQ requests to the SH-3, to access Main RAM
	// and then write it to VRAM.
	// The number of bytes to read are the sum of a 16b fixed header and the pixel
	// data (2 byte per pixel). RAM accesses are 32bit, so divide by four for clocks.
	//
	// TODO: There's additional overhead to these request thats are not included. The BREQ
	// assertion also puts CPU into WAIT, if it needs uncached RAM accesses.
	int num_sram_clk = (16 + dimx * dimy * 2 ) / 4;
	m_blit_delay_ns += num_sram_clk * EP1C_SRAM_CLK_NANOSEC;
	m_blit_idle_op_bytes = 0;
}

static void gfx_upload(UINT32 *addr, bool do_upload)
{
	UINT32 x,y, dst_p,dst_x_start,dst_y_start, dimx,dimy;
	UINT32 *dst;

	// 0x20000000
	READ_NEXT_WORD(addr);
	READ_NEXT_WORD(addr);

	// 0x99999999
	READ_NEXT_WORD(addr);
	READ_NEXT_WORD(addr);

	dst_x_start = READ_NEXT_WORD(addr);
	dst_y_start = READ_NEXT_WORD(addr);

	dst_p = 0;
	dst_x_start &= 0x1fff;
	dst_y_start &= 0x0fff;

	dimx = (READ_NEXT_WORD(addr) & 0x1fff) + 1;
	dimy = (READ_NEXT_WORD(addr) & 0x0fff) + 1;

	/* draws-only pass: skip the texel data (READ_NEXT_WORD steps 2 bytes per word) */
	if (!do_upload) { *addr += (UINT32)dimx * dimy * 2; return; }

	//bprintf(0, _T("GFX COPY: DST %02X,%02X,%03X DIM %02X,%03X\n"), dst_p,dst_x_start,dst_y_start, dimx,dimy);

	for (y = 0; y < dimy; y++)
	{
		//dst = &m_bitmaps->pix(dst_y_start + y, 0);
		dst = m_bitmaps + (dst_y_start + y) * EP1C_PITCH;

		dst += dst_x_start;

		for (x = 0; x < dimx; x++)
		{
			UINT16 pendat = READ_NEXT_WORD(addr);
			// real hw would upload the gfxword directly, but our VRAM is 32-bit, so convert it.
			//dst[dst_x_start + x] = pendat;
			*dst++ = ((pendat&0x8000)<<14) | ((pendat&0x7c00)<<9) | ((pendat&0x03e0)<<6) | ((pendat&0x001f)<<3);  // --t- ---- rrrr r--- gggg g--- bbbb b---  format
			//dst[dst_x_start + x] = ((pendat&0x8000)<<14) | ((pendat&0x7c00)<<6) | ((pendat&0x03e0)<<3) | ((pendat&0x001f)<<0);  // --t- ---- ---r rrrr ---g gggg ---b bbbb  format
		}
	}
}

#define draw_params clip, m_bitmaps,src_x,src_y, x,y, dimx,dimy, flipy, s_alpha, d_alpha, &tint_clr



static epic12_device_blitfunction epic12_device_f0_ti1_tr1_blit_funcs[] =
{
	draw_sprite_f0_ti1_tr1_s0_d0, draw_sprite_f0_ti1_tr1_s1_d0, draw_sprite_f0_ti1_tr1_s2_d0, draw_sprite_f0_ti1_tr1_s3_d0, draw_sprite_f0_ti1_tr1_s4_d0, draw_sprite_f0_ti1_tr1_s5_d0, draw_sprite_f0_ti1_tr1_s6_d0, draw_sprite_f0_ti1_tr1_s7_d0,
	draw_sprite_f0_ti1_tr1_s0_d1, draw_sprite_f0_ti1_tr1_s1_d1, draw_sprite_f0_ti1_tr1_s2_d1, draw_sprite_f0_ti1_tr1_s3_d1, draw_sprite_f0_ti1_tr1_s4_d1, draw_sprite_f0_ti1_tr1_s5_d1, draw_sprite_f0_ti1_tr1_s6_d1, draw_sprite_f0_ti1_tr1_s7_d1,
	draw_sprite_f0_ti1_tr1_s0_d2, draw_sprite_f0_ti1_tr1_s1_d2, draw_sprite_f0_ti1_tr1_s2_d2, draw_sprite_f0_ti1_tr1_s3_d2, draw_sprite_f0_ti1_tr1_s4_d2, draw_sprite_f0_ti1_tr1_s5_d2, draw_sprite_f0_ti1_tr1_s6_d2, draw_sprite_f0_ti1_tr1_s7_d2,
	draw_sprite_f0_ti1_tr1_s0_d3, draw_sprite_f0_ti1_tr1_s1_d3, draw_sprite_f0_ti1_tr1_s2_d3, draw_sprite_f0_ti1_tr1_s3_d3, draw_sprite_f0_ti1_tr1_s4_d3, draw_sprite_f0_ti1_tr1_s5_d3, draw_sprite_f0_ti1_tr1_s6_d3, draw_sprite_f0_ti1_tr1_s7_d3,
	draw_sprite_f0_ti1_tr1_s0_d4, draw_sprite_f0_ti1_tr1_s1_d4, draw_sprite_f0_ti1_tr1_s2_d4, draw_sprite_f0_ti1_tr1_s3_d4, draw_sprite_f0_ti1_tr1_s4_d4, draw_sprite_f0_ti1_tr1_s5_d4, draw_sprite_f0_ti1_tr1_s6_d4, draw_sprite_f0_ti1_tr1_s7_d4,
	draw_sprite_f0_ti1_tr1_s0_d5, draw_sprite_f0_ti1_tr1_s1_d5, draw_sprite_f0_ti1_tr1_s2_d5, draw_sprite_f0_ti1_tr1_s3_d5, draw_sprite_f0_ti1_tr1_s4_d5, draw_sprite_f0_ti1_tr1_s5_d5, draw_sprite_f0_ti1_tr1_s6_d5, draw_sprite_f0_ti1_tr1_s7_d5,
	draw_sprite_f0_ti1_tr1_s0_d6, draw_sprite_f0_ti1_tr1_s1_d6, draw_sprite_f0_ti1_tr1_s2_d6, draw_sprite_f0_ti1_tr1_s3_d6, draw_sprite_f0_ti1_tr1_s4_d6, draw_sprite_f0_ti1_tr1_s5_d6, draw_sprite_f0_ti1_tr1_s6_d6, draw_sprite_f0_ti1_tr1_s7_d6,
	draw_sprite_f0_ti1_tr1_s0_d7, draw_sprite_f0_ti1_tr1_s1_d7, draw_sprite_f0_ti1_tr1_s2_d7, draw_sprite_f0_ti1_tr1_s3_d7, draw_sprite_f0_ti1_tr1_s4_d7, draw_sprite_f0_ti1_tr1_s5_d7, draw_sprite_f0_ti1_tr1_s6_d7, draw_sprite_f0_ti1_tr1_s7_d7,
};

static epic12_device_blitfunction epic12_device_f0_ti1_tr0_blit_funcs[] =
{
	draw_sprite_f0_ti1_tr0_s0_d0, draw_sprite_f0_ti1_tr0_s1_d0, draw_sprite_f0_ti1_tr0_s2_d0, draw_sprite_f0_ti1_tr0_s3_d0, draw_sprite_f0_ti1_tr0_s4_d0, draw_sprite_f0_ti1_tr0_s5_d0, draw_sprite_f0_ti1_tr0_s6_d0, draw_sprite_f0_ti1_tr0_s7_d0,
	draw_sprite_f0_ti1_tr0_s0_d1, draw_sprite_f0_ti1_tr0_s1_d1, draw_sprite_f0_ti1_tr0_s2_d1, draw_sprite_f0_ti1_tr0_s3_d1, draw_sprite_f0_ti1_tr0_s4_d1, draw_sprite_f0_ti1_tr0_s5_d1, draw_sprite_f0_ti1_tr0_s6_d1, draw_sprite_f0_ti1_tr0_s7_d1,
	draw_sprite_f0_ti1_tr0_s0_d2, draw_sprite_f0_ti1_tr0_s1_d2, draw_sprite_f0_ti1_tr0_s2_d2, draw_sprite_f0_ti1_tr0_s3_d2, draw_sprite_f0_ti1_tr0_s4_d2, draw_sprite_f0_ti1_tr0_s5_d2, draw_sprite_f0_ti1_tr0_s6_d2, draw_sprite_f0_ti1_tr0_s7_d2,
	draw_sprite_f0_ti1_tr0_s0_d3, draw_sprite_f0_ti1_tr0_s1_d3, draw_sprite_f0_ti1_tr0_s2_d3, draw_sprite_f0_ti1_tr0_s3_d3, draw_sprite_f0_ti1_tr0_s4_d3, draw_sprite_f0_ti1_tr0_s5_d3, draw_sprite_f0_ti1_tr0_s6_d3, draw_sprite_f0_ti1_tr0_s7_d3,
	draw_sprite_f0_ti1_tr0_s0_d4, draw_sprite_f0_ti1_tr0_s1_d4, draw_sprite_f0_ti1_tr0_s2_d4, draw_sprite_f0_ti1_tr0_s3_d4, draw_sprite_f0_ti1_tr0_s4_d4, draw_sprite_f0_ti1_tr0_s5_d4, draw_sprite_f0_ti1_tr0_s6_d4, draw_sprite_f0_ti1_tr0_s7_d4,
	draw_sprite_f0_ti1_tr0_s0_d5, draw_sprite_f0_ti1_tr0_s1_d5, draw_sprite_f0_ti1_tr0_s2_d5, draw_sprite_f0_ti1_tr0_s3_d5, draw_sprite_f0_ti1_tr0_s4_d5, draw_sprite_f0_ti1_tr0_s5_d5, draw_sprite_f0_ti1_tr0_s6_d5, draw_sprite_f0_ti1_tr0_s7_d5,
	draw_sprite_f0_ti1_tr0_s0_d6, draw_sprite_f0_ti1_tr0_s1_d6, draw_sprite_f0_ti1_tr0_s2_d6, draw_sprite_f0_ti1_tr0_s3_d6, draw_sprite_f0_ti1_tr0_s4_d6, draw_sprite_f0_ti1_tr0_s5_d6, draw_sprite_f0_ti1_tr0_s6_d6, draw_sprite_f0_ti1_tr0_s7_d6,
	draw_sprite_f0_ti1_tr0_s0_d7, draw_sprite_f0_ti1_tr0_s1_d7, draw_sprite_f0_ti1_tr0_s2_d7, draw_sprite_f0_ti1_tr0_s3_d7, draw_sprite_f0_ti1_tr0_s4_d7, draw_sprite_f0_ti1_tr0_s5_d7, draw_sprite_f0_ti1_tr0_s6_d7, draw_sprite_f0_ti1_tr0_s7_d7,
};

static epic12_device_blitfunction epic12_device_f1_ti1_tr1_blit_funcs[] =
{
	draw_sprite_f1_ti1_tr1_s0_d0, draw_sprite_f1_ti1_tr1_s1_d0, draw_sprite_f1_ti1_tr1_s2_d0, draw_sprite_f1_ti1_tr1_s3_d0, draw_sprite_f1_ti1_tr1_s4_d0, draw_sprite_f1_ti1_tr1_s5_d0, draw_sprite_f1_ti1_tr1_s6_d0, draw_sprite_f1_ti1_tr1_s7_d0,
	draw_sprite_f1_ti1_tr1_s0_d1, draw_sprite_f1_ti1_tr1_s1_d1, draw_sprite_f1_ti1_tr1_s2_d1, draw_sprite_f1_ti1_tr1_s3_d1, draw_sprite_f1_ti1_tr1_s4_d1, draw_sprite_f1_ti1_tr1_s5_d1, draw_sprite_f1_ti1_tr1_s6_d1, draw_sprite_f1_ti1_tr1_s7_d1,
	draw_sprite_f1_ti1_tr1_s0_d2, draw_sprite_f1_ti1_tr1_s1_d2, draw_sprite_f1_ti1_tr1_s2_d2, draw_sprite_f1_ti1_tr1_s3_d2, draw_sprite_f1_ti1_tr1_s4_d2, draw_sprite_f1_ti1_tr1_s5_d2, draw_sprite_f1_ti1_tr1_s6_d2, draw_sprite_f1_ti1_tr1_s7_d2,
	draw_sprite_f1_ti1_tr1_s0_d3, draw_sprite_f1_ti1_tr1_s1_d3, draw_sprite_f1_ti1_tr1_s2_d3, draw_sprite_f1_ti1_tr1_s3_d3, draw_sprite_f1_ti1_tr1_s4_d3, draw_sprite_f1_ti1_tr1_s5_d3, draw_sprite_f1_ti1_tr1_s6_d3, draw_sprite_f1_ti1_tr1_s7_d3,
	draw_sprite_f1_ti1_tr1_s0_d4, draw_sprite_f1_ti1_tr1_s1_d4, draw_sprite_f1_ti1_tr1_s2_d4, draw_sprite_f1_ti1_tr1_s3_d4, draw_sprite_f1_ti1_tr1_s4_d4, draw_sprite_f1_ti1_tr1_s5_d4, draw_sprite_f1_ti1_tr1_s6_d4, draw_sprite_f1_ti1_tr1_s7_d4,
	draw_sprite_f1_ti1_tr1_s0_d5, draw_sprite_f1_ti1_tr1_s1_d5, draw_sprite_f1_ti1_tr1_s2_d5, draw_sprite_f1_ti1_tr1_s3_d5, draw_sprite_f1_ti1_tr1_s4_d5, draw_sprite_f1_ti1_tr1_s5_d5, draw_sprite_f1_ti1_tr1_s6_d5, draw_sprite_f1_ti1_tr1_s7_d5,
	draw_sprite_f1_ti1_tr1_s0_d6, draw_sprite_f1_ti1_tr1_s1_d6, draw_sprite_f1_ti1_tr1_s2_d6, draw_sprite_f1_ti1_tr1_s3_d6, draw_sprite_f1_ti1_tr1_s4_d6, draw_sprite_f1_ti1_tr1_s5_d6, draw_sprite_f1_ti1_tr1_s6_d6, draw_sprite_f1_ti1_tr1_s7_d6,
	draw_sprite_f1_ti1_tr1_s0_d7, draw_sprite_f1_ti1_tr1_s1_d7, draw_sprite_f1_ti1_tr1_s2_d7, draw_sprite_f1_ti1_tr1_s3_d7, draw_sprite_f1_ti1_tr1_s4_d7, draw_sprite_f1_ti1_tr1_s5_d7, draw_sprite_f1_ti1_tr1_s6_d7, draw_sprite_f1_ti1_tr1_s7_d7,
};

static epic12_device_blitfunction epic12_device_f1_ti1_tr0_blit_funcs[] =
{
	draw_sprite_f1_ti1_tr0_s0_d0, draw_sprite_f1_ti1_tr0_s1_d0, draw_sprite_f1_ti1_tr0_s2_d0, draw_sprite_f1_ti1_tr0_s3_d0, draw_sprite_f1_ti1_tr0_s4_d0, draw_sprite_f1_ti1_tr0_s5_d0, draw_sprite_f1_ti1_tr0_s6_d0, draw_sprite_f1_ti1_tr0_s7_d0,
	draw_sprite_f1_ti1_tr0_s0_d1, draw_sprite_f1_ti1_tr0_s1_d1, draw_sprite_f1_ti1_tr0_s2_d1, draw_sprite_f1_ti1_tr0_s3_d1, draw_sprite_f1_ti1_tr0_s4_d1, draw_sprite_f1_ti1_tr0_s5_d1, draw_sprite_f1_ti1_tr0_s6_d1, draw_sprite_f1_ti1_tr0_s7_d1,
	draw_sprite_f1_ti1_tr0_s0_d2, draw_sprite_f1_ti1_tr0_s1_d2, draw_sprite_f1_ti1_tr0_s2_d2, draw_sprite_f1_ti1_tr0_s3_d2, draw_sprite_f1_ti1_tr0_s4_d2, draw_sprite_f1_ti1_tr0_s5_d2, draw_sprite_f1_ti1_tr0_s6_d2, draw_sprite_f1_ti1_tr0_s7_d2,
	draw_sprite_f1_ti1_tr0_s0_d3, draw_sprite_f1_ti1_tr0_s1_d3, draw_sprite_f1_ti1_tr0_s2_d3, draw_sprite_f1_ti1_tr0_s3_d3, draw_sprite_f1_ti1_tr0_s4_d3, draw_sprite_f1_ti1_tr0_s5_d3, draw_sprite_f1_ti1_tr0_s6_d3, draw_sprite_f1_ti1_tr0_s7_d3,
	draw_sprite_f1_ti1_tr0_s0_d4, draw_sprite_f1_ti1_tr0_s1_d4, draw_sprite_f1_ti1_tr0_s2_d4, draw_sprite_f1_ti1_tr0_s3_d4, draw_sprite_f1_ti1_tr0_s4_d4, draw_sprite_f1_ti1_tr0_s5_d4, draw_sprite_f1_ti1_tr0_s6_d4, draw_sprite_f1_ti1_tr0_s7_d4,
	draw_sprite_f1_ti1_tr0_s0_d5, draw_sprite_f1_ti1_tr0_s1_d5, draw_sprite_f1_ti1_tr0_s2_d5, draw_sprite_f1_ti1_tr0_s3_d5, draw_sprite_f1_ti1_tr0_s4_d5, draw_sprite_f1_ti1_tr0_s5_d5, draw_sprite_f1_ti1_tr0_s6_d5, draw_sprite_f1_ti1_tr0_s7_d5,
	draw_sprite_f1_ti1_tr0_s0_d6, draw_sprite_f1_ti1_tr0_s1_d6, draw_sprite_f1_ti1_tr0_s2_d6, draw_sprite_f1_ti1_tr0_s3_d6, draw_sprite_f1_ti1_tr0_s4_d6, draw_sprite_f1_ti1_tr0_s5_d6, draw_sprite_f1_ti1_tr0_s6_d6, draw_sprite_f1_ti1_tr0_s7_d6,
	draw_sprite_f1_ti1_tr0_s0_d7, draw_sprite_f1_ti1_tr0_s1_d7, draw_sprite_f1_ti1_tr0_s2_d7, draw_sprite_f1_ti1_tr0_s3_d7, draw_sprite_f1_ti1_tr0_s4_d7, draw_sprite_f1_ti1_tr0_s5_d7, draw_sprite_f1_ti1_tr0_s6_d7, draw_sprite_f1_ti1_tr0_s7_d7,
};



static epic12_device_blitfunction epic12_device_f0_ti0_tr1_blit_funcs[] =
{
	draw_sprite_f0_ti0_tr1_s0_d0, draw_sprite_f0_ti0_tr1_s1_d0, draw_sprite_f0_ti0_tr1_s2_d0, draw_sprite_f0_ti0_tr1_s3_d0, draw_sprite_f0_ti0_tr1_s4_d0, draw_sprite_f0_ti0_tr1_s5_d0, draw_sprite_f0_ti0_tr1_s6_d0, draw_sprite_f0_ti0_tr1_s7_d0,
	draw_sprite_f0_ti0_tr1_s0_d1, draw_sprite_f0_ti0_tr1_s1_d1, draw_sprite_f0_ti0_tr1_s2_d1, draw_sprite_f0_ti0_tr1_s3_d1, draw_sprite_f0_ti0_tr1_s4_d1, draw_sprite_f0_ti0_tr1_s5_d1, draw_sprite_f0_ti0_tr1_s6_d1, draw_sprite_f0_ti0_tr1_s7_d1,
	draw_sprite_f0_ti0_tr1_s0_d2, draw_sprite_f0_ti0_tr1_s1_d2, draw_sprite_f0_ti0_tr1_s2_d2, draw_sprite_f0_ti0_tr1_s3_d2, draw_sprite_f0_ti0_tr1_s4_d2, draw_sprite_f0_ti0_tr1_s5_d2, draw_sprite_f0_ti0_tr1_s6_d2, draw_sprite_f0_ti0_tr1_s7_d2,
	draw_sprite_f0_ti0_tr1_s0_d3, draw_sprite_f0_ti0_tr1_s1_d3, draw_sprite_f0_ti0_tr1_s2_d3, draw_sprite_f0_ti0_tr1_s3_d3, draw_sprite_f0_ti0_tr1_s4_d3, draw_sprite_f0_ti0_tr1_s5_d3, draw_sprite_f0_ti0_tr1_s6_d3, draw_sprite_f0_ti0_tr1_s7_d3,
	draw_sprite_f0_ti0_tr1_s0_d4, draw_sprite_f0_ti0_tr1_s1_d4, draw_sprite_f0_ti0_tr1_s2_d4, draw_sprite_f0_ti0_tr1_s3_d4, draw_sprite_f0_ti0_tr1_s4_d4, draw_sprite_f0_ti0_tr1_s5_d4, draw_sprite_f0_ti0_tr1_s6_d4, draw_sprite_f0_ti0_tr1_s7_d4,
	draw_sprite_f0_ti0_tr1_s0_d5, draw_sprite_f0_ti0_tr1_s1_d5, draw_sprite_f0_ti0_tr1_s2_d5, draw_sprite_f0_ti0_tr1_s3_d5, draw_sprite_f0_ti0_tr1_s4_d5, draw_sprite_f0_ti0_tr1_s5_d5, draw_sprite_f0_ti0_tr1_s6_d5, draw_sprite_f0_ti0_tr1_s7_d5,
	draw_sprite_f0_ti0_tr1_s0_d6, draw_sprite_f0_ti0_tr1_s1_d6, draw_sprite_f0_ti0_tr1_s2_d6, draw_sprite_f0_ti0_tr1_s3_d6, draw_sprite_f0_ti0_tr1_s4_d6, draw_sprite_f0_ti0_tr1_s5_d6, draw_sprite_f0_ti0_tr1_s6_d6, draw_sprite_f0_ti0_tr1_s7_d6,
	draw_sprite_f0_ti0_tr1_s0_d7, draw_sprite_f0_ti0_tr1_s1_d7, draw_sprite_f0_ti0_tr1_s2_d7, draw_sprite_f0_ti0_tr1_s3_d7, draw_sprite_f0_ti0_tr1_s4_d7, draw_sprite_f0_ti0_tr1_s5_d7, draw_sprite_f0_ti0_tr1_s6_d7, draw_sprite_f0_ti0_tr1_s7_d7,
};

static epic12_device_blitfunction epic12_device_f0_ti0_tr0_blit_funcs[] =
{
	draw_sprite_f0_ti0_tr0_s0_d0, draw_sprite_f0_ti0_tr0_s1_d0, draw_sprite_f0_ti0_tr0_s2_d0, draw_sprite_f0_ti0_tr0_s3_d0, draw_sprite_f0_ti0_tr0_s4_d0, draw_sprite_f0_ti0_tr0_s5_d0, draw_sprite_f0_ti0_tr0_s6_d0, draw_sprite_f0_ti0_tr0_s7_d0,
	draw_sprite_f0_ti0_tr0_s0_d1, draw_sprite_f0_ti0_tr0_s1_d1, draw_sprite_f0_ti0_tr0_s2_d1, draw_sprite_f0_ti0_tr0_s3_d1, draw_sprite_f0_ti0_tr0_s4_d1, draw_sprite_f0_ti0_tr0_s5_d1, draw_sprite_f0_ti0_tr0_s6_d1, draw_sprite_f0_ti0_tr0_s7_d1,
	draw_sprite_f0_ti0_tr0_s0_d2, draw_sprite_f0_ti0_tr0_s1_d2, draw_sprite_f0_ti0_tr0_s2_d2, draw_sprite_f0_ti0_tr0_s3_d2, draw_sprite_f0_ti0_tr0_s4_d2, draw_sprite_f0_ti0_tr0_s5_d2, draw_sprite_f0_ti0_tr0_s6_d2, draw_sprite_f0_ti0_tr0_s7_d2,
	draw_sprite_f0_ti0_tr0_s0_d3, draw_sprite_f0_ti0_tr0_s1_d3, draw_sprite_f0_ti0_tr0_s2_d3, draw_sprite_f0_ti0_tr0_s3_d3, draw_sprite_f0_ti0_tr0_s4_d3, draw_sprite_f0_ti0_tr0_s5_d3, draw_sprite_f0_ti0_tr0_s6_d3, draw_sprite_f0_ti0_tr0_s7_d3,
	draw_sprite_f0_ti0_tr0_s0_d4, draw_sprite_f0_ti0_tr0_s1_d4, draw_sprite_f0_ti0_tr0_s2_d4, draw_sprite_f0_ti0_tr0_s3_d4, draw_sprite_f0_ti0_tr0_s4_d4, draw_sprite_f0_ti0_tr0_s5_d4, draw_sprite_f0_ti0_tr0_s6_d4, draw_sprite_f0_ti0_tr0_s7_d4,
	draw_sprite_f0_ti0_tr0_s0_d5, draw_sprite_f0_ti0_tr0_s1_d5, draw_sprite_f0_ti0_tr0_s2_d5, draw_sprite_f0_ti0_tr0_s3_d5, draw_sprite_f0_ti0_tr0_s4_d5, draw_sprite_f0_ti0_tr0_s5_d5, draw_sprite_f0_ti0_tr0_s6_d5, draw_sprite_f0_ti0_tr0_s7_d5,
	draw_sprite_f0_ti0_tr0_s0_d6, draw_sprite_f0_ti0_tr0_s1_d6, draw_sprite_f0_ti0_tr0_s2_d6, draw_sprite_f0_ti0_tr0_s3_d6, draw_sprite_f0_ti0_tr0_s4_d6, draw_sprite_f0_ti0_tr0_s5_d6, draw_sprite_f0_ti0_tr0_s6_d6, draw_sprite_f0_ti0_tr0_s7_d6,
	draw_sprite_f0_ti0_tr0_s0_d7, draw_sprite_f0_ti0_tr0_s1_d7, draw_sprite_f0_ti0_tr0_s2_d7, draw_sprite_f0_ti0_tr0_s3_d7, draw_sprite_f0_ti0_tr0_s4_d7, draw_sprite_f0_ti0_tr0_s5_d7, draw_sprite_f0_ti0_tr0_s6_d7, draw_sprite_f0_ti0_tr0_s7_d7,
};

static epic12_device_blitfunction epic12_device_f1_ti0_tr1_blit_funcs[] =
{
	draw_sprite_f1_ti0_tr1_s0_d0, draw_sprite_f1_ti0_tr1_s1_d0, draw_sprite_f1_ti0_tr1_s2_d0, draw_sprite_f1_ti0_tr1_s3_d0, draw_sprite_f1_ti0_tr1_s4_d0, draw_sprite_f1_ti0_tr1_s5_d0, draw_sprite_f1_ti0_tr1_s6_d0, draw_sprite_f1_ti0_tr1_s7_d0,
	draw_sprite_f1_ti0_tr1_s0_d1, draw_sprite_f1_ti0_tr1_s1_d1, draw_sprite_f1_ti0_tr1_s2_d1, draw_sprite_f1_ti0_tr1_s3_d1, draw_sprite_f1_ti0_tr1_s4_d1, draw_sprite_f1_ti0_tr1_s5_d1, draw_sprite_f1_ti0_tr1_s6_d1, draw_sprite_f1_ti0_tr1_s7_d1,
	draw_sprite_f1_ti0_tr1_s0_d2, draw_sprite_f1_ti0_tr1_s1_d2, draw_sprite_f1_ti0_tr1_s2_d2, draw_sprite_f1_ti0_tr1_s3_d2, draw_sprite_f1_ti0_tr1_s4_d2, draw_sprite_f1_ti0_tr1_s5_d2, draw_sprite_f1_ti0_tr1_s6_d2, draw_sprite_f1_ti0_tr1_s7_d2,
	draw_sprite_f1_ti0_tr1_s0_d3, draw_sprite_f1_ti0_tr1_s1_d3, draw_sprite_f1_ti0_tr1_s2_d3, draw_sprite_f1_ti0_tr1_s3_d3, draw_sprite_f1_ti0_tr1_s4_d3, draw_sprite_f1_ti0_tr1_s5_d3, draw_sprite_f1_ti0_tr1_s6_d3, draw_sprite_f1_ti0_tr1_s7_d3,
	draw_sprite_f1_ti0_tr1_s0_d4, draw_sprite_f1_ti0_tr1_s1_d4, draw_sprite_f1_ti0_tr1_s2_d4, draw_sprite_f1_ti0_tr1_s3_d4, draw_sprite_f1_ti0_tr1_s4_d4, draw_sprite_f1_ti0_tr1_s5_d4, draw_sprite_f1_ti0_tr1_s6_d4, draw_sprite_f1_ti0_tr1_s7_d4,
	draw_sprite_f1_ti0_tr1_s0_d5, draw_sprite_f1_ti0_tr1_s1_d5, draw_sprite_f1_ti0_tr1_s2_d5, draw_sprite_f1_ti0_tr1_s3_d5, draw_sprite_f1_ti0_tr1_s4_d5, draw_sprite_f1_ti0_tr1_s5_d5, draw_sprite_f1_ti0_tr1_s6_d5, draw_sprite_f1_ti0_tr1_s7_d5,
	draw_sprite_f1_ti0_tr1_s0_d6, draw_sprite_f1_ti0_tr1_s1_d6, draw_sprite_f1_ti0_tr1_s2_d6, draw_sprite_f1_ti0_tr1_s3_d6, draw_sprite_f1_ti0_tr1_s4_d6, draw_sprite_f1_ti0_tr1_s5_d6, draw_sprite_f1_ti0_tr1_s6_d6, draw_sprite_f1_ti0_tr1_s7_d6,
	draw_sprite_f1_ti0_tr1_s0_d7, draw_sprite_f1_ti0_tr1_s1_d7, draw_sprite_f1_ti0_tr1_s2_d7, draw_sprite_f1_ti0_tr1_s3_d7, draw_sprite_f1_ti0_tr1_s4_d7, draw_sprite_f1_ti0_tr1_s5_d7, draw_sprite_f1_ti0_tr1_s6_d7, draw_sprite_f1_ti0_tr1_s7_d7,
};

static epic12_device_blitfunction epic12_device_f1_ti0_tr0_blit_funcs[] =
{
	draw_sprite_f1_ti0_tr0_s0_d0, draw_sprite_f1_ti0_tr0_s1_d0, draw_sprite_f1_ti0_tr0_s2_d0, draw_sprite_f1_ti0_tr0_s3_d0, draw_sprite_f1_ti0_tr0_s4_d0, draw_sprite_f1_ti0_tr0_s5_d0, draw_sprite_f1_ti0_tr0_s6_d0, draw_sprite_f1_ti0_tr0_s7_d0,
	draw_sprite_f1_ti0_tr0_s0_d1, draw_sprite_f1_ti0_tr0_s1_d1, draw_sprite_f1_ti0_tr0_s2_d1, draw_sprite_f1_ti0_tr0_s3_d1, draw_sprite_f1_ti0_tr0_s4_d1, draw_sprite_f1_ti0_tr0_s5_d1, draw_sprite_f1_ti0_tr0_s6_d1, draw_sprite_f1_ti0_tr0_s7_d1,
	draw_sprite_f1_ti0_tr0_s0_d2, draw_sprite_f1_ti0_tr0_s1_d2, draw_sprite_f1_ti0_tr0_s2_d2, draw_sprite_f1_ti0_tr0_s3_d2, draw_sprite_f1_ti0_tr0_s4_d2, draw_sprite_f1_ti0_tr0_s5_d2, draw_sprite_f1_ti0_tr0_s6_d2, draw_sprite_f1_ti0_tr0_s7_d2,
	draw_sprite_f1_ti0_tr0_s0_d3, draw_sprite_f1_ti0_tr0_s1_d3, draw_sprite_f1_ti0_tr0_s2_d3, draw_sprite_f1_ti0_tr0_s3_d3, draw_sprite_f1_ti0_tr0_s4_d3, draw_sprite_f1_ti0_tr0_s5_d3, draw_sprite_f1_ti0_tr0_s6_d3, draw_sprite_f1_ti0_tr0_s7_d3,
	draw_sprite_f1_ti0_tr0_s0_d4, draw_sprite_f1_ti0_tr0_s1_d4, draw_sprite_f1_ti0_tr0_s2_d4, draw_sprite_f1_ti0_tr0_s3_d4, draw_sprite_f1_ti0_tr0_s4_d4, draw_sprite_f1_ti0_tr0_s5_d4, draw_sprite_f1_ti0_tr0_s6_d4, draw_sprite_f1_ti0_tr0_s7_d4,
	draw_sprite_f1_ti0_tr0_s0_d5, draw_sprite_f1_ti0_tr0_s1_d5, draw_sprite_f1_ti0_tr0_s2_d5, draw_sprite_f1_ti0_tr0_s3_d5, draw_sprite_f1_ti0_tr0_s4_d5, draw_sprite_f1_ti0_tr0_s5_d5, draw_sprite_f1_ti0_tr0_s6_d5, draw_sprite_f1_ti0_tr0_s7_d5,
	draw_sprite_f1_ti0_tr0_s0_d6, draw_sprite_f1_ti0_tr0_s1_d6, draw_sprite_f1_ti0_tr0_s2_d6, draw_sprite_f1_ti0_tr0_s3_d6, draw_sprite_f1_ti0_tr0_s4_d6, draw_sprite_f1_ti0_tr0_s5_d6, draw_sprite_f1_ti0_tr0_s6_d6, draw_sprite_f1_ti0_tr0_s7_d6,
	draw_sprite_f1_ti0_tr0_s0_d7, draw_sprite_f1_ti0_tr0_s1_d7, draw_sprite_f1_ti0_tr0_s2_d7, draw_sprite_f1_ti0_tr0_s3_d7, draw_sprite_f1_ti0_tr0_s4_d7, draw_sprite_f1_ti0_tr0_s5_d7, draw_sprite_f1_ti0_tr0_s6_d7, draw_sprite_f1_ti0_tr0_s7_d7,
};

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

/*
    Calculate number of VRAM row accesses a draw will perform.
    Source data will typically be aligned well with VRAM, but this is not the case for the destination.
    As an example, drawing a 64x32 pixel image will usually read from two VRAM rows for source data,
    but if the destination start coordinate is (x=10, y=10), each of the 32x32px chunks of source data will
    touch 4 rows of destination VRAM, leading to a total of 8 destination VRAM accesses.
*/
inline UINT16 calculate_vram_accesses(UINT16 start_x, UINT16 start_y, UINT16 dimx, UINT16 dimy)
{
	int x_rows = 0;
	int num_vram_rows = 0;
	for (int x_pixels = dimx; x_pixels > 0; x_pixels -= 32)
	{
		x_rows++;
		if (((start_x & 31) + MIN(32, x_pixels)) > 32)
			x_rows++;  // Drawing across multiple horizontal VRAM row boundaries.
	}
	for (int y_pixels = dimy; y_pixels > 0; y_pixels -= 32)
	{
		num_vram_rows += x_rows;
		if (((start_y & 31) + MIN(32, y_pixels)) > 32)
			num_vram_rows += x_rows;  // Drawing across multiple vertical VRAM row boundaries.	
	}
	return num_vram_rows;
}

static void gfx_draw_shadow_copy(UINT32 *addr)
{
	COPY_NEXT_WORD(addr);
	COPY_NEXT_WORD(addr);
	UINT16 src_x_start = COPY_NEXT_WORD(addr);
	UINT16 src_y_start = COPY_NEXT_WORD(addr);
	UINT16 dst_x_start = COPY_NEXT_WORD(addr);
	UINT16 dst_y_start = COPY_NEXT_WORD(addr);
	UINT16 src_dimx = (COPY_NEXT_WORD(addr) & 0x1fff) + 1;
	UINT16 src_dimy = (COPY_NEXT_WORD(addr) & 0x0fff) + 1;
	COPY_NEXT_WORD(addr);
	COPY_NEXT_WORD(addr);

	// Calculate Blitter delay for the Draw operation.
	// On real hardware, the Blitter will read operations into a FIFO queue
	// by asserting BREQ on the SH3 and then reading from Main RAM.
	// Since the reads are done concurrently to executions of operations, its
	// ok to estimate the delay all at once instead for emulation purposes.
	
	UINT16 dst_x_end = dst_x_start + src_dimx - 1;
	UINT16 dst_y_end = dst_y_start + src_dimy - 1;

	// Sprites fully outside of clipping area should not be drawn.
	if (dst_x_start > m_clip.max_x || dst_x_end < m_clip.min_x || dst_y_start > m_clip.max_y || dst_y_end < m_clip.min_y)
	{
		idle_blitter(EP1C_DRAW_OPERATION_SIZE_BYTES);
		return;
	}

	// Clip the blitter operations, to have the calculations only respect the area being written.
	// It's not 100% clear this is how this is performed, but it is clear that there should be some amount of clipping
	// applied here to match the hardware. This way seems most likely, and maps well to the delays seen on hardware.
	// One example of this being utilized heavily is the transparent fog in Mushihimesama Futari Stage 1. This is drawn as
	// 256x256 sprites, with large parts clipped away.

	dst_x_start = MAX(dst_x_start, (UINT16)m_clip.min_x);
	dst_y_start = MAX(dst_y_start, (UINT16)m_clip.min_y);
	dst_x_end = MIN(dst_x_end, (UINT16)m_clip.max_x);
	dst_y_end = MIN(dst_y_end, (UINT16)m_clip.max_y);
	src_dimx = dst_x_end - dst_x_start + 1;
	src_dimy = dst_y_end - dst_y_start + 1;

	m_blit_idle_op_bytes = 0;  // Blitter no longer idle.

	// VRAM data is laid out in 32x32 pixel rows. Calculate amount of rows accessed. 
	int src_num_vram_rows = calculate_vram_accesses(src_x_start, src_y_start, src_dimx, src_dimy);
	int dst_num_vram_rows = calculate_vram_accesses(dst_x_start, dst_y_start, src_dimx, src_dimy);

	// Since draws are done 4 pixels at the time, extend the draw area to coordinates aligned for this.
	// Doing this after VRAM calculations simplify things a bit, and these extensions will never make the
	// destination area span additional VRAM rows. 
	dst_x_start -= dst_x_start & 3;
	dst_x_end += (4 - ((dst_x_end + 1) & 3)) & 3;
	UINT16 dst_dimx = dst_x_end - dst_x_start + 1;
	UINT16 dst_dimy = dst_y_end - dst_y_start + 1;

	// Number of VRAM CLK cycles needed to draw a sprite is sum of:
	// - Number of pixels read from source divided by 4 (Each CLK reads 4 pixels, since 32bit DDR).
	// - Number of pixels read from destination divided by 4.
	// - Pixels written to destination divided by 4.
	// - VRAM access overhead:
	//   - 6 CLK of overhead after each read from a source VRAM row.
	//   - 20 CLK of overhead between read and write of each destination VRAM row.
	//   - 11 CLK of overhead after each write to a destination VRAM row.
	// - 12 CLK of additional overhead per sprite at the end of writing.
	// Note: Details are from https://buffis.com/docs/CV1000_Blitter_Research_by_buffi.pdf
	//       There may be mistakes.	
	UINT32 num_vram_clk = src_dimx * src_dimy / 4 + dst_dimx * dst_dimy / 2 + src_num_vram_rows * 6 + dst_num_vram_rows * (20 + 11) + 12;
	m_blit_delay_ns += num_vram_clk * EP1C_VRAM_CLK_NANOSEC;
}

static void gfx_draw(UINT32 *addr, const rectangle *clip, bool do_draw)
{
	int x,y, dimx,dimy, flipx,flipy;//, src_p;
	int trans,blend, s_mode, d_mode;
	clr_t tint_clr;
	int tinted = 0;

	UINT16 attr     =   READ_NEXT_WORD(addr);
	UINT16 alpha    =   READ_NEXT_WORD(addr);
	UINT16 src_x    =   READ_NEXT_WORD(addr);
	UINT16 src_y    =   READ_NEXT_WORD(addr);
	UINT16 dst_x_start  =   READ_NEXT_WORD(addr);
	UINT16 dst_y_start  =   READ_NEXT_WORD(addr);
	UINT16 w        =   READ_NEXT_WORD(addr);
	UINT16 h        =   READ_NEXT_WORD(addr);
	UINT16 tint_r   =   READ_NEXT_WORD(addr);
	UINT16 tint_gb  =   READ_NEXT_WORD(addr);

	if (!do_draw) return;   /* uploads-only pass: just step past this draw */

	// 0: +alpha
	// 1: +source
	// 2: +dest
	// 3: *
	// 4: -alpha
	// 5: -source
	// 6: -dest
	// 7: *

	d_mode  =    attr & 0x0007;
	s_mode  =   (attr & 0x0070) >> 4;

	trans   =    attr & 0x0100;
	blend   =      attr & 0x0200;

	flipy   =    attr & 0x0400;
	flipx   =    attr & 0x0800;

	const UINT8 d_alpha =   ((alpha & 0x00ff)       )>>3;
	const UINT8 s_alpha =   ((alpha & 0xff00) >> 8  )>>3;

//  src_p   =   0;
	src_x   =   src_x & 0x1fff;
	src_y   =   src_y & 0x0fff;


	x       =   (dst_x_start & 0x7fff) - (dst_x_start & 0x8000);
	y       =   (dst_y_start & 0x7fff) - (dst_y_start & 0x8000);

	dimx    =   (w & 0x1fff) + 1;
	dimy    =   (h & 0x0fff) + 1;

	// convert parameters to clr


	tint_to_clr(tint_r & 0x00ff, (tint_gb >>  8) & 0xff, tint_gb & 0xff, &tint_clr);

	/* interestingly this gets set to 0x20 for 'normal' not 0x1f */

	if (tint_clr.r!=0x20)
		tinted = 1;

	if (tint_clr.g!=0x20)
		tinted = 1;

	if (tint_clr.b!=0x20)
		tinted = 1;


	// surprisingly frequent, need to verify if it produces a worthwhile speedup tho.
	if ((s_mode==0 && s_alpha==0x1f) && (d_mode==4 && d_alpha==0x1f))
		blend = 0;

	if (tinted)
	{
		if (!flipx)
		{
			if (trans)
			{
				if (!blend)
				{
					draw_sprite_f0_ti1_tr1_plain(draw_params);
				}
				else
				{
					epic12_device_f0_ti1_tr1_blit_funcs[s_mode | (d_mode<<3)](draw_params);
				}
			}
			else
			{
			if (!blend)
				{
					draw_sprite_f0_ti1_tr0_plain(draw_params);
				}
				else
				{
					epic12_device_f0_ti1_tr0_blit_funcs[s_mode | (d_mode<<3)](draw_params);
				}
			}
		}
		else // flipx
		{
			if (trans)
			{
				if (!blend)
				{
					draw_sprite_f1_ti1_tr1_plain(draw_params);
				}
				else
				{
					epic12_device_f1_ti1_tr1_blit_funcs[s_mode | (d_mode<<3)](draw_params);
				}
			}
			else
			{
			if (!blend)
				{
					draw_sprite_f1_ti1_tr0_plain(draw_params);
				}
				else
				{
					epic12_device_f1_ti1_tr0_blit_funcs[s_mode | (d_mode<<3)](draw_params);
				}
			}
		}
	}
	else
	{
		if (blend==0 && tinted==0)
		{
			if (!flipx)
			{
				if (trans)
				{
					draw_sprite_f0_ti0_tr1_simple(draw_params);
				}
				else
				{
					draw_sprite_f0_ti0_tr0_simple(draw_params);
				}
			}
			else
			{
				if (trans)
				{
					draw_sprite_f1_ti0_tr1_simple(draw_params);
				}
				else
				{
					draw_sprite_f1_ti0_tr0_simple(draw_params);
				}

			}

			return;
		}



		//printf("smode %d dmode %d\n", s_mode, d_mode);

		if (!flipx)
		{
			if (trans)
			{
				if (!blend)
				{
					draw_sprite_f0_ti0_plain(draw_params);
				}
				else
				{
					epic12_device_f0_ti0_tr1_blit_funcs[s_mode | (d_mode<<3)](draw_params);
				}
			}
			else
			{
			if (!blend)
				{
					draw_sprite_f0_ti0_tr0_plain(draw_params);
				}
				else
				{
					epic12_device_f0_ti0_tr0_blit_funcs[s_mode | (d_mode<<3)](draw_params);
				}
			}
		}
		else // flipx
		{
			if (trans)
			{
				if (!blend)
				{
					draw_sprite_f1_ti0_plain(draw_params);
				}
				else
				{
					epic12_device_f1_ti0_tr1_blit_funcs[s_mode | (d_mode<<3)](draw_params);
				}
			}
			else
			{
			if (!blend)
				{
					draw_sprite_f1_ti0_tr0_plain(draw_params);
				}
				else
				{
					epic12_device_f1_ti0_tr0_blit_funcs[s_mode | (d_mode<<3)](draw_params);
				}
			}
		}
	}
}

static void gfx_create_shadow_copy()
{
	UINT32 addr = m_gfx_addr & 0x1fffffff;

	m_clip.set(m_gfx_clip_x_shadowcopy - EP1C_CLIP_MARGIN, m_gfx_clip_x_shadowcopy + 320 - 1 + EP1C_CLIP_MARGIN,
			   m_gfx_clip_y_shadowcopy - EP1C_CLIP_MARGIN, m_gfx_clip_y_shadowcopy + 240 - 1 + EP1C_CLIP_MARGIN);

	while (1)
	{
		// request commands from main CPU RAM
		const UINT16 data = COPY_NEXT_WORD(&addr);

		switch (data & 0xf000)
		{
			case 0x0000:
			case 0xf000:
				return;

			case 0xc000:
				if (COPY_NEXT_WORD(&addr)) // cliptype
					m_clip.set(m_gfx_clip_x_shadowcopy - EP1C_CLIP_MARGIN, m_gfx_clip_x_shadowcopy + 320 - 1 + EP1C_CLIP_MARGIN,
							   m_gfx_clip_y_shadowcopy - EP1C_CLIP_MARGIN, m_gfx_clip_y_shadowcopy + 240 - 1 + EP1C_CLIP_MARGIN);
				else
					m_clip.set(0, 0x2000 - 1, 0, 0x1000 - 1);
				idle_blitter(EP1C_CLIP_OPERATION_SIZE_BYTES);
				break;

			case 0x2000:
				addr -= 2;
				gfx_upload_shadow_copy(&addr);
				break;

			case 0x1000:
				addr -= 2;
				gfx_draw_shadow_copy(&addr);
				break;

			default:
				//popmessage("GFX op = %04X", data);
				return;
		}
	}
}


/* Run the list from a0 to a1 with every draw clipped to rows [ylo,yhi] (intersected with the game's clip).
 * BAND_ALL: all ops, one core; BAND_UPLOADS: the uploads pre-pass of a split; BAND_DRAWS: one band's draws. */
static void gfx_exec_band(rectangle *clip, int ylo, int yhi, int phase, UINT32 a0, UINT32 a1, int ct)
{
	UINT32 addr = a0 ? a0 : (m_gfx_addr_shadowcopy & 0x1fffffff);
	const int cx0 = m_gfx_clip_x_shadowcopy - EP1C_CLIP_MARGIN;
	const int cx1 = m_gfx_clip_x_shadowcopy + 320 - 1 + EP1C_CLIP_MARGIN;
	const int cy0 = m_gfx_clip_y_shadowcopy - EP1C_CLIP_MARGIN;
	const int cy1 = m_gfx_clip_y_shadowcopy + 240 - 1 + EP1C_CLIP_MARGIN;
	if (ct) clip->set(cx0, cx1, MAX(cy0, ylo), MIN(cy1, yhi));
	else clip->set(0, 0x2000-1, MAX(0, ylo), MIN(0x1000-1, yhi));

	while (1)
	{
		if (a1 && addr == a1) return;   /* end of this segment */
		UINT16 data = READ_NEXT_WORD(&addr);

		switch( data & 0xf000 )
		{
			case 0x0000:
			case 0xf000:
				return;

			case 0xc000:
				if (READ_NEXT_WORD(&addr)) // cliptype
					clip->set(cx0, cx1, MAX(cy0, ylo), MIN(cy1, yhi));
				else
					clip->set(0, 0x2000-1, MAX(0, ylo), MIN(0x1000-1, yhi));
				break;

			case 0x2000:
				addr -= 2;
				gfx_upload(&addr, phase != BAND_DRAWS);
				break;

			case 0x1000:
				addr -= 2;
				gfx_draw(&addr, clip, phase != BAND_UPLOADS);
				break;

			default:
				//popmessage("GFX op = %04X", data);
				return;
		}
	}
}


void epic12_wait_blitterthread()
{
	thready.notify_wait();
}

static void gfx_exec_write(UINT32 data)
{
//	if ( ACCESSING_BITS_0_7 )
	{
		if (data & 1)
		{
			thready.notify_wait();

			m_gfx_clip_x_shadowcopy = m_gfx_clip_x;
			m_gfx_clip_y_shadowcopy = m_gfx_clip_y;

			// Create a copy of the blit list so we can safely thread it.
			// Copying the Blitter operations will also estimate the delay needed for processing.
			m_blit_delay_ns = 0;
			gfx_create_shadow_copy();

			m_gfx_addr_shadowcopy = m_gfx_addr;

			if (m_delay_method) // old method
			{
				if (epic12_device_blit_delay && m_delay_scale)
				{
					m_blitter_busy = 1;
					int delay = epic12_device_blit_delay*(15 * m_delay_scale / 50);
					INT32 cycles = (INT32)((double)((double)delay / 1000000000) * sh4_get_cpu_speed());

					//bprintf(0, _T("old_blitter_delay  %d   cycles  %d\n"),delay, cycles);

					sh4_set_cave_blitter_delay_timer(cycles);
				}
				else
				{
					m_blitter_busy = 0;
				}
			} else {  // new method (buffis)
				// Every EP1C_VRAM_H_LINE_PERIOD_NANOSEC, the Blitter will block other operations, due
				// to fetching a horizontal line from VRAM for output.
				m_blit_delay_ns += floor( m_blit_delay_ns / EP1C_VRAM_H_LINE_PERIOD_NANOSEC ) * EP1C_VRAM_H_LINE_DURATION_NANOSEC;

				// Check if Blitter takes longer than a frame to render.
				// In practice, there's a bit less time than this to allow for lack of slowdown but
				// for debugging purposes this is an ok approximation.
				//if (m_blit_delay_ns > EP1C_FRAME_DURATION_NANOSEC)
				//	LOGDBG("Blitter delay! Blit duration %lld ns.\n", m_blit_delay_ns);

				m_blitter_busy = 1;

				INT32 cycles = (INT32)((double)((double)m_blit_delay_ns / 1000000000) * sh4_get_cpu_speed());

				//bprintf(0, _T("new blit_delay  %I64d   cycles  %d\n"),m_blit_delay_ns, cycles);

				sh4_set_cave_blitter_delay_timer(cycles);
			}

			epic12_device_blit_delay = 0;
			thready.notify(); // run gfx_exec(), w/ threading if set (via dip option)
		}
	}
}

static void pal16_check_init()
{
	if (nBurnBpp < 3 && !pal16) {
		pal16 = (UINT16 *)BurnMalloc((1 << 24) * sizeof (UINT16));

		for (INT32 i = 0; i < (1 << 24); i++) {
			pal16[i] = BurnHighCol(i / 0x10000, (i / 0x100) & 0xff, i & 0xff, 0);
		}
	}
}

static void epic12_draw_screen16_24bpp()
{
	INT32 scrollx = -m_gfx_scroll_x;
	INT32 scrolly = -m_gfx_scroll_y;

	UINT8  *dst = (UINT8  *)pBurnDraw;
	UINT32 *src = (UINT32 *)m_bitmaps;
	const INT32 heightmask = 0x1000 - 1;
	const INT32 widthmask  = 0x2000 - 1;

	for (INT32 y = 0; y < nScreenHeight; y++)
	{
		UINT32 *s0 = &src[((y - scrolly) & heightmask) * EP1C_PITCH];
		INT32 sx;

		switch (nBurnBpp) {
			case 2: // 16bpp
				for (INT32 x = 0; x < nScreenWidth; x++, dst += nBurnBpp)
				{
					sx = x - scrollx;
					PutPix(dst, pal16[s0[sx & widthmask]&((1<<24)-1)]);
				}
				break;
			case 3: // 24bpp
				for (INT32 x = 0; x < nScreenWidth; x++, dst += nBurnBpp)
				{
					sx = x - scrollx;
					PutPix(dst, s0[sx & widthmask]);
				}
				break;
		}
	}
}

static void comp_rows(int y0, int y1, INT32 scrollx, INT32 scrolly)
{
	UINT32 *dst = (UINT32 *)pBurnDraw;
	UINT32 *src = (UINT32 *)m_bitmaps;
	const INT32 heightmask = 0x1000 - 1;
	const INT32 widthmask  = 0x2000 - 1;

	for (INT32 y = y0; y < y1; y++)
	{
		UINT32 *s0 = &src[((y - scrolly) & heightmask) * EP1C_PITCH];
		UINT32 *d0 = dst + (y * nScreenWidth);
		INT32 sx;
		for (INT32 x = 0; x < nScreenWidth; x+=16)
		{
			sx = x - scrollx;
			d0[x + 0] = s0[((sx + 0)) & widthmask];
			d0[x + 1] = s0[((sx + 1)) & widthmask];
			d0[x + 2] = s0[((sx + 2)) & widthmask];
			d0[x + 3] = s0[((sx + 3)) & widthmask];
			d0[x + 4] = s0[((sx + 4)) & widthmask];
			d0[x + 5] = s0[((sx + 5)) & widthmask];
			d0[x + 6] = s0[((sx + 6)) & widthmask];
			d0[x + 7] = s0[((sx + 7)) & widthmask];
			d0[x + 8] = s0[((sx + 8)) & widthmask];
			d0[x + 9] = s0[((sx + 9)) & widthmask];
			d0[x +10] = s0[((sx +10)) & widthmask];
			d0[x +11] = s0[((sx +11)) & widthmask];
			d0[x +12] = s0[((sx +12)) & widthmask];
			d0[x +13] = s0[((sx +13)) & widthmask];
			d0[x +14] = s0[((sx +14)) & widthmask];
			d0[x +15] = s0[((sx +15)) & widthmask];
		}
	}
}

#if EP1C_SPLIT
/* ---- 32-bit composite on the blit cores ----
 * The VRAM rows were just written by cores 2 (top band) and 0 (bottom band); copying them out on core 1 pulls every
 * line across from another core's cache. Instead each blit core copies the screen rows it drew, in parallel, while
 * core 1 waits. Pure copy: output identical. */
#define COMP_STACK (16 * 1024)
struct comp_job { OSThread *th; UINT8 *stack; OSSemaphore start, done; volatile int y0, y1; INT32 sx, sy; };
static comp_job comp_w[2];   /* [0] = core 2 (top rows), [1] = core 0 (bottom rows) */
static int comp_ready, comp_failed;
static volatile int comp_quit;
static int comp_main(int argc, const char **)
{
	comp_job *j = &comp_w[argc];
	for (;;) {
		OSWaitSemaphore(&j->start);
		if (comp_quit) return 0;
		comp_rows(j->y0, j->y1, j->sx, j->sy);
		OSSignalSemaphore(&j->done);
	}
	return 0;
}
static void comp_stop()   // stop and free the workers that were started
{
	comp_quit = 1;
	for (int i = 0; i < 2; i++) {
		if (comp_w[i].th) {
			OSSignalSemaphore(&comp_w[i].start);
			OSJoinThread(comp_w[i].th, NULL);
		}
		free(comp_w[i].th);
		free(comp_w[i].stack);
		comp_w[i].th = NULL;
		comp_w[i].stack = NULL;
	}
	comp_ready = 0;
}
static void comp_init()
{
	if (comp_ready || comp_failed) return;
	static const UINT32 aff[2] = { OS_THREAD_ATTRIB_AFFINITY_CPU2, OS_THREAD_ATTRIB_AFFINITY_CPU0 };
	comp_quit = 0;
	for (int i = 0; i < 2; i++) {
		OSInitSemaphore(&comp_w[i].start, 0);
		OSInitSemaphore(&comp_w[i].done, 0);
		comp_w[i].stack = (UINT8 *)memalign(16, COMP_STACK);
		comp_w[i].th = (OSThread *)memalign(16, sizeof(OSThread));
		if (!comp_w[i].stack || !comp_w[i].th ||
		    !OSCreateThread(comp_w[i].th, comp_main, i, NULL, comp_w[i].stack + COMP_STACK, COMP_STACK, 16, aff[i])) {
			free(comp_w[i].th);   // not started: comp_stop must not join it
			comp_w[i].th = NULL;
			comp_stop();
			comp_failed = 1;   // composite stays on core 1 until the next init
			return;
		}
		OSResumeThread(comp_w[i].th);
	}
	comp_ready = 1;
}
static void comp_exit()
{
	if (comp_ready) comp_stop();
	comp_failed = 0;
}
#endif // EP1C_SPLIT

void epic12_draw_screen(UINT8 &recalc_palette)
{
#define COMPOSITE_DONE() do { } while (0)
	INT32 scrollx = -m_gfx_scroll_x;
	INT32 scrolly = -m_gfx_scroll_y;

	if (nBurnBpp != 4) {
		if (recalc_palette) {
			pal16_check_init();
			recalc_palette = 0;
		}

		epic12_draw_screen16_24bpp();
		COMPOSITE_DONE();
		return;
	}

#if EP1C_SPLIT
	if (g_comp_split && g_blit_split) comp_init();
	if (g_comp_split && g_blit_split && comp_ready) {
		/* screen row y shows VRAM row (y - scrolly) & 0xfff: rows above the last split row were drawn by core 2 */
		int ys = nScreenHeight / 2;
		if (last_split_mid >= 0) {
			ys = last_split_mid + scrolly;
			if (ys < 0) ys = 0;
			if (ys > nScreenHeight) ys = nScreenHeight;
		}
		comp_w[0].y0 = 0;  comp_w[0].y1 = ys;
		comp_w[1].y0 = ys; comp_w[1].y1 = nScreenHeight;
		for (int i = 0; i < 2; i++) { comp_w[i].sx = scrollx; comp_w[i].sy = scrolly; OSSignalSemaphore(&comp_w[i].start); }
		for (int i = 0; i < 2; i++) OSWaitSemaphore(&comp_w[i].done);
	} else
#endif
	{
		comp_rows(0, nScreenHeight, scrollx, scrolly);
	}
	COMPOSITE_DONE();
#undef COMPOSITE_DONE
}


// 0x18000000 - 0x18000057

UINT32 epic12_blitter_read(UINT32 offset)
{
	switch (offset)
	{
		case 0x10:
			if (m_blitter_busy)
			{
				//m_maincpu->spin_until_time(attotime::from_usec(10));
				if (sleep_on_busy) {
					Sh3BurnCycles(m_burn_cycles); // 0x400 @ (12800000*8)
				}
				//bprintf(0, _T("%d frame - blitter busy read....."), nCurrentFrame);

				return 0x00000000;
			}
			else
				return 0x00000010;

		case 0x24:
			return 0xffffffff;

		case 0x28:
			return 0xffffffff;

		case 0x50:
			return *dips;

		default:
			//logerror("unknownblitter_r %08x %08x\n", offset*4, mem_mask);
			break;

	}
	return 0;
}


void epic12_blitter_write(UINT32 offset, UINT32 data)
{
	switch (offset)
	{
		case 0x04:
			gfx_exec_write(data);
			break;

		case 0x08:
			m_gfx_addr = data & 0xffffff;
			break;

		case 0x14:
			m_gfx_scroll_x = data;
			break;

		case 0x18:
			m_gfx_scroll_y = data;
			break;

		case 0x40:
			m_gfx_clip_x = data;
			break;

		case 0x44:
			m_gfx_clip_y = data;
			break;
	}
}

void epic12_scan(INT32 nAction, INT32 *pnMin)
{
	SCAN_VAR(m_gfx_addr);
//	SCAN_VAR(m_gfx_addr_shadowcopy); // probably not needed!
	SCAN_VAR(m_gfx_scroll_x);
	SCAN_VAR(m_gfx_scroll_y);
	SCAN_VAR(m_gfx_clip_x);
	SCAN_VAR(m_gfx_clip_y);
//	SCAN_VAR(m_gfx_clip_x_shadowcopy);
//	SCAN_VAR(m_gfx_clip_y_shadowcopy);
	SCAN_VAR(epic12_device_blit_delay);
	SCAN_VAR(m_delay_scale);
	SCAN_VAR(m_blitter_busy);
	SCAN_VAR(m_blit_delay_ns);
	SCAN_VAR(m_blit_idle_op_bytes);

	if (~nAction & ACB_RUNAHEAD) {
#if EP1C_PITCH == 0x2000
		ScanVar(m_bitmaps, m_gfx_size * 4, "epic12 vram");
#else
		for (INT32 y = 0; y < 0x1000; y++)   /* same state layout as the unpadded VRAM */
			ScanVar(m_bitmaps + (size_t)y * EP1C_PITCH, 0x2000 * 4, "epic12 vram");
#endif
	}
	thready.scan();
}

