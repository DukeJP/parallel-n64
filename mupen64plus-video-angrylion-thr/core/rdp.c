#include <memory.h>
#include <string.h>

#include <retro_miscellaneous.h>

#include "rdp.h"
#include "vi.h"
#include "common.h"
#include "rdram.h"
#include "msg.h"
#include "plugin.h"
#include "parallel_c.hpp"

/* START OF MACROS */

/* thread-local storage */
#if defined(_MSC_VER)
    #define TLS __declspec(thread)
#elif defined(__GNUC__)
   #define TLS __thread
#else
    #define TLS _Thread_local // C11
#endif

#define SIGN16(x)   ((int16_t)(x))
#define SIGN8(x)    ((int8_t)(x))

#define SIGN(x, numb)   (((x) & ((1 << numb) - 1)) | -((x) & (1 << (numb - 1))))
#define SIGNF(x, numb)  ((x) | -((x) & (1 << (numb - 1))))

#define TRELATIVE(x, y)     ((x) - ((y) << 3))

#define PIXELS_TO_BYTES(pix, siz) (((pix) << (siz)) >> 1)

#define tmem16 ((uint16_t*)globals.tmem)
#define tc16   ((uint16_t*)globals.tmem)
#define tlut   ((uint16_t*)(&globals.tmem[0x800]))

#define GET_LOW_RGBA16_TMEM(x)  (replicated_rgba[((x) >> 1) & 0x1f])
#define GET_MED_RGBA16_TMEM(x)  (replicated_rgba[((x) >> 6) & 0x1f])
#define GET_HI_RGBA16_TMEM(x)   (replicated_rgba[(x) >> 11])

#ifndef CLAMP_AL
#define CLAMP_AL(x, low, high)  (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))
#endif

/* END OF MACROS */

/* START OF DEFINES */

// bit constants for DP_STATUS
#define DP_STATUS_XBUS_DMA      0x001   // DMEM DMA mode is set
#define DP_STATUS_FREEZE        0x002   // Freeze has been set
#define DP_STATUS_FLUSH         0x004   // Flush has been set
#define DP_STATUS_START_GCLK    0x008   // Unknown
#define DP_STATUS_TMEM_BUSY     0x010   // TMEM is in use on the RDP
#define DP_STATUS_PIPE_BUSY     0x020   // Graphics pipe is in use on the RDP
#define DP_STATUS_CMD_BUSY      0x040   // RDP is currently executing a command
#define DP_STATUS_CBUF_BUSY     0x080   // RDRAM RDP command buffer is in use
#define DP_STATUS_DMA_BUSY      0x100   // DMEM RDP command buffer is in use
#define DP_STATUS_END_VALID     0x200   // Unknown
#define DP_STATUS_START_VALID   0x400   // Unknown

#define PIXEL_SIZE_4BIT         0
#define PIXEL_SIZE_8BIT         1
#define PIXEL_SIZE_16BIT        2
#define PIXEL_SIZE_32BIT        3

#define CYCLE_TYPE_1            0
#define CYCLE_TYPE_2            1
#define CYCLE_TYPE_COPY         2
#define CYCLE_TYPE_FILL         3


#define FORMAT_RGBA             0
#define FORMAT_YUV              1
#define FORMAT_CI               2
#define FORMAT_IA               3
#define FORMAT_I                4


#define TEXEL_RGBA4             0
#define TEXEL_RGBA8             1
#define TEXEL_RGBA16            2
#define TEXEL_RGBA32            3
#define TEXEL_YUV4              4
#define TEXEL_YUV8              5
#define TEXEL_YUV16             6
#define TEXEL_YUV32             7
#define TEXEL_CI4               8
#define TEXEL_CI8               9
#define TEXEL_CI16              0xa
#define TEXEL_CI32              0xb
#define TEXEL_IA4               0xc
#define TEXEL_IA8               0xd
#define TEXEL_IA16              0xe
#define TEXEL_IA32              0xf
#define TEXEL_I4                0x10
#define TEXEL_I8                0x11
#define TEXEL_I16               0x12
#define TEXEL_I32               0x13

#define CMD_BUFFER_COUNT        1024

#define CVG_CLAMP               0
#define CVG_WRAP                1
#define CVG_ZAP                 2
#define CVG_SAVE                3

#define ZMODE_OPAQUE            0
#define ZMODE_INTERPENETRATING  1
#define ZMODE_TRANSPARENT       2
#define ZMODE_DECAL             3

/* END OF DEFINES */

/* START OF STRUCTS */

struct color
{
    int32_t r, g, b, a;
};

struct fbcolor
{
    uint8_t r, g, b;
};

struct rectangle
{
    uint16_t xl, yl, xh, yh;
};

struct tex_rectangle
{
    int tilenum;
    uint16_t xl, yl, xh, yh;
    int16_t s, t;
    int16_t dsdx, dtdy;
    uint32_t flip;
};

struct other_modes
{
    int cycle_type;
    int persp_tex_en;
    int detail_tex_en;
    int sharpen_tex_en;
    int tex_lod_en;
    int en_tlut;
    int tlut_type;
    int sample_type;
    int mid_texel;
    int bi_lerp0;
    int bi_lerp1;
    int convert_one;
    int key_en;
    int rgb_dither_sel;
    int alpha_dither_sel;
    int blend_m1a_0;
    int blend_m1a_1;
    int blend_m1b_0;
    int blend_m1b_1;
    int blend_m2a_0;
    int blend_m2a_1;
    int blend_m2b_0;
    int blend_m2b_1;
    int force_blend;
    int alpha_cvg_select;
    int cvg_times_alpha;
    int z_mode;
    int cvg_dest;
    int color_on_cvg;
    int image_read_en;
    int z_update_en;
    int z_compare_en;
    int antialias_en;
    int z_source_sel;
    int dither_alpha_en;
    int alpha_compare_en;
    struct {
        int stalederivs;
        int dolod;
        int partialreject_1cycle;
        int partialreject_2cycle;
        int rgb_alpha_dither;
        int realblendershiftersneeded;
        int interpixelblendershiftersneeded;
        int getditherlevel;
        int textureuselevel0;
        int textureuselevel1;
    } f;
};

struct spansigs
{
   int startspan;
   int endspan;
   int preendspan;
   int nextspan;
   int midspan;
   int longspan;
   int onelessthanmid;
};

/* END OF STRUCTS */

/* START OF VARIABLES */

static struct core_config* config;

static int32_t one_color = 0x100;
static int32_t zero_color = 0x00;

static int rdp_pipeline_crashed = 0;

static TLS struct rdp_globals
{
   uint8_t cvgbuf[1024];
   uint8_t tmem[0x1000];

   uint16_t primitive_delta_z;

   int blshifta;
   int blshiftb;
   int pastblshifta;
   int pastblshiftb;
   int scfield;
   int sckeepodd;
   int ti_format;
   int ti_size;
   int ti_width;
   int fb_format;
   int fb_size;
   int fb_width;

   int32_t iseed;
   int32_t k0_tf;
   int32_t k1_tf;
   int32_t k2_tf;
   int32_t k3_tf;
   int32_t k4;
   int32_t k5;
   int32_t min_level;
   int32_t primitive_lod_frac;
   int32_t lod_frac;
   int32_t noise;
   int32_t pastrawdzmem;

   uint32_t max_level;
   uint32_t primitive_z;
   uint32_t zb_address;
   uint32_t ti_address;
   uint32_t fb_address;
   uint32_t fill_color;

   struct color texel0_color;
   struct color texel1_color;
   struct color nexttexel_color;
   struct color shade_color;
   struct color combined_color;
   struct color pixel_color;
   struct color inv_pixel_color;
   struct color blended_pixel_color;

   struct color prim_color;
   struct color blend_color;
   struct color env_color;
   struct color fog_color;

   struct color memory_color;
   struct color pre_memory_color;

   struct color key_scale;
   struct color key_center;
   struct color key_width;

   struct rectangle clip;
   struct other_modes other_modes;

   void (*fbread1_ptr)(uint32_t, uint32_t*);
   void (*fbread2_ptr)(uint32_t, uint32_t*);
   void (*fbwrite_ptr)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
   void (*tcdiv_ptr)(int32_t, int32_t, int32_t, int32_t*, int32_t*);

   struct
   {
      int lx, rx;
      int unscrx;
      int validline;
      int32_t r, g, b, a, s, t, w, z;
      int32_t majorx[4];
      int32_t minorx[4];
      int32_t invalyscan[4];
   } span[1024];

   /* span states */
   struct
   {
      int ds;
      int dt;
      int dw;
      int dr;
      int dg;
      int db;
      int da;
      int dz;
      int dzpix;

      int drdy;
      int dgdy;
      int dbdy;
      int dady;
      int dzdy;
      int cdr;
      int cdg;
      int cdb;
      int cda;
      int cdz;

      int dsdy;
      int dtdy;
      int dwdy;
   } spans;

   struct tile
   {
      int format;
      int size;
      int line;
      int tmem;
      int palette;
      int ct, mt, cs, ms;
      int mask_t, shift_t, mask_s, shift_s;

      uint16_t sl, tl, sh, th;

      struct
      {
         int clampdiffs, clampdifft;
         int clampens, clampent;
         int masksclamped, masktclamped;
         int notlutswitch, tlutswitch;
      } f;
   } tile[8];

   struct
   {
      int32_t *i1a_r[2];
      int32_t *i1a_g[2];
      int32_t *i1a_b[2];
      int32_t *i1b_a[2];
      int32_t *i2a_r[2];
      int32_t *i2a_g[2];
      int32_t *i2a_b[2];
      int32_t *i2b_a[2];
   } blender;

   struct
   {
      int sub_a_rgb0;
      int sub_b_rgb0;
      int mul_rgb0;
      int add_rgb0;
      int sub_a_a0;
      int sub_b_a0;
      int mul_a0;
      int add_a0;

      int sub_a_rgb1;
      int sub_b_rgb1;
      int mul_rgb1;
      int add_rgb1;
      int sub_a_a1;
      int sub_b_a1;
      int mul_a1;
      int add_a1;
   } combine;

   struct
   {
      int32_t *rgbsub_a_r[2];
      int32_t *rgbsub_a_g[2];
      int32_t *rgbsub_a_b[2];
      int32_t *rgbsub_b_r[2];
      int32_t *rgbsub_b_g[2];
      int32_t *rgbsub_b_b[2];
      int32_t *rgbmul_r[2];
      int32_t *rgbmul_g[2];
      int32_t *rgbmul_b[2];
      int32_t *rgbadd_r[2];
      int32_t *rgbadd_g[2];
      int32_t *rgbadd_b[2];

      int32_t *alphasub_a[2];
      int32_t *alphasub_b[2];
      int32_t *alphamul[2];
      int32_t *alphaadd[2];
   } combiner;

} globals;

static struct
{
    int copymstrangecrashes, fillmcrashes, fillmbitcrashes, syncfullcrash;
} onetimewarnings;


static uint32_t rdp_cmd_data[0x10000];
static uint32_t rdp_cmd_ptr = 0;
static uint32_t rdp_cmd_cur = 0;
static uint32_t ptr_onstart = 0;

static uint32_t rdp_cmd_buf[CMD_BUFFER_COUNT][CMD_MAX_INTS];
static uint32_t rdp_cmd_buf_pos;

static const uint8_t bayer_matrix[16] =
{
     0,  4,  1, 5,
     4,  0,  5, 1,
     3,  7,  2, 6,
     7,  3,  6, 2
};


static const uint8_t magic_matrix[16] =
{
     0,  6,  1, 7,
     4,  2,  5, 3,
     3,  5,  2, 4,
     7,  1,  6, 0
};

static int32_t blenderone = 0xff;

static uint8_t bldiv_hwaccurate_table[0x8000];


static uint8_t special_9bit_clamptable[512];
static int16_t special_9bit_exttable[512];

static struct 
{
   uint8_t cvg;
   uint8_t cvbit;
   uint8_t xoff;
   uint8_t yoff;
} cvarray[0x100];

static uint16_t z_com_table[0x40000];
static uint32_t z_complete_dec_table[0x4000];
static uint16_t deltaz_comparator_lut[0x10000];

static struct {uint32_t shift; uint32_t add;} z_dec_table[8] = {
     6, 0x00000,
     5, 0x20000,
     4, 0x30000,
     3, 0x38000,
     2, 0x3c000,
     1, 0x3e000,
     0, 0x3f000,
     0, 0x3f800,
};

static uint8_t replicated_rgba[32];

static const int32_t norm_point_table[64] = {
    0x4000, 0x3f04, 0x3e10, 0x3d22, 0x3c3c, 0x3b5d, 0x3a83, 0x39b1,
    0x38e4, 0x381c, 0x375a, 0x369d, 0x35e5, 0x3532, 0x3483, 0x33d9,
    0x3333, 0x3291, 0x31f4, 0x3159, 0x30c3, 0x3030, 0x2fa1, 0x2f15,
    0x2e8c, 0x2e06, 0x2d83, 0x2d03, 0x2c86, 0x2c0b, 0x2b93, 0x2b1e,
    0x2aab, 0x2a3a, 0x29cc, 0x2960, 0x28f6, 0x288e, 0x2828, 0x27c4,
    0x2762, 0x2702, 0x26a4, 0x2648, 0x25ed, 0x2594, 0x253d, 0x24e7,
    0x2492, 0x243f, 0x23ee, 0x239e, 0x234f, 0x2302, 0x22b6, 0x226c,
    0x2222, 0x21da, 0x2193, 0x214d, 0x2108, 0x20c5, 0x2082, 0x2041
};

static const int32_t norm_slope_table[64] = {
    0xf03, 0xf0b, 0xf11, 0xf19, 0xf20, 0xf25, 0xf2d, 0xf32,
    0xf37, 0xf3d, 0xf42, 0xf47, 0xf4c, 0xf50, 0xf55, 0xf59,
    0xf5d, 0xf62, 0xf64, 0xf69, 0xf6c, 0xf70, 0xf73, 0xf76,
    0xf79, 0xf7c, 0xf7f, 0xf82, 0xf84, 0xf87, 0xf8a, 0xf8c,
    0xf8e, 0xf91, 0xf93, 0xf95, 0xf97, 0xf99, 0xf9b, 0xf9d,
    0xf9f, 0xfa1, 0xfa3, 0xfa4, 0xfa6, 0xfa8, 0xfa9, 0xfaa,
    0xfac, 0xfae, 0xfaf, 0xfb0, 0xfb2, 0xfb3, 0xfb5, 0xfb5,
    0xfb7, 0xfb8, 0xfb9, 0xfba, 0xfbc, 0xfbc, 0xfbe, 0xfbe
};

/* END OF VARIABLES */

int32_t irand(void)
{
    globals.iseed *= 0x343fd;
    globals.iseed += 0x269ec3;
    return ((globals.iseed >> 16) & 0x7fff);
}

static STRICTINLINE int32_t clamp(int32_t value,int32_t min,int32_t max)
{
    if (value < min)
        return min;
    else if (value > max)
        return max;
    return value;
}

static void deduce_derivatives(void);

static void rdp_invalid(const uint32_t* args);
static void rdp_noop(const uint32_t* args);
static void rdp_tri_noshade(const uint32_t* args);
static void rdp_tri_noshade_z(const uint32_t* args);
static void rdp_tri_tex(const uint32_t* args);
static void rdp_tri_tex_z(const uint32_t* args);
static void rdp_tri_shade(const uint32_t* args);
static void rdp_tri_shade_z(const uint32_t* args);
static void rdp_tri_texshade(const uint32_t* args);
static void rdp_tri_texshade_z(const uint32_t* args);
static void rdp_tex_rect(const uint32_t* args);
static void rdp_tex_rect_flip(const uint32_t* args);
static void rdp_sync_load(const uint32_t* args);
static void rdp_sync_pipe(const uint32_t* args);
static void rdp_sync_tile(const uint32_t* args);
static void rdp_sync_full(const uint32_t* args);
static void rdp_set_key_gb(const uint32_t* args);
static void rdp_set_key_r(const uint32_t* args);
static void rdp_set_convert(const uint32_t* args);
static void rdp_set_scissor(const uint32_t* args);
static void rdp_set_prim_depth(const uint32_t* args);
static void rdp_set_other_modes(const uint32_t* args);
static void rdp_set_tile_size(const uint32_t* args);
static void rdp_load_block(const uint32_t* args);
static void rdp_load_tlut(const uint32_t* args);
static void rdp_load_tile(const uint32_t* args);
static void rdp_set_tile(const uint32_t* args);
static void rdp_fill_rect(const uint32_t* args);
static void rdp_set_fill_color(const uint32_t* args);
static void rdp_set_fog_color(const uint32_t* args);
static void rdp_set_blend_color(const uint32_t* args);
static void rdp_set_prim_color(const uint32_t* args);
static void rdp_set_env_color(const uint32_t* args);
static void rdp_set_combine(const uint32_t* args);
static void rdp_set_texture_image(const uint32_t* args);
static void rdp_set_mask_image(const uint32_t* args);
static void rdp_set_color_image(const uint32_t* args);

static const struct
{
    void (*handler)(const uint32_t*);   // command handler function pointer
    uint32_t length;                    // command data length in bytes
    bool singlethread;                  // run in main thread
    bool multithread;                   // run in worker threads
    bool sync;                          // synchronize all workers before execution
    char name[32];                      // descriptive name for debugging
} rdp_commands[] = {
    {rdp_noop,              8,   true,  false, false, "No_Op"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_tri_noshade,       32,  false, true,  false, "Fill_Triangle"},
    {rdp_tri_noshade_z,     48,  false, true,  false, "Fill_ZBuffer_Triangle"},
    {rdp_tri_tex,           96,  false, true,  false, "Texture_Triangle"},
    {rdp_tri_tex_z,         112, false, true,  false, "Texture_ZBuffer_Triangle"},
    {rdp_tri_shade,         96,  false, true,  false, "Shade_Triangle"},
    {rdp_tri_shade_z,       112, false, true,  false, "Shade_ZBuffer_Triangle"},
    {rdp_tri_texshade,      160, false, true,  false, "Shade_Texture_Triangle"},
    {rdp_tri_texshade_z,    176, false, true,  false, "Shade_Texture_Z_Buffer_Triangle"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_tex_rect,          16,  false, true,  false, "Texture_Rectangle"},
    {rdp_tex_rect_flip,     16,  false, true,  false, "Texture_Rectangle_Flip"},
    {rdp_sync_load,         8,   true,  false, false, "Sync_Load"},
    {rdp_sync_pipe,         8,   true,  false, false, "Sync_Pipe"},
    {rdp_sync_tile,         8,   true,  false, false, "Sync_Tile"},
    {rdp_sync_full,         8,   true,  false, true,  "Sync_Full"},
    {rdp_set_key_gb,        8,   false, true,  false, "Set_Key_GB"},
    {rdp_set_key_r,         8,   false, true,  false, "Set_Key_R"},
    {rdp_set_convert,       8,   false, true,  false, "Set_Convert"},
    {rdp_set_scissor,       8,   false, true,  false, "Set_Scissor"},
    {rdp_set_prim_depth,    8,   false, true,  false, "Set_Prim_Depth"},
    {rdp_set_other_modes,   8,   false, true,  false, "Set_Other_Modes"},
    {rdp_load_tlut,         8,   false, true,  false, "Load_TLUT"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_set_tile_size,     8,   false, true,  false, "Set_Tile_Size"},
    {rdp_load_block,        8,   false, true,  false, "Load_Block"},
    {rdp_load_tile,         8,   false, true,  false, "Load_Tile"},
    {rdp_set_tile,          8,   false, true,  false, "Set_Tile"},
    {rdp_fill_rect,         8,   false, true,  false, "Fill_Rectangle"},
    {rdp_set_fill_color,    8,   false, true,  false, "Set_Fill_Color"},
    {rdp_set_fog_color,     8,   false, true,  false, "Set_Fog_Color"},
    {rdp_set_blend_color,   8,   false, true,  false, "Set_Blend_Color"},
    {rdp_set_prim_color,    8,   false, true,  false, "Set_Prim_Color"},
    {rdp_set_env_color,     8,   false, true,  false, "Set_Env_Color"},
    {rdp_set_combine,       8,   false, true,  false, "Set_Combine"},
    {rdp_set_texture_image, 8,   false, true,  false, "Set_Texture_Image"},
    {rdp_set_mask_image,    8,   true,  true,  true,  "Set_Mask_Image"},
    {rdp_set_color_image,   8,   false, true,  true,  "Set_Color_Image"}
};

static void rdp_cmd_run(const uint32_t* arg)
{
    uint32_t cmd_id = CMD_ID(arg);
    rdp_commands[cmd_id].handler(arg);
}

static void rdp_cmd_run_buffered(void)
{
   uint32_t pos;
   for (pos = 0; pos < rdp_cmd_buf_pos; pos++)
      rdp_cmd_run(rdp_cmd_buf[pos]);
}

static void rdp_cmd_flush(void)
{
    // only run if there's something buffered
    if (rdp_cmd_buf_pos)
    {
        // let workers run all buffered commands in parallel
        parallel_run(rdp_cmd_run_buffered);

        // reset buffer by starting from the beginning
        rdp_cmd_buf_pos = 0;
    }
}

static void rdp_cmd_push(const uint32_t* arg, uint32_t length)
{
    // copy command data to current buffer position
    memcpy(rdp_cmd_buf + rdp_cmd_buf_pos, arg, length * sizeof(uint32_t));

    // increment buffer position and flush buffer when it is full
    if (++rdp_cmd_buf_pos >= CMD_BUFFER_COUNT)
        rdp_cmd_flush();
}

void rdp_cmd(const uint32_t* arg, uint32_t length)
{
    uint32_t cmd_id = CMD_ID(arg);

    if (rdp_commands[cmd_id].sync && config->parallel)
        rdp_cmd_flush();

    if (rdp_commands[cmd_id].singlethread || !config->parallel)
        rdp_cmd_run(arg);

    if (rdp_commands[cmd_id].multithread && config->parallel)
        rdp_cmd_push(arg, length);
}

void rdp_update(void)
{
    int i, length;
    uint32_t cmd, cmd_length;

    uint32_t** dp_reg      = plugin_get_dp_registers();
    uint32_t dp_current_al = *dp_reg[DP_CURRENT] & ~7, dp_end_al = *dp_reg[DP_END] & ~7;

    *dp_reg[DP_STATUS] &= ~DP_STATUS_FREEZE;

    if (dp_end_al <= dp_current_al)
        return;

    length = (dp_end_al - dp_current_al) >> 2;

    ptr_onstart = rdp_cmd_ptr;

    uint32_t remaining_length = length;


    dp_current_al >>= 2;

    while (remaining_length)
    {

    int toload = remaining_length > 0x10000 ? 0x10000 : remaining_length;

    if (*dp_reg[DP_STATUS] & DP_STATUS_XBUS_DMA)
    {
        uint32_t* dmem = (uint32_t*)plugin_get_dmem();
        for (i = 0; i < toload; i ++)
        {
            rdp_cmd_data[rdp_cmd_ptr] = dmem[dp_current_al & 0x3ff];
            rdp_cmd_ptr++;
            dp_current_al++;
        }
    }
    else
    {
        for (i = 0; i < toload; i ++)
        {
            RREADIDX32(rdp_cmd_data[rdp_cmd_ptr], dp_current_al);

            rdp_cmd_ptr++;
            dp_current_al++;
        }
    }

    remaining_length -= toload;

    while (rdp_cmd_cur < rdp_cmd_ptr && !rdp_pipeline_crashed)
    {
        cmd = CMD_ID(rdp_cmd_data + rdp_cmd_cur);
        cmd_length = rdp_commands[cmd].length >> 2;

        if ((rdp_cmd_ptr - rdp_cmd_cur) < cmd_length)
        {
            if (!remaining_length)
            {

                *dp_reg[DP_START] = *dp_reg[DP_CURRENT] = *dp_reg[DP_END];
                return;
            }
            else
            {
                dp_current_al -= (rdp_cmd_ptr - rdp_cmd_cur);
                remaining_length += (rdp_cmd_ptr - rdp_cmd_cur);
                break;
            }
        }

        rdp_cmd(rdp_cmd_data + rdp_cmd_cur, cmd_length);
        rdp_cmd_cur += cmd_length;
    };
    rdp_cmd_ptr = 0;
    rdp_cmd_cur = 0;
    };

    *dp_reg[DP_START] = *dp_reg[DP_CURRENT] = *dp_reg[DP_END];
}

static STRICTINLINE void rgb_dither(int* r, int* g, int* b, int dith)
{

    int32_t newr = *r, newg = *g, newb = *b;
    int32_t rcomp, gcomp, bcomp;


    if (newr > 247)
        newr = 255;
    else
        newr = (newr & 0xf8) + 8;
    if (newg > 247)
        newg = 255;
    else
        newg = (newg & 0xf8) + 8;
    if (newb > 247)
        newb = 255;
    else
        newb = (newb & 0xf8) + 8;

    if (globals.other_modes.rgb_dither_sel != 2)
        rcomp = gcomp = bcomp = dith;
    else
    {
        rcomp = dith & 7;
        gcomp = (dith >> 3) & 7;
        bcomp = (dith >> 6) & 7;
    }





    int32_t replacesign = (rcomp - (*r & 7)) >> 31;

    int32_t ditherdiff = newr - *r;
    *r = *r + (ditherdiff & replacesign);

    replacesign = (gcomp - (*g & 7)) >> 31;
    ditherdiff = newg - *g;
    *g = *g + (ditherdiff & replacesign);

    replacesign = (bcomp - (*b & 7)) >> 31;
    ditherdiff = newb - *b;
    *b = *b + (ditherdiff & replacesign);
}

static STRICTINLINE void get_dither_noise(int x, int y, int* cdith, int* adith)
{
   int dithindex;
   if (!globals.other_modes.f.getditherlevel)
      globals.noise = ((irand() & 7) << 6) | 0x20;

   switch(globals.other_modes.f.rgb_alpha_dither)
   {
      case 0:
         dithindex = ((y & 3) << 2) | (x & 3);
         *adith = *cdith = magic_matrix[dithindex];
         break;
      case 1:
         dithindex = ((y & 3) << 2) | (x & 3);
         *cdith = magic_matrix[dithindex];
         *adith = (~(*cdith)) & 7;
         break;
      case 2:
         dithindex = ((y & 3) << 2) | (x & 3);
         *cdith = magic_matrix[dithindex];
         *adith = (globals.noise >> 6) & 7;
         break;
      case 3:
         dithindex = ((y & 3) << 2) | (x & 3);
         *cdith = magic_matrix[dithindex];
         *adith = 0;
         break;
      case 4:
         dithindex = ((y & 3) << 2) | (x & 3);
         *adith = *cdith = bayer_matrix[dithindex];
         break;
      case 5:
         dithindex = ((y & 3) << 2) | (x & 3);
         *cdith = bayer_matrix[dithindex];
         *adith = (~(*cdith)) & 7;
         break;
      case 6:
         dithindex = ((y & 3) << 2) | (x & 3);
         *cdith = bayer_matrix[dithindex];
         *adith = (globals.noise >> 6) & 7;
         break;
      case 7:
         dithindex = ((y & 3) << 2) | (x & 3);
         *cdith = bayer_matrix[dithindex];
         *adith = 0;
         break;
      case 8:
         dithindex = ((y & 3) << 2) | (x & 3);
         *cdith = irand();
         *adith = magic_matrix[dithindex];
         break;
      case 9:
         dithindex = ((y & 3) << 2) | (x & 3);
         *cdith = irand();
         *adith = (~magic_matrix[dithindex]) & 7;
         break;
      case 10:
         *cdith = irand();
         *adith = (globals.noise >> 6) & 7;
         break;
      case 11:
         *cdith = irand();
         *adith = 0;
         break;
      case 12:
         dithindex = ((y & 3) << 2) | (x & 3);
         *cdith = 7;
         *adith = bayer_matrix[dithindex];
         break;
      case 13:
         dithindex = ((y & 3) << 2) | (x & 3);
         *cdith = 7;
         *adith = (~bayer_matrix[dithindex]) & 7;
         break;
      case 14:
         *cdith = 7;
         *adith = (globals.noise >> 6) & 7;
         break;
      case 15:
         *cdith = 7;
         *adith = 0;
         break;
   }
}

static void dither_init(void)
{
}

static INLINE void set_blender_input(int cycle, int which, int32_t **input_r, int32_t **input_g, int32_t **input_b, int32_t **input_a, int a, int b)
{
   switch (a & 0x3)
   {
      case 0:
         {
            if (cycle == 0)
            {
               *input_r = &globals.pixel_color.r;
               *input_g = &globals.pixel_color.g;
               *input_b = &globals.pixel_color.b;
            }
            else
            {
               *input_r = &globals.blended_pixel_color.r;
               *input_g = &globals.blended_pixel_color.g;
               *input_b = &globals.blended_pixel_color.b;
            }
            break;
         }

      case 1:
         *input_r = &globals.memory_color.r;
         *input_g = &globals.memory_color.g;
         *input_b = &globals.memory_color.b;
         break;

      case 2:
         *input_r = &globals.blend_color.r;
         *input_g = &globals.blend_color.g;
         *input_b = &globals.blend_color.b;
         break;
      case 3:
         *input_r = &globals.fog_color.r;
         *input_g = &globals.fog_color.g;
         *input_b = &globals.fog_color.b;
         break;
   }

   if (which == 0)
   {
      switch (b & 0x3)
      {
         case 0:
            *input_a = &globals.pixel_color.a;
            break;
         case 1:
            *input_a = &globals.fog_color.a;
            break;
         case 2:
            *input_a = &globals.shade_color.a;
            break;
         case 3:
            *input_a = &zero_color;
            break;
      }
   }
   else
   {
      switch (b & 0x3)
      {
         case 0:
            *input_a = &globals.inv_pixel_color.a;
            break;
         case 1:     *input_a = &globals.memory_color.a; break;
         case 2:     *input_a = &blenderone; break;
         case 3:     *input_a = &zero_color; break;
      }
   }
}

static STRICTINLINE int alpha_compare(int32_t comb_alpha)
{
   int32_t threshold;
   if (!globals.other_modes.alpha_compare_en)
      return 1;

   if (!globals.other_modes.dither_alpha_en)
      threshold = globals.blend_color.a;
   else
      threshold = irand() & 0xff;

   if (comb_alpha >= threshold)
      return 1;

   return 0;
}

static STRICTINLINE void blender_equation_cycle0(int* r, int* g, int* b)
{
    int mulb;
    int blr, blg, blb, sum;
    int blend1a = *globals.blender.i1b_a[0] >> 3;
    int blend2a = *globals.blender.i2b_a[0] >> 3;

    if (globals.blender.i2b_a[0] == &globals.memory_color.a)
    {
        blend1a = (blend1a >> globals.blshifta) & 0x3C;
        blend2a = (blend2a >> globals.blshiftb) | 3;
    }

    mulb = blend2a + 1;


    blr = (*globals.blender.i1a_r[0]) * blend1a + (*globals.blender.i2a_r[0]) * mulb;
    blg = (*globals.blender.i1a_g[0]) * blend1a + (*globals.blender.i2a_g[0]) * mulb;
    blb = (*globals.blender.i1a_b[0]) * blend1a + (*globals.blender.i2a_b[0]) * mulb;



    if (!globals.other_modes.force_blend)
    {





        sum = ((blend1a & ~3) + (blend2a & ~3) + 4) << 9;
        *r = bldiv_hwaccurate_table[sum | ((blr >> 2) & 0x7ff)];
        *g = bldiv_hwaccurate_table[sum | ((blg >> 2) & 0x7ff)];
        *b = bldiv_hwaccurate_table[sum | ((blb >> 2) & 0x7ff)];
    }
    else
    {
        *r = (blr >> 5) & 0xff;
        *g = (blg >> 5) & 0xff;
        *b = (blb >> 5) & 0xff;
    }
}

static STRICTINLINE void blender_equation_cycle0_2(int* r, int* g, int* b)
{
    int blend1a = *globals.blender.i1b_a[0] >> 3;
    int blend2a = *globals.blender.i2b_a[0] >> 3;

    if (globals.blender.i2b_a[0] == &globals.memory_color.a)
    {
        blend1a = (blend1a >> globals.pastblshifta) & 0x3C;
        blend2a = (blend2a >> globals.pastblshiftb) | 3;
    }

    blend2a += 1;
    *r = (((*globals.blender.i1a_r[0]) * blend1a + (*globals.blender.i2a_r[0]) * blend2a) >> 5) & 0xff;
    *g = (((*globals.blender.i1a_g[0]) * blend1a + (*globals.blender.i2a_g[0]) * blend2a) >> 5) & 0xff;
    *b = (((*globals.blender.i1a_b[0]) * blend1a + (*globals.blender.i2a_b[0]) * blend2a) >> 5) & 0xff;
}

static STRICTINLINE void blender_equation_cycle1(int* r, int* g, int* b)
{
    int mulb;
    int blr, blg, blb, sum;
    int blend1a = *globals.blender.i1b_a[1] >> 3;
    int blend2a = *globals.blender.i2b_a[1] >> 3;

    if (globals.blender.i2b_a[1] == &globals.memory_color.a)
    {
        blend1a = (blend1a >> globals.blshifta) & 0x3C;
        blend2a = (blend2a >> globals.blshiftb) | 3;
    }

    mulb = blend2a + 1;
    blr = (*globals.blender.i1a_r[1]) * blend1a + (*globals.blender.i2a_r[1]) * mulb;
    blg = (*globals.blender.i1a_g[1]) * blend1a + (*globals.blender.i2a_g[1]) * mulb;
    blb = (*globals.blender.i1a_b[1]) * blend1a + (*globals.blender.i2a_b[1]) * mulb;

    if (!globals.other_modes.force_blend)
    {
        sum = ((blend1a & ~3) + (blend2a & ~3) + 4) << 9;
        *r = bldiv_hwaccurate_table[sum | ((blr >> 2) & 0x7ff)];
        *g = bldiv_hwaccurate_table[sum | ((blg >> 2) & 0x7ff)];
        *b = bldiv_hwaccurate_table[sum | ((blb >> 2) & 0x7ff)];
    }
    else
    {
        *r = (blr >> 5) & 0xff;
        *g = (blg >> 5) & 0xff;
        *b = (blb >> 5) & 0xff;
    }
}

static STRICTINLINE int blender_1cycle(uint32_t* fr, uint32_t* fg, uint32_t* fb, int dith, uint32_t blend_en, uint32_t prewrap, uint32_t curpixel_cvg, uint32_t curpixel_cvbit)
{
    int r, g, b, dontblend;

    if (alpha_compare(globals.pixel_color.a))
    {
       if (globals.other_modes.antialias_en ? curpixel_cvg : curpixel_cvbit)
       {

          if (!globals.other_modes.color_on_cvg || prewrap)
          {
             dontblend = (globals.other_modes.f.partialreject_1cycle && globals.pixel_color.a >= 0xff);
             if (!blend_en || dontblend)
             {
                r = *globals.blender.i1a_r[0];
                g = *globals.blender.i1a_g[0];
                b = *globals.blender.i1a_b[0];
             }
             else
             {
                globals.inv_pixel_color.a =  (~(*globals.blender.i1b_a[0])) & 0xff;

                blender_equation_cycle0(&r, &g, &b);
             }
          }
          else
          {
             r = *globals.blender.i2a_r[0];
             g = *globals.blender.i2a_g[0];
             b = *globals.blender.i2a_b[0];
          }

          if (globals.other_modes.rgb_dither_sel != 3)
             rgb_dither(&r, &g, &b, dith);

          *fr = r;
          *fg = g;
          *fb = b;
          return 1;
       }
    }
    return 0;
}

static STRICTINLINE int blender_2cycle(uint32_t* fr, uint32_t* fg, uint32_t* fb, int dith, uint32_t blend_en, uint32_t prewrap, uint32_t curpixel_cvg, uint32_t curpixel_cvbit, int32_t acalpha)
{
    int r, g, b, dontblend;

    if (alpha_compare(acalpha))
    {
       if (globals.other_modes.antialias_en ? (curpixel_cvg) : (curpixel_cvbit))
       {

          globals.inv_pixel_color.a =  (~(*globals.blender.i1b_a[0])) & 0xff;
          blender_equation_cycle0_2(&r, &g, &b);


          globals.memory_color = globals.pre_memory_color;

          globals.blended_pixel_color.r = r;
          globals.blended_pixel_color.g = g;
          globals.blended_pixel_color.b = b;
          globals.blended_pixel_color.a = globals.pixel_color.a;

          if (!globals.other_modes.color_on_cvg || prewrap)
          {
             dontblend = (globals.other_modes.f.partialreject_2cycle && globals.pixel_color.a >= 0xff);
             if (!blend_en || dontblend)
             {
                r = *globals.blender.i1a_r[1];
                g = *globals.blender.i1a_g[1];
                b = *globals.blender.i1a_b[1];
             }
             else
             {
                globals.inv_pixel_color.a =  (~(*globals.blender.i1b_a[1])) & 0xff;
                blender_equation_cycle1(&r, &g, &b);
             }
          }
          else
          {
             r = *globals.blender.i2a_r[1];
             g = *globals.blender.i2a_g[1];
             b = *globals.blender.i2a_b[1];
          }


          if (globals.other_modes.rgb_dither_sel != 3)
             rgb_dither(&r, &g, &b, dith);
          *fr = r;
          *fg = g;
          *fb = b;
          return 1;
       }
    }

    globals.memory_color = globals.pre_memory_color;
    return 0;
}

static void blender_init(void)
{
   int i;
   int d = 0, n = 0, temp = 0, res = 0, invd = 0, nbit = 0;
   int ps[9];

   for (i = 0; i < 0x8000; i++)
   {
      int k;

      res   = 0;
      d     = (i >> 11) & 0xf;
      n     = i & 0x7ff;
      invd  = (~d) & 0xf;
      temp  = invd + (n >> 8) + 1;
      ps[0] = temp & 7;

      for (k = 0; k < 8; k++)
      {
         nbit = (n >> (7 - k)) & 1;
         if (res & (0x100 >> k))
            temp = invd + (ps[k] << 1) + nbit + 1;
         else
            temp = d + (ps[k] << 1) + nbit;
         ps[k + 1] = temp & 7;
         if (temp & 0x10)
            res |= (1 << (7 - k));
      }
      bldiv_hwaccurate_table[i] = res;
   }
}

static void rdp_set_fog_color(const uint32_t* args)
{
    globals.fog_color.r = (args[1] >> 24) & 0xff;
    globals.fog_color.g = (args[1] >> 16) & 0xff;
    globals.fog_color.b = (args[1] >>  8) & 0xff;
    globals.fog_color.a = (args[1] >>  0) & 0xff;
}

static void rdp_set_blend_color(const uint32_t* args)
{
    globals.blend_color.r = (args[1] >> 24) & 0xff;
    globals.blend_color.g = (args[1] >> 16) & 0xff;
    globals.blend_color.b = (args[1] >>  8) & 0xff;
    globals.blend_color.a = (args[1] >>  0) & 0xff;
}

static INLINE void set_suba_rgb_input(int32_t **input_r, int32_t **input_g, int32_t **input_b, int code)
{
   switch (code & 0xf)
   {
      case 0:
         *input_r = &globals.combined_color.r;
         *input_g = &globals.combined_color.g;
         *input_b = &globals.combined_color.b;
         break;
      case 1:
         *input_r = &globals.texel0_color.r;
         *input_g = &globals.texel0_color.g;
         *input_b = &globals.texel0_color.b;
         break;
      case 2:
         *input_r = &globals.texel1_color.r;
         *input_g = &globals.texel1_color.g;
         *input_b = &globals.texel1_color.b;
         break;
      case 3:
         *input_r = &globals.prim_color.r;
         *input_g = &globals.prim_color.g; 
         *input_b = &globals.prim_color.b;
         break;
      case 4:
         *input_r = &globals.shade_color.r;
         *input_g = &globals.shade_color.g;
         *input_b = &globals.shade_color.b;
         break;
      case 5:
         *input_r = &globals.env_color.r; 
         *input_g = &globals.env_color.g;
         *input_b = &globals.env_color.b;
         break;
      case 6:     *input_r = &one_color;          *input_g = &one_color;          *input_b = &one_color;      break;
      case 7:
                  *input_r = &globals.noise;
                  *input_g = &globals.noise;
                  *input_b = &globals.noise;
                  break;
      case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15:
                  *input_r = &zero_color;
                  *input_g = &zero_color;
                  *input_b = &zero_color;
                  break;
   }
}

static INLINE void set_subb_rgb_input(int32_t **input_r, int32_t **input_g, int32_t **input_b, int code)
{
    switch (code & 0xf)
    {
       case 0:
          *input_r = &globals.combined_color.r;
          *input_g = &globals.combined_color.g;
          *input_b = &globals.combined_color.b;
          break;
       case 1:
          *input_r = &globals.texel0_color.r;
          *input_g = &globals.texel0_color.g;
          *input_b = &globals.texel0_color.b;
          break;
       case 2:
          *input_r = &globals.texel1_color.r;
          *input_g = &globals.texel1_color.g;
          *input_b = &globals.texel1_color.b;
          break;
       case 3:
          *input_r = &globals.prim_color.r;
          *input_g = &globals.prim_color.g;
          *input_b = &globals.prim_color.b;
          break;
       case 4:
          *input_r = &globals.shade_color.r;
          *input_g = &globals.shade_color.g;
          *input_b = &globals.shade_color.b;
          break;
       case 5:
          *input_r = &globals.env_color.r;
          *input_g = &globals.env_color.g;
          *input_b = &globals.env_color.b;
          break;
       case 6:
          *input_r = &globals.key_center.r;
          *input_g = &globals.key_center.g;
          *input_b = &globals.key_center.b;
          break;
       case 7:
          *input_r = &globals.k4;
          *input_g = &globals.k4;
          *input_b = &globals.k4;
          break;
       case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15:
          *input_r = &zero_color;
          *input_g = &zero_color;
          *input_b = &zero_color;
          break;
    }
}

static INLINE void set_mul_rgb_input(int32_t **input_r, int32_t **input_g, int32_t **input_b, int code)
{
    switch (code & 0x1f)
    {
        case 0:
           *input_r = &globals.combined_color.r;
           *input_g = &globals.combined_color.g;
           *input_b = &globals.combined_color.b;
           break;
        case 1:
           *input_r = &globals.texel0_color.r;
           *input_g = &globals.texel0_color.g;
           *input_b = &globals.texel0_color.b;
           break;
        case 2:
           *input_r = &globals.texel1_color.r;
           *input_g = &globals.texel1_color.g;
           *input_b = &globals.texel1_color.b;
           break;
        case 3: 
           *input_r = &globals.prim_color.r; 
           *input_g = &globals.prim_color.g;
           *input_b = &globals.prim_color.b;
           break;
        case 4:     *input_r = &globals.shade_color.r;      *input_g = &globals.shade_color.g;      *input_b = &globals.shade_color.b;      break;
        case 5:     *input_r = &globals.env_color.r;        *input_g = &globals.env_color.g;        *input_b = &globals.env_color.b;        break;
        case 6:     *input_r = &globals.key_scale.r;        *input_g = &globals.key_scale.g;        *input_b = &globals.key_scale.b;        break;
        case 7: 
                    *input_r = &globals.combined_color.a;
                    *input_g = &globals.combined_color.a;
                    *input_b = &globals.combined_color.a;
                    break;
        case 8:     *input_r = &globals.texel0_color.a;     *input_g = &globals.texel0_color.a;     *input_b = &globals.texel0_color.a;     break;
        case 9:     *input_r = &globals.texel1_color.a;     *input_g = &globals.texel1_color.a;     *input_b = &globals.texel1_color.a;     break;
        case 10:    *input_r = &globals.prim_color.a;       *input_g = &globals.prim_color.a;       *input_b = &globals.prim_color.a;       break;
        case 11:    *input_r = &globals.shade_color.a;      *input_g = &globals.shade_color.a;      *input_b = &globals.shade_color.a;      break;
        case 12:    *input_r = &globals.env_color.a;        *input_g = &globals.env_color.a;        *input_b = &globals.env_color.a;        break;
        case 13:
                    *input_r = &globals.lod_frac;
                    *input_g = &globals.lod_frac;
                    *input_b = &globals.lod_frac;
                    break;
        case 14:
                    *input_r = &globals.primitive_lod_frac;
                    *input_g = &globals.primitive_lod_frac;
                    *input_b = &globals.primitive_lod_frac;
                    break;
        case 15:
                    *input_r = &globals.k5;
                    *input_g = &globals.k5;
                    *input_b = &globals.k5;
                    break;
        case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23:
        case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31:
        {
            *input_r = &zero_color;     *input_g = &zero_color;     *input_b = &zero_color;     break;
        }
    }
}

static INLINE void set_add_rgb_input(int32_t **input_r, int32_t **input_g, int32_t **input_b, int code)
{
    switch (code & 0x7)
    {
        case 0:
           *input_r = &globals.combined_color.r;
           *input_g = &globals.combined_color.g;
           *input_b = &globals.combined_color.b;
           break;
        case 1:     *input_r = &globals.texel0_color.r;     *input_g = &globals.texel0_color.g;     *input_b = &globals.texel0_color.b;     break;
        case 2:     *input_r = &globals.texel1_color.r;     *input_g = &globals.texel1_color.g;     *input_b = &globals.texel1_color.b;     break;
        case 3:     *input_r = &globals.prim_color.r;       *input_g = &globals.prim_color.g;       *input_b = &globals.prim_color.b;       break;
        case 4:     *input_r = &globals.shade_color.r;      *input_g = &globals.shade_color.g;      *input_b = &globals.shade_color.b;      break;
        case 5:     *input_r = &globals.env_color.r;        *input_g = &globals.env_color.g;        *input_b = &globals.env_color.b;        break;
        case 6:     *input_r = &one_color;          *input_g = &one_color;          *input_b = &one_color;          break;
        case 7:     *input_r = &zero_color;         *input_g = &zero_color;         *input_b = &zero_color;         break;
    }
}

static INLINE void set_sub_alpha_input(int32_t **input, int code)
{
    switch (code & 0x7)
    {
        case 0: 
           *input = &globals.combined_color.a;
           break;
        case 1:     *input = &globals.texel0_color.a; break;
        case 2:     *input = &globals.texel1_color.a; break;
        case 3:     *input = &globals.prim_color.a; break;
        case 4:     *input = &globals.shade_color.a; break;
        case 5:     *input = &globals.env_color.a; break;
        case 6:     *input = &one_color; break;
        case 7:     *input = &zero_color; break;
    }
}

static INLINE void set_mul_alpha_input(int32_t **input, int code)
{
    switch (code & 0x7)
    {
        case 0:     *input = &globals.lod_frac; break;
        case 1:     *input = &globals.texel0_color.a; break;
        case 2:     *input = &globals.texel1_color.a; break;
        case 3:     *input = &globals.prim_color.a; break;
        case 4:     *input = &globals.shade_color.a; break;
        case 5:     *input = &globals.env_color.a; break;
        case 6:
                    *input = &globals.primitive_lod_frac;
                    break;
        case 7:     *input = &zero_color; break;
    }
}

static STRICTINLINE int32_t color_combiner_equation(int32_t a, int32_t b, int32_t c, int32_t d)
{





    a = special_9bit_exttable[a];
    b = special_9bit_exttable[b];
    c = SIGNF(c, 9);
    d = special_9bit_exttable[d];
    a = ((a - b) * c) + (d << 8) + 0x80;
    return (a & 0x1ffff);
}

static STRICTINLINE int32_t alpha_combiner_equation(int32_t a, int32_t b, int32_t c, int32_t d)
{
    a = special_9bit_exttable[a];
    b = special_9bit_exttable[b];
    c = SIGNF(c, 9);
    d = special_9bit_exttable[d];
    a = (((a - b) * c) + (d << 8) + 0x80) >> 8;
    return (a & 0x1ff);
}

static STRICTINLINE int32_t chroma_key_min(struct color* col)
{
    int32_t keyalpha;
    int32_t redkey   = SIGN(col->r, 17);
    int32_t greenkey = SIGN(col->g, 17);
    int32_t bluekey  = SIGN(col->b, 17);

    if (redkey > 0)
        redkey = ((redkey & 0xf) == 8) ? (-redkey + 0x10) : (-redkey);

    redkey = (globals.key_width.r << 4) + redkey;

    if (greenkey > 0)
        greenkey = ((greenkey & 0xf) == 8) ? (-greenkey + 0x10) : (-greenkey);

    greenkey = (globals.key_width.g << 4) + greenkey;

    if (bluekey > 0)
        bluekey = ((bluekey & 0xf) == 8) ? (-bluekey + 0x10) : (-bluekey);

    bluekey = (globals.key_width.b << 4) + bluekey;

    keyalpha = (redkey < greenkey) ? redkey : greenkey;
    keyalpha = (bluekey < keyalpha) ? bluekey : keyalpha;
    keyalpha = clamp(keyalpha, 0, 0xff);
    return keyalpha;
}

static STRICTINLINE void combiner_1cycle(int adseed, uint32_t* curpixel_cvg)
{

    int32_t keyalpha, temp;
    struct color chromabypass;

    if (globals.other_modes.key_en)
    {
        chromabypass.r = *globals.combiner.rgbsub_a_r[1];
        chromabypass.g = *globals.combiner.rgbsub_a_g[1];
        chromabypass.b = *globals.combiner.rgbsub_a_b[1];
    }

    if (globals.combiner.rgbmul_r[1] != &zero_color)
    {
        globals.combined_color.r = color_combiner_equation(*globals.combiner.rgbsub_a_r[1],*globals.combiner.rgbsub_b_r[1],*globals.combiner.rgbmul_r[1],*globals.combiner.rgbadd_r[1]);
        globals.combined_color.g = color_combiner_equation(*globals.combiner.rgbsub_a_g[1],*globals.combiner.rgbsub_b_g[1],*globals.combiner.rgbmul_g[1],*globals.combiner.rgbadd_g[1]);
        globals.combined_color.b = color_combiner_equation(*globals.combiner.rgbsub_a_b[1],*globals.combiner.rgbsub_b_b[1],*globals.combiner.rgbmul_b[1],*globals.combiner.rgbadd_b[1]);
    }
    else
    {
        globals.combined_color.r = ((special_9bit_exttable[*globals.combiner.rgbadd_r[1]] << 8) + 0x80) & 0x1ffff;
        globals.combined_color.g = ((special_9bit_exttable[*globals.combiner.rgbadd_g[1]] << 8) + 0x80) & 0x1ffff;
        globals.combined_color.b = ((special_9bit_exttable[*globals.combiner.rgbadd_b[1]] << 8) + 0x80) & 0x1ffff;
    }

    if (globals.combiner.alphamul[1] != &zero_color)
        globals.combined_color.a = alpha_combiner_equation(*globals.combiner.alphasub_a[1],*globals.combiner.alphasub_b[1],*globals.combiner.alphamul[1],*globals.combiner.alphaadd[1]);
    else
        globals.combined_color.a = special_9bit_exttable[*globals.combiner.alphaadd[1]] & 0x1ff;

    globals.pixel_color.a = special_9bit_clamptable[globals.combined_color.a];
    if (globals.pixel_color.a == 0xff)
        globals.pixel_color.a = 0x100;

    if (!globals.other_modes.key_en)
    {

        globals.combined_color.r >>= 8;
        globals.combined_color.g >>= 8;
        globals.combined_color.b >>= 8;
        globals.pixel_color.r = special_9bit_clamptable[globals.combined_color.r];
        globals.pixel_color.g = special_9bit_clamptable[globals.combined_color.g];
        globals.pixel_color.b = special_9bit_clamptable[globals.combined_color.b];
    }
    else
    {
        keyalpha = chroma_key_min(&globals.combined_color);

        globals.pixel_color.r = special_9bit_clamptable[chromabypass.r];
        globals.pixel_color.g = special_9bit_clamptable[chromabypass.g];
        globals.pixel_color.b = special_9bit_clamptable[chromabypass.b];


        globals.combined_color.r >>= 8;
        globals.combined_color.g >>= 8;
        globals.combined_color.b >>= 8;
    }


    if (globals.other_modes.cvg_times_alpha)
    {
        temp = (globals.pixel_color.a * (*curpixel_cvg) + 4) >> 3;
        *curpixel_cvg = (temp >> 5) & 0xf;
    }

    if (!globals.other_modes.alpha_cvg_select)
    {
        if (!globals.other_modes.key_en)
        {
            globals.pixel_color.a += adseed;
            if (globals.pixel_color.a & 0x100)
                globals.pixel_color.a = 0xff;
        }
        else
            globals.pixel_color.a = keyalpha;
    }
    else
    {
        if (globals.other_modes.cvg_times_alpha)
            globals.pixel_color.a = temp;
        else
            globals.pixel_color.a = (*curpixel_cvg) << 5;
        if (globals.pixel_color.a > 0xff)
            globals.pixel_color.a = 0xff;
    }

    globals.shade_color.a += adseed;
    if (globals.shade_color.a & 0x100)
        globals.shade_color.a = 0xff;
}

static STRICTINLINE void combiner_2cycle(int adseed, uint32_t* curpixel_cvg, int32_t* acalpha)
{
    int32_t keyalpha, temp;
    struct color chromabypass;

    if (globals.combiner.rgbmul_r[0] != &zero_color)
    {
        globals.combined_color.r = color_combiner_equation(*globals.combiner.rgbsub_a_r[0],*globals.combiner.rgbsub_b_r[0],*globals.combiner.rgbmul_r[0],*globals.combiner.rgbadd_r[0]);
        globals.combined_color.g = color_combiner_equation(*globals.combiner.rgbsub_a_g[0],*globals.combiner.rgbsub_b_g[0],*globals.combiner.rgbmul_g[0],*globals.combiner.rgbadd_g[0]);
        globals.combined_color.b = color_combiner_equation(*globals.combiner.rgbsub_a_b[0],*globals.combiner.rgbsub_b_b[0],*globals.combiner.rgbmul_b[0],*globals.combiner.rgbadd_b[0]);
    }
    else
    {
        globals.combined_color.r = ((special_9bit_exttable[*globals.combiner.rgbadd_r[0]] << 8) + 0x80) & 0x1ffff;
        globals.combined_color.g = ((special_9bit_exttable[*globals.combiner.rgbadd_g[0]] << 8) + 0x80) & 0x1ffff;
        globals.combined_color.b = ((special_9bit_exttable[*globals.combiner.rgbadd_b[0]] << 8) + 0x80) & 0x1ffff;
    }

    if (globals.combiner.alphamul[0] != &zero_color)
        globals.combined_color.a = alpha_combiner_equation(*globals.combiner.alphasub_a[0],*globals.combiner.alphasub_b[0],*globals.combiner.alphamul[0],*globals.combiner.alphaadd[0]);
    else
        globals.combined_color.a = special_9bit_exttable[*globals.combiner.alphaadd[0]] & 0x1ff;



    if (globals.other_modes.alpha_compare_en)
    {
        if (globals.other_modes.key_en)
            keyalpha = chroma_key_min(&globals.combined_color);

        int32_t preacalpha = special_9bit_clamptable[globals.combined_color.a];
        if (preacalpha == 0xff)
            preacalpha = 0x100;

        if (globals.other_modes.cvg_times_alpha)
            temp = (preacalpha * (*curpixel_cvg) + 4) >> 3;

        if (!globals.other_modes.alpha_cvg_select)
        {
            if (!globals.other_modes.key_en)
            {
                preacalpha += adseed;
                if (preacalpha & 0x100)
                    preacalpha = 0xff;
            }
            else
                preacalpha = keyalpha;
        }
        else
        {
            if (globals.other_modes.cvg_times_alpha)
                preacalpha = temp;
            else
                preacalpha = (*curpixel_cvg) << 5;
            if (preacalpha > 0xff)
                preacalpha = 0xff;
        }

        *acalpha = preacalpha;
    }





    globals.combined_color.r >>= 8;
    globals.combined_color.g >>= 8;
    globals.combined_color.b >>= 8;


    globals.texel0_color = globals.texel1_color;
    globals.texel1_color = globals.nexttexel_color;









    if (globals.other_modes.key_en)
    {
        chromabypass.r = *globals.combiner.rgbsub_a_r[1];
        chromabypass.g = *globals.combiner.rgbsub_a_g[1];
        chromabypass.b = *globals.combiner.rgbsub_a_b[1];
    }

    if (globals.combiner.rgbmul_r[1] != &zero_color)
    {
        globals.combined_color.r = color_combiner_equation(*globals.combiner.rgbsub_a_r[1],*globals.combiner.rgbsub_b_r[1],*globals.combiner.rgbmul_r[1],*globals.combiner.rgbadd_r[1]);
        globals.combined_color.g = color_combiner_equation(*globals.combiner.rgbsub_a_g[1],*globals.combiner.rgbsub_b_g[1],*globals.combiner.rgbmul_g[1],*globals.combiner.rgbadd_g[1]);
        globals.combined_color.b = color_combiner_equation(*globals.combiner.rgbsub_a_b[1],*globals.combiner.rgbsub_b_b[1],*globals.combiner.rgbmul_b[1],*globals.combiner.rgbadd_b[1]);
    }
    else
    {
        globals.combined_color.r = ((special_9bit_exttable[*globals.combiner.rgbadd_r[1]] << 8) + 0x80) & 0x1ffff;
        globals.combined_color.g = ((special_9bit_exttable[*globals.combiner.rgbadd_g[1]] << 8) + 0x80) & 0x1ffff;
        globals.combined_color.b = ((special_9bit_exttable[*globals.combiner.rgbadd_b[1]] << 8) + 0x80) & 0x1ffff;
    }

    if (globals.combiner.alphamul[1] != &zero_color)
        globals.combined_color.a = alpha_combiner_equation(*globals.combiner.alphasub_a[1],*globals.combiner.alphasub_b[1],*globals.combiner.alphamul[1],*globals.combiner.alphaadd[1]);
    else
        globals.combined_color.a = special_9bit_exttable[*globals.combiner.alphaadd[1]] & 0x1ff;

    if (!globals.other_modes.key_en)
    {

        globals.combined_color.r >>= 8;
        globals.combined_color.g >>= 8;
        globals.combined_color.b >>= 8;

        globals.pixel_color.r = special_9bit_clamptable[globals.combined_color.r];
        globals.pixel_color.g = special_9bit_clamptable[globals.combined_color.g];
        globals.pixel_color.b = special_9bit_clamptable[globals.combined_color.b];
    }
    else
    {
        keyalpha = chroma_key_min(&globals.combined_color);



        globals.pixel_color.r = special_9bit_clamptable[chromabypass.r];
        globals.pixel_color.g = special_9bit_clamptable[chromabypass.g];
        globals.pixel_color.b = special_9bit_clamptable[chromabypass.b];


        globals.combined_color.r >>= 8;
        globals.combined_color.g >>= 8;
        globals.combined_color.b >>= 8;
    }

    globals.pixel_color.a = special_9bit_clamptable[globals.combined_color.a];
    if (globals.pixel_color.a == 0xff)
        globals.pixel_color.a = 0x100;


    if (globals.other_modes.cvg_times_alpha)
    {
        temp = (globals.pixel_color.a * (*curpixel_cvg) + 4) >> 3;

        *curpixel_cvg = (temp >> 5) & 0xf;


    }

    if (!globals.other_modes.alpha_cvg_select)
    {
        if (!globals.other_modes.key_en)
        {
            globals.pixel_color.a += adseed;
            if (globals.pixel_color.a & 0x100)
                globals.pixel_color.a = 0xff;
        }
        else
            globals.pixel_color.a = keyalpha;
    }
    else
    {
        if (globals.other_modes.cvg_times_alpha)
            globals.pixel_color.a = temp;
        else
            globals.pixel_color.a = (*curpixel_cvg) << 5;
        if (globals.pixel_color.a > 0xff)
            globals.pixel_color.a = 0xff;
    }

    globals.shade_color.a += adseed;
    if (globals.shade_color.a & 0x100)
        globals.shade_color.a = 0xff;
}

static void combiner_init(void)
{
    globals.combiner.rgbsub_a_r[0] = globals.combiner.rgbsub_a_r[1] = &one_color;
    globals.combiner.rgbsub_a_g[0] = globals.combiner.rgbsub_a_g[1] = &one_color;
    globals.combiner.rgbsub_a_b[0] = globals.combiner.rgbsub_a_b[1] = &one_color;
    globals.combiner.rgbsub_b_r[0] = globals.combiner.rgbsub_b_r[1] = &one_color;
    globals.combiner.rgbsub_b_g[0] = globals.combiner.rgbsub_b_g[1] = &one_color;
    globals.combiner.rgbsub_b_b[0] = globals.combiner.rgbsub_b_b[1] = &one_color;
    globals.combiner.rgbmul_r[0] = globals.combiner.rgbmul_r[1] = &one_color;
    globals.combiner.rgbmul_g[0] = globals.combiner.rgbmul_g[1] = &one_color;
    globals.combiner.rgbmul_b[0] = globals.combiner.rgbmul_b[1] = &one_color;
    globals.combiner.rgbadd_r[0] = globals.combiner.rgbadd_r[1] = &one_color;
    globals.combiner.rgbadd_g[0] = globals.combiner.rgbadd_g[1] = &one_color;
    globals.combiner.rgbadd_b[0] = globals.combiner.rgbadd_b[1] = &one_color;

    globals.combiner.alphasub_a[0] = globals.combiner.alphasub_a[1] = &one_color;
    globals.combiner.alphasub_b[0] = globals.combiner.alphasub_b[1] = &one_color;
    globals.combiner.alphamul[0] = globals.combiner.alphamul[1] = &one_color;
    globals.combiner.alphaadd[0] = globals.combiner.alphaadd[1] = &one_color;

    for(int i = 0; i < 0x200; i++)
    {
        switch((i >> 7) & 3)
        {
        case 0:
        case 1:
            special_9bit_clamptable[i] = i & 0xff;
            break;
        case 2:
            special_9bit_clamptable[i] = 0xff;
            break;
        case 3:
            special_9bit_clamptable[i] = 0;
            break;
        }
    }

    for (int i = 0; i < 0x200; i++)
    {
        special_9bit_exttable[i] = ((i & 0x180) == 0x180) ? (i | ~0x1ff) : (i & 0x1ff);
    }
}

static void rdp_set_prim_color(const uint32_t* args)
{
    globals.min_level          = (args[0] >> 8) & 0x1f;
    globals.primitive_lod_frac = args[0] & 0xff;
    globals.prim_color.r = (args[1] >> 24) & 0xff;
    globals.prim_color.g = (args[1] >> 16) & 0xff;
    globals.prim_color.b = (args[1] >>  8) & 0xff;
    globals.prim_color.a = (args[1] >>  0) & 0xff;
}

static void rdp_set_env_color(const uint32_t* args)
{
    globals.env_color.r = (args[1] >> 24) & 0xff;
    globals.env_color.g = (args[1] >> 16) & 0xff;
    globals.env_color.b = (args[1] >>  8) & 0xff;
    globals.env_color.a = (args[1] >>  0) & 0xff;
}

static void rdp_set_combine(const uint32_t* args)
{
    globals.combine.sub_a_rgb0  = (args[0] >> 20) & 0xf;
    globals.combine.mul_rgb0    = (args[0] >> 15) & 0x1f;
    globals.combine.sub_a_a0    = (args[0] >> 12) & 0x7;
    globals.combine.mul_a0      = (args[0] >>  9) & 0x7;
    globals.combine.sub_a_rgb1  = (args[0] >>  5) & 0xf;
    globals.combine.mul_rgb1    = (args[0] >>  0) & 0x1f;

    globals.combine.sub_b_rgb0  = (args[1] >> 28) & 0xf;
    globals.combine.sub_b_rgb1  = (args[1] >> 24) & 0xf;
    globals.combine.sub_a_a1    = (args[1] >> 21) & 0x7;
    globals.combine.mul_a1      = (args[1] >> 18) & 0x7;
    globals.combine.add_rgb0    = (args[1] >> 15) & 0x7;
    globals.combine.sub_b_a0    = (args[1] >> 12) & 0x7;
    globals.combine.add_a0      = (args[1] >>  9) & 0x7;
    globals.combine.add_rgb1    = (args[1] >>  6) & 0x7;
    globals.combine.sub_b_a1    = (args[1] >>  3) & 0x7;
    globals.combine.add_a1      = (args[1] >>  0) & 0x7;


    set_suba_rgb_input(&globals.combiner.rgbsub_a_r[0], &globals.combiner.rgbsub_a_g[0], &globals.combiner.rgbsub_a_b[0], globals.combine.sub_a_rgb0);
    set_subb_rgb_input(&globals.combiner.rgbsub_b_r[0], &globals.combiner.rgbsub_b_g[0], &globals.combiner.rgbsub_b_b[0], globals.combine.sub_b_rgb0);
    set_mul_rgb_input(&globals.combiner.rgbmul_r[0], &globals.combiner.rgbmul_g[0], &globals.combiner.rgbmul_b[0], globals.combine.mul_rgb0);
    set_add_rgb_input(&globals.combiner.rgbadd_r[0], &globals.combiner.rgbadd_g[0], &globals.combiner.rgbadd_b[0], globals.combine.add_rgb0);
    set_sub_alpha_input(&globals.combiner.alphasub_a[0], globals.combine.sub_a_a0);
    set_sub_alpha_input(&globals.combiner.alphasub_b[0], globals.combine.sub_b_a0);
    set_mul_alpha_input(&globals.combiner.alphamul[0], globals.combine.mul_a0);
    set_sub_alpha_input(&globals.combiner.alphaadd[0], globals.combine.add_a0);

    set_suba_rgb_input(&globals.combiner.rgbsub_a_r[1], &globals.combiner.rgbsub_a_g[1], &globals.combiner.rgbsub_a_b[1], globals.combine.sub_a_rgb1);
    set_subb_rgb_input(&globals.combiner.rgbsub_b_r[1], &globals.combiner.rgbsub_b_g[1], &globals.combiner.rgbsub_b_b[1], globals.combine.sub_b_rgb1);
    set_mul_rgb_input(&globals.combiner.rgbmul_r[1], &globals.combiner.rgbmul_g[1], &globals.combiner.rgbmul_b[1], globals.combine.mul_rgb1);
    set_add_rgb_input(&globals.combiner.rgbadd_r[1], &globals.combiner.rgbadd_g[1], &globals.combiner.rgbadd_b[1], globals.combine.add_rgb1);
    set_sub_alpha_input(&globals.combiner.alphasub_a[1], globals.combine.sub_a_a1);
    set_sub_alpha_input(&globals.combiner.alphasub_b[1], globals.combine.sub_b_a1);
    set_mul_alpha_input(&globals.combiner.alphamul[1], globals.combine.mul_a1);
    set_sub_alpha_input(&globals.combiner.alphaadd[1], globals.combine.add_a1);

    globals.other_modes.f.stalederivs = 1;
}

static void rdp_set_key_gb(const uint32_t* args)
{
    globals.key_width.g   = (args[0] >> 12) & 0xfff;
    globals.key_width.b   = args[0] & 0xfff;
    globals.key_center.g  = (args[1] >> 24) & 0xff;
    globals.key_scale.g   = (args[1] >> 16) & 0xff;
    globals.key_center.b  = (args[1] >> 8) & 0xff;
    globals.key_scale.b   = args[1] & 0xff;
}

static void rdp_set_key_r(const uint32_t* args)
{
    globals.key_width.r  = (args[1] >> 16) & 0xfff;
    globals.key_center.r = (args[1] >> 8) & 0xff;
    globals.key_scale.r  = args[1] & 0xff;
}

static STRICTINLINE uint32_t rightcvghex(uint32_t x, uint32_t fmask)
{
    uint32_t covered = ((x & 7) + 1) >> 1;
    covered = 0xf0 >> covered;
    return (covered & fmask);
}

static STRICTINLINE uint32_t leftcvghex(uint32_t x, uint32_t fmask)
{
    uint32_t covered = ((x & 7) + 1) >> 1;
    covered = 0xf >> covered;
    return (covered & fmask);
}



static STRICTINLINE void compute_cvg_flip(int32_t scanline)
{
    int i, fmask, maskshift, fmaskshifted;
    int32_t minorcur, majorcur, minorcurint, majorcurint, samecvg;
    int32_t purgestart = globals.span[scanline].rx;
    int32_t purgeend = globals.span[scanline].lx;
    int length = purgeend - purgestart;

    if (length >= 0)
    {
        memset(&globals.cvgbuf[purgestart], 0xff, length + 1);
        for(i = 0; i < 4; i++)
        {
                fmask        = 0xa >> (i & 1);
                maskshift    = (i - 2) & 4;
                fmaskshifted = fmask << maskshift;

                if (!globals.span[scanline].invalyscan[i])
                {
                    minorcur = globals.span[scanline].minorx[i];
                    majorcur = globals.span[scanline].majorx[i];
                    minorcurint = minorcur >> 3;
                    majorcurint = majorcur >> 3;


                    for (int k = purgestart; k <= majorcurint; k++)
                        globals.cvgbuf[k] &= ~fmaskshifted;
                    for (int k = minorcurint; k <= purgeend; k++)
                        globals.cvgbuf[k] &= ~fmaskshifted;

                    if (minorcurint > majorcurint)
                    {
                        globals.cvgbuf[minorcurint] |= (rightcvghex(minorcur, fmask) << maskshift);
                        globals.cvgbuf[majorcurint] |= (leftcvghex(majorcur, fmask) << maskshift);
                    }
                    else if (minorcurint == majorcurint)
                    {
                        samecvg = rightcvghex(minorcur, fmask) & leftcvghex(majorcur, fmask);
                        globals.cvgbuf[majorcurint] |= (samecvg << maskshift);
                    }
                }
                else
                {
                   int k;
                   for (k = purgestart; k <= purgeend; k++)
                      globals.cvgbuf[k] &= ~fmaskshifted;
                }

        }
    }
}

static STRICTINLINE void compute_cvg_noflip(int32_t scanline)
{
    int i, fmask, maskshift, fmaskshifted;
    int32_t minorcur, majorcur, minorcurint, majorcurint, samecvg;
    int32_t purgestart = globals.span[scanline].lx;
    int32_t purgeend = globals.span[scanline].rx;
    int length = purgeend - purgestart;

    if (length >= 0)
    {
        memset(&globals.cvgbuf[purgestart], 0xff, length + 1);

        for(i = 0; i < 4; i++)
        {
            fmask = 0xa >> (i & 1);
            maskshift = (i - 2) & 4;
            fmaskshifted = fmask << maskshift;

            if (!globals.span[scanline].invalyscan[i])
            {
               int k;

               minorcur = globals.span[scanline].minorx[i];
               majorcur = globals.span[scanline].majorx[i];
               minorcurint = minorcur >> 3;
               majorcurint = majorcur >> 3;

               for (k = purgestart; k <= minorcurint; k++)
                  globals.cvgbuf[k] &= ~fmaskshifted;
               for (k = majorcurint; k <= purgeend; k++)
                  globals.cvgbuf[k] &= ~fmaskshifted;

               if (majorcurint > minorcurint)
               {
                  globals.cvgbuf[minorcurint] |= (leftcvghex(minorcur, fmask) << maskshift);
                  globals.cvgbuf[majorcurint] |= (rightcvghex(majorcur, fmask) << maskshift);
               }
               else if (minorcurint == majorcurint)
               {
                  samecvg = leftcvghex(minorcur, fmask) & rightcvghex(majorcur, fmask);
                  globals.cvgbuf[majorcurint] |= (samecvg << maskshift);
               }
            }
            else
            {
               int k;
               for (k = purgestart; k <= purgeend; k++)
                  globals.cvgbuf[k] &= ~fmaskshifted;
            }
        }
    }
}

static STRICTINLINE int finalize_spanalpha(uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg)
{
    int finalcvg;

    switch(globals.other_modes.cvg_dest)
    {
    case CVG_CLAMP:
        if (!blend_en)
            finalcvg = curpixel_cvg - 1;
        else
            finalcvg = curpixel_cvg + curpixel_memcvg;

        if (!(finalcvg & 8))
            finalcvg &= 7;
        else
            finalcvg = 7;

        break;
    case CVG_WRAP:
        finalcvg = (curpixel_cvg + curpixel_memcvg) & 7;
        break;
    case CVG_ZAP:
        finalcvg = 7;
        break;
    case CVG_SAVE:
        finalcvg = curpixel_memcvg;
        break;
    }

    return finalcvg;
}

static STRICTINLINE uint16_t decompress_cvmask_frombyte(uint8_t x)
{
    uint16_t y = (x & 0x5) | ((x & 0x5a) << 4) | ((x & 0xa0) << 8);
    return y;
}

static STRICTINLINE void lookup_cvmask_derivatives(uint32_t idx, uint8_t* offx, uint8_t* offy, uint32_t* curpixel_cvg, uint32_t* curpixel_cvbit)
{
    uint8_t mask    = globals.cvgbuf[idx];
    *curpixel_cvg   = cvarray[mask].cvg;
    *curpixel_cvbit = cvarray[mask].cvbit;
    *offx           = cvarray[mask].xoff;
    *offy           = cvarray[mask].yoff;
}

static INLINE void precalc_cvmask_derivatives(void)
{
    int i = 0, k = 0;
    uint16_t mask = 0, maskx = 0, masky = 0;
    uint8_t offx = 0, offy = 0;
    const uint8_t yarray[16] = {0, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0};
    const uint8_t xarray[16] = {0, 3, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};

    for (; i < 0x100; i++)
    {
        mask = decompress_cvmask_frombyte(i);
        cvarray[i].cvg = cvarray[i].cvbit = 0;
        cvarray[i].cvbit = (i >> 7) & 1;
        for (k = 0; k < 8; k++)
            cvarray[i].cvg += ((i >> k) & 1);


        masky = maskx = offx = offy = 0;
        for (k = 0; k < 4; k++)
            masky |= ((mask & (0xf000 >> (k << 2))) > 0) << k;

        offy = yarray[masky];

        maskx = (mask & (0xf000 >> (offy << 2))) >> ((offy ^ 3) << 2);


        offx = xarray[maskx];

        cvarray[i].xoff = offx;
        cvarray[i].yoff = offy;
    }
}

static STRICTINLINE uint32_t z_decompress(uint32_t zb)
{
    return z_complete_dec_table[(zb >> 2) & 0x3fff];
}

static INLINE void z_build_com_table(void)
{
   int z;
   uint16_t altmem = 0;

   for(z = 0; z < 0x40000; z++)
   {
      switch((z >> 11) & 0x7f)
      {
         case 0x00:
         case 0x01:
         case 0x02:
         case 0x03:
         case 0x04:
         case 0x05:
         case 0x06:
         case 0x07:
         case 0x08:
         case 0x09:
         case 0x0a:
         case 0x0b:
         case 0x0c:
         case 0x0d:
         case 0x0e:
         case 0x0f:
         case 0x10:
         case 0x11:
         case 0x12:
         case 0x13:
         case 0x14:
         case 0x15:
         case 0x16:
         case 0x17:
         case 0x18:
         case 0x19:
         case 0x1a:
         case 0x1b:
         case 0x1c:
         case 0x1d:
         case 0x1e:
         case 0x1f:
         case 0x20:
         case 0x21:
         case 0x22:
         case 0x23:
         case 0x24:
         case 0x25:
         case 0x26:
         case 0x27:
         case 0x28:
         case 0x29:
         case 0x2a:
         case 0x2b:
         case 0x2c:
         case 0x2d:
         case 0x2e:
         case 0x2f:
         case 0x30:
         case 0x31:
         case 0x32:
         case 0x33:
         case 0x34:
         case 0x35:
         case 0x36:
         case 0x37:
         case 0x38:
         case 0x39:
         case 0x3a:
         case 0x3b:
         case 0x3c:
         case 0x3d:
         case 0x3e:
         case 0x3f:
            altmem = (z >> 4) & 0x1ffc;
            break;
         case 0x40:
         case 0x41:
         case 0x42:
         case 0x43:
         case 0x44:
         case 0x45:
         case 0x46:
         case 0x47:
         case 0x48:
         case 0x49:
         case 0x4a:
         case 0x4b:
         case 0x4c:
         case 0x4d:
         case 0x4e:
         case 0x4f:
         case 0x50:
         case 0x51:
         case 0x52:
         case 0x53:
         case 0x54:
         case 0x55:
         case 0x56:
         case 0x57:
         case 0x58:
         case 0x59:
         case 0x5a:
         case 0x5b:
         case 0x5c:
         case 0x5d:
         case 0x5e:
         case 0x5f:
            altmem = ((z >> 3) & 0x1ffc) | 0x2000;
            break;
         case 0x60:
         case 0x61:
         case 0x62:
         case 0x63:
         case 0x64:
         case 0x65:
         case 0x66:
         case 0x67:
         case 0x68:
         case 0x69:
         case 0x6a:
         case 0x6b:
         case 0x6c:
         case 0x6d:
         case 0x6e:
         case 0x6f:
            altmem = ((z >> 2) & 0x1ffc) | 0x4000;
            break;
         case 0x70:
         case 0x71:
         case 0x72:
         case 0x73:
         case 0x74:
         case 0x75:
         case 0x76:
         case 0x77:
            altmem = ((z >> 1) & 0x1ffc) | 0x6000;
            break;
         case 0x78:
         case 0x79:
         case 0x7a:
         case 0x7b:
            altmem = (z & 0x1ffc) | 0x8000;
            break;
         case 0x7c:
         case 0x7d:
            altmem = ((z << 1) & 0x1ffc) | 0xa000;
            break;
         case 0x7e:
            altmem = ((z << 2) & 0x1ffc) | 0xc000;
            break;
         case 0x7f:
            altmem = ((z << 2) & 0x1ffc) | 0xe000;
            break;
         default:
            msg_error("z_build_com_table failed");
            break;
      }

      z_com_table[z] = altmem;

   }
}

static STRICTINLINE void z_store(uint32_t zcurpixel, uint32_t z, int dzpixenc)
{
    uint16_t zval = z_com_table[z & 0x3ffff]|(dzpixenc >> 2);
    uint8_t hval = dzpixenc & 3;
    PAIRWRITE16(zcurpixel, zval, hval);
}

static STRICTINLINE uint32_t dz_decompress(uint32_t dz_compressed)
{
    return (1 << dz_compressed);
}


static STRICTINLINE uint32_t dz_compress(uint32_t value)
{
    int j = 0;
    if (value & 0xff00)
        j |= 8;
    if (value & 0xf0f0)
        j |= 4;
    if (value & 0xcccc)
        j |= 2;
    if (value & 0xaaaa)
        j |= 1;
    return j;
}

static STRICTINLINE uint32_t z_compare(uint32_t zcurpixel, uint32_t sz, uint16_t dzpix, int dzpixenc, uint32_t* blend_en, uint32_t* prewrap, uint32_t* curpixel_cvg, uint32_t curpixel_memcvg)
{
   uint8_t hval;
   uint16_t zval;
   uint32_t oz, dzmem;
   int32_t rawdzmem;
   int force_coplanar = 0;
   sz &= 0x3ffff;

   if (globals.other_modes.z_compare_en)
   {
      int cvgcoeff = 0;
      uint32_t dzenc = 0;

      int32_t diff;
      uint32_t nearer, max, infront;
      uint32_t dzmemmodifier;

      PAIRREAD16(zval, hval, zcurpixel);
      oz = z_decompress(zval);
      rawdzmem = ((zval & 3) << 2) | hval;
      dzmem = dz_decompress(rawdzmem);

      if (globals.other_modes.f.realblendershiftersneeded)
      {
         globals.blshifta = clamp(dzpixenc - rawdzmem, 0, 4);
         globals.blshiftb = clamp(rawdzmem - dzpixenc, 0, 4);
      }


      if (globals.other_modes.f.interpixelblendershiftersneeded)
      {
         globals.pastblshifta = clamp(dzpixenc - globals.pastrawdzmem, 0, 4);
         globals.pastblshiftb = clamp(globals.pastrawdzmem - dzpixenc, 0, 4);
      }

      globals.pastrawdzmem = rawdzmem;

      int precision_factor = (zval >> 13) & 0xf;

      if (precision_factor < 3)
      {
         if (dzmem != 0x8000)
            dzmem = MAX(dzmem << 1, 16 >> precision_factor);
         else
         {
            force_coplanar = 1;
            dzmem = 0xffff;
         }
      }

      uint32_t dznew = (uint32_t)deltaz_comparator_lut[dzpix | dzmem];

      uint32_t dznotshift = dznew;
      dznew <<= 3;


      uint32_t farther = force_coplanar || ((sz + dznew) >= oz);

      int overflow = (curpixel_memcvg + *curpixel_cvg) & 8;
      *blend_en = globals.other_modes.force_blend || (!overflow && globals.other_modes.antialias_en && farther);

      *prewrap = overflow;

      switch(globals.other_modes.z_mode)
      {
         case ZMODE_OPAQUE:
            infront = sz < oz;
            diff = (int32_t)sz - (int32_t)dznew;
            nearer = force_coplanar || (diff <= (int32_t)oz);
            max = (oz == 0x3ffff);
            return (max || (overflow ? infront : nearer));
            break;
         case ZMODE_INTERPENETRATING:
            infront = sz < oz;
            if (!infront || !farther || !overflow)
            {
               diff = (int32_t)sz - (int32_t)dznew;
               nearer = force_coplanar || (diff <= (int32_t)oz);
               max = (oz == 0x3ffff);
               return (max || (overflow ? infront : nearer));
            }
            else
            {
               dzenc = dz_compress(dznotshift & 0xffff);
               cvgcoeff = ((oz >> dzenc) - (sz >> dzenc)) & 0xf;
               *curpixel_cvg = ((cvgcoeff * (*curpixel_cvg)) >> 3) & 0xf;
               return 1;
            }
            break;
         case ZMODE_TRANSPARENT:
            infront = sz < oz;
            max = (oz == 0x3ffff);
            return (infront || max);
            break;
         case ZMODE_DECAL:
            diff = (int32_t)sz - (int32_t)dznew;
            nearer = force_coplanar || (diff <= (int32_t)oz);
            max = (oz == 0x3ffff);
            return (farther && nearer && !max);
            break;
      }
      return 0;
   }
   else
   {


      if (globals.other_modes.f.realblendershiftersneeded)
      {
         globals.blshifta = 0;
         if (dzpixenc < 0xb)
            globals.blshiftb = 4;
         else
            globals.blshiftb = 0xf - dzpixenc;
      }

      if (globals.other_modes.f.interpixelblendershiftersneeded)
      {
         globals.pastblshifta = 0;
         if (dzpixenc < 0xb)
            globals.pastblshiftb = 4;
         else
            globals.pastblshiftb = 0xf - dzpixenc;
      }

      globals.pastrawdzmem = 0xf;

      int overflow = (curpixel_memcvg + *curpixel_cvg) & 8;
      *blend_en = globals.other_modes.force_blend || (!overflow && globals.other_modes.antialias_en);
      *prewrap = overflow;

      return 1;
   }
}

static void rdp_set_mask_image(const uint32_t* args)
{
    globals.zb_address  = args[1] & 0x0ffffff;
}

uint32_t rdp_get_zb_address(void)
{
    return globals.zb_address;
}

void z_init(void)
{
   int i;
   uint32_t exponent;
   uint32_t mantissa;

   z_build_com_table();

   for (i = 0; i < 0x4000; i++)
   {
      exponent = (i >> 11) & 7;
      mantissa = i & 0x7ff;
      z_complete_dec_table[i] = ((mantissa << z_dec_table[exponent].shift) + z_dec_table[exponent].add) & 0x3ffff;
   }

   deltaz_comparator_lut[0] = 0;
   for (i = 1; i < 0x10000; i++)
   {
      int k;
      for (k = 15; k >= 0; k--)
      {
         if (i & (1 << k))
         {
            deltaz_comparator_lut[i] = 1 << k;
            break;
         }
      }
   }
}

static void fbwrite_4(uint32_t curpixel, uint32_t r, uint32_t g, uint32_t b, uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg);
static void fbwrite_8(uint32_t curpixel, uint32_t r, uint32_t g, uint32_t b, uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg);
static void fbwrite_16(uint32_t curpixel, uint32_t r, uint32_t g, uint32_t b, uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg);
static void fbwrite_32(uint32_t curpixel, uint32_t r, uint32_t g, uint32_t b, uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg);
static void fbread_4(uint32_t num, uint32_t* curpixel_memcvg);
static void fbread_8(uint32_t num, uint32_t* curpixel_memcvg);
static void fbread_16(uint32_t num, uint32_t* curpixel_memcvg);
static void fbread_32(uint32_t num, uint32_t* curpixel_memcvg);
static void fbread2_4(uint32_t num, uint32_t* curpixel_memcvg);
static void fbread2_8(uint32_t num, uint32_t* curpixel_memcvg);
static void fbread2_16(uint32_t num, uint32_t* curpixel_memcvg);
static void fbread2_32(uint32_t num, uint32_t* curpixel_memcvg);

static void (*fbread_func[4])(uint32_t, uint32_t*) =
{
    fbread_4, fbread_8, fbread_16, fbread_32
};

static void (*fbread2_func[4])(uint32_t, uint32_t*) =
{
    fbread2_4, fbread2_8, fbread2_16, fbread2_32
};

static void (*fbwrite_func[4])(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) =
{
    fbwrite_4, fbwrite_8, fbwrite_16, fbwrite_32
};

static void fbwrite_4(uint32_t curpixel, uint32_t r, uint32_t g, uint32_t b, uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg)
{
    uint32_t fb = globals.fb_address + curpixel;
    RWRITEADDR8(fb, 0);
}

static void fbwrite_8(uint32_t curpixel, uint32_t r, uint32_t g, uint32_t b, uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg)
{
    uint32_t fb = globals.fb_address + curpixel;
    PAIRWRITE8(fb, r & 0xff, (r & 1) ? 3 : 0);
}

static void fbwrite_16(uint32_t curpixel, uint32_t r, uint32_t g, uint32_t b, uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg)
{
#undef CVG_DRAW
#ifdef CVG_DRAW
    int covdraw = (curpixel_cvg - 1) << 5;
    r=covdraw; g=covdraw; b=covdraw;
#endif

    uint32_t fb;
    uint16_t rval;
    uint8_t hval;
    fb = (globals.fb_address >> 1) + curpixel;

    int32_t finalcvg = finalize_spanalpha(blend_en, curpixel_cvg, curpixel_memcvg);
    int16_t finalcolor;

    if (globals.fb_format == FORMAT_RGBA)
    {
        finalcolor = ((r & ~7) << 8) | ((g & ~7) << 3) | ((b & ~7) >> 2);
    }
    else
    {
        finalcolor = (r << 8) | (finalcvg << 5);
        finalcvg = 0;
    }


    rval = finalcolor|(finalcvg >> 2);
    hval = finalcvg & 3;
    PAIRWRITE16(fb, rval, hval);
}

static void fbwrite_32(uint32_t curpixel, uint32_t r, uint32_t g, uint32_t b, uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg)
{
    uint32_t fb = (globals.fb_address >> 2) + curpixel;

    int32_t finalcolor;
    int32_t finalcvg = finalize_spanalpha(blend_en, curpixel_cvg, curpixel_memcvg);

    finalcolor = (r << 24) | (g << 16) | (b << 8);
    finalcolor |= (finalcvg << 5);

    PAIRWRITE32(fb, finalcolor, (g & 1) ? 3 : 0, 0);
}

static void fbfill_4(uint32_t curpixel)
{
    rdp_pipeline_crashed = 1;
}

static void fbfill_8(uint32_t curpixel)
{
    uint32_t fb = globals.fb_address + curpixel;
    uint32_t val = (globals.fill_color >> (((fb & 3) ^ 3) << 3)) & 0xff;
    uint8_t hval = ((val & 1) << 1) | (val & 1);
    PAIRWRITE8(fb, val, hval);
}

static void fbfill_16(uint32_t curpixel)
{
    uint16_t val;
    uint8_t hval;
    uint32_t fb = (globals.fb_address >> 1) + curpixel;
    if (fb & 1)
        val = globals.fill_color & 0xffff;
    else
        val = (globals.fill_color >> 16) & 0xffff;
    hval = ((val & 1) << 1) | (val & 1);
    PAIRWRITE16(fb, val, hval);
}

static void fbfill_32(uint32_t curpixel)
{
    uint32_t fb = (globals.fb_address >> 2) + curpixel;
    PAIRWRITE32(fb, globals.fill_color, (globals.fill_color & 0x10000) ? 3 : 0, (globals.fill_color & 0x1) ? 3 : 0);
}

static void fbread_4(uint32_t curpixel, uint32_t* curpixel_memcvg)
{
   globals.memory_color.r = 0;
   globals.memory_color.g = 0;
   globals.memory_color.b = 0;

   *curpixel_memcvg       = 7;
   globals.memory_color.a = 0xe0;
}

static void fbread2_4(uint32_t curpixel, uint32_t* curpixel_memcvg)
{
    globals.pre_memory_color.r = 0;
    globals.pre_memory_color.g = 0;
    globals.pre_memory_color.b = 0;
    globals.pre_memory_color.a = 0xe0;
    *curpixel_memcvg = 7;
}

static void fbread_8(uint32_t curpixel, uint32_t* curpixel_memcvg)
{
    uint8_t mem;
    uint32_t addr = globals.fb_address + curpixel;
    RREADADDR8(mem, addr);
    globals.memory_color.r = globals.memory_color.g = globals.memory_color.b = mem;
    *curpixel_memcvg = 7;
    globals.memory_color.a = 0xe0;
}

static void fbread2_8(uint32_t curpixel, uint32_t* curpixel_memcvg)
{
    uint8_t mem;
    uint32_t addr = globals.fb_address + curpixel;
    RREADADDR8(mem, addr);
    globals.pre_memory_color.r = mem;
    globals.pre_memory_color.g = mem;
    globals.pre_memory_color.b = mem;
    globals.pre_memory_color.a = 0xe0;
    *curpixel_memcvg = 7;
}

static void fbread_16(uint32_t curpixel, uint32_t* curpixel_memcvg)
{
    uint16_t fword;
    uint8_t hbyte;
    uint32_t addr = (globals.fb_address >> 1) + curpixel;

    uint8_t lowbits;


    if (globals.other_modes.image_read_en)
    {
        PAIRREAD16(fword, hbyte, addr);

        if (globals.fb_format == FORMAT_RGBA)
        {
            globals.memory_color.r = GET_HI(fword);
            globals.memory_color.g = GET_MED(fword);
            globals.memory_color.b = GET_LOW(fword);
            lowbits = ((fword & 1) << 2) | hbyte;
        }
        else
        {
            globals.memory_color.r = globals.memory_color.g = globals.memory_color.b = fword >> 8;
            lowbits = (fword >> 5) & 7;
        }

        *curpixel_memcvg = lowbits;
        globals.memory_color.a = lowbits << 5;
    }
    else
    {
        RREADIDX16(fword, addr);

        if (globals.fb_format == FORMAT_RGBA)
        {
            globals.memory_color.r = GET_HI(fword);
            globals.memory_color.g = GET_MED(fword);
            globals.memory_color.b = GET_LOW(fword);
        }
        else
            globals.memory_color.r = globals.memory_color.g = globals.memory_color.b = fword >> 8;

        *curpixel_memcvg = 7;
        globals.memory_color.a = 0xe0;
    }
}

static void fbread2_16(uint32_t curpixel, uint32_t* curpixel_memcvg)
{
    uint16_t fword;
    uint8_t hbyte;
    uint32_t addr = (globals.fb_address >> 1) + curpixel;

    uint8_t lowbits;

    if (globals.other_modes.image_read_en)
    {
        PAIRREAD16(fword, hbyte, addr);

        if (globals.fb_format == FORMAT_RGBA)
        {
            globals.pre_memory_color.r = GET_HI(fword);
            globals.pre_memory_color.g = GET_MED(fword);
            globals.pre_memory_color.b = GET_LOW(fword);
            lowbits = ((fword & 1) << 2) | hbyte;
        }
        else
        {
            globals.pre_memory_color.r = globals.pre_memory_color.g = globals.pre_memory_color.b = fword >> 8;
            lowbits = (fword >> 5) & 7;
        }

        *curpixel_memcvg = lowbits;
        globals.pre_memory_color.a = lowbits << 5;
    }
    else
    {
        RREADIDX16(fword, addr);

        if (globals.fb_format == FORMAT_RGBA)
        {
            globals.pre_memory_color.r = GET_HI(fword);
            globals.pre_memory_color.g = GET_MED(fword);
            globals.pre_memory_color.b = GET_LOW(fword);
        }
        else
            globals.pre_memory_color.r = globals.pre_memory_color.g = globals.pre_memory_color.b = fword >> 8;

        *curpixel_memcvg = 7;
        globals.pre_memory_color.a = 0xe0;
    }

}

static void fbread_32(uint32_t curpixel, uint32_t* curpixel_memcvg)
{
    uint32_t mem, addr = (globals.fb_address >> 2) + curpixel;
    RREADIDX32(mem, addr);
    globals.memory_color.r = (mem >> 24) & 0xff;
    globals.memory_color.g = (mem >> 16) & 0xff;
    globals.memory_color.b = (mem >> 8) & 0xff;
    if (globals.other_modes.image_read_en)
    {
        *curpixel_memcvg = (mem >> 5) & 7;
        globals.memory_color.a = mem & 0xe0;
    }
    else
    {
        *curpixel_memcvg = 7;
        globals.memory_color.a = 0xe0;
    }
}

static INLINE void fbread2_32(uint32_t curpixel, uint32_t* curpixel_memcvg)
{
    uint32_t mem, addr = (globals.fb_address >> 2) + curpixel;
    RREADIDX32(mem, addr);
    globals.pre_memory_color.r = (mem >> 24) & 0xff;
    globals.pre_memory_color.g = (mem >> 16) & 0xff;
    globals.pre_memory_color.b = (mem >> 8) & 0xff;
    if (globals.other_modes.image_read_en)
    {
        *curpixel_memcvg = (mem >> 5) & 7;
        globals.pre_memory_color.a = mem & 0xe0;
    }
    else
    {
        *curpixel_memcvg = 7;
        globals.pre_memory_color.a = 0xe0;
    }
}

static void rdp_set_color_image(const uint32_t* args)
{
    globals.fb_format   = (args[0] >> 21) & 0x7;
    globals.fb_size     = (args[0] >> 19) & 0x3;
    globals.fb_width    = (args[0] & 0x3ff) + 1;
    globals.fb_address  = args[1] & 0x0ffffff;

    globals.fbread1_ptr = fbread_func[globals.fb_size];
    globals.fbread2_ptr = fbread2_func[globals.fb_size];
    globals.fbwrite_ptr = fbwrite_func[globals.fb_size];
}

static void rdp_set_fill_color(const uint32_t* args)
{
    globals.fill_color = args[1];
}

static void fb_init()
{
    globals.fb_format   = FORMAT_RGBA;
    globals.fb_size     = PIXEL_SIZE_4BIT;
    globals.fb_width    = 0;
    globals.fb_address  = 0;


    globals.fbread1_ptr = fbread_func[globals.fb_size];
    globals.fbread2_ptr = fbread2_func[globals.fb_size];
    globals.fbwrite_ptr = fbwrite_func[globals.fb_size];
}

static uint32_t sort_tmem_idx(uint32_t idxa, uint32_t idxb, uint32_t idxc, uint32_t idxd, uint32_t bankno)
{
    if ((idxa & 3) == bankno)
        return idxa & 0x3ff;
    else if ((idxb & 3) == bankno)
        return idxb & 0x3ff;
    else if ((idxc & 3) == bankno)
        return idxc & 0x3ff;
    else if ((idxd & 3) == bankno)
        return idxd & 0x3ff;
    return 0;
}

static void sort_tmem_shorts_lowhalf(uint32_t* bindshort, uint32_t short0, uint32_t short1, uint32_t short2, uint32_t short3, uint32_t bankno)
{
    switch(bankno)
    {
    case 0:
        *bindshort = short0;
        break;
    case 1:
        *bindshort = short1;
        break;
    case 2:
        *bindshort = short2;
        break;
    case 3:
        *bindshort = short3;
        break;
    }
}

static void compute_color_index(uint32_t* cidx, uint32_t readshort, uint32_t nybbleoffset, uint32_t tilenum)
{
    uint32_t lownib, hinib;
    if (globals.tile[tilenum].size == PIXEL_SIZE_4BIT)
    {
        lownib = (nybbleoffset ^ 3) << 2;
        hinib = globals.tile[tilenum].palette;
    }
    else
    {
        lownib = ((nybbleoffset & 2) ^ 2) << 2;
        hinib = lownib ? ((readshort >> 12) & 0xf) : ((readshort >> 4) & 0xf);
    }
    lownib = (readshort >> lownib) & 0xf;
    *cidx = (hinib << 4) | lownib;
}

static INLINE void fetch_texel(struct color *color, int s, int t, uint32_t tilenum)
{
   uint32_t tbase  = globals.tile[tilenum].line * (t & 0xff) + globals.tile[tilenum].tmem;
   uint32_t tpal   = globals.tile[tilenum].palette;
   uint32_t taddr  = 0;

   switch (globals.tile[tilenum].f.notlutswitch)
   {
      case TEXEL_RGBA4:
         {
            uint8_t byteval, c;
            taddr = ((tbase << 4) + s) >> 1;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            byteval = globals.tmem[taddr & 0xfff];
            c = ((s & 1)) ? (byteval & 0xf) : (byteval >> 4);
            c |= (c << 4);
            color->r = c;
            color->g = c;
            color->b = c;
            color->a = c;
         }
         break;
      case TEXEL_RGBA8:
         {
            uint8_t p;
            taddr    = (tbase << 3) + s;
            taddr   ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);
            p        = globals.tmem[taddr & 0xfff];
            color->r = p;
            color->g = p;
            color->b = p;
            color->a = p;
         }
         break;
      case TEXEL_RGBA16:
         {
            uint16_t c;
            taddr    = (tbase << 2) + s;
            taddr   ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            c        = tc16[taddr & 0x7ff];
            color->r = GET_HI_RGBA16_TMEM(c);
            color->g = GET_MED_RGBA16_TMEM(c);
            color->b = GET_LOW_RGBA16_TMEM(c);
            color->a = (c & 1) ? 0xff : 0;
         }
         break;
      case TEXEL_RGBA32:
         {
            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            uint16_t c;


            taddr &= 0x3ff;
            c = tc16[taddr];
            color->r = c >> 8;
            color->g = c & 0xff;
            c = tc16[taddr | 0x400];
            color->b = c >> 8;
            color->a = c & 0xff;
         }
         break;
      case TEXEL_YUV4:
         {
            taddr = (tbase << 3) + s;

            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            int32_t u, save;

            save = globals.tmem[taddr & 0x7ff];

            save &= 0xf0;
            save |= (save >> 4);

            u = save - 0x80;

            color->r = u;
            color->g = u;
            color->b = save;
            color->a = save;
         }
         break;
      case TEXEL_YUV8:
         {
            taddr = (tbase << 3) + s;

            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            int32_t u, save;

            save = u = globals.tmem[taddr & 0x7ff];

            u = u - 0x80;

            color->r = u;
            color->g = u;
            color->b = save;
            color->a = save;
         }
         break;
      case TEXEL_YUV16:
         {
            taddr = (tbase << 3) + s;
            int taddrlow = taddr >> 1;

            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);
            taddrlow ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            taddr &= 0x7ff;
            taddrlow &= 0x3ff;

            uint16_t c = tc16[taddrlow];

            int32_t y, u, v;
            y = globals.tmem[taddr | 0x800];
            u = c >> 8;
            v = c & 0xff;

            u = u - 0x80;
            v = v - 0x80;



            color->r = u;
            color->g = v;
            color->b = y;
            color->a = y;
         }
         break;
      case TEXEL_YUV32:
         {
            int taddrlow;
            uint16_t c;
            int32_t y, u, v;

            taddr = (tbase << 3) + s;
            taddrlow = taddr >> 1;

            taddrlow ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            taddrlow &= 0x3ff;

            c = tc16[taddrlow];

            u = c >> 8;
            v = c & 0xff;

            u = u - 0x80;
            v = v - 0x80;

            color->r = u;
            color->g = v;

            if (s & 1)
            {
               taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);
               taddr &= 0x7ff;
               y = globals.tmem[taddr | 0x800];

               color->b = y;
               color->a = y;
            }
            else
            {
               y = tc16[taddrlow | 0x400];

               color->b = y >> 8;
               color->a = ((y >> 8) & 0xf) | (y & 0xf0);
            }
         }
         break;
      case TEXEL_CI4:
         {
            taddr = ((tbase << 4) + s) >> 1;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t p;



            p = globals.tmem[taddr & 0xfff];
            p = (s & 1) ? (p & 0xf) : (p >> 4);
            p = (tpal << 4) | p;
            color->r = color->g = color->b = color->a = p;
         }
         break;
      case TEXEL_CI8:
         {
            taddr = (tbase << 3) + s;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t p;


            p = globals.tmem[taddr & 0xfff];
            color->r = p;
            color->g = p;
            color->b = p;
            color->a = p;
         }
         break;
      case TEXEL_CI16:
      case TEXEL_CI32:
         {
            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            uint16_t c;

            c = tc16[taddr & 0x7ff];
            color->r = c >> 8;
            color->g = c & 0xff;
            color->b = color->r;
            color->a = color->g;
         }
         break;
      case TEXEL_IA4:
         {
            taddr = ((tbase << 4) + s) >> 1;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t p, i;


            p = globals.tmem[taddr & 0xfff];
            p = (s & 1) ? (p & 0xf) : (p >> 4);
            i = p & 0xe;
            i = (i << 4) | (i << 1) | (i >> 2);
            color->r = i;
            color->g = i;
            color->b = i;
            color->a = (p & 0x1) ? 0xff : 0;
         }
         break;
      case TEXEL_IA8:
         {
            taddr = (tbase << 3) + s;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t p, i;


            p = globals.tmem[taddr & 0xfff];
            i = p & 0xf0;
            i |= (i >> 4);
            color->r = i;
            color->g = i;
            color->b = i;
            color->a = ((p & 0xf) << 4) | (p & 0xf);
         }
         break;
      case TEXEL_IA16:
         {


            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            uint16_t c;

            c = tc16[taddr & 0x7ff];
            color->r = color->g = color->b = (c >> 8);
            color->a = c & 0xff;
         }
         break;
      case TEXEL_IA32:
         {
            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            uint16_t c;

            c = tc16[taddr & 0x7ff];
            color->r = c >> 8;
            color->g = c & 0xff;
            color->b = color->r;
            color->a = color->g;
         }
         break;
      case TEXEL_I4:
         {
            taddr = ((tbase << 4) + s) >> 1;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t byteval, c;

            byteval = globals.tmem[taddr & 0xfff];
            c = (s & 1) ? (byteval & 0xf) : (byteval >> 4);
            c |= (c << 4);
            color->r = c;
            color->g = c;
            color->b = c;
            color->a = c;
         }
         break;
      case TEXEL_I8:
         {
            taddr = (tbase << 3) + s;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t c;

            c = globals.tmem[taddr & 0xfff];
            color->r = c;
            color->g = c;
            color->b = c;
            color->a = c;
         }
         break;
      case TEXEL_I16:
      case TEXEL_I32:
      default:
         {
            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            uint16_t c;

            c = tc16[taddr & 0x7ff];
            color->r = c >> 8;
            color->g = c & 0xff;
            color->b = color->r;
            color->a = color->g;
         }
         break;
   }
}

static INLINE void fetch_texel_quadro(struct color *color0, struct color *color1, struct color *color2, struct color *color3, int s0, int sdiff, int t0, int tdiff, uint32_t tilenum, int unequaluppers)
{
   uint32_t xort, ands;
   uint32_t taddr0, taddr1, taddr2, taddr3;
   uint32_t taddrlow0, taddrlow1, taddrlow2, taddrlow3;
   uint32_t tbase0 = globals.tile[tilenum].line * (t0 & 0xff) + globals.tile[tilenum].tmem;
   int t1 = (t0 & 0xff) + tdiff;
   int s1 = s0 + sdiff;
   uint32_t tbase2 = globals.tile[tilenum].line * t1 + globals.tile[tilenum].tmem;
   uint32_t tpal = globals.tile[tilenum].palette;

   switch (globals.tile[tilenum].f.notlutswitch)
   {
      case TEXEL_RGBA4:
         {
            taddr0 = ((tbase0 << 4) + s0) >> 1;
            taddr1 = ((tbase0 << 4) + s1) >> 1;
            taddr2 = ((tbase2 << 4) + s0) >> 1;
            taddr3 = ((tbase2 << 4) + s1) >> 1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t byteval, c;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            ands = s0 & 1;
            byteval = globals.tmem[taddr0];
            c = (ands) ? (byteval & 0xf) : (byteval >> 4);
            c |= (c << 4);
            color0->r = c;
            color0->g = c;
            color0->b = c;
            color0->a = c;
            byteval = globals.tmem[taddr2];
            c = (ands) ? (byteval & 0xf) : (byteval >> 4);
            c |= (c << 4);
            color2->r = c;
            color2->g = c;
            color2->b = c;
            color2->a = c;

            ands = s1 & 1;
            byteval = globals.tmem[taddr1];
            c = (ands) ? (byteval & 0xf) : (byteval >> 4);
            c |= (c << 4);
            color1->r = c;
            color1->g = c;
            color1->b = c;
            color1->a = c;
            byteval = globals.tmem[taddr3];
            c = (ands) ? (byteval & 0xf) : (byteval >> 4);
            c |= (c << 4);
            color3->r = c;
            color3->g = c;
            color3->b = c;
            color3->a = c;
         }
         break;
      case TEXEL_RGBA8:
         {
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            p = globals.tmem[taddr0];
            color0->r = p;
            color0->g = p;
            color0->b = p;
            color0->a = p;
            p = globals.tmem[taddr2];
            color2->r = p;
            color2->g = p;
            color2->b = p;
            color2->a = p;
            p = globals.tmem[taddr1];
            color1->r = p;
            color1->g = p;
            color1->b = p;
            color1->a = p;
            p = globals.tmem[taddr3];
            color3->r = p;
            color3->g = p;
            color3->b = p;
            color3->a = p;
         }
         break;
      case TEXEL_RGBA16:
         {
            taddr0 = (tbase0 << 2) + s0;
            taddr1 = (tbase0 << 2) + s1;
            taddr2 = (tbase2 << 2) + s0;
            taddr3 = (tbase2 << 2) + s1;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t c0, c1, c2, c3;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            c0 = tc16[taddr0];
            c1 = tc16[taddr1];
            c2 = tc16[taddr2];
            c3 = tc16[taddr3];
            color0->r = GET_HI_RGBA16_TMEM(c0);
            color0->g = GET_MED_RGBA16_TMEM(c0);
            color0->b = GET_LOW_RGBA16_TMEM(c0);
            color0->a = (c0 & 1) ? 0xff : 0;
            color1->r = GET_HI_RGBA16_TMEM(c1);
            color1->g = GET_MED_RGBA16_TMEM(c1);
            color1->b = GET_LOW_RGBA16_TMEM(c1);
            color1->a = (c1 & 1) ? 0xff : 0;
            color2->r = GET_HI_RGBA16_TMEM(c2);
            color2->g = GET_MED_RGBA16_TMEM(c2);
            color2->b = GET_LOW_RGBA16_TMEM(c2);
            color2->a = (c2 & 1) ? 0xff : 0;
            color3->r = GET_HI_RGBA16_TMEM(c3);
            color3->g = GET_MED_RGBA16_TMEM(c3);
            color3->b = GET_LOW_RGBA16_TMEM(c3);
            color3->a = (c3 & 1) ? 0xff : 0;
         }
         break;
      case TEXEL_RGBA32:
         {
            taddr0 = (tbase0 << 2) + s0;
            taddr1 = (tbase0 << 2) + s1;
            taddr2 = (tbase2 << 2) + s0;
            taddr3 = (tbase2 << 2) + s1;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint16_t c0, c1, c2, c3;

            taddr0 &= 0x3ff;
            taddr1 &= 0x3ff;
            taddr2 &= 0x3ff;
            taddr3 &= 0x3ff;
            c0 = tc16[taddr0];
            color0->r = c0 >> 8;
            color0->g = c0 & 0xff;
            c0 = tc16[taddr0 | 0x400];
            color0->b = c0 >>  8;
            color0->a = c0 & 0xff;
            c1 = tc16[taddr1];
            color1->r = c1 >> 8;
            color1->g = c1 & 0xff;
            c1 = tc16[taddr1 | 0x400];
            color1->b = c1 >>  8;
            color1->a = c1 & 0xff;
            c2 = tc16[taddr2];
            color2->r = c2 >> 8;
            color2->g = c2 & 0xff;
            c2 = tc16[taddr2 | 0x400];
            color2->b = c2 >>  8;
            color2->a = c2 & 0xff;
            c3 = tc16[taddr3];
            color3->r = c3 >> 8;
            color3->g = c3 & 0xff;
            c3 = tc16[taddr3 | 0x400];
            color3->b = c3 >>  8;
            color3->a = c3 & 0xff;
         }
         break;
      case TEXEL_YUV4:
         {
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1 + sdiff;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1 + sdiff;

            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;

            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            int32_t u0, u1, u2, u3, save0, save1, save2, save3;

            save0 = globals.tmem[taddr0 & 0x7ff];
            save0 &= 0xf0;
            save0 |= (save0 >> 4);
            u0 = save0 - 0x80;

            save1 = globals.tmem[taddr1 & 0x7ff];
            save1 &= 0xf0;
            save1 |= (save1 >> 4);
            u1 = save1 - 0x80;

            save2 = globals.tmem[taddr2 & 0x7ff];
            save2 &= 0xf0;
            save2 |= (save2 >> 4);
            u2 = save2 - 0x80;

            save3 = globals.tmem[taddr3 & 0x7ff];
            save3 &= 0xf0;
            save3 |= (save3 >> 4);
            u3 = save3 - 0x80;

            color0->r = u0;
            color0->g = u0;
            color1->r = u1;
            color1->g = u1;
            color2->r = u2;
            color2->g = u2;
            color3->r = u3;
            color3->g = u3;

            if (unequaluppers)
            {
               color0->b = color0->a = save3;
               color1->b = color1->a = save2;
               color2->b = color2->a = save1;
               color3->b = color3->a = save0;
            }
            else
            {
               color0->b = color0->a = save0;
               color1->b = color1->a = save1;
               color2->b = color2->a = save2;
               color3->b = color3->a = save3;
            }
         }
         break;
      case TEXEL_YUV8:
         {
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1 + sdiff;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1 + sdiff;

            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            int32_t u0, u1, u2, u3, save0, save1, save2, save3;

            save0 = u0 = globals.tmem[taddr0 & 0x7ff];
            u0 = u0 - 0x80;
            save1 = u1 = globals.tmem[taddr1 & 0x7ff];
            u1 = u1 - 0x80;
            save2 = u2 = globals.tmem[taddr2 & 0x7ff];
            u2 = u2 - 0x80;
            save3 = u3 = globals.tmem[taddr3 & 0x7ff];
            u3 = u3 - 0x80;

            color0->r = u0;
            color0->g = u0;
            color1->r = u1;
            color1->g = u1;
            color2->r = u2;
            color2->g = u2;
            color3->r = u3;
            color3->g = u3;

            if (unequaluppers)
            {
               color0->b = color0->a = save3;
               color1->b = color1->a = save2;
               color2->b = color2->a = save1;
               color3->b = color3->a = save0;
            }
            else
            {
               color0->b = color0->a = save0;
               color1->b = color1->a = save1;
               color2->b = color2->a = save2;
               color3->b = color3->a = save3;
            }
         }
         break;
      case TEXEL_YUV16:
         {
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;


            taddrlow0 = (taddr0) >> 1;
            taddrlow1 = (taddr1 + sdiff) >> 1;
            taddrlow2 = (taddr2) >> 1;
            taddrlow3 = (taddr3 + sdiff) >> 1;

            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddrlow0 ^= xort;
            taddrlow1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddrlow2 ^= xort;
            taddrlow3 ^= xort;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            taddrlow0 &= 0x3ff;
            taddrlow1 &= 0x3ff;
            taddrlow2 &= 0x3ff;
            taddrlow3 &= 0x3ff;

            uint16_t c0, c1, c2, c3;
            int32_t y0, y1, y2, y3, u0, u1, u2, u3, v0, v1, v2, v3;

            c0 = tc16[taddrlow0];
            c1 = tc16[taddrlow1];
            c2 = tc16[taddrlow2];
            c3 = tc16[taddrlow3];

            y0 = globals.tmem[taddr0 | 0x800];
            u0 = c0 >> 8;
            v0 = c0 & 0xff;
            y1 = globals.tmem[taddr1 | 0x800];
            u1 = c1 >> 8;
            v1 = c1 & 0xff;
            y2 = globals.tmem[taddr2 | 0x800];
            u2 = c2 >> 8;
            v2 = c2 & 0xff;
            y3 = globals.tmem[taddr3 | 0x800];
            u3 = c3 >> 8;
            v3 = c3 & 0xff;

            u0 = u0 - 0x80;
            v0 = v0 - 0x80;
            u1 = u1 - 0x80;
            v1 = v1 - 0x80;
            u2 = u2 - 0x80;
            v2 = v2 - 0x80;
            u3 = u3 - 0x80;
            v3 = v3 - 0x80;

            color0->r = u0;
            color0->g = v0;
            color1->r = u1;
            color1->g = v1;
            color2->r = u2;
            color2->g = v2;
            color3->r = u3;
            color3->g = v3;

            color0->b = color0->a = y0;
            color1->b = color1->a = y1;
            color2->b = color2->a = y2;
            color3->b = color3->a = y3;
         }
         break;
      case TEXEL_YUV32:
         {
            uint16_t c0, c1, c2, c3;
            int32_t y0, y1, y2, y3, u0, u1, u2, u3, v0, v1, v2, v3;
            uint32_t xort0, xort1;

            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;

            taddrlow0 = (taddr0) >> 1;
            taddrlow1 = (taddr1 + sdiff) >> 1;
            taddrlow2 = (taddr2) >> 1;
            taddrlow3 = (taddr3 + sdiff) >> 1;

            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddrlow0 ^= xort;
            taddrlow1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddrlow2 ^= xort;
            taddrlow3 ^= xort;

            taddrlow0 &= 0x3ff;
            taddrlow1 &= 0x3ff;
            taddrlow2 &= 0x3ff;
            taddrlow3 &= 0x3ff;

            c0 = tc16[taddrlow0];
            c1 = tc16[taddrlow1];
            c2 = tc16[taddrlow2];
            c3 = tc16[taddrlow3];

            u0 = c0 >> 8;
            v0 = c0 & 0xff;
            u1 = c1 >> 8;
            v1 = c1 & 0xff;
            u2 = c2 >> 8;
            v2 = c2 & 0xff;
            u3 = c3 >> 8;
            v3 = c3 & 0xff;

            u0 = u0 - 0x80;
            v0 = v0 - 0x80;
            u1 = u1 - 0x80;
            v1 = v1 - 0x80;
            u2 = u2 - 0x80;
            v2 = v2 - 0x80;
            u3 = u3 - 0x80;
            v3 = v3 - 0x80;

            color0->r = u0;
            color0->g = v0;
            color1->r = u1;
            color1->g = v1;
            color2->r = u2;
            color2->g = v2;
            color3->r = u3;
            color3->g = v3;

            xort0 = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            xort1 = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;

            if (s0 & 1)
            {
               taddr0 ^= xort0;
               taddr2 ^= xort1;

               taddr0 &= 0x7ff;
               taddr2 &= 0x7ff;

               y0 = globals.tmem[taddr0 | 0x800];
               y2 = globals.tmem[taddr2 | 0x800];

               color0->b = color0->a = y0;
               color2->b = color2->a = y2;
            }
            else
            {
               y0 = tc16[taddrlow0 | 0x400];
               y2 = tc16[taddrlow2 | 0x400];

               color0->b = y0 >> 8;
               color0->a = ((y0 >> 8) & 0xf) | (y0 & 0xf0);
               color2->b = y2 >> 8;
               color2->a = ((y2 >> 8) & 0xf) | (y2 & 0xf0);
            }

            if (s1 & 1)
            {
               taddr1 ^= xort0;
               taddr3 ^= xort1;

               taddr1 &= 0x7ff;
               taddr3 &= 0x7ff;

               y1 = globals.tmem[taddr1 | 0x800];
               y3 = globals.tmem[taddr3 | 0x800];

               color1->b = color1->a = y1;
               color3->b = color3->a = y3;
            }
            else
            {
               taddr1 ^= xort0;
               taddr3 ^= xort1;

               taddr1 = (taddr1 >> 1) & 0x3ff;
               taddr3 = (taddr3 >> 1) & 0x3ff;

               y1 = tc16[taddr1 | 0x400];
               y3 = tc16[taddr3 | 0x400];

               color1->b = y1 >> 8;
               color1->a = ((y1 >> 8) & 0xf) | (y1 & 0xf0);
               color3->b = y3 >> 8;
               color3->a = ((y3 >> 8) & 0xf) | (y3 & 0xf0);
            }
         }
         break;
      case TEXEL_CI4:
         {
            taddr0 = ((tbase0 << 4) + s0) >> 1;
            taddr1 = ((tbase0 << 4) + s1) >> 1;
            taddr2 = ((tbase2 << 4) + s0) >> 1;
            taddr3 = ((tbase2 << 4) + s1) >> 1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            ands = s0 & 1;
            p = globals.tmem[taddr0];
            p = (ands) ? (p & 0xf) : (p >> 4);
            p = (tpal << 4) | p;
            color0->r = color0->g = color0->b = color0->a = p;
            p = globals.tmem[taddr2];
            p = (ands) ? (p & 0xf) : (p >> 4);
            p = (tpal << 4) | p;
            color2->r = color2->g = color2->b = color2->a = p;

            ands = s1 & 1;
            p = globals.tmem[taddr1];
            p = (ands) ? (p & 0xf) : (p >> 4);
            p = (tpal << 4) | p;
            color1->r = color1->g = color1->b = color1->a = p;
            p = globals.tmem[taddr3];
            p = (ands) ? (p & 0xf) : (p >> 4);
            p = (tpal << 4) | p;
            color3->r = color3->g = color3->b = color3->a = p;
         }
         break;
      case TEXEL_CI8:
         {
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            p = globals.tmem[taddr0];
            color0->r = p;
            color0->g = p;
            color0->b = p;
            color0->a = p;
            p = globals.tmem[taddr2];
            color2->r = p;
            color2->g = p;
            color2->b = p;
            color2->a = p;
            p = globals.tmem[taddr1];
            color1->r = p;
            color1->g = p;
            color1->b = p;
            color1->a = p;
            p = globals.tmem[taddr3];
            color3->r = p;
            color3->g = p;
            color3->b = p;
            color3->a = p;
         }
         break;
      case TEXEL_CI16:
      case TEXEL_CI32:
         {
            taddr0 = (tbase0 << 2) + s0;
            taddr1 = (tbase0 << 2) + s1;
            taddr2 = (tbase2 << 2) + s0;
            taddr3 = (tbase2 << 2) + s1;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint16_t c0, c1, c2, c3;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            c0 = tc16[taddr0];
            color0->r = c0 >> 8;
            color0->g = c0 & 0xff;
            color0->b = c0 >> 8;
            color0->a = c0 & 0xff;
            c1 = tc16[taddr1];
            color1->r = c1 >> 8;
            color1->g = c1 & 0xff;
            color1->b = c1 >> 8;
            color1->a = c1 & 0xff;
            c2 = tc16[taddr2];
            color2->r = c2 >> 8;
            color2->g = c2 & 0xff;
            color2->b = c2 >> 8;
            color2->a = c2 & 0xff;
            c3 = tc16[taddr3];
            color3->r = c3 >> 8;
            color3->g = c3 & 0xff;
            color3->b = c3 >> 8;
            color3->a = c3 & 0xff;
         }
         break;
      case TEXEL_IA4:
         {
            taddr0 = ((tbase0 << 4) + s0) >> 1;
            taddr1 = ((tbase0 << 4) + s1) >> 1;
            taddr2 = ((tbase2 << 4) + s0) >> 1;
            taddr3 = ((tbase2 << 4) + s1) >> 1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p, i;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            ands = s0 & 1;
            p = globals.tmem[taddr0];
            p = ands ? (p & 0xf) : (p >> 4);
            i = p & 0xe;
            i = (i << 4) | (i << 1) | (i >> 2);
            color0->r = i;
            color0->g = i;
            color0->b = i;
            color0->a = (p & 0x1) ? 0xff : 0;
            p = globals.tmem[taddr2];
            p = ands ? (p & 0xf) : (p >> 4);
            i = p & 0xe;
            i = (i << 4) | (i << 1) | (i >> 2);
            color2->r = i;
            color2->g = i;
            color2->b = i;
            color2->a = (p & 0x1) ? 0xff : 0;

            ands = s1 & 1;
            p = globals.tmem[taddr1];
            p = ands ? (p & 0xf) : (p >> 4);
            i = p & 0xe;
            i = (i << 4) | (i << 1) | (i >> 2);
            color1->r = i;
            color1->g = i;
            color1->b = i;
            color1->a = (p & 0x1) ? 0xff : 0;
            p = globals.tmem[taddr3];
            p = ands ? (p & 0xf) : (p >> 4);
            i = p & 0xe;
            i = (i << 4) | (i << 1) | (i >> 2);
            color3->r = i;
            color3->g = i;
            color3->b = i;
            color3->a = (p & 0x1) ? 0xff : 0;
         }
         break;
      case TEXEL_IA8:
         {
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p, i;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            p = globals.tmem[taddr0];
            i = p & 0xf0;
            i |= (i >> 4);
            color0->r = i;
            color0->g = i;
            color0->b = i;
            color0->a = ((p & 0xf) << 4) | (p & 0xf);
            p = globals.tmem[taddr1];
            i = p & 0xf0;
            i |= (i >> 4);
            color1->r = i;
            color1->g = i;
            color1->b = i;
            color1->a = ((p & 0xf) << 4) | (p & 0xf);
            p = globals.tmem[taddr2];
            i = p & 0xf0;
            i |= (i >> 4);
            color2->r = i;
            color2->g = i;
            color2->b = i;
            color2->a = ((p & 0xf) << 4) | (p & 0xf);
            p = globals.tmem[taddr3];
            i = p & 0xf0;
            i |= (i >> 4);
            color3->r = i;
            color3->g = i;
            color3->b = i;
            color3->a = ((p & 0xf) << 4) | (p & 0xf);
         }
         break;
      case TEXEL_IA16:
         {
            taddr0 = (tbase0 << 2) + s0;
            taddr1 = (tbase0 << 2) + s1;
            taddr2 = (tbase2 << 2) + s0;
            taddr3 = (tbase2 << 2) + s1;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint16_t c0, c1, c2, c3;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            c0 = tc16[taddr0];
            color0->r = color0->g = color0->b = c0 >> 8;
            color0->a = c0 & 0xff;
            c1 = tc16[taddr1];
            color1->r = color1->g = color1->b = c1 >> 8;
            color1->a = c1 & 0xff;
            c2 = tc16[taddr2];
            color2->r = color2->g = color2->b = c2 >> 8;
            color2->a = c2 & 0xff;
            c3 = tc16[taddr3];
            color3->r = color3->g = color3->b = c3 >> 8;
            color3->a = c3 & 0xff;

         }
         break;
      case TEXEL_IA32:
         {
            taddr0 = (tbase0 << 2) + s0;
            taddr1 = (tbase0 << 2) + s1;
            taddr2 = (tbase2 << 2) + s0;
            taddr3 = (tbase2 << 2) + s1;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint16_t c0, c1, c2, c3;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            c0 = tc16[taddr0];
            color0->r = c0 >> 8;
            color0->g = c0 & 0xff;
            color0->b = c0 >> 8;
            color0->a = c0 & 0xff;
            c1 = tc16[taddr1];
            color1->r = c1 >> 8;
            color1->g = c1 & 0xff;
            color1->b = c1 >> 8;
            color1->a = c1 & 0xff;
            c2 = tc16[taddr2];
            color2->r = c2 >> 8;
            color2->g = c2 & 0xff;
            color2->b = c2 >> 8;
            color2->a = c2 & 0xff;
            c3 = tc16[taddr3];
            color3->r = c3 >> 8;
            color3->g = c3 & 0xff;
            color3->b = c3 >> 8;
            color3->a = c3 & 0xff;

         }
         break;
      case TEXEL_I4:
         {
            taddr0 = ((tbase0 << 4) + s0) >> 1;
            taddr1 = ((tbase0 << 4) + s1) >> 1;
            taddr2 = ((tbase2 << 4) + s0) >> 1;
            taddr3 = ((tbase2 << 4) + s1) >> 1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p, c0, c1, c2, c3;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            ands = s0 & 1;
            p = globals.tmem[taddr0];
            c0 = ands ? (p & 0xf) : (p >> 4);
            c0 |= (c0 << 4);
            color0->r = color0->g = color0->b = color0->a = c0;
            p = globals.tmem[taddr2];
            c2 = ands ? (p & 0xf) : (p >> 4);
            c2 |= (c2 << 4);
            color2->r = color2->g = color2->b = color2->a = c2;

            ands = s1 & 1;
            p = globals.tmem[taddr1];
            c1 = ands ? (p & 0xf) : (p >> 4);
            c1 |= (c1 << 4);
            color1->r = color1->g = color1->b = color1->a = c1;
            p = globals.tmem[taddr3];
            c3 = ands ? (p & 0xf) : (p >> 4);
            c3 |= (c3 << 4);
            color3->r = color3->g = color3->b = color3->a = c3;

         }
         break;
      case TEXEL_I8:
         {
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;

            p = globals.tmem[taddr0];
            color0->r = p;
            color0->g = p;
            color0->b = p;
            color0->a = p;
            p = globals.tmem[taddr1];
            color1->r = p;
            color1->g = p;
            color1->b = p;
            color1->a = p;
            p = globals.tmem[taddr2];
            color2->r = p;
            color2->g = p;
            color2->b = p;
            color2->a = p;
            p = globals.tmem[taddr3];
            color3->r = p;
            color3->g = p;
            color3->b = p;
            color3->a = p;
         }
         break;
      case TEXEL_I16:
      case TEXEL_I32:
      default:
         {
            taddr0 = (tbase0 << 2) + s0;
            taddr1 = (tbase0 << 2) + s1;
            taddr2 = (tbase2 << 2) + s0;
            taddr3 = (tbase2 << 2) + s1;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint16_t c0, c1, c2, c3;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            c0 = tc16[taddr0];
            color0->r = c0 >> 8;
            color0->g = c0 & 0xff;
            color0->b = c0 >> 8;
            color0->a = c0 & 0xff;
            c1 = tc16[taddr1];
            color1->r = c1 >> 8;
            color1->g = c1 & 0xff;
            color1->b = c1 >> 8;
            color1->a = c1 & 0xff;
            c2 = tc16[taddr2];
            color2->r = c2 >> 8;
            color2->g = c2 & 0xff;
            color2->b = c2 >> 8;
            color2->a = c2 & 0xff;
            c3 = tc16[taddr3];
            color3->r = c3 >> 8;
            color3->g = c3 & 0xff;
            color3->b = c3 >> 8;
            color3->a = c3 & 0xff;
         }
         break;
   }
}

static INLINE void fetch_texel_entlut_quadro(struct color *color0, struct color *color1, struct color *color2, struct color *color3, int s0, int sdiff, int t0, int tdiff, uint32_t tilenum, int isupper, int isupperrg)
{
   uint32_t tbase0 = globals.tile[tilenum].line * (t0 & 0xff) + globals.tile[tilenum].tmem;
   int t1 = (t0 & 0xff) + tdiff;
   int s1;

   uint32_t tbase2 = globals.tile[tilenum].line * t1 + globals.tile[tilenum].tmem;
   uint32_t tpal = globals.tile[tilenum].palette << 4;
   uint32_t xort, ands;

   uint32_t taddr0, taddr1, taddr2, taddr3;
   uint16_t c0, c1, c2, c3;




   uint32_t xorupperrg = isupperrg ? (WORD_ADDR_XOR ^ 3) : WORD_ADDR_XOR;



   switch(globals.tile[tilenum].f.tlutswitch)
   {
      case 0:
      case 1:
      case 2:
         {
            s1 = s0 + sdiff;
            taddr0 = ((tbase0 << 4) + s0) >> 1;
            taddr1 = ((tbase0 << 4) + s1) >> 1;
            taddr2 = ((tbase2 << 4) + s0) >> 1;
            taddr3 = ((tbase2 << 4) + s1) >> 1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            ands = s0 & 1;
            c0 = globals.tmem[taddr0 & 0x7ff];
            c0 = (ands) ? (c0 & 0xf) : (c0 >> 4);
            taddr0 = (tpal | c0) << 2;
            c2 = globals.tmem[taddr2 & 0x7ff];
            c2 = (ands) ? (c2 & 0xf) : (c2 >> 4);
            taddr2 = ((tpal | c2) << 2) + 2;

            ands = s1 & 1;
            c1 = globals.tmem[taddr1 & 0x7ff];
            c1 = (ands) ? (c1 & 0xf) : (c1 >> 4);
            taddr1 = ((tpal | c1) << 2) + 1;
            c3 = globals.tmem[taddr3 & 0x7ff];
            c3 = (ands) ? (c3 & 0xf) : (c3 >> 4);
            taddr3 = ((tpal | c3) << 2) + 3;
         }
         break;
      case 3:
         {
            s1 = s0 + (sdiff << 1);
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;

            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            c0 = globals.tmem[taddr0 & 0x7ff];
            c0 >>= 4;
            taddr0 = (tpal | c0) << 2;
            c2 = globals.tmem[taddr2 & 0x7ff];
            c2 >>= 4;
            taddr2 = ((tpal | c2) << 2) + 2;

            c1 = globals.tmem[taddr1 & 0x7ff];
            c1 >>= 4;
            taddr1 = ((tpal | c1) << 2) + 1;
            c3 = globals.tmem[taddr3 & 0x7ff];
            c3 >>= 4;
            taddr3 = ((tpal | c3) << 2) + 3;
         }
         break;
      case 4:
      case 5:
      case 6:
         {
            s1 = s0 + sdiff;
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;

            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            c0 = globals.tmem[taddr0 & 0x7ff];
            taddr0 = c0 << 2;
            c2 = globals.tmem[taddr2 & 0x7ff];
            taddr2 = (c2 << 2) + 2;
            c1 = globals.tmem[taddr1 & 0x7ff];
            taddr1 = (c1 << 2) + 1;
            c3 = globals.tmem[taddr3 & 0x7ff];
            taddr3 = (c3 << 2) + 3;
         }
         break;
      case 7:
         {
            s1 = s0 + (sdiff << 1);
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;

            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            c0 = globals.tmem[taddr0 & 0x7ff];
            taddr0 = c0 << 2;
            c2 = globals.tmem[taddr2 & 0x7ff];
            taddr2 = (c2 << 2) + 2;
            c1 = globals.tmem[taddr1 & 0x7ff];
            taddr1 = (c1 << 2) + 1;
            c3 = globals.tmem[taddr3 & 0x7ff];
            taddr3 = (c3 << 2) + 3;
         }
         break;
      case 8:
      case 9:
      case 10:
         {
            s1 = s0 + sdiff;
            taddr0 = (tbase0 << 2) + s0;
            taddr1 = (tbase0 << 2) + s1;
            taddr2 = (tbase2 << 2) + s0;
            taddr3 = (tbase2 << 2) + s1;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            c0 = tc16[taddr0 & 0x3ff];
            taddr0 = (c0 >> 6) & ~3;
            c1 = tc16[taddr1 & 0x3ff];
            taddr1 = ((c1 >> 6) & ~3) + 1;
            c2 = tc16[taddr2 & 0x3ff];
            taddr2 = ((c2 >> 6) & ~3) + 2;
            c3 = tc16[taddr3 & 0x3ff];
            taddr3 = (c3 >> 6) | 3;
         }
         break;
      case 11:
         {
            s1 = s0 + (sdiff << 1);
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;

            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            c0 = globals.tmem[taddr0 & 0x7ff];
            taddr0 = c0 << 2;
            c2 = globals.tmem[taddr2 & 0x7ff];
            taddr2 = (c2 << 2) + 2;
            c1 = globals.tmem[taddr1 & 0x7ff];
            taddr1 = (c1 << 2) + 1;
            c3 = globals.tmem[taddr3 & 0x7ff];
            taddr3 = (c3 << 2) + 3;
         }
         break;
      case 12:
      case 13:
      case 14:
         {
            s1 = s0 + sdiff;
            taddr0 = (tbase0 << 2) + s0;
            taddr1 = (tbase0 << 2) + s1;
            taddr2 = (tbase2 << 2) + s0;
            taddr3 = (tbase2 << 2) + s1;

            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            c0 = tc16[taddr0 & 0x3ff];
            taddr0 = (c0 >> 6) & ~3;
            c1 = tc16[taddr1 & 0x3ff];
            taddr1 = ((c1 >> 6) & ~3) + 1;
            c2 = tc16[taddr2 & 0x3ff];
            taddr2 = ((c2 >> 6) & ~3) + 2;
            c3 = tc16[taddr3 & 0x3ff];
            taddr3 = (c3 >> 6) | 3;
         }
         break;
      case 15:
      default:
         {
            s1 = s0 + (sdiff << 1);
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;

            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            c0 = globals.tmem[taddr0 & 0x7ff];
            taddr0 = c0 << 2;
            c2 = globals.tmem[taddr2 & 0x7ff];
            taddr2 = (c2 << 2) + 2;
            c1 = globals.tmem[taddr1 & 0x7ff];
            taddr1 = (c1 << 2) + 1;
            c3 = globals.tmem[taddr3 & 0x7ff];
            taddr3 = (c3 << 2) + 3;
         }
         break;
   }

   c0 = tlut[taddr0 ^ xorupperrg];
   c2 = tlut[taddr2 ^ xorupperrg];
   c1 = tlut[taddr1 ^ xorupperrg];
   c3 = tlut[taddr3 ^ xorupperrg];

   if (!globals.other_modes.tlut_type)
   {
      color0->r = GET_HI_RGBA16_TMEM(c0);
      color0->g = GET_MED_RGBA16_TMEM(c0);
      color1->r = GET_HI_RGBA16_TMEM(c1);
      color1->g = GET_MED_RGBA16_TMEM(c1);
      color2->r = GET_HI_RGBA16_TMEM(c2);
      color2->g = GET_MED_RGBA16_TMEM(c2);
      color3->r = GET_HI_RGBA16_TMEM(c3);
      color3->g = GET_MED_RGBA16_TMEM(c3);

      if (isupper == isupperrg)
      {
         color0->b = GET_LOW_RGBA16_TMEM(c0);
         color0->a = (c0 & 1) ? 0xff : 0;
         color1->b = GET_LOW_RGBA16_TMEM(c1);
         color1->a = (c1 & 1) ? 0xff : 0;
         color2->b = GET_LOW_RGBA16_TMEM(c2);
         color2->a = (c2 & 1) ? 0xff : 0;
         color3->b = GET_LOW_RGBA16_TMEM(c3);
         color3->a = (c3 & 1) ? 0xff : 0;
      }
      else
      {
         color0->b = GET_LOW_RGBA16_TMEM(c3);
         color0->a = (c3 & 1) ? 0xff : 0;
         color1->b = GET_LOW_RGBA16_TMEM(c2);
         color1->a = (c2 & 1) ? 0xff : 0;
         color2->b = GET_LOW_RGBA16_TMEM(c1);
         color2->a = (c1 & 1) ? 0xff : 0;
         color3->b = GET_LOW_RGBA16_TMEM(c0);
         color3->a = (c0 & 1) ? 0xff : 0;
      }
   }
   else
   {
      color0->r = color0->g = c0 >> 8;
      color1->r = color1->g = c1 >> 8;
      color2->r = color2->g = c2 >> 8;
      color3->r = color3->g = c3 >> 8;

      if (isupper == isupperrg)
      {
         color0->b = c0 >> 8;
         color0->a = c0 & 0xff;
         color1->b = c1 >> 8;
         color1->a = c1 & 0xff;
         color2->b = c2 >> 8;
         color2->a = c2 & 0xff;
         color3->b = c3 >> 8;
         color3->a = c3 & 0xff;
      }
      else
      {
         color0->b = c3 >> 8;
         color0->a = c3 & 0xff;
         color1->b = c2 >> 8;
         color1->a = c2 & 0xff;
         color2->b = c1 >> 8;
         color2->a = c1 & 0xff;
         color3->b = c0 >> 8;
         color3->a = c0 & 0xff;
      }
   }
}

static INLINE void fetch_texel_entlut_quadro_nearest(struct color *color0, struct color *color1, struct color *color2, struct color *color3, int s0, int t0, uint32_t tilenum, int isupper, int isupperrg)
{
   uint32_t tbase0 = globals.tile[tilenum].line * t0 + globals.tile[tilenum].tmem;
   uint32_t tpal = globals.tile[tilenum].palette << 4;
   uint32_t xort, ands;

   uint32_t taddr0 = 0;
   uint16_t c0, c1, c2, c3;

   uint32_t xorupperrg = isupperrg ? (WORD_ADDR_XOR ^ 3) : WORD_ADDR_XOR;

   switch(globals.tile[tilenum].f.tlutswitch)
   {
      case 0:
      case 1:
      case 2:
         {
            taddr0 = ((tbase0 << 4) + s0) >> 1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;

            ands = s0 & 1;
            c0 = globals.tmem[taddr0 & 0x7ff];
            c0 = (ands) ? (c0 & 0xf) : (c0 >> 4);

            taddr0 = (tpal | c0) << 2;
         }
         break;
      case 3:
         {
            taddr0 = (tbase0 << 3) + s0;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;

            c0 = globals.tmem[taddr0 & 0x7ff];

            c0 >>= 4;

            taddr0 = (tpal | c0) << 2;
         }
         break;
      case 4:
      case 5:
      case 6:
         {
            taddr0 = (tbase0 << 3) + s0;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;

            c0 = globals.tmem[taddr0 & 0x7ff];

            taddr0 = c0 << 2;
         }
         break;
      case 7:
         {
            taddr0 = (tbase0 << 3) + s0;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;

            c0 = globals.tmem[taddr0 & 0x7ff];

            taddr0 = c0 << 2;
         }
         break;
      case 8:
      case 9:
      case 10:
         {
            taddr0 = (tbase0 << 2) + s0;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;

            c0 = tc16[taddr0 & 0x3ff];

            taddr0 = (c0 >> 6) & ~3;
         }
         break;
      case 11:
         {
            taddr0 = (tbase0 << 3) + s0;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;

            c0 = globals.tmem[taddr0 & 0x7ff];

            taddr0 = c0 << 2;
         }
         break;
      case 12:
      case 13:
      case 14:
         {
            taddr0 = (tbase0 << 2) + s0;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;

            c0 = tc16[taddr0 & 0x3ff];

            taddr0 = (c0 >> 6) & ~3;
         }
         break;
      case 15:
      default:
         {
            taddr0 = (tbase0 << 3) + s0;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;

            c0 = globals.tmem[taddr0 & 0x7ff];

            taddr0 = c0 << 2;
         }
         break;
   }

   c0 = tlut[taddr0 ^ xorupperrg];
   c1 = tlut[(taddr0 + 1) ^ xorupperrg];
   c2 = tlut[(taddr0 + 2) ^ xorupperrg];
   c3 = tlut[(taddr0 + 3) ^ xorupperrg];

   if (!globals.other_modes.tlut_type)
   {
      color0->r = GET_HI_RGBA16_TMEM(c0);
      color0->g = GET_MED_RGBA16_TMEM(c0);
      color1->r = GET_HI_RGBA16_TMEM(c1);
      color1->g = GET_MED_RGBA16_TMEM(c1);
      color2->r = GET_HI_RGBA16_TMEM(c2);
      color2->g = GET_MED_RGBA16_TMEM(c2);
      color3->r = GET_HI_RGBA16_TMEM(c3);
      color3->g = GET_MED_RGBA16_TMEM(c3);

      if (isupper == isupperrg)
      {
         color0->b = GET_LOW_RGBA16_TMEM(c0);
         color0->a = (c0 & 1) ? 0xff : 0;
         color1->b = GET_LOW_RGBA16_TMEM(c1);
         color1->a = (c1 & 1) ? 0xff : 0;
         color2->b = GET_LOW_RGBA16_TMEM(c2);
         color2->a = (c2 & 1) ? 0xff : 0;
         color3->b = GET_LOW_RGBA16_TMEM(c3);
         color3->a = (c3 & 1) ? 0xff : 0;
      }
      else
      {
         color0->b = GET_LOW_RGBA16_TMEM(c3);
         color0->a = (c3 & 1) ? 0xff : 0;
         color1->b = GET_LOW_RGBA16_TMEM(c2);
         color1->a = (c2 & 1) ? 0xff : 0;
         color2->b = GET_LOW_RGBA16_TMEM(c1);
         color2->a = (c1 & 1) ? 0xff : 0;
         color3->b = GET_LOW_RGBA16_TMEM(c0);
         color3->a = (c0 & 1) ? 0xff : 0;
      }
   }
   else
   {
      color0->r = color0->g = c0 >> 8;
      color1->r = color1->g = c1 >> 8;
      color2->r = color2->g = c2 >> 8;
      color3->r = color3->g = c3 >> 8;

      if (isupper == isupperrg)
      {
         color0->b = c0 >> 8;
         color0->a = c0 & 0xff;
         color1->b = c1 >> 8;
         color1->a = c1 & 0xff;
         color2->b = c2 >> 8;
         color2->a = c2 & 0xff;
         color3->b = c3 >> 8;
         color3->a = c3 & 0xff;
      }
      else
      {
         color0->b = c3 >> 8;
         color0->a = c3 & 0xff;
         color1->b = c2 >> 8;
         color1->a = c2 & 0xff;
         color2->b = c1 >> 8;
         color2->a = c1 & 0xff;
         color3->b = c0 >> 8;
         color3->a = c0 & 0xff;
      }
   }
}

static void get_tmem_idx(int s, int t, uint32_t tilenum, uint32_t* idx0, uint32_t* idx1, uint32_t* idx2, uint32_t* idx3, uint32_t* bit3flipped, uint32_t* hibit)
{
   uint32_t tbase = (globals.tile[tilenum].line * t) & 0x1ff;
   tbase += globals.tile[tilenum].tmem;
   uint32_t tsize = globals.tile[tilenum].size;
   uint32_t tformat = globals.tile[tilenum].format;
   uint32_t sshorts = 0;


   if (tsize == PIXEL_SIZE_8BIT || tformat == FORMAT_YUV)
      sshorts = s >> 1;
   else if (tsize >= PIXEL_SIZE_16BIT)
      sshorts = s;
   else
      sshorts = s >> 2;
   sshorts &= 0x7ff;

   *bit3flipped = ((sshorts & 2) != 0) ^ (t & 1);

   int tidx_a = ((tbase << 2) + sshorts) & 0x7fd;
   int tidx_b = (tidx_a + 1) & 0x7ff;
   int tidx_c = (tidx_a + 2) & 0x7ff;
   int tidx_d = (tidx_a + 3) & 0x7ff;

   *hibit = (tidx_a & 0x400) != 0;

   if (t & 1)
   {
      tidx_a ^= 2;
      tidx_b ^= 2;
      tidx_c ^= 2;
      tidx_d ^= 2;
   }


   *idx0 = sort_tmem_idx(tidx_a, tidx_b, tidx_c, tidx_d, 0);
   *idx1 = sort_tmem_idx(tidx_a, tidx_b, tidx_c, tidx_d, 1);
   *idx2 = sort_tmem_idx(tidx_a, tidx_b, tidx_c, tidx_d, 2);
   *idx3 = sort_tmem_idx(tidx_a, tidx_b, tidx_c, tidx_d, 3);
}

static void read_tmem_copy(int s, int s1, int s2, int s3, int t, uint32_t tilenum, uint32_t* sortshort, int* hibits, int* lowbits)
{
   uint32_t tbase = (globals.tile[tilenum].line * t) & 0x1ff;
   tbase += globals.tile[tilenum].tmem;
   uint32_t tsize = globals.tile[tilenum].size;
   uint32_t tformat = globals.tile[tilenum].format;
   uint32_t shbytes = 0, shbytes1 = 0, shbytes2 = 0, shbytes3 = 0;
   int32_t delta = 0;
   uint32_t sortidx[8];


   if (tsize == PIXEL_SIZE_8BIT || tformat == FORMAT_YUV)
   {
      shbytes = s << 1;
      shbytes1 = s1 << 1;
      shbytes2 = s2 << 1;
      shbytes3 = s3 << 1;
   }
   else if (tsize >= PIXEL_SIZE_16BIT)
   {
      shbytes = s << 2;
      shbytes1 = s1 << 2;
      shbytes2 = s2 << 2;
      shbytes3 = s3 << 2;
   }
   else
   {
      shbytes = s;
      shbytes1 = s1;
      shbytes2 = s2;
      shbytes3 = s3;
   }

   shbytes &= 0x1fff;
   shbytes1 &= 0x1fff;
   shbytes2 &= 0x1fff;
   shbytes3 &= 0x1fff;

   int tidx_a, tidx_blow, tidx_bhi, tidx_c, tidx_dlow, tidx_dhi;

   tbase <<= 4;
   tidx_a = (tbase + shbytes) & 0x1fff;
   tidx_bhi = (tbase + shbytes1) & 0x1fff;
   tidx_c = (tbase + shbytes2) & 0x1fff;
   tidx_dhi = (tbase + shbytes3) & 0x1fff;

   if (tformat == FORMAT_YUV)
   {
      delta = shbytes1 - shbytes;
      tidx_blow = (tidx_a + (delta << 1)) & 0x1fff;
      tidx_dlow = (tidx_blow + shbytes3 - shbytes) & 0x1fff;
   }
   else
   {
      tidx_blow = tidx_bhi;
      tidx_dlow = tidx_dhi;
   }

   if (t & 1)
   {
      tidx_a ^= 8;
      tidx_blow ^= 8;
      tidx_bhi ^= 8;
      tidx_c ^= 8;
      tidx_dlow ^= 8;
      tidx_dhi ^= 8;
   }

   hibits[0] = (tidx_a & 0x1000) != 0;
   hibits[1] = (tidx_blow & 0x1000) != 0;
   hibits[2] = (tidx_bhi & 0x1000) != 0;
   hibits[3] = (tidx_c & 0x1000) != 0;
   hibits[4] = (tidx_dlow & 0x1000) != 0;
   hibits[5] = (tidx_dhi & 0x1000) != 0;
   lowbits[0] = tidx_a & 0xf;
   lowbits[1] = tidx_blow & 0xf;
   lowbits[2] = tidx_bhi & 0xf;
   lowbits[3] = tidx_c & 0xf;
   lowbits[4] = tidx_dlow & 0xf;
   lowbits[5] = tidx_dhi & 0xf;

   uint32_t short0, short1, short2, short3;


   tidx_a >>= 2;
   tidx_blow >>= 2;
   tidx_bhi >>= 2;
   tidx_c >>= 2;
   tidx_dlow >>= 2;
   tidx_dhi >>= 2;


   sortidx[0] = sort_tmem_idx(tidx_a, tidx_blow, tidx_c, tidx_dlow, 0);
   sortidx[1] = sort_tmem_idx(tidx_a, tidx_blow, tidx_c, tidx_dlow, 1);
   sortidx[2] = sort_tmem_idx(tidx_a, tidx_blow, tidx_c, tidx_dlow, 2);
   sortidx[3] = sort_tmem_idx(tidx_a, tidx_blow, tidx_c, tidx_dlow, 3);

   short0 = tmem16[sortidx[0] ^ WORD_ADDR_XOR];
   short1 = tmem16[sortidx[1] ^ WORD_ADDR_XOR];
   short2 = tmem16[sortidx[2] ^ WORD_ADDR_XOR];
   short3 = tmem16[sortidx[3] ^ WORD_ADDR_XOR];


   sort_tmem_shorts_lowhalf(&sortshort[0], short0, short1, short2, short3, lowbits[0] >> 2);
   sort_tmem_shorts_lowhalf(&sortshort[1], short0, short1, short2, short3, lowbits[1] >> 2);
   sort_tmem_shorts_lowhalf(&sortshort[2], short0, short1, short2, short3, lowbits[3] >> 2);
   sort_tmem_shorts_lowhalf(&sortshort[3], short0, short1, short2, short3, lowbits[4] >> 2);

   if (globals.other_modes.en_tlut)
   {

      compute_color_index(&short0, sortshort[0], lowbits[0] & 3, tilenum);
      compute_color_index(&short1, sortshort[1], lowbits[1] & 3, tilenum);
      compute_color_index(&short2, sortshort[2], lowbits[3] & 3, tilenum);
      compute_color_index(&short3, sortshort[3], lowbits[4] & 3, tilenum);


      sortidx[4] = (short0 << 2);
      sortidx[5] = (short1 << 2) | 1;
      sortidx[6] = (short2 << 2) | 2;
      sortidx[7] = (short3 << 2) | 3;
   }
   else
   {
      sortidx[4] = sort_tmem_idx(tidx_a, tidx_bhi, tidx_c, tidx_dhi, 0);
      sortidx[5] = sort_tmem_idx(tidx_a, tidx_bhi, tidx_c, tidx_dhi, 1);
      sortidx[6] = sort_tmem_idx(tidx_a, tidx_bhi, tidx_c, tidx_dhi, 2);
      sortidx[7] = sort_tmem_idx(tidx_a, tidx_bhi, tidx_c, tidx_dhi, 3);
   }

   short0 = tmem16[(sortidx[4] | 0x400) ^ WORD_ADDR_XOR];
   short1 = tmem16[(sortidx[5] | 0x400) ^ WORD_ADDR_XOR];
   short2 = tmem16[(sortidx[6] | 0x400) ^ WORD_ADDR_XOR];
   short3 = tmem16[(sortidx[7] | 0x400) ^ WORD_ADDR_XOR];



   if (globals.other_modes.en_tlut)
   {
      sort_tmem_shorts_lowhalf(&sortshort[4], short0, short1, short2, short3, 0);
      sort_tmem_shorts_lowhalf(&sortshort[5], short0, short1, short2, short3, 1);
      sort_tmem_shorts_lowhalf(&sortshort[6], short0, short1, short2, short3, 2);
      sort_tmem_shorts_lowhalf(&sortshort[7], short0, short1, short2, short3, 3);
   }
   else
   {
      sort_tmem_shorts_lowhalf(&sortshort[4], short0, short1, short2, short3, lowbits[0] >> 2);
      sort_tmem_shorts_lowhalf(&sortshort[5], short0, short1, short2, short3, lowbits[2] >> 2);
      sort_tmem_shorts_lowhalf(&sortshort[6], short0, short1, short2, short3, lowbits[3] >> 2);
      sort_tmem_shorts_lowhalf(&sortshort[7], short0, short1, short2, short3, lowbits[5] >> 2);
   }
}

static void tmem_init(void)
{
   int i;
   for (i = 0; i < 32; i++)
      replicated_rgba[i] = (i << 3) | ((i >> 2) & 7);

   memset(globals.tmem, 0, 0x1000);
}

static void tcdiv_persp(int32_t ss, int32_t st, int32_t sw, int32_t* sss, int32_t* sst);
static void tcdiv_nopersp(int32_t ss, int32_t st, int32_t sw, int32_t* sss, int32_t* sst);

static void (*tcdiv_func[2])(int32_t, int32_t, int32_t, int32_t*, int32_t*) =
{
    tcdiv_nopersp, tcdiv_persp
};

static int32_t maskbits_table[16];
static int32_t log2table[256];
static int32_t tcdiv_table[0x8000];

static STRICTINLINE void tcmask_copy(int32_t* S, int32_t* S1, int32_t* S2, int32_t* S3, int32_t* T, int32_t num)
{
    int32_t wrap;
    int32_t maskbits_s;
    int32_t swrapthreshold;

    if (globals.tile[num].mask_s)
    {
        if (globals.tile[num].ms)
        {
            swrapthreshold = globals.tile[num].f.masksclamped;

            wrap = (*S >> swrapthreshold) & 1;
            *S ^= (-wrap);

            wrap = (*S1 >> swrapthreshold) & 1;
            *S1 ^= (-wrap);

            wrap = (*S2 >> swrapthreshold) & 1;
            *S2 ^= (-wrap);

            wrap = (*S3 >> swrapthreshold) & 1;
            *S3 ^= (-wrap);
        }

        maskbits_s = maskbits_table[globals.tile[num].mask_s];
        *S &= maskbits_s;
        *S1 &= maskbits_s;
        *S2 &= maskbits_s;
        *S3 &= maskbits_s;
    }

    if (globals.tile[num].mask_t)
    {
        if (globals.tile[num].mt)
        {
            wrap = *T >> globals.tile[num].f.masktclamped;
            wrap &= 1;
            *T ^= (-wrap);
        }

        *T &= maskbits_table[globals.tile[num].mask_t];
    }
}


static STRICTINLINE void tcshift_cycle(int32_t* S, int32_t* T, int32_t* maxs, int32_t* maxt, uint32_t num)
{



    int32_t coord = *S;
    int32_t shifter = globals.tile[num].shift_s;


    if (shifter < 11)
    {
        coord = SIGN16(coord);
        coord >>= shifter;
    }
    else
    {
        coord <<= (16 - shifter);
        coord = SIGN16(coord);
    }
    *S = coord;




    *maxs = ((coord >> 3) >= globals.tile[num].sh);



    coord = *T;
    shifter = globals.tile[num].shift_t;

    if (shifter < 11)
    {
        coord = SIGN16(coord);
        coord >>= shifter;
    }
    else
    {
        coord <<= (16 - shifter);
        coord = SIGN16(coord);
    }
    *T = coord;
    *maxt = ((coord >> 3) >= globals.tile[num].th);
}

static STRICTINLINE void tcclamp_cycle(int32_t* S, int32_t* T, int32_t* SFRAC, int32_t* TFRAC, int32_t maxs, int32_t maxt, int32_t num)
{
    int32_t locs = *S, loct = *T;
    if (globals.tile[num].f.clampens)
    {

        if (maxs)
        {
            *S = globals.tile[num].f.clampdiffs;
            *SFRAC = 0;
        }
        else if (!(locs & 0x10000))
            *S = locs >> 5;
        else
        {
            *S = 0;
            *SFRAC = 0;
        }
    }
    else
        *S = (locs >> 5);

    if (globals.tile[num].f.clampent)
    {
        if (maxt)
        {
            *T = globals.tile[num].f.clampdifft;
            *TFRAC = 0;
        }
        else if (!(loct & 0x10000))
            *T = loct >> 5;
        else
        {
            *T = 0;
            *TFRAC = 0;
        }
    }
    else
        *T = (loct >> 5);
}


static STRICTINLINE void tcclamp_cycle_light(int32_t* S, int32_t* T, int32_t maxs, int32_t maxt, int32_t num)
{
    int32_t locs = *S, loct = *T;
    if (globals.tile[num].f.clampens)
    {
        if (maxs)
            *S = globals.tile[num].f.clampdiffs;
        else if (!(locs & 0x10000))
            *S = locs >> 5;
        else
            *S = 0;
    }
    else
        *S = (locs >> 5);

    if (globals.tile[num].f.clampent)
    {
        if (maxt)
            *T = globals.tile[num].f.clampdifft;
        else if (!(loct & 0x10000))
            *T = loct >> 5;
        else
            *T = 0;
    }
    else
        *T = (loct >> 5);
}

static STRICTINLINE void tcshift_copy(int32_t* S, int32_t* T, uint32_t num)
{
    int32_t coord = *S;
    int32_t shifter = globals.tile[num].shift_s;

    if (shifter < 11)
    {
        coord = SIGN16(coord);
        coord >>= shifter;
    }
    else
    {
        coord <<= (16 - shifter);
        coord = SIGN16(coord);
    }
    *S = coord;

    coord = *T;
    shifter = globals.tile[num].shift_t;

    if (shifter < 11)
    {
        coord = SIGN16(coord);
        coord >>= shifter;
    }
    else
    {
        coord <<= (16 - shifter);
        coord = SIGN16(coord);
    }
    *T = coord;

}


static STRICTINLINE void tclod_4x17_to_15(int32_t scurr, int32_t snext, int32_t tcurr, int32_t tnext, int32_t previous, int32_t* lod)
{
   int delt;
   int dels = SIGN(snext, 17) - SIGN(scurr, 17);
   if (dels & 0x20000)
      dels = ~dels & 0x1ffff;
   delt = SIGN(tnext, 17) - SIGN(tcurr, 17);
   if(delt & 0x20000)
      delt = ~delt & 0x1ffff;


   dels = (dels > delt) ? dels : delt;
   dels = (previous > dels) ? previous : dels;
   *lod = dels & 0x7fff;
   if (dels & 0x1c000)
      *lod |= 0x4000;
}

static STRICTINLINE void tclod_tcclamp(int32_t* sss, int32_t* sst)
{
    int32_t tempanded, temps = *sss, tempt = *sst;

    if (!(temps & 0x40000))
    {
        if (!(temps & 0x20000))
        {
            tempanded = temps & 0x18000;
            if (tempanded != 0x8000)
            {
                if (tempanded != 0x10000)
                    *sss &= 0xffff;
                else
                    *sss = 0x8000;
            }
            else
                *sss = 0x7fff;
        }
        else
            *sss = 0x8000;
    }
    else
        *sss = 0x7fff;

    if (!(tempt & 0x40000))
    {
        if (!(tempt & 0x20000))
        {
            tempanded = tempt & 0x18000;
            if (tempanded != 0x8000)
            {
                if (tempanded != 0x10000)
                    *sst &= 0xffff;
                else
                    *sst = 0x8000;
            }
            else
                *sst = 0x7fff;
        }
        else
            *sst = 0x8000;
    }
    else
        *sst = 0x7fff;

}


static STRICTINLINE void lodfrac_lodtile_signals(int lodclamp, int32_t lod, uint32_t* l_tile, uint32_t* magnify, uint32_t* distant, int32_t* lfdst)
{
    uint32_t ltil, dis, mag;
    int32_t lf;


    if ((lod & 0x4000) || lodclamp)
    {


        mag = 0;
        ltil = 7;
        dis = 1;
        lf = 0xff;
    }
    else if (lod < globals.min_level)
    {


        mag = 1;
        ltil = 0;
        dis = globals.max_level == 0;

        if(!globals.other_modes.sharpen_tex_en && !globals.other_modes.detail_tex_en)
        {
            if (dis)
                lf = 0xff;
            else
                lf = 0;
        }
        else
        {
            lf = globals.min_level << 3;
            if (globals.other_modes.sharpen_tex_en)
                lf |= 0x100;
        }
    }
    else if (lod < 32)
    {
        mag = 1;
        ltil = 0;
        dis = globals.max_level == 0;

        if(!globals.other_modes.sharpen_tex_en && !globals.other_modes.detail_tex_en)
        {
            if (dis)
                lf = 0xff;
            else
                lf = 0;
        }
        else
        {
            lf = lod << 3;
            if (globals.other_modes.sharpen_tex_en)
                lf |= 0x100;
        }
    }
    else
    {
        mag = 0;
        ltil =  log2table[(lod >> 5) & 0xff];

        if (globals.max_level)
            dis = ((lod & 0x6000) || (ltil >= globals.max_level)) != 0;
        else
            dis = 1;


        if(!globals.other_modes.sharpen_tex_en && !globals.other_modes.detail_tex_en && dis)
            lf = 0xff;
        else
            lf = ((lod << 3) >> ltil) & 0xff;






    }

    *distant = dis;
    *l_tile = ltil;
    *magnify = mag;
    *lfdst = lf;
}

static STRICTINLINE void tclod_2cycle_current(int32_t* sss, int32_t* sst, int32_t nexts, int32_t nextt, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1, int32_t* t2)
{








    int nextys, nextyt, nextysw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile;
    uint32_t magnify = 0;
    uint32_t distant = 0;
    int inits = *sss, initt = *sst;

    tclod_tcclamp(sss, sst);

    if (globals.other_modes.f.dolod)
    {






        nextys = (s + globals.spans.dsdy) >> 16;
        nextyt = (t + globals.spans.dtdy) >> 16;
        nextysw = (w + globals.spans.dwdy) >> 16;

        globals.tcdiv_ptr(nextys, nextyt, nextysw, &nextys, &nextyt);

        lodclamp = (initt & 0x60000) || (nextt & 0x60000) || (inits & 0x60000) || (nexts & 0x60000) || (nextys & 0x60000) || (nextyt & 0x60000);




        if (!lodclamp)
        {
            tclod_4x17_to_15(inits, nexts, initt, nextt, 0, &lod);
            tclod_4x17_to_15(inits, nextys, initt, nextyt, lod, &lod);
        }

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, &globals.lod_frac);


        if (globals.other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = globals.max_level;
            if (!globals.other_modes.detail_tex_en)
            {
                *t1 = (prim_tile + l_tile) & 7;
                if (!(distant || (!globals.other_modes.sharpen_tex_en && magnify)))
                    *t2 = (*t1 + 1) & 7;
                else
                    *t2 = *t1;
            }
            else
            {
                if (!magnify)
                    *t1 = (prim_tile + l_tile + 1);
                else
                    *t1 = (prim_tile + l_tile);
                *t1 &= 7;
                if (!distant && !magnify)
                    *t2 = (prim_tile + l_tile + 2) & 7;
                else
                    *t2 = (prim_tile + l_tile + 1) & 7;
            }
        }
    }
}


static STRICTINLINE void tclod_2cycle_current_simple(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1, int32_t* t2)
{
    int nextys, nextyt, nextysw, nexts, nextt, nextsw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile;
    uint32_t magnify = 0;
    uint32_t distant = 0;
    int inits = *sss, initt = *sst;

    tclod_tcclamp(sss, sst);

    if (globals.other_modes.f.dolod)
    {
        nextsw = (w + dwinc) >> 16;
        nexts = (s + dsinc) >> 16;
        nextt = (t + dtinc) >> 16;
        nextys = (s + globals.spans.dsdy) >> 16;
        nextyt = (t + globals.spans.dtdy) >> 16;
        nextysw = (w + globals.spans.dwdy) >> 16;

        globals.tcdiv_ptr(nexts, nextt, nextsw, &nexts, &nextt);
        globals.tcdiv_ptr(nextys, nextyt, nextysw, &nextys, &nextyt);

        lodclamp = (initt & 0x60000) || (nextt & 0x60000) || (inits & 0x60000) || (nexts & 0x60000) || (nextys & 0x60000) || (nextyt & 0x60000);

        if (!lodclamp)
        {
            tclod_4x17_to_15(inits, nexts, initt, nextt, 0, &lod);
            tclod_4x17_to_15(inits, nextys, initt, nextyt, lod, &lod);
        }

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, &globals.lod_frac);

        if (globals.other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = globals.max_level;
            if (!globals.other_modes.detail_tex_en)
            {
                *t1 = (prim_tile + l_tile) & 7;
                if (!(distant || (!globals.other_modes.sharpen_tex_en && magnify)))
                    *t2 = (*t1 + 1) & 7;
                else
                    *t2 = *t1;
            }
            else
            {
                if (!magnify)
                    *t1 = (prim_tile + l_tile + 1);
                else
                    *t1 = (prim_tile + l_tile);
                *t1 &= 7;
                if (!distant && !magnify)
                    *t2 = (prim_tile + l_tile + 2) & 7;
                else
                    *t2 = (prim_tile + l_tile + 1) & 7;
            }
        }
    }
}


static STRICTINLINE void tclod_2cycle_current_notexel1(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1)
{
    int nextys, nextyt, nextysw, nexts, nextt, nextsw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile;
    uint32_t magnify = 0;
    uint32_t distant = 0;
    int inits = *sss, initt = *sst;

    tclod_tcclamp(sss, sst);

    if (globals.other_modes.f.dolod)
    {
        nextsw = (w + dwinc) >> 16;
        nexts = (s + dsinc) >> 16;
        nextt = (t + dtinc) >> 16;
        nextys = (s + globals.spans.dsdy) >> 16;
        nextyt = (t + globals.spans.dtdy) >> 16;
        nextysw = (w + globals.spans.dwdy) >> 16;

        globals.tcdiv_ptr(nexts, nextt, nextsw, &nexts, &nextt);
        globals.tcdiv_ptr(nextys, nextyt, nextysw, &nextys, &nextyt);

        lodclamp = (initt & 0x60000) || (nextt & 0x60000) || (inits & 0x60000) || (nexts & 0x60000) || (nextys & 0x60000) || (nextyt & 0x60000);

        if (!lodclamp)
        {
            tclod_4x17_to_15(inits, nexts, initt, nextt, 0, &lod);
            tclod_4x17_to_15(inits, nextys, initt, nextyt, lod, &lod);
        }

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, &globals.lod_frac);

        if (globals.other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = globals.max_level;
            if (!globals.other_modes.detail_tex_en || magnify)
                *t1 = (prim_tile + l_tile) & 7;
            else
                *t1 = (prim_tile + l_tile + 1) & 7;
        }

    }
}

static STRICTINLINE void tclod_2cycle_next(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1, int32_t* t2, int32_t* prelodfrac)
{
    int nexts, nextt, nextsw, nextys, nextyt, nextysw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile;
    uint32_t magnify = 0;
    uint32_t distant = 0;
    int inits = *sss, initt = *sst;

    tclod_tcclamp(sss, sst);

    if (globals.other_modes.f.dolod)
    {
        nextsw = (w + dwinc) >> 16;
        nexts = (s + dsinc) >> 16;
        nextt = (t + dtinc) >> 16;
        nextys = (s + globals.spans.dsdy) >> 16;
        nextyt = (t + globals.spans.dtdy) >> 16;
        nextysw = (w + globals.spans.dwdy) >> 16;

        globals.tcdiv_ptr(nexts, nextt, nextsw, &nexts, &nextt);
        globals.tcdiv_ptr(nextys, nextyt, nextysw, &nextys, &nextyt);

        lodclamp = (initt & 0x60000) || (nextt & 0x60000) || (inits & 0x60000) || (nexts & 0x60000) || (nextys & 0x60000) || (nextyt & 0x60000);

        if (!lodclamp)
        {
            tclod_4x17_to_15(inits, nexts, initt, nextt, 0, &lod);
            tclod_4x17_to_15(inits, nextys, initt, nextyt, lod, &lod);
        }

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, prelodfrac);

        if (globals.other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = globals.max_level;
            if (!globals.other_modes.detail_tex_en)
            {
                *t1 = (prim_tile + l_tile) & 7;
                if (!(distant || (!globals.other_modes.sharpen_tex_en && magnify)))
                    *t2 = (*t1 + 1) & 7;
                else
                    *t2 = *t1;
            }
            else
            {
                if (!magnify)
                    *t1 = (prim_tile + l_tile + 1);
                else
                    *t1 = (prim_tile + l_tile);
                *t1 &= 7;
                if (!distant && !magnify)
                    *t2 = (prim_tile + l_tile + 2) & 7;
                else
                    *t2 = (prim_tile + l_tile + 1) & 7;
            }
        }
    }
}

static STRICTINLINE void tclod_1cycle_current(int32_t* sss, int32_t* sst, int32_t nexts, int32_t nextt, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, int32_t prim_tile, int32_t* t1, struct spansigs* sigs)
{









    int fars, fart, farsw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile = 0, magnify = 0, distant = 0;

    tclod_tcclamp(sss, sst);

    if (globals.other_modes.f.dolod)
    {
        int nextscan = scanline + 1;


        if (globals.span[nextscan].validline)
        {
            if (!sigs->endspan || !sigs->longspan)
            {
                if (!(sigs->preendspan && sigs->longspan) && !(sigs->endspan && sigs->midspan))
                {
                    farsw = (w + (dwinc << 1)) >> 16;
                    fars = (s + (dsinc << 1)) >> 16;
                    fart = (t + (dtinc << 1)) >> 16;
                }
                else
                {
                    farsw = (w - dwinc) >> 16;
                    fars = (s - dsinc) >> 16;
                    fart = (t - dtinc) >> 16;
                }
            }
            else
            {
                fart = (globals.span[nextscan].t + dtinc) >> 16;
                fars = (globals.span[nextscan].s + dsinc) >> 16;
                farsw = (globals.span[nextscan].w + dwinc) >> 16;
            }
        }
        else
        {
            farsw = (w + (dwinc << 1)) >> 16;
            fars = (s + (dsinc << 1)) >> 16;
            fart = (t + (dtinc << 1)) >> 16;
        }

        globals.tcdiv_ptr(fars, fart, farsw, &fars, &fart);

        lodclamp = (fart & 0x60000) || (nextt & 0x60000) || (fars & 0x60000) || (nexts & 0x60000);




        if (!lodclamp)
            tclod_4x17_to_15(nexts, fars, nextt, fart, 0, &lod);

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, &globals.lod_frac);

        if (globals.other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = globals.max_level;



            if (!globals.other_modes.detail_tex_en || magnify)
                *t1 = (prim_tile + l_tile) & 7;
            else
                *t1 = (prim_tile + l_tile + 1) & 7;
        }
    }
}



static STRICTINLINE void tclod_1cycle_current_simple(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, int32_t prim_tile, int32_t* t1, struct spansigs* sigs)
{
    int fars, fart, farsw, nexts, nextt, nextsw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile = 0, magnify = 0, distant = 0;

    tclod_tcclamp(sss, sst);

    if (globals.other_modes.f.dolod)
    {

        int nextscan = scanline + 1;
        if (globals.span[nextscan].validline)
        {
            if (!sigs->endspan || !sigs->longspan)
            {
                nextsw = (w + dwinc) >> 16;
                nexts = (s + dsinc) >> 16;
                nextt = (t + dtinc) >> 16;

                if (!(sigs->preendspan && sigs->longspan) && !(sigs->endspan && sigs->midspan))
                {
                    farsw = (w + (dwinc << 1)) >> 16;
                    fars = (s + (dsinc << 1)) >> 16;
                    fart = (t + (dtinc << 1)) >> 16;
                }
                else
                {
                    farsw = (w - dwinc) >> 16;
                    fars = (s - dsinc) >> 16;
                    fart = (t - dtinc) >> 16;
                }
            }
            else
            {
                nextt = globals.span[nextscan].t >> 16;
                nexts = globals.span[nextscan].s >> 16;
                nextsw = globals.span[nextscan].w >> 16;
                fart = (globals.span[nextscan].t + dtinc) >> 16;
                fars = (globals.span[nextscan].s + dsinc) >> 16;
                farsw = (globals.span[nextscan].w + dwinc) >> 16;
            }
        }
        else
        {
            nextsw = (w + dwinc) >> 16;
            nexts = (s + dsinc) >> 16;
            nextt = (t + dtinc) >> 16;
            farsw = (w + (dwinc << 1)) >> 16;
            fars = (s + (dsinc << 1)) >> 16;
            fart = (t + (dtinc << 1)) >> 16;
        }

        globals.tcdiv_ptr(nexts, nextt, nextsw, &nexts, &nextt);
        globals.tcdiv_ptr(fars, fart, farsw, &fars, &fart);

        lodclamp = (fart & 0x60000) || (nextt & 0x60000) || (fars & 0x60000) || (nexts & 0x60000);

        if (!lodclamp)
            tclod_4x17_to_15(nexts, fars, nextt, fart, 0, &lod);

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, &globals.lod_frac);

        if (globals.other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = globals.max_level;
            if (!globals.other_modes.detail_tex_en || magnify)
                *t1 = (prim_tile + l_tile) & 7;
            else
                *t1 = (prim_tile + l_tile + 1) & 7;
        }
    }
}

static STRICTINLINE void tclod_1cycle_next(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, int32_t prim_tile, int32_t* t1, struct spansigs* sigs, int32_t* prelodfrac)
{
    int nexts, nextt, nextsw, fars, fart, farsw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile = 0, magnify = 0, distant = 0;

    tclod_tcclamp(sss, sst);

    if (globals.other_modes.f.dolod)
    {

        int nextscan = scanline + 1;

        if (globals.span[nextscan].validline)
        {

            if (!sigs->nextspan)
            {
                if (!sigs->endspan || !sigs->longspan)
                {
                    nextsw = (w + dwinc) >> 16;
                    nexts = (s + dsinc) >> 16;
                    nextt = (t + dtinc) >> 16;

                    if (!(sigs->preendspan && sigs->longspan) && !(sigs->endspan && sigs->midspan))
                    {
                        farsw = (w + (dwinc << 1)) >> 16;
                        fars = (s + (dsinc << 1)) >> 16;
                        fart = (t + (dtinc << 1)) >> 16;
                    }
                    else
                    {
                        farsw = (w - dwinc) >> 16;
                        fars = (s - dsinc) >> 16;
                        fart = (t - dtinc) >> 16;
                    }
                }
                else
                {
                    nextt = globals.span[nextscan].t;
                    nexts = globals.span[nextscan].s;
                    nextsw = globals.span[nextscan].w;
                    fart = (nextt + dtinc) >> 16;
                    fars = (nexts + dsinc) >> 16;
                    farsw = (nextsw + dwinc) >> 16;
                    nextt >>= 16;
                    nexts >>= 16;
                    nextsw >>= 16;
                }
            }
            else
            {









                if (sigs->longspan)
                {
                    nextt = (globals.span[nextscan].t + dtinc) >> 16;
                    nexts = (globals.span[nextscan].s + dsinc) >> 16;
                    nextsw = (globals.span[nextscan].w + dwinc) >> 16;
                    fart = (globals.span[nextscan].t + (dtinc << 1)) >> 16;
                    fars = (globals.span[nextscan].s + (dsinc << 1)) >> 16;
                    farsw = (globals.span[nextscan].w  + (dwinc << 1)) >> 16;
                }
                else if (sigs->midspan)
                {
                    nextt = globals.span[nextscan].t >> 16;
                    nexts = globals.span[nextscan].s >> 16;
                    nextsw = globals.span[nextscan].w >> 16;
                    fart = (globals.span[nextscan].t + dtinc) >> 16;
                    fars = (globals.span[nextscan].s + dsinc) >> 16;
                    farsw = (globals.span[nextscan].w  + dwinc) >> 16;
                }
                else if (sigs->onelessthanmid)
                {
                    nextsw = (w + dwinc) >> 16;
                    nexts = (s + dsinc) >> 16;
                    nextt = (t + dtinc) >> 16;
                    farsw = (w - dwinc) >> 16;
                    fars = (s - dsinc) >> 16;
                    fart = (t - dtinc) >> 16;
                }
                else
                {
                    nextt = (t + dtinc) >> 16;
                    nexts = (s + dsinc) >> 16;
                    nextsw = (w + dwinc) >> 16;
                    fart = (t + (dtinc << 1)) >> 16;
                    fars = (s + (dsinc << 1)) >> 16;
                    farsw = (w + (dwinc << 1)) >> 16;
                }
            }
        }
        else
        {
            nextsw = (w + dwinc) >> 16;
            nexts = (s + dsinc) >> 16;
            nextt = (t + dtinc) >> 16;
            farsw = (w + (dwinc << 1)) >> 16;
            fars = (s + (dsinc << 1)) >> 16;
            fart = (t + (dtinc << 1)) >> 16;
        }

        globals.tcdiv_ptr(nexts, nextt, nextsw, &nexts, &nextt);
        globals.tcdiv_ptr(fars, fart, farsw, &fars, &fart);

        lodclamp = (fart & 0x60000) || (nextt & 0x60000) || (fars & 0x60000) || (nexts & 0x60000);



        if (!lodclamp)
            tclod_4x17_to_15(nexts, fars, nextt, fart, 0, &lod);

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, prelodfrac);

        if (globals.other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = globals.max_level;
            if (!globals.other_modes.detail_tex_en || magnify)
                *t1 = (prim_tile + l_tile) & 7;
            else
                *t1 = (prim_tile + l_tile + 1) & 7;
        }
    }
}

static STRICTINLINE void tclod_copy(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1)
{




    int nexts, nextt, nextsw, fars, fart, farsw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile = 0, magnify = 0, distant = 0;

    tclod_tcclamp(sss, sst);

    if (globals.other_modes.tex_lod_en)
    {



        nextsw = (w + dwinc) >> 16;
        nexts = (s + dsinc) >> 16;
        nextt = (t + dtinc) >> 16;
        farsw = (w + (dwinc << 1)) >> 16;
        fars = (s + (dsinc << 1)) >> 16;
        fart = (t + (dtinc << 1)) >> 16;

        globals.tcdiv_ptr(nexts, nextt, nextsw, &nexts, &nextt);
        globals.tcdiv_ptr(fars, fart, farsw, &fars, &fart);

        lodclamp = (fart & 0x60000) || (nextt & 0x60000) || (fars & 0x60000) || (nexts & 0x60000);

        if (!lodclamp)
            tclod_4x17_to_15(nexts, fars, nextt, fart, 0, &lod);

        if ((lod & 0x4000) || lodclamp)
        {


            magnify = 0;
            l_tile = globals.max_level;
        }
        else if (lod < 32)
        {
            magnify = 1;
            l_tile = 0;
        }
        else
        {
            magnify = 0;
            l_tile =  log2table[(lod >> 5) & 0xff];

            if (globals.max_level)
                distant = ((lod & 0x6000) || (l_tile >= globals.max_level)) != 0;
            else
                distant = 1;

            if (distant)
                l_tile = globals.max_level;
        }

        if (!globals.other_modes.detail_tex_en || magnify)
            *t1 = (prim_tile + l_tile) & 7;
        else
            *t1 = (prim_tile + l_tile + 1) & 7;
    }

}

static STRICTINLINE void tc_pipeline_copy(int32_t* sss0, int32_t* sss1, int32_t* sss2, int32_t* sss3, int32_t* sst, int tilenum)
{
    int ss0 = *sss0, ss1 = 0, ss2 = 0, ss3 = 0, st = *sst;

    tcshift_copy(&ss0, &st, tilenum);



    ss0 = TRELATIVE(ss0, globals.tile[tilenum].sl);
    st = TRELATIVE(st, globals.tile[tilenum].tl);
    ss0 = (ss0 >> 5);
    st = (st >> 5);

    ss1 = ss0 + 1;
    ss2 = ss0 + 2;
    ss3 = ss0 + 3;

    tcmask_copy(&ss0, &ss1, &ss2, &ss3, &st, tilenum);

    *sss0 = ss0;
    *sss1 = ss1;
    *sss2 = ss2;
    *sss3 = ss3;
    *sst = st;
}

static STRICTINLINE void tc_pipeline_load(int32_t* sss, int32_t* sst, int tilenum, int coord_quad)
{
    int sss1 = *sss, sst1 = *sst;
    sss1 = SIGN16(sss1);
    sst1 = SIGN16(sst1);


    sss1 = TRELATIVE(sss1, globals.tile[tilenum].sl);
    sst1 = TRELATIVE(sst1, globals.tile[tilenum].tl);



    if (!coord_quad)
    {
        sss1 = (sss1 >> 5);
        sst1 = (sst1 >> 5);
    }
    else
    {
        sss1 = (sss1 >> 3);
        sst1 = (sst1 >> 3);
    }

    *sss = sss1;
    *sst = sst1;
}

static void tcdiv_nopersp(int32_t ss, int32_t st, int32_t sw, int32_t* sss, int32_t* sst)
{



    *sss = (SIGN16(ss)) & 0x1ffff;
    *sst = (SIGN16(st)) & 0x1ffff;
}

static void tcdiv_persp(int32_t ss, int32_t st, int32_t sw, int32_t* sss, int32_t* sst)
{


    int w_carry = 0;
    int shift;
    int tlu_rcp;
    int sprod, tprod;
    int outofbounds_s, outofbounds_t;
    int tempmask;
    int shift_value;
    int32_t temps, tempt;



    int overunder_s = 0, overunder_t = 0;


    if (SIGN16(sw) <= 0)
        w_carry = 1;

    sw &= 0x7fff;



    shift = tcdiv_table[sw];
    tlu_rcp = shift >> 4;
    shift &= 0xf;

    sprod = SIGN16(ss) * tlu_rcp;
    tprod = SIGN16(st) * tlu_rcp;




    tempmask = ((1 << 30) - 1) & -((1 << 29) >> shift);

    outofbounds_s = sprod & tempmask;
    outofbounds_t = tprod & tempmask;

    if (shift != 0xe)
    {
        shift_value = 13 - shift;
        temps = sprod = (sprod >> shift_value);
        tempt = tprod = (tprod >> shift_value);
    }
    else
    {
        temps = sprod << 1;
        tempt = tprod << 1;
    }

    if (outofbounds_s != tempmask && outofbounds_s != 0)
    {
        if (!(sprod & (1 << 29)))
            overunder_s = 2 << 17;
        else
            overunder_s = 1 << 17;
    }

    if (outofbounds_t != tempmask && outofbounds_t != 0)
    {
        if (!(tprod & (1 << 29)))
            overunder_t = 2 << 17;
        else
            overunder_t = 1 << 17;
    }

    if (w_carry)
    {
        overunder_s |= (2 << 17);
        overunder_t |= (2 << 17);
    }

    *sss = (temps & 0x1ffff) | overunder_s;
    *sst = (tempt & 0x1ffff) | overunder_t;
}

static void tcoord_init(void)
{
    int i, k;

    globals.tcdiv_ptr = tcdiv_func[0];

    log2table[0] = log2table[1] = 0;
    for (i = 2; i < 256; i++)
    {
        for (k = 7; k > 0; k--)
        {
            if((i >> k) & 1)
            {
                log2table[i] = k;
                break;
            }
        }
    }

    int temppoint, tempslope;
    int normout;
    int wnorm;
    int shift, tlu_rcp;

    for (i = 0; i < 0x8000; i++)
    {
        for (k = 1; k <= 14 && !((i << k) & 0x8000); k++)
            ;
        shift = k - 1;
        normout = (i << shift) & 0x3fff;
        wnorm = (normout & 0xff) << 2;
        normout >>= 8;



        temppoint = norm_point_table[normout];
        tempslope = norm_slope_table[normout];

        tempslope = (tempslope | ~0x3ff) + 1;

        tlu_rcp = (((tempslope * wnorm) >> 10) + temppoint) & 0x7fff;

        tcdiv_table[i] = shift | (tlu_rcp << 4);
    }

    maskbits_table[0] = 0x3ff;
    for (i = 1; i < 16; i++)
        maskbits_table[i] = ((uint16_t)(0xffff) >> (16 - i)) & 0x3ff;
}

static STRICTINLINE void tcmask(int32_t* S, int32_t* T, int32_t num)
{
    int32_t wrap;

    if (globals.tile[num].mask_s)
    {
        if (globals.tile[num].ms)
        {
            wrap = *S >> globals.tile[num].f.masksclamped;
            wrap &= 1;
            *S ^= (-wrap);
        }
        *S &= maskbits_table[globals.tile[num].mask_s];
    }

    if (globals.tile[num].mask_t)
    {
        if (globals.tile[num].mt)
        {
            wrap = *T >> globals.tile[num].f.masktclamped;
            wrap &= 1;
            *T ^= (-wrap);
        }

        *T &= maskbits_table[globals.tile[num].mask_t];
    }
}


static STRICTINLINE void tcmask_coupled(int32_t* S, int32_t* sdiff, int32_t* T, int32_t* tdiff, int32_t num)
{
    int32_t wrap;
    int32_t maskbits;
    int32_t wrapthreshold;


    if (globals.tile[num].mask_s)
    {
        maskbits = maskbits_table[globals.tile[num].mask_s];

        if (globals.tile[num].ms)
        {
            wrapthreshold = globals.tile[num].f.masksclamped;

            wrap = (*S >> wrapthreshold) & 1;
            *S ^= (-wrap);
            *S &= maskbits;


            if (((*S - wrap) & maskbits) == maskbits)
                *sdiff = 0;
            else
                *sdiff = 1 - (wrap << 1);
        }
        else
        {
            *S &= maskbits;
            if (*S == maskbits)
                *sdiff = -(*S);
            else
                *sdiff = 1;
        }
    }
    else
        *sdiff = 1;

    if (globals.tile[num].mask_t)
    {
        maskbits = maskbits_table[globals.tile[num].mask_t];

        if (globals.tile[num].mt)
        {
            wrapthreshold = globals.tile[num].f.masktclamped;

            wrap = (*T >> wrapthreshold) & 1;
            *T ^= (-wrap);
            *T &= maskbits;

            if (((*T - wrap) & maskbits) == maskbits)
                *tdiff = 0;
            else
                *tdiff = 1 - (wrap << 1);
        }
        else
        {
            *T &= maskbits;
            if (*T == maskbits)
                *tdiff = -(*T & 0xff);
            else
                *tdiff = 1;
        }
    }
    else
        *tdiff = 1;
}


static INLINE void calculate_clamp_diffs(uint32_t i)
{
    globals.tile[i].f.clampdiffs = ((globals.tile[i].sh >> 2) - (globals.tile[i].sl >> 2)) & 0x3ff;
    globals.tile[i].f.clampdifft = ((globals.tile[i].th >> 2) - (globals.tile[i].tl >> 2)) & 0x3ff;
}


static INLINE void calculate_tile_derivs(uint32_t i)
{
    globals.tile[i].f.clampens = globals.tile[i].cs || !globals.tile[i].mask_s;
    globals.tile[i].f.clampent = globals.tile[i].ct || !globals.tile[i].mask_t;
    globals.tile[i].f.masksclamped = globals.tile[i].mask_s <= 10 ? globals.tile[i].mask_s : 10;
    globals.tile[i].f.masktclamped = globals.tile[i].mask_t <= 10 ? globals.tile[i].mask_t : 10;
    globals.tile[i].f.notlutswitch = (globals.tile[i].format << 2) | globals.tile[i].size;
    globals.tile[i].f.tlutswitch = (globals.tile[i].size << 2) | ((globals.tile[i].format + 2) & 3);

    if (globals.tile[i].format < 5)
    {
        globals.tile[i].f.notlutswitch = (globals.tile[i].format << 2) | globals.tile[i].size;
        globals.tile[i].f.tlutswitch = (globals.tile[i].size << 2) | ((globals.tile[i].format + 2) & 3);
    }
    else
    {
        globals.tile[i].f.notlutswitch = 0x10 | globals.tile[i].size;
        globals.tile[i].f.tlutswitch = (globals.tile[i].size << 2) | 2;
    }
}

static STRICTINLINE void get_texel1_1cycle(int32_t* s1, int32_t* t1, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, struct spansigs* sigs)
{
    int32_t nexts, nextt, nextsw;

    if (!sigs->endspan || !sigs->longspan || !globals.span[scanline + 1].validline)
    {
        nextsw = (w + dwinc) >> 16;
        nexts = (s + dsinc) >> 16;
        nextt = (t + dtinc) >> 16;
    }
    else
    {
        int32_t nextscan = scanline + 1;
        nextt = globals.span[nextscan].t >> 16;
        nexts = globals.span[nextscan].s >> 16;
        nextsw = globals.span[nextscan].w >> 16;
    }

    globals.tcdiv_ptr(nexts, nextt, nextsw, s1, t1);
}

static STRICTINLINE void get_nexttexel0_2cycle(int32_t* s1, int32_t* t1, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc)
{


    int32_t nexts, nextt, nextsw;
    nextsw = (w + dwinc) >> 16;
    nexts = (s + dsinc) >> 16;
    nextt = (t + dtinc) >> 16;

    globals.tcdiv_ptr(nexts, nextt, nextsw, s1, t1);
}

static STRICTINLINE void texture_pipeline_cycle(struct color* TEX, struct color* prev, int32_t SSS, int32_t SST, uint32_t tilenum, uint32_t cycle)
{
    int32_t maxs, maxt, invt3r, invt3g, invt3b, invt3a;
    int32_t sfrac, tfrac, invsf, invtf, sfracrg, invsfrg;
    int upper, upperrg, center, centerrg;


    int bilerp = cycle ? globals.other_modes.bi_lerp1 : globals.other_modes.bi_lerp0;
    int convert = globals.other_modes.convert_one && cycle;
    struct color t0, t1, t2, t3;
    int sss1, sst1, sdiff, tdiff;

    sss1 = SSS;
    sst1 = SST;

    tcshift_cycle(&sss1, &sst1, &maxs, &maxt, tilenum);

    sss1 = TRELATIVE(sss1, globals.tile[tilenum].sl);
    sst1 = TRELATIVE(sst1, globals.tile[tilenum].tl);

    if (globals.other_modes.sample_type || globals.other_modes.en_tlut)
    {
        sfrac = sss1 & 0x1f;
        tfrac = sst1 & 0x1f;

        tcclamp_cycle(&sss1, &sst1, &sfrac, &tfrac, maxs, maxt, tilenum);
        tcmask_coupled(&sss1, &sdiff, &sst1, &tdiff, tilenum);

        upper = (sfrac + tfrac) & 0x20;

        if (globals.tile[tilenum].format == FORMAT_YUV)
        {
            sfracrg = (sfrac >> 1) | ((sss1 & 1) << 4);
            upperrg = (sfracrg + tfrac) & 0x20;
        }
        else
        {
            upperrg = upper;
            sfracrg = sfrac;
        }

        if (!globals.other_modes.sample_type)
            fetch_texel_entlut_quadro_nearest(&t0, &t1, &t2, &t3, sss1, sst1, tilenum, upper, upperrg);
        else if (globals.other_modes.en_tlut)
            fetch_texel_entlut_quadro(&t0, &t1, &t2, &t3, sss1, sdiff, sst1, tdiff, tilenum, upper, upperrg);
        else
            fetch_texel_quadro(&t0, &t1, &t2, &t3, sss1, sdiff, sst1, tdiff, tilenum, upper - upperrg);

        if (bilerp)
        {
            if (!globals.other_modes.mid_texel)
                center = centerrg = 0;
            else
            {
                center = (sfrac == 0x10 && tfrac == 0x10);
                centerrg = (sfracrg == 0x10 && tfrac == 0x10);
            }

            if (!convert)
            {
                invtf = 0x20 - tfrac;

                if (!centerrg)
                {
                    if (upperrg)
                    {

                        invsfrg = 0x20 - sfracrg;

                        TEX->r = t3.r + ((invsfrg * (t2.r - t3.r) + invtf * (t1.r - t3.r) + 0x10) >> 5);
                        TEX->g = t3.g + ((invsfrg * (t2.g - t3.g) + invtf * (t1.g - t3.g) + 0x10) >> 5);
                    }
                    else
                    {
                        TEX->r = t0.r + ((sfracrg * (t1.r - t0.r) + tfrac * (t2.r - t0.r) + 0x10) >> 5);
                        TEX->g = t0.g + ((sfracrg * (t1.g - t0.g) + tfrac * (t2.g - t0.g) + 0x10) >> 5);
                    }
                }
                else
                {

                    invt3r  = ~t3.r;
                    invt3g = ~t3.g;


                    TEX->r = t3.r + ((((t1.r + t2.r) << 6) - (t3.r << 7) + ((invt3r + t0.r) << 6) + 0xc0) >> 8);
                    TEX->g = t3.g + ((((t1.g + t2.g) << 6) - (t3.g << 7) + ((invt3g + t0.g) << 6) + 0xc0) >> 8);
                }

                if (!center)
                {
                    if (upper)
                    {
                        invsf = 0x20 - sfrac;

                        TEX->b = t3.b + ((invsf * (t2.b - t3.b) + invtf * (t1.b - t3.b) + 0x10) >> 5);
                        TEX->a = t3.a + ((invsf * (t2.a - t3.a) + invtf * (t1.a - t3.a) + 0x10) >> 5);
                    }
                    else
                    {
                        TEX->b = t0.b + ((sfrac * (t1.b - t0.b) + tfrac * (t2.b - t0.b) + 0x10) >> 5);
                        TEX->a = t0.a + ((sfrac * (t1.a - t0.a) + tfrac * (t2.a - t0.a) + 0x10) >> 5);
                    }
                }
                else
                {
                    invt3b = ~t3.b;
                    invt3a = ~t3.a;

                    TEX->b = t3.b + ((((t1.b + t2.b) << 6) - (t3.b << 7) + ((invt3b + t0.b) << 6) + 0xc0) >> 8);
                    TEX->a = t3.a + ((((t1.a + t2.a) << 6) - (t3.a << 7) + ((invt3a + t0.a) << 6) + 0xc0) >> 8);
                }
            }
            else
            {
                if (!centerrg)
                {
                    if (upperrg)
                    {
                        TEX->r = prev->b + ((prev->r * (t2.r - t3.r) + prev->g * (t1.r - t3.r) + 0x80) >> 8);
                        TEX->g = prev->b + ((prev->r * (t2.g - t3.g) + prev->g * (t1.g - t3.g) + 0x80) >> 8);
                    }
                    else
                    {
                        TEX->r = prev->b + ((prev->r * (t1.r - t0.r) + prev->g * (t2.r - t0.r) + 0x80) >> 8);
                        TEX->g = prev->b + ((prev->r * (t1.g - t0.g) + prev->g * (t2.g - t0.g) + 0x80) >> 8);
                    }
                }
                else
                {
                    invt3r = ~t3.r;
                    invt3g = ~t3.g;

                    TEX->r = prev->b + ((prev->r * (t2.r - t3.r) + prev->g * (t1.r - t3.r) + ((invt3r + t0.r) << 6) + 0xc0) >> 8);
                    TEX->g = prev->b + ((prev->r * (t2.g - t3.g) + prev->g * (t1.g - t3.g) + ((invt3g + t0.g) << 6) + 0xc0) >> 8);
                }

                if (!center)
                {
                    if (upper)
                    {
                        TEX->b = prev->b + ((prev->r * (t2.b - t3.b) + prev->g * (t1.b - t3.b) + 0x80) >> 8);
                        TEX->a = prev->b + ((prev->r * (t2.a - t3.a) + prev->g * (t1.a - t3.a) + 0x80) >> 8);
                    }
                    else
                    {
                        TEX->b = prev->b + ((prev->r * (t1.b - t0.b) + prev->g * (t2.b - t0.b) + 0x80) >> 8);
                        TEX->a = prev->b + ((prev->r * (t1.a - t0.a) + prev->g * (t2.a - t0.a) + 0x80) >> 8);
                    }
                }
                else
                {
                    invt3b = ~t3.b;
                    invt3a = ~t3.a;

                    TEX->b = prev->b + ((prev->r * (t2.b - t3.b) + prev->g * (t1.b - t3.b) + ((invt3b + t0.b) << 6) + 0xc0) >> 8);
                    TEX->a = prev->b + ((prev->r * (t2.a - t3.a) + prev->g * (t1.a - t3.a) + ((invt3a + t0.a) << 6) + 0xc0) >> 8);
                }
            }
        }
        else
        {

            if (convert)
                t0 = t3 = *prev;


            if (upperrg)
            {
                if (upper)
                {
                    TEX->r = t3.b + ((globals.k0_tf * t3.g + 0x80) >> 8);
                    TEX->g = t3.b + ((globals.k1_tf * t3.r + globals.k2_tf * t3.g + 0x80) >> 8);
                    TEX->b = t3.b + ((globals.k3_tf * t3.r + 0x80) >> 8);
                    TEX->a = t3.b;
                }
                else
                {
                    TEX->r = t0.b + ((globals.k0_tf * t3.g + 0x80) >> 8);
                    TEX->g = t0.b + ((globals.k1_tf * t3.r + globals.k2_tf * t3.g + 0x80) >> 8);
                    TEX->b = t0.b + ((globals.k3_tf * t3.r + 0x80) >> 8);
                    TEX->a = t0.b;
                }
            }
            else
            {
                if (upper)
                {
                    TEX->r = t3.b + ((globals.k0_tf * t0.g + 0x80) >> 8);
                    TEX->g = t3.b + ((globals.k1_tf * t0.r + globals.k2_tf * t0.g + 0x80) >> 8);
                    TEX->b = t3.b + ((globals.k3_tf * t0.r + 0x80) >> 8);
                    TEX->a = t3.b;
                }
                else
                {
                    TEX->r = t0.b + ((globals.k0_tf * t0.g + 0x80) >> 8);
                    TEX->g = t0.b + ((globals.k1_tf * t0.r + globals.k2_tf * t0.g + 0x80) >> 8);
                    TEX->b = t0.b + ((globals.k3_tf * t0.r + 0x80) >> 8);
                    TEX->a = t0.b;
                }
            }
        }

        TEX->r &= 0x1ff;
        TEX->g &= 0x1ff;
        TEX->b &= 0x1ff;
        TEX->a &= 0x1ff;


    }
    else
    {
        tcclamp_cycle_light(&sss1, &sst1, maxs, maxt, tilenum);

        tcmask(&sss1, &sst1, tilenum);


        fetch_texel(&t0, sss1, sst1, tilenum);

        if (bilerp)
        {
            if (!convert)
            {
                TEX->r = t0.r & 0x1ff;
                TEX->g = t0.g & 0x1ff;
                TEX->b = t0.b;
                TEX->a = t0.a;
            }
            else
                TEX->r = TEX->g = TEX->b = TEX->a = prev->b;
        }
        else
        {
            if (convert)
                t0 = *prev;

            TEX->r = t0.b + ((globals.k0_tf * t0.g + 0x80) >> 8);
            TEX->g = t0.b + ((globals.k1_tf * t0.r + globals.k2_tf * t0.g + 0x80) >> 8);
            TEX->b = t0.b + ((globals.k3_tf * t0.r + 0x80) >> 8);
            TEX->a = t0.b;
            TEX->r &= 0x1ff;
            TEX->g &= 0x1ff;
            TEX->b &= 0x1ff;
        }
    }

}

static void loading_pipeline(int start, int end, int tilenum, int coord_quad, int ltlut)
{
   int localdebugmode = 0, cnt = 0;
   int i, j;

   int dsinc, dtinc;
   dsinc = globals.spans.ds;
   dtinc = globals.spans.dt;

   int s, t;
   int ss, st;
   int xstart, xend, xendsc;
   int sss = 0, sst = 0;
   int ti_index, length;

   uint32_t tmemidx0 = 0, tmemidx1 = 0, tmemidx2 = 0, tmemidx3 = 0;
   int dswap = 0;
   uint32_t readval0, readval1, readval2, readval3;
   uint32_t readidx32;
   uint64_t loadqword;
   uint16_t tempshort;
   int tmem_formatting = 0;
   uint32_t bit3fl = 0, hibit = 0;

   if (end > start && ltlut)
   {
      rdp_pipeline_crashed = 1;
      return;
   }

   if (globals.tile[tilenum].format == FORMAT_YUV)
      tmem_formatting = 0;
   else if (globals.tile[tilenum].format == FORMAT_RGBA && globals.tile[tilenum].size == PIXEL_SIZE_32BIT)
      tmem_formatting = 1;
   else
      tmem_formatting = 2;

   int tiadvance = 0, spanadvance = 0;
   int tiptr = 0;

   switch (globals.ti_size)
   {
      case PIXEL_SIZE_4BIT:
         rdp_pipeline_crashed = 1;
         return;
      case PIXEL_SIZE_8BIT:
         tiadvance = 8;
         spanadvance = 8;
         break;
      case PIXEL_SIZE_16BIT:
         if (!ltlut)
         {
            tiadvance = 8;
            spanadvance = 4;
         }
         else
         {
            tiadvance = 2;
            spanadvance = 1;
         }
         break;
      case PIXEL_SIZE_32BIT:
         tiadvance = 8;
         spanadvance = 2;
         break;
   }

   for (i = start; i <= end; i++)
   {
      xstart   = globals.span[i].lx;
      xend     = globals.span[i].unscrx;
      xendsc   = globals.span[i].rx;
      s        = globals.span[i].s;
      t        = globals.span[i].t;

      ti_index = globals.ti_width * i + xend;
      tiptr    = globals.ti_address + PIXELS_TO_BYTES(ti_index, globals.ti_size);

      length   = (xstart - xend + 1) & 0xfff;

      for (j = 0; j < length; j+= spanadvance)
      {
         ss = s >> 16;
         st = t >> 16;

         sss = ss & 0xffff;
         sst = st & 0xffff;

         tc_pipeline_load(&sss, &sst, tilenum, coord_quad);

         dswap = sst & 1;

         get_tmem_idx(sss, sst, tilenum, &tmemidx0, &tmemidx1, &tmemidx2, &tmemidx3, &bit3fl, &hibit);

         readidx32 = (tiptr >> 2) & ~1;
         RREADIDX32(readval0, readidx32);
         readidx32++;
         RREADIDX32(readval1, readidx32);
         readidx32++;
         RREADIDX32(readval2, readidx32);
         readidx32++;
         RREADIDX32(readval3, readidx32);

         switch(tiptr & 7)
         {
            case 0:
               if (!ltlut)
                  loadqword = ((uint64_t)readval0 << 32) | readval1;
               else
               {
                  tempshort = readval0 >> 16;
                  loadqword = ((uint64_t)tempshort << 48) | ((uint64_t) tempshort << 32) | ((uint64_t) tempshort << 16) | tempshort;
               }
               break;
            case 1:
               loadqword = ((uint64_t)readval0 << 40) | ((uint64_t)readval1 << 8) | (readval2 >> 24);
               break;
            case 2:
               if (!ltlut)
                  loadqword = ((uint64_t)readval0 << 48) | ((uint64_t)readval1 << 16) | (readval2 >> 16);
               else
               {
                  tempshort = readval0 & 0xffff;
                  loadqword = ((uint64_t)tempshort << 48) | ((uint64_t) tempshort << 32) | ((uint64_t) tempshort << 16) | tempshort;
               }
               break;
            case 3:
               loadqword = ((uint64_t)readval0 << 56) | ((uint64_t)readval1 << 24) | (readval2 >> 8);
               break;
            case 4:
               if (!ltlut)
                  loadqword = ((uint64_t)readval1 << 32) | readval2;
               else
               {
                  tempshort = readval1 >> 16;
                  loadqword = ((uint64_t)tempshort << 48) | ((uint64_t) tempshort << 32) | ((uint64_t) tempshort << 16) | tempshort;
               }
               break;
            case 5:
               loadqword = ((uint64_t)readval1 << 40) | ((uint64_t)readval2 << 8) | (readval3 >> 24);
               break;
            case 6:
               if (!ltlut)
                  loadqword = ((uint64_t)readval1 << 48) | ((uint64_t)readval2 << 16) | (readval3 >> 16);
               else
               {
                  tempshort = readval1 & 0xffff;
                  loadqword = ((uint64_t)tempshort << 48) | ((uint64_t) tempshort << 32) | ((uint64_t) tempshort << 16) | tempshort;
               }
               break;
            case 7:
               loadqword = ((uint64_t)readval1 << 56) | ((uint64_t)readval2 << 24) | (readval3 >> 8);
               break;
         }


         switch(tmem_formatting)
         {
            case 0:
               readval0 = (uint32_t)((((loadqword >> 56) & 0xff) << 24) | (((loadqword >> 40) & 0xff) << 16) | (((loadqword >> 24) & 0xff) << 8) | (((loadqword >> 8) & 0xff) << 0));
               readval1 = (uint32_t)((((loadqword >> 48) & 0xff) << 24) | (((loadqword >> 32) & 0xff) << 16) | (((loadqword >> 16) & 0xff) << 8) | (((loadqword >> 0) & 0xff) << 0));
               if (bit3fl)
               {
                  tmem16[tmemidx2 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 >> 16);
                  tmem16[tmemidx3 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 & 0xffff);
                  tmem16[(tmemidx2 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 >> 16);
                  tmem16[(tmemidx3 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 & 0xffff);
               }
               else
               {
                  tmem16[tmemidx0 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 >> 16);
                  tmem16[tmemidx1 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 & 0xffff);
                  tmem16[(tmemidx0 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 >> 16);
                  tmem16[(tmemidx1 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 & 0xffff);
               }
               break;
            case 1:
               readval0 = (uint32_t)(((loadqword >> 48) << 16) | ((loadqword >> 16) & 0xffff));
               readval1 = (uint32_t)((((loadqword >> 32) & 0xffff) << 16) | (loadqword & 0xffff));

               if (bit3fl)
               {
                  tmem16[tmemidx2 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 >> 16);
                  tmem16[tmemidx3 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 & 0xffff);
                  tmem16[(tmemidx2 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 >> 16);
                  tmem16[(tmemidx3 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 & 0xffff);
               }
               else
               {
                  tmem16[tmemidx0 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 >> 16);
                  tmem16[tmemidx1 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 & 0xffff);
                  tmem16[(tmemidx0 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 >> 16);
                  tmem16[(tmemidx1 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 & 0xffff);
               }
               break;
            case 2:
               if (!dswap)
               {
                  if (!hibit)
                  {
                     tmem16[tmemidx0 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 48);
                     tmem16[tmemidx1 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 32);
                     tmem16[tmemidx2 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 16);
                     tmem16[tmemidx3 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword & 0xffff);
                  }
                  else
                  {
                     tmem16[(tmemidx0 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 48);
                     tmem16[(tmemidx1 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 32);
                     tmem16[(tmemidx2 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 16);
                     tmem16[(tmemidx3 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword & 0xffff);
                  }
               }
               else
               {
                  if (!hibit)
                  {
                     tmem16[tmemidx0 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 16);
                     tmem16[tmemidx1 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword & 0xffff);
                     tmem16[tmemidx2 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 48);
                     tmem16[tmemidx3 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 32);
                  }
                  else
                  {
                     tmem16[(tmemidx0 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 16);
                     tmem16[(tmemidx1 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword & 0xffff);
                     tmem16[(tmemidx2 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 48);
                     tmem16[(tmemidx3 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 32);
                  }
               }
               break;
         }


         s = (s + dsinc) & ~0x1f;
         t = (t + dtinc) & ~0x1f;
         tiptr += tiadvance;
      }
   }
}

#define ADJUST_ATTR_LOAD()                                      \
   {                                                               \
      globals.span[j].s = s & ~0x3ff;                                     \
      globals.span[j].t = t & ~0x3ff;                                     \
   }


#define ADDVALUES_LOAD() {  \
   t += dtde;      \
}

static void edgewalker_for_loads(int32_t* lewdata)
{
   int k = 0;

   int sign_dxhdy = 0;

   int do_offset = 0;

   int32_t maxxmx, minxhx;

   int spix = 0;
   int valid_y = 1;
   int length = 0;
   int32_t xrsc = 0, xlsc = 0, stickybit = 0;

   int xfrac = 0;
   int j = 0;
   int xleft = 0, xright = 0;
   int xstart = 0, xend = 0;

   int cmd_id = CMD_ID(lewdata);
   int ltlut = (cmd_id == CMD_ID_LOAD_TLUT);
   int coord_quad = ltlut || (cmd_id == CMD_ID_LOAD_BLOCK);
   int flip = 1;
   int tilenum = (lewdata[0] >> 16) & 7;

   int yl = SIGN(lewdata[0], 14);
   int ym = lewdata[1] >> 16;
   int yh = SIGN(lewdata[1], 14);
   int xl = SIGN(lewdata[2], 28);
   int xh = SIGN(lewdata[3], 28);
   int xm = SIGN(lewdata[4], 28);

   int dxldy = 0;
   int dxhdy = 0;
   int dxmdy = 0;

   int s    = lewdata[5] & 0xffff0000;
   int t    = (lewdata[5] & 0xffff) << 16;
   int w    = 0;

   int dsdx = (lewdata[7] & 0xffff0000) | ((lewdata[6] >> 16) & 0xffff);
   int dtdx = ((lewdata[7] << 16) & 0xffff0000)    | (lewdata[6] & 0xffff);
   int dsde = 0;
   int dtde = (lewdata[9] & 0xffff) << 16;
   int dsdy = 0;
   int dtdy = (lewdata[8] & 0xffff) << 16;

   ym        = SIGN(ym, 14);

   globals.max_level = 0;

   globals.spans.ds = dsdx & ~0x1f;
   globals.spans.dt = dtdx & ~0x1f;
   globals.spans.dw = 0;

   xright = xh & ~0x1;
   xleft = xm & ~0x1;

   int ycur  =  yh & ~3;
   int ylfar = yl | 3;

   int32_t yllimit = yl;
   int32_t yhlimit = yh;

   xfrac = 0;
   xend = xright >> 16;

   for (k = ycur; k <= ylfar; k++)
   {
      if (k == ym)
         xleft = xl & ~1;

      spix = k & 3;

      if (!(k & ~0xfff))
      {
         j = k >> 2;
         valid_y = !(k < yhlimit || k >= yllimit);

         if (spix == 0)
         {
            maxxmx = 0;
            minxhx = 0xfff;
         }

         xrsc = (xright >> 13) & 0x7ffe;



         xlsc = (xleft >> 13) & 0x7ffe;

         if (valid_y)
         {
            maxxmx = (((xlsc >> 3) & 0xfff) > maxxmx) ? (xlsc >> 3) & 0xfff : maxxmx;
            minxhx = (((xrsc >> 3) & 0xfff) < minxhx) ? (xrsc >> 3) & 0xfff : minxhx;
         }

         if (spix == 0)
         {
            globals.span[j].unscrx = xend;
            ADJUST_ATTR_LOAD();
         }

         if (spix == 3)
         {
            globals.span[j].lx = maxxmx;
            globals.span[j].rx = minxhx;


         }


      }

      if (spix == 3)
      {
         ADDVALUES_LOAD();
      }



   }

   loading_pipeline(yhlimit >> 2, yllimit >> 2, tilenum, coord_quad, ltlut);
}

static void rdp_set_tile_size(const uint32_t* args)
{
    int tilenum      = (args[1] >> 24) & 0x7;
    globals.tile[tilenum].sl = (args[0] >> 12) & 0xfff;
    globals.tile[tilenum].tl = (args[0] >>  0) & 0xfff;
    globals.tile[tilenum].sh = (args[1] >> 12) & 0xfff;
    globals.tile[tilenum].th = (args[1] >>  0) & 0xfff;

    calculate_clamp_diffs(tilenum);
}

static void rdp_load_block(const uint32_t* args)
{
   int32_t lewdata[10];
   int tilenum = (args[1] >> 24) & 0x7;
   int sl, sh, tl, dxt;

   globals.tile[tilenum].sl = sl = ((args[0] >> 12) & 0xfff);
   globals.tile[tilenum].tl = tl = ((args[0] >>  0) & 0xfff);
   globals.tile[tilenum].sh = sh = ((args[1] >> 12) & 0xfff);
   globals.tile[tilenum].th = dxt  = ((args[1] >>  0) & 0xfff);

   calculate_clamp_diffs(tilenum);

   int tlclamped = tl & 0x3ff;


   lewdata[0] = (args[0] & 0xff000000) | (0x10 << 19) | (tilenum << 16) | ((tlclamped << 2) | 3);
   lewdata[1] = (((tlclamped << 2) | 3) << 16) | (tlclamped << 2);
   lewdata[2] = sh << 16;
   lewdata[3] = sl << 16;
   lewdata[4] = sh << 16;
   lewdata[5] = ((sl << 3) << 16) | (tl << 3);
   lewdata[6] = (dxt & 0xff) << 8;
   lewdata[7] = ((0x80 >> globals.ti_size) << 16) | (dxt >> 8);
   lewdata[8] = 0x20;
   lewdata[9] = 0x20;

   edgewalker_for_loads(lewdata);

}

static void tile_tlut_common_cs_decoder(const uint32_t* args)
{
   int32_t lewdata[10];
   int tilenum = (args[1] >> 24) & 0x7;
   int sl, tl, sh, th;

   globals.tile[tilenum].sl = sl = ((args[0] >> 12) & 0xfff);
   globals.tile[tilenum].tl = tl = ((args[0] >>  0) & 0xfff);
   globals.tile[tilenum].sh = sh = ((args[1] >> 12) & 0xfff);
   globals.tile[tilenum].th = th = ((args[1] >>  0) & 0xfff);

   calculate_clamp_diffs(tilenum);



   lewdata[0] = (args[0] & 0xff000000) | (0x10 << 19) | (tilenum << 16) | (th | 3);
   lewdata[1] = ((th | 3) << 16) | (tl);
   lewdata[2] = ((sh >> 2) << 16) | ((sh & 3) << 14);
   lewdata[3] = ((sl >> 2) << 16) | ((sl & 3) << 14);
   lewdata[4] = ((sh >> 2) << 16) | ((sh & 3) << 14);
   lewdata[5] = ((sl << 3) << 16) | (tl << 3);
   lewdata[6] = 0;
   lewdata[7] = (0x200 >> globals.ti_size) << 16;
   lewdata[8] = 0x20;
   lewdata[9] = 0x20;

   edgewalker_for_loads(lewdata);
}

static void rdp_load_tlut(const uint32_t* args)
{
    tile_tlut_common_cs_decoder(args);
}

static void rdp_load_tile(const uint32_t* args)
{
    tile_tlut_common_cs_decoder(args);
}

static void rdp_set_tile(const uint32_t* args)
{
    int tilenum = (args[1] >> 24) & 0x7;

    globals.tile[tilenum].format    = (args[0] >> 21) & 0x7;
    globals.tile[tilenum].size      = (args[0] >> 19) & 0x3;
    globals.tile[tilenum].line      = (args[0] >>  9) & 0x1ff;
    globals.tile[tilenum].tmem      = (args[0] >>  0) & 0x1ff;
    globals.tile[tilenum].palette   = (args[1] >> 20) & 0xf;
    globals.tile[tilenum].ct        = (args[1] >> 19) & 0x1;
    globals.tile[tilenum].mt        = (args[1] >> 18) & 0x1;
    globals.tile[tilenum].mask_t    = (args[1] >> 14) & 0xf;
    globals.tile[tilenum].shift_t   = (args[1] >> 10) & 0xf;
    globals.tile[tilenum].cs        = (args[1] >>  9) & 0x1;
    globals.tile[tilenum].ms        = (args[1] >>  8) & 0x1;
    globals.tile[tilenum].mask_s    = (args[1] >>  4) & 0xf;
    globals.tile[tilenum].shift_s   = (args[1] >>  0) & 0xf;

    calculate_tile_derivs(tilenum);
}

static void rdp_set_texture_image(const uint32_t* args)
{
    globals.ti_format   = (args[0] >> 21) & 0x7;
    globals.ti_size     = (args[0] >> 19) & 0x3;
    globals.ti_width    = (args[0] & 0x3ff) + 1;
    globals.ti_address  = args[1] & 0x0ffffff;
}

static void rdp_set_convert(const uint32_t* args)
{
    int32_t k0 = (args[0] >> 13) & 0x1ff;
    int32_t k1 = (args[0] >> 4) & 0x1ff;
    int32_t k2 = ((args[0] & 0xf) << 5) | ((args[1] >> 27) & 0x1f);
    int32_t k3 = (args[1] >> 18) & 0x1ff;
    globals.k0_tf = (SIGN(k0, 9) << 1) + 1;
    globals.k1_tf = (SIGN(k1, 9) << 1) + 1;
    globals.k2_tf = (SIGN(k2, 9) << 1) + 1;
    globals.k3_tf = (SIGN(k3, 9) << 1) + 1;
    globals.k4    = (args[1] >> 9) & 0x1ff;
    globals.k5    = args[1] & 0x1ff;
}

static void tex_init(void)
{
    globals.ti_format  = FORMAT_RGBA;
    globals.ti_size    = PIXEL_SIZE_4BIT;
    globals.ti_width   = 0;
    globals.ti_address = 0;

    tmem_init();
    tcoord_init();
}

static STRICTINLINE int32_t normalize_dzpix(int32_t sum)
{
   int count;
   if (sum & 0xc000)
      return 0x8000;
   if (!(sum & 0xffff))
      return 1;

   if (sum == 1)
      return 3;

   for(count = 0x2000; count > 0; count >>= 1)
   {
      if (sum & count)
         return(count << 1);
   }
   msg_error("normalize_dzpix: invalid codepath taken");
   return 0;
}

static void replicate_for_copy(uint32_t* outbyte, uint32_t inshort, uint32_t nybbleoffset, uint32_t tilenum, uint32_t tformat, uint32_t tsize)
{
    uint32_t lownib, hinib;
    switch(tsize)
    {
    case PIXEL_SIZE_4BIT:
        lownib = (nybbleoffset ^ 3) << 2;
        lownib = hinib = (inshort >> lownib) & 0xf;
        if (tformat == FORMAT_CI)
        {
            *outbyte = (globals.tile[tilenum].palette << 4) | lownib;
        }
        else if (tformat == FORMAT_IA)
        {
            lownib = (lownib << 4) | lownib;
            *outbyte = (lownib & 0xe0) | ((lownib & 0xe0) >> 3) | ((lownib & 0xc0) >> 6);
        }
        else
            *outbyte = (lownib << 4) | lownib;
        break;
    case PIXEL_SIZE_8BIT:
        hinib = ((nybbleoffset ^ 3) | 1) << 2;
        if (tformat == FORMAT_IA)
        {
            lownib = (inshort >> hinib) & 0xf;
            *outbyte = (lownib << 4) | lownib;
        }
        else
        {
            lownib = (inshort >> (hinib & ~4)) & 0xf;
            hinib = (inshort >> hinib) & 0xf;
            *outbyte = (hinib << 4) | lownib;
        }
        break;
    default:
        *outbyte = (inshort >> 8) & 0xff;
        break;
    }
}

static void fetch_qword_copy(uint32_t* hidword, uint32_t* lowdword, int32_t ssss, int32_t ssst, uint32_t tilenum)
{
    uint32_t shorta, shortb, shortc, shortd;
    uint32_t sortshort[8];
    int hibits[6];
    int lowbits[6];
    int32_t sss = ssss, sst = ssst, sss1 = 0, sss2 = 0, sss3 = 0;
    int largetex = 0;
    uint32_t tformat, tsize;

    if (globals.other_modes.en_tlut)
    {
        tsize = PIXEL_SIZE_16BIT;
        tformat = globals.other_modes.tlut_type ? FORMAT_IA : FORMAT_RGBA;
    }
    else
    {
        tsize = globals.tile[tilenum].size;
        tformat = globals.tile[tilenum].format;
    }

    tc_pipeline_copy(&sss, &sss1, &sss2, &sss3, &sst, tilenum);
    read_tmem_copy(sss, sss1, sss2, sss3, sst, tilenum, sortshort, hibits, lowbits);
    largetex = (tformat == FORMAT_YUV || (tformat == FORMAT_RGBA && tsize == PIXEL_SIZE_32BIT));


    if (globals.other_modes.en_tlut)
    {
        shorta = sortshort[4];
        shortb = sortshort[5];
        shortc = sortshort[6];
        shortd = sortshort[7];
    }
    else if (largetex)
    {
        shorta = sortshort[0];
        shortb = sortshort[1];
        shortc = sortshort[2];
        shortd = sortshort[3];
    }
    else
    {
        shorta = hibits[0] ? sortshort[4] : sortshort[0];
        shortb = hibits[1] ? sortshort[5] : sortshort[1];
        shortc = hibits[3] ? sortshort[6] : sortshort[2];
        shortd = hibits[4] ? sortshort[7] : sortshort[3];
    }

    *lowdword = (shortc << 16) | shortd;

    if (tsize == PIXEL_SIZE_16BIT)
        *hidword = (shorta << 16) | shortb;
    else
    {
        replicate_for_copy(&shorta, shorta, lowbits[0] & 3, tilenum, tformat, tsize);
        replicate_for_copy(&shortb, shortb, lowbits[1] & 3, tilenum, tformat, tsize);
        replicate_for_copy(&shortc, shortc, lowbits[3] & 3, tilenum, tformat, tsize);
        replicate_for_copy(&shortd, shortd, lowbits[4] & 3, tilenum, tformat, tsize);
        *hidword = (shorta << 24) | (shortb << 16) | (shortc << 8) | shortd;
    }
}

static STRICTINLINE void rgbaz_correct_clip(int offx, int offy, int r, int g, int b, int a, int* z, uint32_t curpixel_cvg)
{
    unsigned temp;
    int sz = *z;
    int zanded;

    if (curpixel_cvg != 8)
    {
        int summand_r = offx * globals.spans.cdr + offy * globals.spans.drdy;
        int summand_g = offx * globals.spans.cdg + offy * globals.spans.dgdy;
        int summand_b = offx * globals.spans.cdb + offy * globals.spans.dbdy;
        int summand_a = offx * globals.spans.cda + offy * globals.spans.dady;
        int summand_z = offx * globals.spans.cdz + offy * globals.spans.dzdy;

        r = ((r << 2) + summand_r) >> 4;
        g = ((g << 2) + summand_g) >> 4;
        b = ((b << 2) + summand_b) >> 4;
        a = ((a << 2) + summand_a) >> 4;
        sz = ((sz << 2) + summand_z) >> 5;
    }
    else
    {
        r >>= 2;
        g >>= 2;
        b >>= 2;
        a >>= 2;
        sz >>= 3;
    }


    globals.shade_color.r = special_9bit_clamptable[r & 0x1ff];
    globals.shade_color.g = special_9bit_clamptable[g & 0x1ff];
    globals.shade_color.b = special_9bit_clamptable[b & 0x1ff];
    globals.shade_color.a = special_9bit_clamptable[a & 0x1ff];

    /* Should be a correct branchless version of the awkward
     * zanded in Angrylion. This pattern seems very similar
     * to the special_9bit clamp, hrm... */
    temp   = ((sz + 0x20000) & 0x7ffff) - 0x20000;
    zanded = CLAMP_AL(temp, 0, 0x3ffff);
     *z    = zanded;
}

static void render_spans_1cycle_complete(int start, int end, int tilenum, int flip)
{
    int zbcur;
    uint8_t offx, offy;
    struct spansigs sigs;
    uint32_t blend_en;
    uint32_t prewrap;
    uint32_t curpixel_cvg, curpixel_cvbit, curpixel_memcvg;
    int zb = globals.zb_address >> 1;

    int prim_tile = tilenum;
    int tile1 = tilenum;
    int newtile = tilenum;
    int news, newt;

    int i, j;

    int drinc, dginc, dbinc, dainc, dzinc, dsinc, dtinc, dwinc;
    int xinc;

    if (flip)
    {
        drinc = globals.spans.dr;
        dginc = globals.spans.dg;
        dbinc = globals.spans.db;
        dainc = globals.spans.da;
        dzinc = globals.spans.dz;
        dsinc = globals.spans.ds;
        dtinc = globals.spans.dt;
        dwinc = globals.spans.dw;
        xinc = 1;
    }
    else
    {
        drinc = -globals.spans.dr;
        dginc = -globals.spans.dg;
        dbinc = -globals.spans.db;
        dainc = -globals.spans.da;
        dzinc = -globals.spans.dz;
        dsinc = -globals.spans.ds;
        dtinc = -globals.spans.dt;
        dwinc = -globals.spans.dw;
        xinc = -1;
    }

    int dzpix;
    if (!globals.other_modes.z_source_sel)
        dzpix = globals.spans.dzpix;
    else
    {
        dzpix = globals.primitive_delta_z;
        dzinc = globals.spans.cdz = globals.spans.dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int cdith = 7, adith = 0;
    int r, g, b, a, z, s, t, w;
    int sr, sg, sb, sa, sz, ss, st, sw;
    int xstart, xend, xendsc;
    int sss = 0, sst = 0;
    int32_t prelodfrac;
    int curpixel = 0;
    int x, length, scdiff, lodlength;
    uint32_t fir, fig, fib;

    for (i = start; i <= end; i++)
    {
        if (globals.span[i].validline)
        {

        xstart = globals.span[i].lx;
        xend = globals.span[i].unscrx;
        xendsc = globals.span[i].rx;
        r = globals.span[i].r;
        g = globals.span[i].g;
        b = globals.span[i].b;
        a = globals.span[i].a;
        z = globals.other_modes.z_source_sel ? globals.primitive_z : globals.span[i].z;
        s = globals.span[i].s;
        t = globals.span[i].t;
        w = globals.span[i].w;

        x = xendsc;
        curpixel = globals.fb_width * i + x;
        zbcur = zb + curpixel;

        if (!flip)
        {
            length = xendsc - xstart;
            scdiff = xend - xendsc;
            compute_cvg_noflip(i);
        }
        else
        {
            length = xstart - xendsc;
            scdiff = xendsc - xend;
            compute_cvg_flip(i);
        }



        if (scdiff)
        {


            scdiff &= 0xfff;
            r += (drinc * scdiff);
            g += (dginc * scdiff);
            b += (dbinc * scdiff);
            a += (dainc * scdiff);
            z += (dzinc * scdiff);
            s += (dsinc * scdiff);
            t += (dtinc * scdiff);
            w += (dwinc * scdiff);
        }

        lodlength = length + scdiff;

        sigs.longspan = (lodlength > 7);
        sigs.midspan = (lodlength == 7);
        sigs.onelessthanmid = (lodlength == 6);

        sigs.startspan = 1;

        for (j = 0; j <= length; j++)
        {
            sr = r >> 14;
            sg = g >> 14;
            sb = b >> 14;
            sa = a >> 14;
            ss = s >> 16;
            st = t >> 16;
            sw = w >> 16;
            sz = (z >> 10) & 0x3fffff;


            sigs.endspan = (j == length);
            sigs.preendspan = (j == (length - 1));

            lookup_cvmask_derivatives(x, &offx, &offy, &curpixel_cvg, &curpixel_cvbit);


            get_texel1_1cycle(&news, &newt, s, t, w, dsinc, dtinc, dwinc, i, &sigs);



            if (!sigs.startspan)
            {
                globals.texel0_color = globals.texel1_color;
                globals.lod_frac = prelodfrac;
            }
            else
            {
                globals.tcdiv_ptr(ss, st, sw, &sss, &sst);

                tclod_1cycle_current(&sss, &sst, news, newt, s, t, w, dsinc, dtinc, dwinc, i, prim_tile, &tile1, &sigs);




                texture_pipeline_cycle(&globals.texel0_color, &globals.texel0_color, sss, sst, tile1, 0);


                sigs.startspan = 0;
            }

            sigs.nextspan = sigs.endspan;
            sigs.endspan = sigs.preendspan;
            sigs.preendspan = (j == (length - 2));

            s += dsinc;
            t += dtinc;
            w += dwinc;

            tclod_1cycle_next(&news, &newt, s, t, w, dsinc, dtinc, dwinc, i, prim_tile, &newtile, &sigs, &prelodfrac);

            texture_pipeline_cycle(&globals.texel1_color, &globals.texel1_color, news, newt, newtile, 0);

            rgbaz_correct_clip(offx, offy, sr, sg, sb, sa, &sz, curpixel_cvg);

            if (globals.other_modes.f.getditherlevel < 2)
                get_dither_noise(x, i, &cdith, &adith);

            combiner_1cycle(adith, &curpixel_cvg);

            globals.fbread1_ptr(curpixel, &curpixel_memcvg);

            if (z_compare(zbcur, sz, dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg, curpixel_memcvg))
            {
                if (blender_1cycle(&fir, &fig, &fib, cdith, blend_en, prewrap, curpixel_cvg, curpixel_cvbit))
                {
                    globals.fbwrite_ptr(curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg);
                    if (globals.other_modes.z_update_en)
                        z_store(zbcur, sz, dzpixenc);
                }
            }




            r += drinc;
            g += dginc;
            b += dbinc;
            a += dainc;
            z += dzinc;

            x += xinc;
            curpixel += xinc;
            zbcur += xinc;
        }
        }
    }
}


static void render_spans_1cycle_notexel1(int start, int end, int tilenum, int flip)
{
    int zbcur;
    uint8_t offx, offy;
    struct spansigs sigs;
    uint32_t blend_en;
    uint32_t prewrap;
    uint32_t curpixel_cvg, curpixel_cvbit, curpixel_memcvg;
    int zb = globals.zb_address >> 1;
    int prim_tile = tilenum;
    int tile1 = tilenum;

    int i, j;

    int drinc, dginc, dbinc, dainc, dzinc, dsinc, dtinc, dwinc;
    int xinc;
    if (flip)
    {
        drinc = globals.spans.dr;
        dginc = globals.spans.dg;
        dbinc = globals.spans.db;
        dainc = globals.spans.da;
        dzinc = globals.spans.dz;
        dsinc = globals.spans.ds;
        dtinc = globals.spans.dt;
        dwinc = globals.spans.dw;
        xinc = 1;
    }
    else
    {
        drinc = -globals.spans.dr;
        dginc = -globals.spans.dg;
        dbinc = -globals.spans.db;
        dainc = -globals.spans.da;
        dzinc = -globals.spans.dz;
        dsinc = -globals.spans.ds;
        dtinc = -globals.spans.dt;
        dwinc = -globals.spans.dw;
        xinc = -1;
    }

    int dzpix;
    if (!globals.other_modes.z_source_sel)
        dzpix = globals.spans.dzpix;
    else
    {
        dzpix = globals.primitive_delta_z;
        dzinc = globals.spans.cdz = globals.spans.dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int cdith = 7, adith = 0;
    int r, g, b, a, z, s, t, w;
    int sr, sg, sb, sa, sz, ss, st, sw;
    int xstart, xend, xendsc;
    int sss = 0, sst = 0;
    int curpixel = 0;
    int x, length, scdiff, lodlength;
    uint32_t fir, fig, fib;

    for (i = start; i <= end; i++)
    {
        if (globals.span[i].validline)
        {

        xstart = globals.span[i].lx;
        xend = globals.span[i].unscrx;
        xendsc = globals.span[i].rx;
        r = globals.span[i].r;
        g = globals.span[i].g;
        b = globals.span[i].b;
        a = globals.span[i].a;
        z = globals.other_modes.z_source_sel ? globals.primitive_z : globals.span[i].z;
        s = globals.span[i].s;
        t = globals.span[i].t;
        w = globals.span[i].w;

        x = xendsc;
        curpixel = globals.fb_width * i + x;
        zbcur = zb + curpixel;

        if (!flip)
        {
            length = xendsc - xstart;
            scdiff = xend - xendsc;
            compute_cvg_noflip(i);
        }
        else
        {
            length = xstart - xendsc;
            scdiff = xendsc - xend;
            compute_cvg_flip(i);
        }

        if (scdiff)
        {
            scdiff &= 0xfff;
            r += (drinc * scdiff);
            g += (dginc * scdiff);
            b += (dbinc * scdiff);
            a += (dainc * scdiff);
            z += (dzinc * scdiff);
            s += (dsinc * scdiff);
            t += (dtinc * scdiff);
            w += (dwinc * scdiff);
        }

        lodlength = length + scdiff;

        sigs.longspan = (lodlength > 7);
        sigs.midspan = (lodlength == 7);

        for (j = 0; j <= length; j++)
        {
            sr = r >> 14;
            sg = g >> 14;
            sb = b >> 14;
            sa = a >> 14;
            ss = s >> 16;
            st = t >> 16;
            sw = w >> 16;
            sz = (z >> 10) & 0x3fffff;

            sigs.endspan = (j == length);
            sigs.preendspan = (j == (length - 1));

            lookup_cvmask_derivatives(x, &offx, &offy, &curpixel_cvg, &curpixel_cvbit);

            globals.tcdiv_ptr(ss, st, sw, &sss, &sst);

            tclod_1cycle_current_simple(&sss, &sst, s, t, w, dsinc, dtinc, dwinc, i, prim_tile, &tile1, &sigs);

            texture_pipeline_cycle(&globals.texel0_color, &globals.texel0_color, sss, sst, tile1, 0);

            rgbaz_correct_clip(offx, offy, sr, sg, sb, sa, &sz, curpixel_cvg);

            if (globals.other_modes.f.getditherlevel < 2)
                get_dither_noise(x, i, &cdith, &adith);

            combiner_1cycle(adith, &curpixel_cvg);

            globals.fbread1_ptr(curpixel, &curpixel_memcvg);

            if (z_compare(zbcur, sz, dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg, curpixel_memcvg))
            {
                if (blender_1cycle(&fir, &fig, &fib, cdith, blend_en, prewrap, curpixel_cvg, curpixel_cvbit))
                {
                    globals.fbwrite_ptr(curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg);
                    if (globals.other_modes.z_update_en)
                        z_store(zbcur, sz, dzpixenc);
                }
            }

            s += dsinc;
            t += dtinc;
            w += dwinc;
            r += drinc;
            g += dginc;
            b += dbinc;
            a += dainc;
            z += dzinc;

            x += xinc;
            curpixel += xinc;
            zbcur += xinc;
        }
        }
    }
}


static void render_spans_1cycle_notex(int start, int end, int tilenum, int flip)
{
    int zbcur;
    uint8_t offx, offy;
    uint32_t blend_en;
    uint32_t prewrap;
    uint32_t curpixel_cvg, curpixel_cvbit, curpixel_memcvg;
    int i, j;

    int drinc, dginc, dbinc, dainc, dzinc;
    int xinc;
    int zb = globals.zb_address >> 1;

    if (flip)
    {
        drinc = globals.spans.dr;
        dginc = globals.spans.dg;
        dbinc = globals.spans.db;
        dainc = globals.spans.da;
        dzinc = globals.spans.dz;
        xinc = 1;
    }
    else
    {
        drinc = -globals.spans.dr;
        dginc = -globals.spans.dg;
        dbinc = -globals.spans.db;
        dainc = -globals.spans.da;
        dzinc = -globals.spans.dz;
        xinc = -1;
    }

    int dzpix;
    if (!globals.other_modes.z_source_sel)
        dzpix = globals.spans.dzpix;
    else
    {
        dzpix = globals.primitive_delta_z;
        dzinc = globals.spans.cdz = globals.spans.dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int cdith = 7, adith = 0;
    int r, g, b, a, z;
    int sr, sg, sb, sa, sz;
    int xstart, xend, xendsc;
    int curpixel = 0;
    int x, length, scdiff;
    uint32_t fir, fig, fib;

    for (i = start; i <= end; i++)
    {
        if (globals.span[i].validline)
        {

        xstart = globals.span[i].lx;
        xend = globals.span[i].unscrx;
        xendsc = globals.span[i].rx;
        r = globals.span[i].r;
        g = globals.span[i].g;
        b = globals.span[i].b;
        a = globals.span[i].a;
        z = globals.other_modes.z_source_sel ? globals.primitive_z : globals.span[i].z;

        x = xendsc;
        curpixel = globals.fb_width * i + x;
        zbcur = zb + curpixel;

        if (!flip)
        {
            length = xendsc - xstart;
            scdiff = xend - xendsc;
            compute_cvg_noflip(i);
        }
        else
        {
            length = xstart - xendsc;
            scdiff = xendsc - xend;
            compute_cvg_flip(i);
        }

        if (scdiff)
        {
            scdiff &= 0xfff;
            r += (drinc * scdiff);
            g += (dginc * scdiff);
            b += (dbinc * scdiff);
            a += (dainc * scdiff);
            z += (dzinc * scdiff);
        }

        for (j = 0; j <= length; j++)
        {
            sr = r >> 14;
            sg = g >> 14;
            sb = b >> 14;
            sa = a >> 14;
            sz = (z >> 10) & 0x3fffff;

            lookup_cvmask_derivatives(x, &offx, &offy, &curpixel_cvg, &curpixel_cvbit);

            rgbaz_correct_clip(offx, offy, sr, sg, sb, sa, &sz, curpixel_cvg);

            if (globals.other_modes.f.getditherlevel < 2)
                get_dither_noise(x, i, &cdith, &adith);

            combiner_1cycle(adith, &curpixel_cvg);

            globals.fbread1_ptr(curpixel, &curpixel_memcvg);

            if (z_compare(zbcur, sz, dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg, curpixel_memcvg))
            {
                if (blender_1cycle(&fir, &fig, &fib, cdith, blend_en, prewrap, curpixel_cvg, curpixel_cvbit))
                {
                    globals.fbwrite_ptr(curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg);
                    if (globals.other_modes.z_update_en)
                        z_store(zbcur, sz, dzpixenc);
                }
            }
            r += drinc;
            g += dginc;
            b += dbinc;
            a += dainc;
            z += dzinc;

            x += xinc;
            curpixel += xinc;
            zbcur += xinc;
        }
        }
    }
}

static void render_spans_2cycle_complete(int start, int end, int tilenum, int flip)
{
    int zbcur;
    uint8_t offx, offy;
    struct spansigs sigs;
    int32_t prelodfrac;
    struct color nexttexel1_color;
    uint32_t blend_en;
    uint32_t prewrap;
    uint32_t curpixel_cvg, curpixel_cvbit, curpixel_memcvg;
    int32_t acalpha;

    int zb = globals.zb_address >> 1;


    int tile2 = (tilenum + 1) & 7;
    int tile1 = tilenum;
    int prim_tile = tilenum;

    int newtile1 = tile1;
    int newtile2 = tile2;
    int news, newt;

    int i, j;

    int drinc, dginc, dbinc, dainc, dzinc, dsinc, dtinc, dwinc;
    int xinc;
    if (flip)
    {
        drinc = globals.spans.dr;
        dginc = globals.spans.dg;
        dbinc = globals.spans.db;
        dainc = globals.spans.da;
        dzinc = globals.spans.dz;
        dsinc = globals.spans.ds;
        dtinc = globals.spans.dt;
        dwinc = globals.spans.dw;
        xinc = 1;
    }
    else
    {
        drinc = -globals.spans.dr;
        dginc = -globals.spans.dg;
        dbinc = -globals.spans.db;
        dainc = -globals.spans.da;
        dzinc = -globals.spans.dz;
        dsinc = -globals.spans.ds;
        dtinc = -globals.spans.dt;
        dwinc = -globals.spans.dw;
        xinc = -1;
    }

    int dzpix;
    if (!globals.other_modes.z_source_sel)
        dzpix = globals.spans.dzpix;
    else
    {
        dzpix = globals.primitive_delta_z;
        dzinc = globals.spans.cdz = globals.spans.dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int cdith = 7, adith = 0;
    int r, g, b, a, z, s, t, w;
    int sr, sg, sb, sa, sz, ss, st, sw;
    int xstart, xend, xendsc;
    int sss = 0, sst = 0;
    int curpixel = 0;

    int x, length, scdiff;
    uint32_t fir, fig, fib;

    for (i = start; i <= end; i++)
    {
        if (globals.span[i].validline)
        {

        xstart = globals.span[i].lx;
        xend = globals.span[i].unscrx;
        xendsc = globals.span[i].rx;
        r = globals.span[i].r;
        g = globals.span[i].g;
        b = globals.span[i].b;
        a = globals.span[i].a;
        z = globals.other_modes.z_source_sel ? globals.primitive_z : globals.span[i].z;
        s = globals.span[i].s;
        t = globals.span[i].t;
        w = globals.span[i].w;

        x = xendsc;
        curpixel = globals.fb_width * i + x;
        zbcur = zb + curpixel;

        if (!flip)
        {
            length = xendsc - xstart;
            scdiff = xend - xendsc;
            compute_cvg_noflip(i);
        }
        else
        {
            length = xstart - xendsc;
            scdiff = xendsc - xend;
            compute_cvg_flip(i);
        }








        if (scdiff)
        {
            scdiff &= 0xfff;
            r += (drinc * scdiff);
            g += (dginc * scdiff);
            b += (dbinc * scdiff);
            a += (dainc * scdiff);
            z += (dzinc * scdiff);
            s += (dsinc * scdiff);
            t += (dtinc * scdiff);
            w += (dwinc * scdiff);
        }
        sigs.startspan = 1;

        for (j = 0; j <= length; j++)
        {
            sr = r >> 14;
            sg = g >> 14;
            sb = b >> 14;
            sa = a >> 14;
            ss = s >> 16;
            st = t >> 16;
            sw = w >> 16;
            sz = (z >> 10) & 0x3fffff;


            lookup_cvmask_derivatives(x, &offx, &offy, &curpixel_cvg, &curpixel_cvbit);

            get_nexttexel0_2cycle(&news, &newt, s, t, w, dsinc, dtinc, dwinc);

            if (!sigs.startspan)
            {
                globals.lod_frac = prelodfrac;
                globals.texel0_color = globals.nexttexel_color;
                globals.texel1_color = nexttexel1_color;
            }
            else
            {
                globals.tcdiv_ptr(ss, st, sw, &sss, &sst);

                tclod_2cycle_current(&sss, &sst, news, newt, s, t, w, dsinc, dtinc, dwinc, prim_tile, &tile1, &tile2);



                texture_pipeline_cycle(&globals.texel0_color, &globals.texel0_color, sss, sst, tile1, 0);
                texture_pipeline_cycle(&globals.texel1_color, &globals.texel0_color, sss, sst, tile2, 1);

                sigs.startspan = 0;
            }

            s += dsinc;
            t += dtinc;
            w += dwinc;

            tclod_2cycle_next(&news, &newt, s, t, w, dsinc, dtinc, dwinc, prim_tile, &newtile1, &newtile2, &prelodfrac);

            texture_pipeline_cycle(&globals.nexttexel_color, &globals.nexttexel_color, news, newt, newtile1, 0);
            texture_pipeline_cycle(&nexttexel1_color, &globals.nexttexel_color, news, newt, newtile2, 1);

            rgbaz_correct_clip(offx, offy, sr, sg, sb, sa, &sz, curpixel_cvg);

            if (globals.other_modes.f.getditherlevel < 2)
                get_dither_noise(x, i, &cdith, &adith);

            combiner_2cycle(adith, &curpixel_cvg, &acalpha);

            globals.fbread2_ptr(curpixel, &curpixel_memcvg);

            if (z_compare(zbcur, sz, dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg, curpixel_memcvg))
            {
                if (blender_2cycle(&fir, &fig, &fib, cdith, blend_en, prewrap, curpixel_cvg, curpixel_cvbit, acalpha))
                {
                    globals.fbwrite_ptr(curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg);
                    if (globals.other_modes.z_update_en)
                        z_store(zbcur, sz, dzpixenc);
                }
            }
            else
                globals.memory_color = globals.pre_memory_color;

            r += drinc;
            g += dginc;
            b += dbinc;
            a += dainc;
            z += dzinc;

            x += xinc;
            curpixel += xinc;
            zbcur += xinc;
        }
        }
    }
}



static void render_spans_2cycle_notexelnext(int start, int end, int tilenum, int flip)
{
    int zbcur;
    uint8_t offx, offy;
    uint32_t blend_en;
    uint32_t prewrap;
    uint32_t curpixel_cvg, curpixel_cvbit, curpixel_memcvg;
    int32_t acalpha;

    int tile2 = (tilenum + 1) & 7;
    int tile1 = tilenum;
    int prim_tile = tilenum;

    int i, j;

    int drinc, dginc, dbinc, dainc, dzinc, dsinc, dtinc, dwinc;
    int xinc;
    int zb = globals.zb_address >> 1;
    if (flip)
    {
        drinc = globals.spans.dr;
        dginc = globals.spans.dg;
        dbinc = globals.spans.db;
        dainc = globals.spans.da;
        dzinc = globals.spans.dz;
        dsinc = globals.spans.ds;
        dtinc = globals.spans.dt;
        dwinc = globals.spans.dw;
        xinc = 1;
    }
    else
    {
        drinc = -globals.spans.dr;
        dginc = -globals.spans.dg;
        dbinc = -globals.spans.db;
        dainc = -globals.spans.da;
        dzinc = -globals.spans.dz;
        dsinc = -globals.spans.ds;
        dtinc = -globals.spans.dt;
        dwinc = -globals.spans.dw;
        xinc = -1;
    }

    int dzpix;
    if (!globals.other_modes.z_source_sel)
        dzpix = globals.spans.dzpix;
    else
    {
        dzpix = globals.primitive_delta_z;
        dzinc = globals.spans.cdz = globals.spans.dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int cdith = 7, adith = 0;
    int r, g, b, a, z, s, t, w;
    int sr, sg, sb, sa, sz, ss, st, sw;
    int xstart, xend, xendsc;
    int sss = 0, sst = 0;
    int curpixel = 0;

    int x, length, scdiff;
    uint32_t fir, fig, fib;

    for (i = start; i <= end; i++)
    {
        if (globals.span[i].validline)
        {

        xstart = globals.span[i].lx;
        xend = globals.span[i].unscrx;
        xendsc = globals.span[i].rx;
        r = globals.span[i].r;
        g = globals.span[i].g;
        b = globals.span[i].b;
        a = globals.span[i].a;
        z = globals.other_modes.z_source_sel ? globals.primitive_z : globals.span[i].z;
        s = globals.span[i].s;
        t = globals.span[i].t;
        w = globals.span[i].w;

        x = xendsc;
        curpixel = globals.fb_width * i + x;
        zbcur = zb + curpixel;

        if (!flip)
        {
            length = xendsc - xstart;
            scdiff = xend - xendsc;
            compute_cvg_noflip(i);
        }
        else
        {
            length = xstart - xendsc;
            scdiff = xendsc - xend;
            compute_cvg_flip(i);
        }

        if (scdiff)
        {
            scdiff &= 0xfff;
            r += (drinc * scdiff);
            g += (dginc * scdiff);
            b += (dbinc * scdiff);
            a += (dainc * scdiff);
            z += (dzinc * scdiff);
            s += (dsinc * scdiff);
            t += (dtinc * scdiff);
            w += (dwinc * scdiff);
        }

        for (j = 0; j <= length; j++)
        {
            sr = r >> 14;
            sg = g >> 14;
            sb = b >> 14;
            sa = a >> 14;
            ss = s >> 16;
            st = t >> 16;
            sw = w >> 16;
            sz = (z >> 10) & 0x3fffff;

            lookup_cvmask_derivatives(x, &offx, &offy, &curpixel_cvg, &curpixel_cvbit);

            globals.tcdiv_ptr(ss, st, sw, &sss, &sst);

            tclod_2cycle_current_simple(&sss, &sst, s, t, w, dsinc, dtinc, dwinc, prim_tile, &tile1, &tile2);

            texture_pipeline_cycle(&globals.texel0_color, &globals.texel0_color, sss, sst, tile1, 0);
            texture_pipeline_cycle(&globals.texel1_color, &globals.texel0_color, sss, sst, tile2, 1);

            rgbaz_correct_clip(offx, offy, sr, sg, sb, sa, &sz, curpixel_cvg);

            if (globals.other_modes.f.getditherlevel < 2)
                get_dither_noise(x, i, &cdith, &adith);

            combiner_2cycle(adith, &curpixel_cvg, &acalpha);

            globals.fbread2_ptr(curpixel, &curpixel_memcvg);

            if (z_compare(zbcur, sz, dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg, curpixel_memcvg))
            {
                if (blender_2cycle(&fir, &fig, &fib, cdith, blend_en, prewrap, curpixel_cvg, curpixel_cvbit, acalpha))
                {
                    globals.fbwrite_ptr(curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg);
                    if (globals.other_modes.z_update_en)
                        z_store(zbcur, sz, dzpixenc);
                }
            }
            else
                globals.memory_color = globals.pre_memory_color;

            s += dsinc;
            t += dtinc;
            w += dwinc;
            r += drinc;
            g += dginc;
            b += dbinc;
            a += dainc;
            z += dzinc;

            x += xinc;
            curpixel += xinc;
            zbcur += xinc;
        }
        }
    }
}


static void render_spans_2cycle_notexel1(int start, int end, int tilenum, int flip)
{
    int zbcur;
    uint8_t offx, offy;
    uint32_t blend_en;
    uint32_t prewrap;
    uint32_t curpixel_cvg, curpixel_cvbit, curpixel_memcvg;
    int32_t acalpha;

    int tile1 = tilenum;
    int prim_tile = tilenum;

    int i, j;

    int drinc, dginc, dbinc, dainc, dzinc, dsinc, dtinc, dwinc;
    int xinc;
    int zb = globals.zb_address >> 1;
    if (flip)
    {
        drinc = globals.spans.dr;
        dginc = globals.spans.dg;
        dbinc = globals.spans.db;
        dainc = globals.spans.da;
        dzinc = globals.spans.dz;
        dsinc = globals.spans.ds;
        dtinc = globals.spans.dt;
        dwinc = globals.spans.dw;
        xinc = 1;
    }
    else
    {
        drinc = -globals.spans.dr;
        dginc = -globals.spans.dg;
        dbinc = -globals.spans.db;
        dainc = -globals.spans.da;
        dzinc = -globals.spans.dz;
        dsinc = -globals.spans.ds;
        dtinc = -globals.spans.dt;
        dwinc = -globals.spans.dw;
        xinc = -1;
    }

    int dzpix;
    if (!globals.other_modes.z_source_sel)
        dzpix = globals.spans.dzpix;
    else
    {
        dzpix = globals.primitive_delta_z;
        dzinc = globals.spans.cdz = globals.spans.dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int cdith = 7, adith = 0;
    int r, g, b, a, z, s, t, w;
    int sr, sg, sb, sa, sz, ss, st, sw;
    int xstart, xend, xendsc;
    int sss = 0, sst = 0;
    int curpixel = 0;

    int x, length, scdiff;
    uint32_t fir, fig, fib;

    for (i = start; i <= end; i++)
    {
        if (globals.span[i].validline)
        {

        xstart = globals.span[i].lx;
        xend = globals.span[i].unscrx;
        xendsc = globals.span[i].rx;
        r = globals.span[i].r;
        g = globals.span[i].g;
        b = globals.span[i].b;
        a = globals.span[i].a;
        z = globals.other_modes.z_source_sel ? globals.primitive_z : globals.span[i].z;
        s = globals.span[i].s;
        t = globals.span[i].t;
        w = globals.span[i].w;

        x = xendsc;
        curpixel = globals.fb_width * i + x;
        zbcur = zb + curpixel;

        if (!flip)
        {
            length = xendsc - xstart;
            scdiff = xend - xendsc;
            compute_cvg_noflip(i);
        }
        else
        {
            length = xstart - xendsc;
            scdiff = xendsc - xend;
            compute_cvg_flip(i);
        }

        if (scdiff)
        {
            scdiff &= 0xfff;
            r += (drinc * scdiff);
            g += (dginc * scdiff);
            b += (dbinc * scdiff);
            a += (dainc * scdiff);
            z += (dzinc * scdiff);
            s += (dsinc * scdiff);
            t += (dtinc * scdiff);
            w += (dwinc * scdiff);
        }

        for (j = 0; j <= length; j++)
        {
            sr = r >> 14;
            sg = g >> 14;
            sb = b >> 14;
            sa = a >> 14;
            ss = s >> 16;
            st = t >> 16;
            sw = w >> 16;
            sz = (z >> 10) & 0x3fffff;

            lookup_cvmask_derivatives(x, &offx, &offy, &curpixel_cvg, &curpixel_cvbit);

            globals.tcdiv_ptr(ss, st, sw, &sss, &sst);

            tclod_2cycle_current_notexel1(&sss, &sst, s, t, w, dsinc, dtinc, dwinc, prim_tile, &tile1);


            texture_pipeline_cycle(&globals.texel0_color, &globals.texel0_color, sss, sst, tile1, 0);

            rgbaz_correct_clip(offx, offy, sr, sg, sb, sa, &sz, curpixel_cvg);

            if (globals.other_modes.f.getditherlevel < 2)
                get_dither_noise(x, i, &cdith, &adith);

            combiner_2cycle(adith, &curpixel_cvg, &acalpha);

            globals.fbread2_ptr(curpixel, &curpixel_memcvg);

            if (z_compare(zbcur, sz, dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg, curpixel_memcvg))
            {
                if (blender_2cycle(&fir, &fig, &fib, cdith, blend_en, prewrap, curpixel_cvg, curpixel_cvbit, acalpha))
                {
                    globals.fbwrite_ptr(curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg);
                    if (globals.other_modes.z_update_en)
                        z_store(zbcur, sz, dzpixenc);
                }

            }
            else
                globals.memory_color = globals.pre_memory_color;

            s += dsinc;
            t += dtinc;
            w += dwinc;
            r += drinc;
            g += dginc;
            b += dbinc;
            a += dainc;
            z += dzinc;

            x += xinc;
            curpixel += xinc;
            zbcur += xinc;
        }
        }
    }
}


static void render_spans_2cycle_notex(int start, int end, int tilenum, int flip)
{
    int zbcur;
    uint8_t offx, offy;
    int i, j;
    uint32_t blend_en;
    uint32_t prewrap;
    uint32_t curpixel_cvg, curpixel_cvbit, curpixel_memcvg;
    int32_t acalpha;

    int drinc, dginc, dbinc, dainc, dzinc;
    int xinc;
    int zb = globals.zb_address >> 1;
    if (flip)
    {
        drinc = globals.spans.dr;
        dginc = globals.spans.dg;
        dbinc = globals.spans.db;
        dainc = globals.spans.da;
        dzinc = globals.spans.dz;
        xinc = 1;
    }
    else
    {
        drinc = -globals.spans.dr;
        dginc = -globals.spans.dg;
        dbinc = -globals.spans.db;
        dainc = -globals.spans.da;
        dzinc = -globals.spans.dz;
        xinc = -1;
    }

    int dzpix;
    if (!globals.other_modes.z_source_sel)
        dzpix = globals.spans.dzpix;
    else
    {
        dzpix = globals.primitive_delta_z;
        dzinc = globals.spans.cdz = globals.spans.dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int cdith = 7, adith = 0;
    int r, g, b, a, z;
    int sr, sg, sb, sa, sz;
    int xstart, xend, xendsc;
    int curpixel = 0;

    int x, length, scdiff;
    uint32_t fir, fig, fib;

    for (i = start; i <= end; i++)
    {
        if (globals.span[i].validline)
        {

        xstart = globals.span[i].lx;
        xend = globals.span[i].unscrx;
        xendsc = globals.span[i].rx;
        r = globals.span[i].r;
        g = globals.span[i].g;
        b = globals.span[i].b;
        a = globals.span[i].a;
        z = globals.other_modes.z_source_sel ? globals.primitive_z : globals.span[i].z;

        x = xendsc;
        curpixel = globals.fb_width * i + x;
        zbcur = zb + curpixel;

        if (!flip)
        {
            length = xendsc - xstart;
            scdiff = xend - xendsc;
            compute_cvg_noflip(i);
        }
        else
        {
            length = xstart - xendsc;
            scdiff = xendsc - xend;
            compute_cvg_flip(i);
        }

        if (scdiff)
        {
            scdiff &= 0xfff;
            r += (drinc * scdiff);
            g += (dginc * scdiff);
            b += (dbinc * scdiff);
            a += (dainc * scdiff);
            z += (dzinc * scdiff);
        }

        for (j = 0; j <= length; j++)
        {
            sr = r >> 14;
            sg = g >> 14;
            sb = b >> 14;
            sa = a >> 14;
            sz = (z >> 10) & 0x3fffff;

            lookup_cvmask_derivatives(x, &offx, &offy, &curpixel_cvg, &curpixel_cvbit);

            rgbaz_correct_clip(offx, offy, sr, sg, sb, sa, &sz, curpixel_cvg);

            if (globals.other_modes.f.getditherlevel < 2)
                get_dither_noise(x, i, &cdith, &adith);

            combiner_2cycle(adith, &curpixel_cvg, &acalpha);

            globals.fbread2_ptr(curpixel, &curpixel_memcvg);

            if (z_compare(zbcur, sz, dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg, curpixel_memcvg))
            {
                if (blender_2cycle(&fir, &fig, &fib, cdith, blend_en, prewrap, curpixel_cvg, curpixel_cvbit, acalpha))
                {
                    globals.fbwrite_ptr(curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg);
                    if (globals.other_modes.z_update_en)
                        z_store(zbcur, sz, dzpixenc);
                }
            }
            else
                globals.memory_color = globals.pre_memory_color;

            r += drinc;
            g += dginc;
            b += dbinc;
            a += dainc;
            z += dzinc;

            x += xinc;
            curpixel += xinc;
            zbcur += xinc;
        }
        }
    }
}

static void render_spans_fill(int start, int end, int flip)
{
    if (globals.fb_size == PIXEL_SIZE_4BIT)
    {
        rdp_pipeline_crashed = 1;
        return;
    }

    int i, j;

    int fastkillbits = globals.other_modes.image_read_en || globals.other_modes.z_compare_en;
    int slowkillbits = globals.other_modes.z_update_en && !globals.other_modes.z_source_sel && !fastkillbits;

    int xinc = flip ? 1 : -1;

    int xstart = 0, xendsc;
    int prevxstart;
    int curpixel = 0;
    int x, length;

    for (i = start; i <= end; i++)
    {
        prevxstart = xstart;
        xstart = globals.span[i].lx;
        xendsc = globals.span[i].rx;

        x = xendsc;
        curpixel = globals.fb_width * i + x;
        length = flip ? (xstart - xendsc) : (xendsc - xstart);

        if (globals.span[i].validline)
        {
            if (fastkillbits && length >= 0)
            {
                if (!onetimewarnings.fillmbitcrashes)
                    msg_warning("render_spans_fill: image_read_en %x z_update_en %x z_compare_en %x. RDP crashed",
                    globals.other_modes.image_read_en, globals.other_modes.z_update_en, globals.other_modes.z_compare_en);
                onetimewarnings.fillmbitcrashes = 1;
                rdp_pipeline_crashed = 1;
                return;
            }







            for (j = 0; j <= length; j++)
            {
               switch(globals.fb_size)
               {
                  case 0:
                     fbfill_4(curpixel);
                     break;
                  case 1:
                     fbfill_8(curpixel);
                     break;
                  case 2:
                     fbfill_16(curpixel);
                     break;
                  case 3:
                  default:
                     fbfill_32(curpixel);
                     break;
               }

               x += xinc;
               curpixel += xinc;
            }

            if (slowkillbits && length >= 0)
            {
                if (!onetimewarnings.fillmbitcrashes)
                    msg_warning("render_spans_fill: image_read_en %x z_update_en %x z_compare_en %x z_source_sel %x. RDP crashed",
                    globals.other_modes.image_read_en, globals.other_modes.z_update_en, globals.other_modes.z_compare_en, globals.other_modes.z_source_sel);
                onetimewarnings.fillmbitcrashes = 1;
                rdp_pipeline_crashed = 1;
                return;
            }
        }
    }
}

static void render_spans_copy(int start, int end, int tilenum, int flip)
{
    int i, j, k;

    if (globals.fb_size == PIXEL_SIZE_32BIT)
    {
        rdp_pipeline_crashed = 1;
        return;
    }

    int tile1 = tilenum;
    int prim_tile = tilenum;

    int dsinc, dtinc, dwinc;
    int xinc;
    if (flip)
    {
        dsinc = globals.spans.ds;
        dtinc = globals.spans.dt;
        dwinc = globals.spans.dw;
        xinc = 1;
    }
    else
    {
        dsinc = -globals.spans.ds;
        dtinc = -globals.spans.dt;
        dwinc = -globals.spans.dw;
        xinc = -1;
    }

    int xstart = 0, xendsc;
    int s = 0, t = 0, w = 0, ss = 0, st = 0, sw = 0, sss = 0, sst = 0, ssw = 0;
    int fb_index, length;
    int diff = 0;

    uint32_t hidword = 0, lowdword = 0;
    uint32_t hidword1 = 0, lowdword1 = 0;
    int fbadvance = (globals.fb_size == PIXEL_SIZE_4BIT) ? 8 : 16 >> globals.fb_size;
    uint32_t fbptr = 0;
    int fbptr_advance = flip ? 8 : -8;
    uint64_t copyqword = 0;
    uint32_t tempdword = 0, tempbyte = 0;
    int copywmask = 0, alphamask = 0;
    int bytesperpixel = (globals.fb_size == PIXEL_SIZE_4BIT) ? 1 : (1 << (globals.fb_size - 1));
    uint32_t fbendptr = 0;
    int32_t threshold, currthreshold;

#define PIXELS_TO_BYTES_SPECIAL4(pix, siz) ((siz) ? PIXELS_TO_BYTES(pix, siz) : (pix))

    for (i = start; i <= end; i++)
    {
        if (globals.span[i].validline)
        {

        s = globals.span[i].s;
        t = globals.span[i].t;
        w = globals.span[i].w;

        xstart = globals.span[i].lx;
        xendsc = globals.span[i].rx;

        fb_index = globals.fb_width * i + xendsc;
        fbptr = globals.fb_address + PIXELS_TO_BYTES_SPECIAL4(fb_index, globals.fb_size);
        fbendptr = globals.fb_address + PIXELS_TO_BYTES_SPECIAL4((globals.fb_width * i + xstart), globals.fb_size);
        length = flip ? (xstart - xendsc) : (xendsc - xstart);




        for (j = 0; j <= length; j += fbadvance)
        {
            ss = s >> 16;
            st = t >> 16;
            sw = w >> 16;

            globals.tcdiv_ptr(ss, st, sw, &sss, &sst);

            tclod_copy(&sss, &sst, s, t, w, dsinc, dtinc, dwinc, prim_tile, &tile1);



            fetch_qword_copy(&hidword, &lowdword, sss, sst, tile1);



            if (globals.fb_size == PIXEL_SIZE_16BIT || globals.fb_size == PIXEL_SIZE_8BIT)
                copyqword = ((uint64_t)hidword << 32) | ((uint64_t)lowdword);
            else
                copyqword = 0;


            if (!globals.other_modes.alpha_compare_en)
                alphamask = 0xff;
            else if (globals.fb_size == PIXEL_SIZE_16BIT)
            {
                alphamask = 0;
                alphamask |= (((copyqword >> 48) & 1) ? 0xC0 : 0);
                alphamask |= (((copyqword >> 32) & 1) ? 0x30 : 0);
                alphamask |= (((copyqword >> 16) & 1) ? 0xC : 0);
                alphamask |= ((copyqword & 1) ? 0x3 : 0);
            }
            else if (globals.fb_size == PIXEL_SIZE_8BIT)
            {
                alphamask = 0;
                threshold = (globals.other_modes.dither_alpha_en) ? (irand() & 0xff) : globals.blend_color.a;
                if (globals.other_modes.dither_alpha_en)
                {
                    currthreshold = threshold;
                    alphamask |= (((copyqword >> 24) & 0xff) >= currthreshold ? 0xC0 : 0);
                    currthreshold = ((threshold & 3) << 6) | (threshold >> 2);
                    alphamask |= (((copyqword >> 16) & 0xff) >= currthreshold ? 0x30 : 0);
                    currthreshold = ((threshold & 0xf) << 4) | (threshold >> 4);
                    alphamask |= (((copyqword >> 8) & 0xff) >= currthreshold ? 0xC : 0);
                    currthreshold = ((threshold & 0x3f) << 2) | (threshold >> 6);
                    alphamask |= ((copyqword & 0xff) >= currthreshold ? 0x3 : 0);
                }
                else
                {
                    alphamask |= (((copyqword >> 24) & 0xff) >= threshold ? 0xC0 : 0);
                    alphamask |= (((copyqword >> 16) & 0xff) >= threshold ? 0x30 : 0);
                    alphamask |= (((copyqword >> 8) & 0xff) >= threshold ? 0xC : 0);
                    alphamask |= ((copyqword & 0xff) >= threshold ? 0x3 : 0);
                }
            }
            else
                alphamask = 0;

            copywmask = (flip) ? (fbendptr - fbptr + bytesperpixel) : (fbptr - fbendptr + bytesperpixel);

            if (copywmask > 8)
                copywmask = 8;
            tempdword = fbptr;
            k = 7;
            while(copywmask > 0)
            {
                tempbyte = (uint32_t)((copyqword >> (k << 3)) & 0xff);
                if (alphamask & (1 << k))
                {
                    PAIRWRITE8(tempdword, tempbyte, (tempbyte & 1) ? 3 : 0);
                }
                k--;
                tempdword += xinc;
                copywmask--;
            }

            s += dsinc;
            t += dtinc;
            w += dwinc;
            fbptr += fbptr_advance;
        }
        }
    }
}

static void edgewalker_for_prims(int32_t* ewdata)
{
    int j = 0;
    int xleft = 0, xright = 0, xleft_inc = 0, xright_inc = 0;
    int r = 0, g = 0, b = 0, a = 0, z = 0, s = 0, t = 0, w = 0;
    int dr = 0, dg = 0, db = 0, da = 0;
    int drdx = 0, dgdx = 0, dbdx = 0, dadx = 0, dzdx = 0, dsdx = 0, dtdx = 0, dwdx = 0;
    int drdy = 0, dgdy = 0, dbdy = 0, dady = 0, dzdy = 0, dsdy = 0, dtdy = 0, dwdy = 0;
    int drde = 0, dgde = 0, dbde = 0, dade = 0, dzde = 0, dsde = 0, dtde = 0, dwde = 0;
    int tilenum = 0, flip = 0;
    int32_t yl = 0, ym = 0, yh = 0;
    int32_t xl = 0, xm = 0, xh = 0;
    int32_t dxldy = 0, dxhdy = 0, dxmdy = 0;

    if (globals.other_modes.f.stalederivs)
    {
        deduce_derivatives();
        globals.other_modes.f.stalederivs = 0;
    }


    flip = (ewdata[0] & 0x800000) != 0;
    globals.max_level = (ewdata[0] >> 19) & 7;
    tilenum = (ewdata[0] >> 16) & 7;


    yl = SIGN(ewdata[0], 14);
    ym = ewdata[1] >> 16;
    ym = SIGN(ym, 14);
    yh = SIGN(ewdata[1], 14);

    xl = SIGN(ewdata[2], 28);
    xh = SIGN(ewdata[4], 28);
    xm = SIGN(ewdata[6], 28);

    dxldy = SIGN(ewdata[3], 30);



    dxhdy = SIGN(ewdata[5], 30);
    dxmdy = SIGN(ewdata[7], 30);


    r    = (ewdata[8] & 0xffff0000) | ((ewdata[12] >> 16) & 0x0000ffff);
    g    = ((ewdata[8] << 16) & 0xffff0000) | (ewdata[12] & 0x0000ffff);
    b    = (ewdata[9] & 0xffff0000) | ((ewdata[13] >> 16) & 0x0000ffff);
    a    = ((ewdata[9] << 16) & 0xffff0000) | (ewdata[13] & 0x0000ffff);
    drdx = (ewdata[10] & 0xffff0000) | ((ewdata[14] >> 16) & 0x0000ffff);
    dgdx = ((ewdata[10] << 16) & 0xffff0000) | (ewdata[14] & 0x0000ffff);
    dbdx = (ewdata[11] & 0xffff0000) | ((ewdata[15] >> 16) & 0x0000ffff);
    dadx = ((ewdata[11] << 16) & 0xffff0000) | (ewdata[15] & 0x0000ffff);
    drde = (ewdata[16] & 0xffff0000) | ((ewdata[20] >> 16) & 0x0000ffff);
    dgde = ((ewdata[16] << 16) & 0xffff0000) | (ewdata[20] & 0x0000ffff);
    dbde = (ewdata[17] & 0xffff0000) | ((ewdata[21] >> 16) & 0x0000ffff);
    dade = ((ewdata[17] << 16) & 0xffff0000) | (ewdata[21] & 0x0000ffff);
    drdy = (ewdata[18] & 0xffff0000) | ((ewdata[22] >> 16) & 0x0000ffff);
    dgdy = ((ewdata[18] << 16) & 0xffff0000) | (ewdata[22] & 0x0000ffff);
    dbdy = (ewdata[19] & 0xffff0000) | ((ewdata[23] >> 16) & 0x0000ffff);
    dady = ((ewdata[19] << 16) & 0xffff0000) | (ewdata[23] & 0x0000ffff);


    s    = (ewdata[24] & 0xffff0000) | ((ewdata[28] >> 16) & 0x0000ffff);
    t    = ((ewdata[24] << 16) & 0xffff0000)    | (ewdata[28] & 0x0000ffff);
    w    = (ewdata[25] & 0xffff0000) | ((ewdata[29] >> 16) & 0x0000ffff);
    dsdx = (ewdata[26] & 0xffff0000) | ((ewdata[30] >> 16) & 0x0000ffff);
    dtdx = ((ewdata[26] << 16) & 0xffff0000)    | (ewdata[30] & 0x0000ffff);
    dwdx = (ewdata[27] & 0xffff0000) | ((ewdata[31] >> 16) & 0x0000ffff);
    dsde = (ewdata[32] & 0xffff0000) | ((ewdata[36] >> 16) & 0x0000ffff);
    dtde = ((ewdata[32] << 16) & 0xffff0000)    | (ewdata[36] & 0x0000ffff);
    dwde = (ewdata[33] & 0xffff0000) | ((ewdata[37] >> 16) & 0x0000ffff);
    dsdy = (ewdata[34] & 0xffff0000) | ((ewdata[38] >> 16) & 0x0000ffff);
    dtdy = ((ewdata[34] << 16) & 0xffff0000)    | (ewdata[38] & 0x0000ffff);
    dwdy = (ewdata[35] & 0xffff0000) | ((ewdata[39] >> 16) & 0x0000ffff);


    z    = ewdata[40];
    dzdx = ewdata[41];
    dzde = ewdata[42];
    dzdy = ewdata[43];







    globals.spans.ds = dsdx & ~0x1f;
    globals.spans.dt = dtdx & ~0x1f;
    globals.spans.dw = dwdx & ~0x1f;
    globals.spans.dr = drdx & ~0x1f;
    globals.spans.dg = dgdx & ~0x1f;
    globals.spans.db = dbdx & ~0x1f;
    globals.spans.da = dadx & ~0x1f;
    globals.spans.dz = dzdx;


    globals.spans.drdy = drdy >> 14;
    globals.spans.dgdy = dgdy >> 14;
    globals.spans.dbdy = dbdy >> 14;
    globals.spans.dady = dady >> 14;
    globals.spans.dzdy = dzdy >> 10;
    globals.spans.drdy = SIGN(globals.spans.drdy, 13);
    globals.spans.dgdy = SIGN(globals.spans.dgdy, 13);
    globals.spans.dbdy = SIGN(globals.spans.dbdy, 13);
    globals.spans.dady = SIGN(globals.spans.dady, 13);
    globals.spans.dzdy = SIGN(globals.spans.dzdy, 22);
    globals.spans.cdr = globals.spans.dr >> 14;
    globals.spans.cdr = SIGN(globals.spans.cdr, 13);
    globals.spans.cdg = globals.spans.dg >> 14;
    globals.spans.cdg = SIGN(globals.spans.cdg, 13);
    globals.spans.cdb = globals.spans.db >> 14;
    globals.spans.cdb = SIGN(globals.spans.cdb, 13);
    globals.spans.cda = globals.spans.da >> 14;
    globals.spans.cda = SIGN(globals.spans.cda, 13);
    globals.spans.cdz = globals.spans.dz >> 10;
    globals.spans.cdz = SIGN(globals.spans.cdz, 22);

    globals.spans.dsdy = dsdy & ~0x7fff;
    globals.spans.dtdy = dtdy & ~0x7fff;
    globals.spans.dwdy = dwdy & ~0x7fff;


    int dzdy_dz = (dzdy >> 16) & 0xffff;
    int dzdx_dz = (dzdx >> 16) & 0xffff;

    globals.spans.dzpix = ((dzdy_dz & 0x8000) ? ((~dzdy_dz) & 0x7fff) : dzdy_dz) + ((dzdx_dz & 0x8000) ? ((~dzdx_dz) & 0x7fff) : dzdx_dz);
    globals.spans.dzpix = normalize_dzpix(globals.spans.dzpix & 0xffff) & 0xffff;



    xleft_inc = (dxmdy >> 2) & ~0x1;
    xright_inc = (dxhdy >> 2) & ~0x1;



    xright = xh & ~0x1;
    xleft = xm & ~0x1;

    int k = 0;

    int dsdiff, dtdiff, dwdiff, drdiff, dgdiff, dbdiff, dadiff, dzdiff;
    int sign_dxhdy = (ewdata[5] & 0x80000000) != 0;

    int dsdeh, dtdeh, dwdeh, drdeh, dgdeh, dbdeh, dadeh, dzdeh, dsdyh, dtdyh, dwdyh, drdyh, dgdyh, dbdyh, dadyh, dzdyh;
    int do_offset = !(sign_dxhdy ^ flip);

    if (do_offset)
    {
        dsdeh = dsde & ~0x1ff;
        dtdeh = dtde & ~0x1ff;
        dwdeh = dwde & ~0x1ff;
        drdeh = drde & ~0x1ff;
        dgdeh = dgde & ~0x1ff;
        dbdeh = dbde & ~0x1ff;
        dadeh = dade & ~0x1ff;
        dzdeh = dzde & ~0x1ff;

        dsdyh = dsdy & ~0x1ff;
        dtdyh = dtdy & ~0x1ff;
        dwdyh = dwdy & ~0x1ff;
        drdyh = drdy & ~0x1ff;
        dgdyh = dgdy & ~0x1ff;
        dbdyh = dbdy & ~0x1ff;
        dadyh = dady & ~0x1ff;
        dzdyh = dzdy & ~0x1ff;







        dsdiff = dsdeh - (dsdeh >> 2) - dsdyh + (dsdyh >> 2);
        dtdiff = dtdeh - (dtdeh >> 2) - dtdyh + (dtdyh >> 2);
        dwdiff = dwdeh - (dwdeh >> 2) - dwdyh + (dwdyh >> 2);
        drdiff = drdeh - (drdeh >> 2) - drdyh + (drdyh >> 2);
        dgdiff = dgdeh - (dgdeh >> 2) - dgdyh + (dgdyh >> 2);
        dbdiff = dbdeh - (dbdeh >> 2) - dbdyh + (dbdyh >> 2);
        dadiff = dadeh - (dadeh >> 2) - dadyh + (dadyh >> 2);
        dzdiff = dzdeh - (dzdeh >> 2) - dzdyh + (dzdyh >> 2);

    }
    else
        dsdiff = dtdiff = dwdiff = drdiff = dgdiff = dbdiff = dadiff = dzdiff = 0;

    int xfrac = 0;

    int dsdxh, dtdxh, dwdxh, drdxh, dgdxh, dbdxh, dadxh, dzdxh;
    if (globals.other_modes.cycle_type != CYCLE_TYPE_COPY)
    {
        dsdxh = (dsdx >> 8) & ~1;
        dtdxh = (dtdx >> 8) & ~1;
        dwdxh = (dwdx >> 8) & ~1;
        drdxh = (drdx >> 8) & ~1;
        dgdxh = (dgdx >> 8) & ~1;
        dbdxh = (dbdx >> 8) & ~1;
        dadxh = (dadx >> 8) & ~1;
        dzdxh = (dzdx >> 8) & ~1;
    }
    else
        dsdxh = dtdxh = dwdxh = drdxh = dgdxh = dbdxh = dadxh = dzdxh = 0;





#define ADJUST_ATTR_PRIM()      \
{                           \
    globals.span[j].s = ((s & ~0x1ff) + dsdiff - (xfrac * dsdxh)) & ~0x3ff;             \
    globals.span[j].t = ((t & ~0x1ff) + dtdiff - (xfrac * dtdxh)) & ~0x3ff;             \
    globals.span[j].w = ((w & ~0x1ff) + dwdiff - (xfrac * dwdxh)) & ~0x3ff;             \
    globals.span[j].r = ((r & ~0x1ff) + drdiff - (xfrac * drdxh)) & ~0x3ff;             \
    globals.span[j].g = ((g & ~0x1ff) + dgdiff - (xfrac * dgdxh)) & ~0x3ff;             \
    globals.span[j].b = ((b & ~0x1ff) + dbdiff - (xfrac * dbdxh)) & ~0x3ff;             \
    globals.span[j].a = ((a & ~0x1ff) + dadiff - (xfrac * dadxh)) & ~0x3ff;             \
    globals.span[j].z = ((z & ~0x1ff) + dzdiff - (xfrac * dzdxh)) & ~0x3ff;             \
}


#define ADDVALUES_PRIM() {  \
            s += dsde;  \
            t += dtde;  \
            w += dwde; \
            r += drde; \
            g += dgde; \
            b += dbde; \
            a += dade; \
            z += dzde; \
}

    int32_t maxxmx, minxmx, maxxhx, minxhx;

    int spix = 0;
    int ycur =  yh & ~3;
    int ldflag = (sign_dxhdy ^ flip) ? 0 : 3;
    int invaly = 1;
    int length = 0;
    int32_t xrsc = 0, xlsc = 0, stickybit = 0;
    int32_t yllimit = 0, yhlimit = 0;
    if (yl & 0x2000)
        yllimit = 1;
    else if (yl & 0x1000)
        yllimit = 0;
    else
        yllimit = (yl & 0xfff) < globals.clip.yl;
    yllimit = yllimit ? yl : globals.clip.yl;

    int ylfar = yllimit | 3;
    if ((yl >> 2) > (ylfar >> 2))
        ylfar += 4;
    else if ((yllimit >> 2) >= 0 && (yllimit >> 2) < 1023)
        globals.span[(yllimit >> 2) + 1].validline = 0;


    if (yh & 0x2000)
        yhlimit = 0;
    else if (yh & 0x1000)
        yhlimit = 1;
    else
        yhlimit = (yh >= globals.clip.yh);
    yhlimit = yhlimit ? yh : globals.clip.yh;

    int yhclose = yhlimit & ~3;

    int32_t clipxlshift = globals.clip.xl << 1;
    int32_t clipxhshift = globals.clip.xh << 1;
    int allover = 1, allunder = 1, curover = 0, curunder = 0;
    int allinval = 1;
    int32_t curcross = 0;

    xfrac = ((xright >> 8) & 0xff);


    uint32_t worker_id = parallel_worker_id();
    uint32_t worker_num = parallel_worker_num();

    if (flip)
    {
    for (k = ycur; k <= ylfar; k++)
    {
        if (k == ym)
        {

            xleft = xl & ~1;
            xleft_inc = (dxldy >> 2) & ~1;
        }

        spix = k & 3;

        if (k >= yhclose)
        {
            invaly = k < yhlimit || k >= yllimit;

            j = k >> 2;

            if (spix == 0)
            {
                maxxmx = 0;
                minxhx = 0xfff;
                allover = allunder = 1;
                allinval = 1;
            }

            stickybit = ((xright >> 1) & 0x1fff) > 0;
            xrsc = ((xright >> 13) & 0x1ffe) | stickybit;


            curunder = ((xright & 0x8000000) || (xrsc < clipxhshift && !(xright & 0x4000000)));

            xrsc = curunder ? clipxhshift : (((xright >> 13) & 0x3ffe) | stickybit);
            curover = ((xrsc & 0x2000) || (xrsc & 0x1fff) >= clipxlshift);
            xrsc = curover ? clipxlshift : xrsc;
            globals.span[j].majorx[spix] = xrsc & 0x1fff;
            allover &= curover;
            allunder &= curunder;

            stickybit = ((xleft >> 1) & 0x1fff) > 0;
            xlsc = ((xleft >> 13) & 0x1ffe) | stickybit;
            curunder = ((xleft & 0x8000000) || (xlsc < clipxhshift && !(xleft & 0x4000000)));
            xlsc = curunder ? clipxhshift : (((xleft >> 13) & 0x3ffe) | stickybit);
            curover = ((xlsc & 0x2000) || (xlsc & 0x1fff) >= clipxlshift);
            xlsc = curover ? clipxlshift : xlsc;
            globals.span[j].minorx[spix] = xlsc & 0x1fff;
            allover &= curover;
            allunder &= curunder;



            curcross = ((xleft ^ (1 << 27)) & (0x3fff << 14)) < ((xright ^ (1 << 27)) & (0x3fff << 14));


            invaly |= curcross;
            globals.span[j].invalyscan[spix] = invaly;
            allinval &= invaly;

            if (!invaly)
            {
                maxxmx = (((xlsc >> 3) & 0xfff) > maxxmx) ? (xlsc >> 3) & 0xfff : maxxmx;
                minxhx = (((xrsc >> 3) & 0xfff) < minxhx) ? (xrsc >> 3) & 0xfff : minxhx;
            }

            if (spix == ldflag)
            {




                globals.span[j].unscrx = SIGN(xright >> 16, 12);
                xfrac = (xright >> 8) & 0xff;
                ADJUST_ATTR_PRIM();
            }

            if (spix == 3)
            {
                globals.span[j].lx = maxxmx;
                globals.span[j].rx = minxhx;
                globals.span[j].validline  = !allinval && !allover && !allunder && (!globals.scfield || (globals.scfield && !(globals.sckeepodd ^ (j & 1)))) && (!config->parallel || j % worker_num == worker_id);

            }


        }

        if (spix == 3)
        {
            ADDVALUES_PRIM();
        }



        xleft += xleft_inc;
        xright += xright_inc;

    }
    }
    else
    {
    for (k = ycur; k <= ylfar; k++)
    {
        if (k == ym)
        {
            xleft = xl & ~1;
            xleft_inc = (dxldy >> 2) & ~1;
        }

        spix = k & 3;

        if (k >= yhclose)
        {
            invaly = k < yhlimit || k >= yllimit;
            j = k >> 2;

            if (spix == 0)
            {
                maxxhx = 0;
                minxmx = 0xfff;
                allover = allunder = 1;
                allinval = 1;
            }

            stickybit = ((xright >> 1) & 0x1fff) > 0;
            xrsc = ((xright >> 13) & 0x1ffe) | stickybit;
            curunder = ((xright & 0x8000000) || (xrsc < clipxhshift && !(xright & 0x4000000)));
            xrsc = curunder ? clipxhshift : (((xright >> 13) & 0x3ffe) | stickybit);
            curover = ((xrsc & 0x2000) || (xrsc & 0x1fff) >= clipxlshift);
            xrsc = curover ? clipxlshift : xrsc;
            globals.span[j].majorx[spix] = xrsc & 0x1fff;
            allover &= curover;
            allunder &= curunder;

            stickybit = ((xleft >> 1) & 0x1fff) > 0;
            xlsc = ((xleft >> 13) & 0x1ffe) | stickybit;
            curunder = ((xleft & 0x8000000) || (xlsc < clipxhshift && !(xleft & 0x4000000)));
            xlsc = curunder ? clipxhshift : (((xleft >> 13) & 0x3ffe) | stickybit);
            curover = ((xlsc & 0x2000) || (xlsc & 0x1fff) >= clipxlshift);
            xlsc = curover ? clipxlshift : xlsc;
            globals.span[j].minorx[spix] = xlsc & 0x1fff;
            allover &= curover;
            allunder &= curunder;

            curcross = ((xright ^ (1 << 27)) & (0x3fff << 14)) < ((xleft ^ (1 << 27)) & (0x3fff << 14));

            invaly |= curcross;
            globals.span[j].invalyscan[spix] = invaly;
            allinval &= invaly;

            if (!invaly)
            {
                minxmx = (((xlsc >> 3) & 0xfff) < minxmx) ? (xlsc >> 3) & 0xfff : minxmx;
                maxxhx = (((xrsc >> 3) & 0xfff) > maxxhx) ? (xrsc >> 3) & 0xfff : maxxhx;
            }

            if (spix == ldflag)
            {
                globals.span[j].unscrx  = SIGN(xright >> 16, 12);
                xfrac = (xright >> 8) & 0xff;
                ADJUST_ATTR_PRIM();
            }

            if (spix == 3)
            {
                globals.span[j].lx = minxmx;
                globals.span[j].rx = maxxhx;
                globals.span[j].validline  = !allinval && !allover && !allunder && (!globals.scfield || (globals.scfield && !(globals.sckeepodd ^ (j & 1)))) && (!config->parallel || j % worker_num == worker_id);
            }

        }

        if (spix == 3)
        {
            ADDVALUES_PRIM();
        }

        xleft += xleft_inc;
        xright += xright_inc;

    }
    }




    switch(globals.other_modes.cycle_type)
    {
        case CYCLE_TYPE_1:
            switch (globals.other_modes.f.textureuselevel0)
            {
                case 0: render_spans_1cycle_complete(yhlimit >> 2, yllimit >> 2, tilenum, flip); break;
                case 1: render_spans_1cycle_notexel1(yhlimit >> 2, yllimit >> 2, tilenum, flip); break;
                case 2: default: render_spans_1cycle_notex(yhlimit >> 2, yllimit >> 2, tilenum, flip); break;
            }
            break;
        case CYCLE_TYPE_2:
            switch (globals.other_modes.f.textureuselevel1)
            {
                case 0: render_spans_2cycle_complete(yhlimit >> 2, yllimit >> 2, tilenum, flip); break;
                case 1: render_spans_2cycle_notexelnext(yhlimit >> 2, yllimit >> 2, tilenum, flip); break;
                case 2: render_spans_2cycle_notexel1(yhlimit >> 2, yllimit >> 2, tilenum, flip); break;
                case 3: default: render_spans_2cycle_notex(yhlimit >> 2, yllimit >> 2, tilenum, flip); break;
            }
            break;
        case CYCLE_TYPE_COPY: render_spans_copy(yhlimit >> 2, yllimit >> 2, tilenum, flip); break;
        case CYCLE_TYPE_FILL: render_spans_fill(yhlimit >> 2, yllimit >> 2, flip); break;
        default: msg_error("cycle_type %d", globals.other_modes.cycle_type); break;
    }


}

static void rasterizer_init(void)
{
    globals.clip.xl           = 0;
    globals.clip.yl           = 0;
    globals.clip.xh           = 0x2000;
    globals.clip.yh           = 0x2000;
    globals.scfield           = 0;
    globals.sckeepodd         = 0;
}

static void rdp_tri_noshade(const uint32_t* args)
{
    int32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 8 * sizeof(int32_t));
    memset(&ewdata[8], 0, 36 * sizeof(int32_t));
    edgewalker_for_prims(ewdata);
}

static void rdp_tri_noshade_z(const uint32_t* args)
{
    int32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 8 * sizeof(int32_t));
    memset(&ewdata[8], 0, 32 * sizeof(int32_t));
    memcpy(&ewdata[40], args + 8, 4 * sizeof(int32_t));
    edgewalker_for_prims(ewdata);
}

static void rdp_tri_tex(const uint32_t* args)
{
    int32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 8 * sizeof(int32_t));
    ewdata[8]  = 0;
    ewdata[9]  = 0;
    ewdata[10] = 0;
    ewdata[11] = 0;
    ewdata[12] = 0;
    ewdata[13] = 0;
    ewdata[14] = 0;
    ewdata[15] = 0;
    ewdata[16] = 0;
    ewdata[17] = 0;
    ewdata[18] = 0;
    ewdata[19] = 0;
    ewdata[20] = 0;
    ewdata[21] = 0;
    ewdata[22] = 0;
    ewdata[23] = 0;
    memcpy(&ewdata[24], args + 8, 16 * sizeof(int32_t));
    ewdata[40] = 0;
    ewdata[41] = 0;
    ewdata[42] = 0;
    ewdata[43] = 0;
    edgewalker_for_prims(ewdata);
}

static void rdp_tri_tex_z(const uint32_t* args)
{
    int32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 8 * sizeof(int32_t));
    ewdata[8]  = 0;
    ewdata[9]  = 0;
    ewdata[10] = 0;
    ewdata[11] = 0;
    ewdata[12] = 0;
    ewdata[13] = 0;
    ewdata[14] = 0;
    ewdata[15] = 0;
    ewdata[16] = 0;
    ewdata[17] = 0;
    ewdata[18] = 0;
    ewdata[19] = 0;
    ewdata[20] = 0;
    ewdata[21] = 0;
    ewdata[22] = 0;
    ewdata[23] = 0;
    memcpy(&ewdata[24], args + 8, 16 * sizeof(int32_t));
    memcpy(&ewdata[40], args + 24, 4 * sizeof(int32_t));






    edgewalker_for_prims(ewdata);


}

static void rdp_tri_shade(const uint32_t* args)
{
    int32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 24 * sizeof(int32_t));
    memset(&ewdata[24], 0, 20 * sizeof(int32_t));
    edgewalker_for_prims(ewdata);
}

static void rdp_tri_shade_z(const uint32_t* args)
{
    int32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 24 * sizeof(int32_t));
    memset(&ewdata[24], 0, 16 * sizeof(int32_t));
    memcpy(&ewdata[40], args + 24, 4 * sizeof(int32_t));
    edgewalker_for_prims(ewdata);
}

static void rdp_tri_texshade(const uint32_t* args)
{
    int32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 40 * sizeof(int32_t));
    ewdata[40] = 0;
    ewdata[41] = 0;
    ewdata[42] = 0;
    ewdata[43] = 0;
    edgewalker_for_prims(ewdata);
}

static void rdp_tri_texshade_z(const uint32_t* args)
{
    int32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, CMD_MAX_SIZE);





    edgewalker_for_prims(ewdata);


}

static void rdp_tex_rect(const uint32_t* args)
{
    uint32_t tilenum    = (args[1] >> 24) & 0x7;
    uint32_t xl = (args[0] >> 12) & 0xfff;
    uint32_t yl = (args[0] >>  0) & 0xfff;
    uint32_t xh = (args[1] >> 12) & 0xfff;
    uint32_t yh = (args[1] >>  0) & 0xfff;

    int32_t s = (args[2] >> 16) & 0xffff;
    int32_t t = (args[2] >>  0) & 0xffff;
    int32_t dsdx = (args[3] >> 16) & 0xffff;
    int32_t dtdy = (args[3] >>  0) & 0xffff;

    dsdx = SIGN16(dsdx);
    dtdy = SIGN16(dtdy);

    if (globals.other_modes.cycle_type == CYCLE_TYPE_FILL || globals.other_modes.cycle_type == CYCLE_TYPE_COPY)
        yl |= 3;

    uint32_t xlint = (xl >> 2) & 0x3ff;
    uint32_t xhint = (xh >> 2) & 0x3ff;

    int32_t ewdata[CMD_MAX_INTS];
    ewdata[0]  = (0x24 << 24) | ((0x80 | tilenum) << 16) | yl;
    ewdata[1]  = (yl << 16) | yh;
    ewdata[2]  = (xlint << 16) | ((xl & 3) << 14);
    ewdata[3]  = 0;
    ewdata[4]  = (xhint << 16) | ((xh & 3) << 14);
    ewdata[5]  = 0;
    ewdata[6]  = (xlint << 16) | ((xl & 3) << 14);
    ewdata[7]  = 0;
    ewdata[8]  = 0;
    ewdata[9]  = 0;
    ewdata[10] = 0;
    ewdata[11] = 0;
    ewdata[12] = 0;
    ewdata[13] = 0;
    ewdata[14] = 0;
    ewdata[15] = 0;
    ewdata[16] = 0;
    ewdata[17] = 0;
    ewdata[18] = 0;
    ewdata[19] = 0;
    ewdata[20] = 0;
    ewdata[21] = 0;
    ewdata[22] = 0;
    ewdata[23] = 0;
    ewdata[24] = (s << 16) | t;
    ewdata[25] = 0;
    ewdata[26] = ((dsdx >> 5) << 16);
    ewdata[27] = 0;
    ewdata[28] = 0;
    ewdata[29] = 0;
    ewdata[30] = ((dsdx & 0x1f) << 11) << 16;
    ewdata[31] = 0;
    ewdata[32] = (dtdy >> 5) & 0xffff;
    ewdata[33] = 0;
    ewdata[34] = (dtdy >> 5) & 0xffff;
    ewdata[35] = 0;
    ewdata[36] = (dtdy & 0x1f) << 11;
    ewdata[37] = 0;
    ewdata[38] = (dtdy & 0x1f) << 11;
    ewdata[39] = 0;
    ewdata[40] = 0;
    ewdata[41] = 0;
    ewdata[42] = 0;
    ewdata[43] = 0;



    edgewalker_for_prims(ewdata);

}

static void rdp_tex_rect_flip(const uint32_t* args)
{
    uint32_t tilenum    = (args[1] >> 24) & 0x7;
    uint32_t xl = (args[0] >> 12) & 0xfff;
    uint32_t yl = (args[0] >>  0) & 0xfff;
    uint32_t xh = (args[1] >> 12) & 0xfff;
    uint32_t yh = (args[1] >>  0) & 0xfff;

    int32_t s = (args[2] >> 16) & 0xffff;
    int32_t t = (args[2] >>  0) & 0xffff;
    int32_t dsdx = (args[3] >> 16) & 0xffff;
    int32_t dtdy = (args[3] >>  0) & 0xffff;

    dsdx = SIGN16(dsdx);
    dtdy = SIGN16(dtdy);

    if (globals.other_modes.cycle_type == CYCLE_TYPE_FILL || globals.other_modes.cycle_type == CYCLE_TYPE_COPY)
        yl |= 3;

    uint32_t xlint = (xl >> 2) & 0x3ff;
    uint32_t xhint = (xh >> 2) & 0x3ff;

    int32_t ewdata[CMD_MAX_INTS];
    ewdata[0] = (0x25 << 24) | ((0x80 | tilenum) << 16) | yl;
    ewdata[1] = (yl << 16) | yh;
    ewdata[2] = (xlint << 16) | ((xl & 3) << 14);
    ewdata[3] = 0;
    ewdata[4] = (xhint << 16) | ((xh & 3) << 14);
    ewdata[5] = 0;
    ewdata[6] = (xlint << 16) | ((xl & 3) << 14);
    ewdata[7] = 0;
    ewdata[8]  = 0;
    ewdata[9]  = 0;
    ewdata[10] = 0;
    ewdata[11] = 0;
    ewdata[12] = 0;
    ewdata[13] = 0;
    ewdata[14] = 0;
    ewdata[15] = 0;
    ewdata[16] = 0;
    ewdata[17] = 0;
    ewdata[18] = 0;
    ewdata[19] = 0;
    ewdata[20] = 0;
    ewdata[21] = 0;
    ewdata[22] = 0;
    ewdata[23] = 0;
    ewdata[24] = (s << 16) | t;
    ewdata[25] = 0;

    ewdata[26] = (dtdy >> 5) & 0xffff;
    ewdata[27] = 0;
    ewdata[28] = 0;
    ewdata[29] = 0;
    ewdata[30] = ((dtdy & 0x1f) << 11);
    ewdata[31] = 0;
    ewdata[32] = (dsdx >> 5) << 16;
    ewdata[33] = 0;
    ewdata[34] = (dsdx >> 5) << 16;
    ewdata[35] = 0;
    ewdata[36] = (dsdx & 0x1f) << 27;
    ewdata[37] = 0;
    ewdata[38] = (dsdx & 0x1f) << 27;
    ewdata[39] = 0;
    ewdata[39] = 0;
    ewdata[40] = 0;
    ewdata[41] = 0;
    ewdata[42] = 0;
    ewdata[43] = 0;

    edgewalker_for_prims(ewdata);
}

static void rdp_fill_rect(const uint32_t* args)
{
    uint32_t xl = (args[0] >> 12) & 0xfff;
    uint32_t yl = (args[0] >>  0) & 0xfff;
    uint32_t xh = (args[1] >> 12) & 0xfff;
    uint32_t yh = (args[1] >>  0) & 0xfff;

    if (globals.other_modes.cycle_type == CYCLE_TYPE_FILL || globals.other_modes.cycle_type == CYCLE_TYPE_COPY)
        yl |= 3;

    uint32_t xlint = (xl >> 2) & 0x3ff;
    uint32_t xhint = (xh >> 2) & 0x3ff;

    int32_t ewdata[CMD_MAX_INTS];
    ewdata[0] = (0x3680 << 16) | yl;
    ewdata[1] = (yl << 16) | yh;
    ewdata[2] = (xlint << 16) | ((xl & 3) << 14);
    ewdata[3] = 0;
    ewdata[4] = (xhint << 16) | ((xh & 3) << 14);
    ewdata[5] = 0;
    ewdata[6] = (xlint << 16) | ((xl & 3) << 14);
    ewdata[7] = 0;
    memset(&ewdata[8], 0, 36 * sizeof(int32_t));

    edgewalker_for_prims(ewdata);
}

static void rdp_set_prim_depth(const uint32_t* args)
{
    globals.primitive_z       = args[1] & (0x7fff << 16);
    globals.primitive_delta_z = (uint16_t)(args[1]);
}

static void rdp_set_scissor(const uint32_t* args)
{
    globals.clip.xh = (args[0] >> 12) & 0xfff;
    globals.clip.yh = (args[0] >>  0) & 0xfff;
    globals.clip.xl = (args[1] >> 12) & 0xfff;
    globals.clip.yl = (args[1] >>  0) & 0xfff;

    globals.scfield = (args[1] >> 25) & 1;
    globals.sckeepodd = (args[1] >> 24) & 1;
}

int rdp_init(struct core_config* _config)
{
   int i;
   uint32_t tmp[2] = {0};

   config = _config;
   rdp_set_other_modes(tmp);
   globals.other_modes.f.stalederivs = 1;

   globals.iseed = 1;
   memset(&globals, 0, sizeof(globals));
   memset(globals.tile, 0, sizeof(globals.tile));

   for (i = 0; i < 8; i++)
   {
      calculate_tile_derivs(i);
      calculate_clamp_diffs(i);
   }

   memset(&globals.combined_color, 0, sizeof(struct color));
   memset(&globals.prim_color, 0, sizeof(struct color));
   memset(&globals.env_color, 0, sizeof(struct color));
   memset(&globals.key_scale, 0, sizeof(struct color));
   memset(&globals.key_center, 0, sizeof(struct color));

   rdp_pipeline_crashed = 0;
   memset(&onetimewarnings, 0, sizeof(onetimewarnings));

   precalc_cvmask_derivatives();
   z_init();
   dither_init();
   fb_init();
   blender_init();
   combiner_init();
   tex_init();
   rasterizer_init();

   return 0;
}

static void rdp_invalid(const uint32_t* args)
{
}

static void rdp_noop(const uint32_t* args)
{
}

static void rdp_sync_load(const uint32_t* args)
{
}

static void rdp_sync_pipe(const uint32_t* args)
{
}

static void rdp_sync_tile(const uint32_t* args)
{
}

static void rdp_sync_full(const uint32_t* args)
{
    core_dp_sync();
}

static void rdp_set_other_modes(const uint32_t* args)
{
    globals.other_modes.cycle_type          = (args[0] >> 20) & 3;
    globals.other_modes.persp_tex_en        = (args[0] >> 19) & 1;
    globals.other_modes.detail_tex_en       = (args[0] >> 18) & 1;
    globals.other_modes.sharpen_tex_en      = (args[0] >> 17) & 1;
    globals.other_modes.tex_lod_en          = (args[0] >> 16) & 1;
    globals.other_modes.en_tlut             = (args[0] >> 15) & 1;
    globals.other_modes.tlut_type           = (args[0] >> 14) & 1;
    globals.other_modes.sample_type         = (args[0] >> 13) & 1;
    globals.other_modes.mid_texel           = (args[0] >> 12) & 1;
    globals.other_modes.bi_lerp0            = (args[0] >> 11) & 1;
    globals.other_modes.bi_lerp1            = (args[0] >> 10) & 1;
    globals.other_modes.convert_one         = (args[0] >>  9) & 1;
    globals.other_modes.key_en              = (args[0] >>  8) & 1;
    globals.other_modes.rgb_dither_sel      = (args[0] >>  6) & 3;
    globals.other_modes.alpha_dither_sel    = (args[0] >>  4) & 3;
    globals.other_modes.blend_m1a_0         = (args[1] >> 30) & 3;
    globals.other_modes.blend_m1a_1         = (args[1] >> 28) & 3;
    globals.other_modes.blend_m1b_0         = (args[1] >> 26) & 3;
    globals.other_modes.blend_m1b_1         = (args[1] >> 24) & 3;
    globals.other_modes.blend_m2a_0         = (args[1] >> 22) & 3;
    globals.other_modes.blend_m2a_1         = (args[1] >> 20) & 3;
    globals.other_modes.blend_m2b_0         = (args[1] >> 18) & 3;
    globals.other_modes.blend_m2b_1         = (args[1] >> 16) & 3;
    globals.other_modes.force_blend         = (args[1] >> 14) & 1;
    globals.other_modes.alpha_cvg_select    = (args[1] >> 13) & 1;
    globals.other_modes.cvg_times_alpha     = (args[1] >> 12) & 1;
    globals.other_modes.z_mode              = (args[1] >> 10) & 3;
    globals.other_modes.cvg_dest            = (args[1] >>  8) & 3;
    globals.other_modes.color_on_cvg        = (args[1] >>  7) & 1;
    globals.other_modes.image_read_en       = (args[1] >>  6) & 1;
    globals.other_modes.z_update_en         = (args[1] >>  5) & 1;
    globals.other_modes.z_compare_en        = (args[1] >>  4) & 1;
    globals.other_modes.antialias_en        = (args[1] >>  3) & 1;
    globals.other_modes.z_source_sel        = (args[1] >>  2) & 1;
    globals.other_modes.dither_alpha_en     = (args[1] >>  1) & 1;
    globals.other_modes.alpha_compare_en    = (args[1] >>  0) & 1;

    set_blender_input(0, 0, &globals.blender.i1a_r[0], &globals.blender.i1a_g[0], &globals.blender.i1a_b[0], &globals.blender.i1b_a[0],
                      globals.other_modes.blend_m1a_0, globals.other_modes.blend_m1b_0);
    set_blender_input(0, 1, &globals.blender.i2a_r[0], &globals.blender.i2a_g[0], &globals.blender.i2a_b[0], &globals.blender.i2b_a[0],
                      globals.other_modes.blend_m2a_0, globals.other_modes.blend_m2b_0);
    set_blender_input(1, 0, &globals.blender.i1a_r[1], &globals.blender.i1a_g[1], &globals.blender.i1a_b[1], &globals.blender.i1b_a[1],
                      globals.other_modes.blend_m1a_1, globals.other_modes.blend_m1b_1);
    set_blender_input(1, 1, &globals.blender.i2a_r[1], &globals.blender.i2a_g[1], &globals.blender.i2a_b[1], &globals.blender.i2b_a[1],
                      globals.other_modes.blend_m2a_1, globals.other_modes.blend_m2b_1);

    globals.other_modes.f.stalederivs = 1;
}

static void deduce_derivatives(void)
{
    int special_bsel0, special_bsel1;

    globals.other_modes.f.partialreject_1cycle = (globals.blender.i2b_a[0] == &globals.inv_pixel_color.a && globals.blender.i1b_a[0] == &globals.pixel_color.a);
    globals.other_modes.f.partialreject_2cycle = (globals.blender.i2b_a[1] == &globals.inv_pixel_color.a && globals.blender.i1b_a[1] == &globals.pixel_color.a);


    special_bsel0 = (globals.blender.i2b_a[0] == &globals.memory_color.a);
    special_bsel1 = (globals.blender.i2b_a[1] == &globals.memory_color.a);


    globals.other_modes.f.realblendershiftersneeded = (special_bsel0 && globals.other_modes.cycle_type == CYCLE_TYPE_1) || (special_bsel1 && globals.other_modes.cycle_type == CYCLE_TYPE_2);
    globals.other_modes.f.interpixelblendershiftersneeded = (special_bsel0 && globals.other_modes.cycle_type == CYCLE_TYPE_2);

    globals.other_modes.f.rgb_alpha_dither = (globals.other_modes.rgb_dither_sel << 2) | globals.other_modes.alpha_dither_sel;

    globals.tcdiv_ptr = tcdiv_func[globals.other_modes.persp_tex_en];


    int texel1_used_in_cc1 = 0, texel0_used_in_cc1 = 0, texel0_used_in_cc0 = 0, texel1_used_in_cc0 = 0;
    int texels_in_cc0 = 0, texels_in_cc1 = 0;
    int lod_frac_used_in_cc1 = 0, lod_frac_used_in_cc0 = 0;

    if ((globals.combiner.rgbmul_r[1] == &globals.lod_frac) || (globals.combiner.alphamul[1] == &globals.lod_frac))
        lod_frac_used_in_cc1 = 1;
    if ((globals.combiner.rgbmul_r[0] == &globals.lod_frac) || (globals.combiner.alphamul[0] == &globals.lod_frac))
        lod_frac_used_in_cc0 = 1;

    if (globals.combiner.rgbmul_r[1] == &globals.texel1_color.r || globals.combiner.rgbsub_a_r[1] == &globals.texel1_color.r || globals.combiner.rgbsub_b_r[1] == &globals.texel1_color.r || globals.combiner.rgbadd_r[1] == &globals.texel1_color.r || \
        globals.combiner.alphamul[1] == &globals.texel1_color.a || globals.combiner.alphasub_a[1] == &globals.texel1_color.a || globals.combiner.alphasub_b[1] == &globals.texel1_color.a || globals.combiner.alphaadd[1] == &globals.texel1_color.a || \
        globals.combiner.rgbmul_r[1] == &globals.texel1_color.a)
        texel1_used_in_cc1 = 1;
    if (globals.combiner.rgbmul_r[1] == &globals.texel0_color.r || globals.combiner.rgbsub_a_r[1] == &globals.texel0_color.r || globals.combiner.rgbsub_b_r[1] == &globals.texel0_color.r || globals.combiner.rgbadd_r[1] == &globals.texel0_color.r || \
        globals.combiner.alphamul[1] == &globals.texel0_color.a || globals.combiner.alphasub_a[1] == &globals.texel0_color.a || globals.combiner.alphasub_b[1] == &globals.texel0_color.a || globals.combiner.alphaadd[1] == &globals.texel0_color.a || \
        globals.combiner.rgbmul_r[1] == &globals.texel0_color.a)
        texel0_used_in_cc1 = 1;
    if (globals.combiner.rgbmul_r[0] == &globals.texel1_color.r || globals.combiner.rgbsub_a_r[0] == &globals.texel1_color.r || globals.combiner.rgbsub_b_r[0] == &globals.texel1_color.r || globals.combiner.rgbadd_r[0] == &globals.texel1_color.r || \
        globals.combiner.alphamul[0] == &globals.texel1_color.a || globals.combiner.alphasub_a[0] == &globals.texel1_color.a || globals.combiner.alphasub_b[0] == &globals.texel1_color.a || globals.combiner.alphaadd[0] == &globals.texel1_color.a || \
        globals.combiner.rgbmul_r[0] == &globals.texel1_color.a)
        texel1_used_in_cc0 = 1;
    if (globals.combiner.rgbmul_r[0] == &globals.texel0_color.r || globals.combiner.rgbsub_a_r[0] == &globals.texel0_color.r || globals.combiner.rgbsub_b_r[0] == &globals.texel0_color.r || globals.combiner.rgbadd_r[0] == &globals.texel0_color.r || \
        globals.combiner.alphamul[0] == &globals.texel0_color.a || globals.combiner.alphasub_a[0] == &globals.texel0_color.a || globals.combiner.alphasub_b[0] == &globals.texel0_color.a || globals.combiner.alphaadd[0] == &globals.texel0_color.a || \
        globals.combiner.rgbmul_r[0] == &globals.texel0_color.a)
        texel0_used_in_cc0 = 1;
    texels_in_cc0 = texel0_used_in_cc0 || texel1_used_in_cc0;
    texels_in_cc1 = texel0_used_in_cc1 || texel1_used_in_cc1;


    if (texel1_used_in_cc1)
        globals.other_modes.f.textureuselevel0 = 0;
    else if (texel0_used_in_cc1 || lod_frac_used_in_cc1)
        globals.other_modes.f.textureuselevel0 = 1;
    else
        globals.other_modes.f.textureuselevel0 = 2;

    if (texel1_used_in_cc1)
        globals.other_modes.f.textureuselevel1 = 0;
    else if (texel1_used_in_cc0 || texel0_used_in_cc1)
        globals.other_modes.f.textureuselevel1 = 1;
    else if (texel0_used_in_cc0 || lod_frac_used_in_cc0 || lod_frac_used_in_cc1)
        globals.other_modes.f.textureuselevel1 = 2;
    else
        globals.other_modes.f.textureuselevel1 = 3;


    int lodfracused = 0;

    if ((globals.other_modes.cycle_type == CYCLE_TYPE_2 && (lod_frac_used_in_cc0 || lod_frac_used_in_cc1)) || \
        (globals.other_modes.cycle_type == CYCLE_TYPE_1 && lod_frac_used_in_cc1))
        lodfracused = 1;

    if ((globals.other_modes.cycle_type == CYCLE_TYPE_1 && globals.combiner.rgbsub_a_r[1] == &globals.noise) || \
        (globals.other_modes.cycle_type == CYCLE_TYPE_2 && (globals.combiner.rgbsub_a_r[0] == &globals.noise || globals.combiner.rgbsub_a_r[1] == &globals.noise)) || \
        globals.other_modes.alpha_dither_sel == 2)
        globals.other_modes.f.getditherlevel = 0;
    else if (globals.other_modes.f.rgb_alpha_dither != 0xf)
        globals.other_modes.f.getditherlevel = 1;
    else
        globals.other_modes.f.getditherlevel = 2;

    globals.other_modes.f.dolod = globals.other_modes.tex_lod_en || lodfracused;
}