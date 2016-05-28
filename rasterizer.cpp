#include "rasterizer.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#include <intrin.h>

// runs unit tests automatically when the library is used
#define RASTERIZER_UNIT_TESTS

#ifdef RASTERIZER_UNIT_TESTS
void run_rasterizer_unit_tests();
#endif

// Sized according to the Larrabee rasterizer's description
#define FRAMEBUFFER_TILE_WIDTH_IN_PIXELS 128
#define FRAMEBUFFER_COARSE_BLOCK_WIDTH_IN_PIXELS 16
#define FRAMEBUFFER_FINE_BLOCK_WIDTH_IN_PIXELS 4

// small sizes (for testing)
// #define FRAMEBUFFER_TILE_WIDTH_IN_PIXELS 4
// #define FRAMEBUFFER_COARSE_BLOCK_WIDTH_IN_PIXELS 2
// #define FRAMEBUFFER_FINE_BLOCK_WIDTH_IN_PIXELS 1

// Convenience
#define FRAMEBUFFER_PIXELS_PER_TILE (FRAMEBUFFER_TILE_WIDTH_IN_PIXELS * FRAMEBUFFER_TILE_WIDTH_IN_PIXELS)
 
// The swizzle masks, using alternating yxyxyx bit pattern for morton-code swizzling pixels in a tile.
// This makes the pixels morton code swizzled within every rasterization level (fine/coarse/tile)
// The tiles themselves are stored row major.
// For examples of this concept, see:
// https://software.intel.com/en-us/node/514045
// https://msdn.microsoft.com/en-us/library/windows/desktop/dn770442%28v=vs.85%29.aspx
#define FRAMEBUFFER_TILE_X_SWIZZLE_MASK (0x55555555 & (FRAMEBUFFER_PIXELS_PER_TILE - 1))
#define FRAMEBUFFER_TILE_Y_SWIZZLE_MASK (0xAAAAAAAA & (FRAMEBUFFER_PIXELS_PER_TILE - 1))

// If there are too many commands and this buffer gets filled up,
// then the command buffer for that tile must be flushed.
#define TILE_COMMAND_BUFFER_SIZE_IN_DWORDS 128

// parallel bit deposit low-order source bits according to mask bits
#ifdef __AVX2__
__forceinline uint32_t pdep_u32(uint32_t source, uint32_t mask)
{
    return _pdep_u32(source, mask);
}
#else
__forceinline uint32_t pdep_u32(uint32_t source, uint32_t mask)
{
    // horribly inefficient, but that's life without AVX2.
    // however, typically not a problem since you only need to swizzle once up front.
    uint32_t dst = 0;
    for (uint32_t mask_i = 0, dst_i = 0; mask_i < 32; mask_i++)
    {
        if (mask & (1 << mask_i))
        {
            uint32_t src_bit = (source & (1 << dst_i)) >> dst_i;
            dst |= src_bit << mask_i;

            dst_i++;
        }
    }
    return dst;
}
#endif

typedef struct tile_cmdbuf_t
{
    // start and past-the-end of the allocation for the buffer
    uint32_t* cmdbuf_start;
    uint32_t* cmdbuf_end;
    // the next location where to read and write commands
    uint32_t* cmdbuf_read;
    uint32_t* cmdbuf_write;
} tile_cmdbuf_t;

typedef struct framebuffer_t
{
    pixel_t* backbuffer;
    
    uint32_t* tile_cmdpool;
    tile_cmdbuf_t* tile_cmdbufs;
    
    uint32_t width_in_pixels;
    uint32_t height_in_pixels;
    
    // num_tiles_per_row * num_pixels_per_tile
    uint32_t pixels_per_row_of_tiles;

    // pixels_per_row_of_tiles * num_tile_rows
    uint32_t pixels_per_slice;
} framebuffer_t;

framebuffer_t* new_framebuffer(uint32_t width, uint32_t height)
{
#ifdef RASTERIZER_UNIT_TESTS
    static int ran_rasterizer_unit_tests_once = 0;
    if (!ran_rasterizer_unit_tests_once)
    {
        // set this before running the tests, so that unit tests can create framebuffers without causing infinite recursion
        ran_rasterizer_unit_tests_once = 1;
        run_rasterizer_unit_tests();
    }
#endif

    // limits of the rasterizer's precision
    // this is based on an analysis of the range of results of the 2D cross product between two fixed16.8 numbers.
    assert(width < 16384);
    assert(height < 16384);

    framebuffer_t* fb = (framebuffer_t*)malloc(sizeof(framebuffer_t));
    assert(fb);

    fb->width_in_pixels = width;
    fb->height_in_pixels = height;

    // pad framebuffer up to size of next tile
    // that way the rasterization code doesn't have to handlep otential out of bounds access after tile binning
    uint32_t padded_width_in_pixels = (width + (FRAMEBUFFER_TILE_WIDTH_IN_PIXELS - 1)) & -FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
    uint32_t padded_height_in_pixels = (height + (FRAMEBUFFER_TILE_WIDTH_IN_PIXELS - 1)) & -FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
    
    uint32_t width_in_tiles = padded_width_in_pixels / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
    uint32_t height_in_tiles = padded_height_in_pixels / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
    uint32_t total_num_tiles = width_in_tiles * height_in_tiles;

    fb->pixels_per_row_of_tiles = padded_width_in_pixels * FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
    fb->pixels_per_slice = padded_height_in_pixels / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS * fb->pixels_per_row_of_tiles;

    fb->backbuffer = (pixel_t*)malloc(fb->pixels_per_slice * sizeof(pixel_t));
    assert(fb->backbuffer);
 
    // clear to black/transparent initially
    memset(fb->backbuffer, 0, fb->pixels_per_slice * sizeof(pixel_t));

    // allocate command lists for each tile
    fb->tile_cmdpool = (uint32_t*)malloc(total_num_tiles * TILE_COMMAND_BUFFER_SIZE_IN_DWORDS * sizeof(uint32_t));
    assert(fb->tile_cmdpool);

    fb->tile_cmdbufs = (tile_cmdbuf_t*)malloc(total_num_tiles * sizeof(tile_cmdbuf_t));
    assert(fb->tile_cmdbufs);

    // command lists are circular queues that are initially empty
    for (uint32_t i = 0; i < total_num_tiles; i++)
    {
        fb->tile_cmdbufs[i].cmdbuf_start = &fb->tile_cmdpool[i * TILE_COMMAND_BUFFER_SIZE_IN_DWORDS];
        fb->tile_cmdbufs[i].cmdbuf_end = fb->tile_cmdbufs[i].cmdbuf_start + TILE_COMMAND_BUFFER_SIZE_IN_DWORDS;
        fb->tile_cmdbufs[i].cmdbuf_read = fb->tile_cmdbufs[i].cmdbuf_start;
        fb->tile_cmdbufs[i].cmdbuf_write = fb->tile_cmdbufs[i].cmdbuf_start;
    }

    return fb;
}

void delete_framebuffer(framebuffer_t* fb)
{
    if (!fb)
        return;

    free(fb->tile_cmdbufs);
    free(fb->tile_cmdpool);
    free(fb->backbuffer);
    free(fb);
}

void framebuffer_resolve(framebuffer_t* fb)
{
    assert(fb);
}

void framebuffer_pack_row_major(framebuffer_t* fb, uint32_t x, uint32_t y, uint32_t width, uint32_t height, pixelformat_t format, void* data)
{
    assert(fb);
    assert(x < fb->width_in_pixels);
    assert(y < fb->height_in_pixels);
    assert(width <= fb->width_in_pixels);
    assert(height <= fb->height_in_pixels);
    assert(x + width <= fb->width_in_pixels);
    assert(y + height <= fb->height_in_pixels);
    assert(data);

    uint32_t topleft_tile_y = y / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
    uint32_t topleft_tile_x = x / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
    uint32_t bottomright_tile_y = (y + (height - 1)) / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
    uint32_t bottomright_tile_x = (x + (width - 1)) / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
    
    uint32_t dst_i = 0;

    uint32_t curr_tile_row_start = topleft_tile_y * fb->pixels_per_row_of_tiles + topleft_tile_x * FRAMEBUFFER_PIXELS_PER_TILE;
    for (uint32_t tile_y = topleft_tile_y; tile_y <= bottomright_tile_y; tile_y++)
    {
        uint32_t curr_tile_start = curr_tile_row_start;

        for (uint32_t tile_x = topleft_tile_x; tile_x <= bottomright_tile_x; tile_x++)
        {
            uint32_t topleft_y = tile_y * FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
            uint32_t topleft_x = tile_x * FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
            uint32_t bottomright_y = topleft_y + FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
            uint32_t bottomright_x = topleft_x + FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
            uint32_t pixel_y_min = topleft_y < y ? y : topleft_y;
            uint32_t pixel_x_min = topleft_x < x ? x : topleft_x;
            uint32_t pixel_y_max = bottomright_y > y + height ? y + height : bottomright_y;
            uint32_t pixel_x_max = bottomright_x > x + width ? x + width : bottomright_x;

            for (uint32_t pixel_y = pixel_y_min, pixel_y_bits = pdep_u32(topleft_y, FRAMEBUFFER_TILE_Y_SWIZZLE_MASK);
                pixel_y < pixel_y_max;
                pixel_y++, pixel_y_bits = (pixel_y_bits - FRAMEBUFFER_TILE_Y_SWIZZLE_MASK) & FRAMEBUFFER_TILE_Y_SWIZZLE_MASK)
            {
                for (uint32_t pixel_x = pixel_x_min, pixel_x_bits = pdep_u32(topleft_x, FRAMEBUFFER_TILE_X_SWIZZLE_MASK);
                    pixel_x < pixel_x_max;
                    pixel_x++, pixel_x_bits = (pixel_x_bits - FRAMEBUFFER_TILE_X_SWIZZLE_MASK) & FRAMEBUFFER_TILE_X_SWIZZLE_MASK)
                {
                    uint32_t src_i = curr_tile_start + (pixel_y_bits | pixel_x_bits);
                    pixel_t src = fb->backbuffer[src_i];
                    if (format == pixelformat_r8g8b8a8_unorm)
                    {
                        uint8_t* dst = (uint8_t*)data + dst_i * 4;
                        dst[0] = (uint8_t)((src & 0x00FF0000) >> 16);
                        dst[1] = (uint8_t)((src & 0x0000FF00) >> 8);
                        dst[2] = (uint8_t)((src & 0x000000FF) >> 0);
                        dst[3] = (uint8_t)((src & 0xFF000000) >> 24);
                    }
                    else if (format == pixelformat_b8g8r8a8_unorm)
                    {
                        uint8_t* dst = (uint8_t*)data + dst_i * 4;
                        dst[0] = (uint8_t)((src & 0x000000FF) >> 0);
                        dst[1] = (uint8_t)((src & 0x0000FF00) >> 8);
                        dst[2] = (uint8_t)((src & 0x00FF0000) >> 16);
                        dst[3] = (uint8_t)((src & 0xFF000000) >> 24);
                    }
                    else
                    {
                        assert(!"Unknown pixel format");
                    }

                    dst_i++;
                }
            }

            curr_tile_start += FRAMEBUFFER_PIXELS_PER_TILE;
        }

        curr_tile_row_start += fb->pixels_per_row_of_tiles;
    }
}

// hack
uint32_t g_Color;

// Rasterizes a triangle with its vertices represented as 16.8 fixed point values
// Arguments:
// * fb: The framebuffer the triangle is written to. Pixels are assumed in BGRA format
// * fb_width: The width in pixels of the framebuffer
// * window_xi, window_yi (i in [0..2]): coordinate of vertex i of the triangle, encoded as 16.8 fixed point.
// * window_zi (i in [0..2]): depth of the vertex i of the triangle.
// Preconditions:
// * The triangle vertices are stored clockwise (relative to their position on the display)
void rasterize_triangle_fixed16_8(
    framebuffer_t* fb,
    uint32_t window_x0, uint32_t window_y0, uint32_t window_z0,
    uint32_t window_x1, uint32_t window_y1, uint32_t window_z1,
    uint32_t window_x2, uint32_t window_y2, uint32_t window_z2)
{

}

void draw(
    framebuffer_t* fb,
    const uint32_t* vertices,
    uint32_t num_vertices)
{
    assert(fb);
    assert(vertices);
    assert(num_vertices % 3 == 0);

    for (uint32_t vertex_id = 0; vertex_id < num_vertices; vertex_id += 3)
    {
        uint32_t x0 = vertices[vertex_id + 0 + 0];
        uint32_t y0 = vertices[vertex_id + 0 + 1];
        uint32_t z0 = vertices[vertex_id + 0 + 2];
        uint32_t x1 = vertices[vertex_id + 3 + 0];
        uint32_t y1 = vertices[vertex_id + 3 + 1];
        uint32_t z1 = vertices[vertex_id + 3 + 2];
        uint32_t x2 = vertices[vertex_id + 6 + 0];
        uint32_t y2 = vertices[vertex_id + 6 + 1];
        uint32_t z2 = vertices[vertex_id + 6 + 2];

        rasterize_triangle_fixed16_8(
            fb,
            x0, y0, z0,
            x1, y1, z1,
            x2, y2, z2);
    }
}

void draw_indexed(
    framebuffer_t* fb,
    const uint32_t* vertices,
    const uint32_t* indices,
    uint32_t num_indices)
{
    assert(fb);
    assert(vertices);
    assert(indices);
    assert(num_indices % 3 == 0);

    for (uint32_t index_id = 0; index_id < num_indices; index_id += 3)
    {
        uint32_t i0 = indices[index_id + 0];
        i0 = i0 + i0 + i0;
        uint32_t i1 = indices[index_id + 1];
        i1 = i1 + i1 + i1;
        uint32_t i2 = indices[index_id + 2];
        i2 = i2 + i2 + i2;

        uint32_t x0 = vertices[i0 + 0];
        uint32_t y0 = vertices[i0 + 1];
        uint32_t z0 = vertices[i0 + 2];
        uint32_t x1 = vertices[i1 + 0];
        uint32_t y1 = vertices[i1 + 1];
        uint32_t z1 = vertices[i1 + 2];
        uint32_t x2 = vertices[i2 + 0];
        uint32_t y2 = vertices[i2 + 1];
        uint32_t z2 = vertices[i2 + 2];

        rasterize_triangle_fixed16_8(
            fb,
            x0, y0, z0,
            x1, y1, z1,
            x2, y2, z2);
    }
}

#ifdef RASTERIZER_UNIT_TESTS
void run_rasterizer_unit_tests()
{
    // pdep tests
    //             source  mask
    assert(pdep_u32(0b000, 0b000000) == 0b000000);
    assert(pdep_u32(0b001, 0b000001) == 0b000001);
    assert(pdep_u32(0b001, 0b000010) == 0b000010);
    assert(pdep_u32(0b011, 0b001100) == 0b001100);
    assert(pdep_u32(0b101, 0b101010) == 0b100010);
    assert(pdep_u32(0b010, 0b010101) == 0b000100);

    // swizzle test
    {
        uint32_t w = FRAMEBUFFER_TILE_WIDTH_IN_PIXELS * 2;
        uint32_t h = FRAMEBUFFER_TILE_WIDTH_IN_PIXELS * 2;
        
        framebuffer_t* fb = new_framebuffer(w, h);
        uint8_t* rowmajor_data = (uint8_t*)malloc(w * h * 4);

        // write indices of pixels linearly in memory (ignoring swizzling)
        // this will be read back and checked to verify the layout
        // For tiles of 4x4 pixels, a 8x8 row major image should look something like:
        //  0  1  4  5 | 16 17 20 21
        //  2  3  6  7 | 18 19 22 23
        //  8  9 12 13 | 24 25 28 29
        // 10 11 14 15 | 26 27 30 31
        // -------------------------
        // 32 33 36 37 | 48 49 52 53
        // 34 35 38 39 | 50 51 54 55
        // 40 41 44 45 | 56 57 60 61
        // 42 43 46 47 | 58 59 62 63
        // see: https://en.wikipedia.org/wiki/Z-order_curve
        for (uint32_t i = 0; i < fb->pixels_per_slice; i++)
        {
            fb->backbuffer[i] = i;
        }
        
        framebuffer_pack_row_major(fb, 0, 0, w, h, pixelformat_r8g8b8a8_unorm, rowmajor_data);
        
        for (uint32_t y = 0; y < h; y++)
        {
            uint32_t tile_y = y / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;

            for (uint32_t x = 0; x < w; x++)
            {
                uint32_t tile_x = x / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
                uint32_t tile_i = tile_y * (fb->pixels_per_row_of_tiles / FRAMEBUFFER_PIXELS_PER_TILE) + tile_x;
                uint32_t topleft_pixel_i = tile_i * FRAMEBUFFER_PIXELS_PER_TILE;

                uint32_t tile_relative_x = x - tile_x * FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
                uint32_t tile_relative_y = y - tile_y * FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
                uint32_t rowmajor_i = topleft_pixel_i + (tile_relative_y * FRAMEBUFFER_TILE_WIDTH_IN_PIXELS + tile_relative_x);

                uint32_t xmask = FRAMEBUFFER_TILE_X_SWIZZLE_MASK;
                uint32_t ymask = FRAMEBUFFER_TILE_Y_SWIZZLE_MASK;
                uint32_t xbits = pdep_u32(x, xmask);
                uint32_t ybits = pdep_u32(y, ymask);
                uint32_t swizzled_i = topleft_pixel_i + xbits + ybits;

                assert(rowmajor_data[rowmajor_i * 4 + 0] == ((fb->backbuffer[swizzled_i] & 0x00FF0000) >> 16));
                assert(rowmajor_data[rowmajor_i * 4 + 1] == ((fb->backbuffer[swizzled_i] & 0x0000FF00) >> 8));
                assert(rowmajor_data[rowmajor_i * 4 + 2] == ((fb->backbuffer[swizzled_i] & 0x000000FF) >> 0));
                assert(rowmajor_data[rowmajor_i * 4 + 3] == ((fb->backbuffer[swizzled_i] & 0xFF000000) >> 24));
            }
        }
        
        free(rowmajor_data);
        delete_framebuffer(fb);
    }
}
#endif