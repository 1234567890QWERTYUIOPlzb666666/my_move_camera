#include "camera_media.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef WSL
/* WSL ships libjpeg.so.8 (IJG ABI 80) without development headers. */
#define JCONFIG_INCLUDED
#define JPEG_LIB_VERSION 80
#define LIBJPEG_TURBO_VERSION 2.0.3
#define LIBJPEG_TURBO_VERSION_NUMBER 2000003
#define C_ARITH_CODING_SUPPORTED 1
#define D_ARITH_CODING_SUPPORTED 1
#define MEM_SRCDST_SUPPORTED 1
#define BITS_IN_JSAMPLE 8
#define HAVE_LOCALE_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDLIB_H 1
#define NEED_SYS_TYPES_H 1
#define HAVE_UNSIGNED_CHAR 1
#define HAVE_UNSIGNED_SHORT 1
#endif

#include <jpeglib.h>

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump_buffer;
} jpeg_error_handler_t;

static void jpeg_error_exit(j_common_ptr context)
{
    jpeg_error_handler_t * error = (jpeg_error_handler_t *)context->err;
    (*context->err->output_message)(context);
    longjmp(error->jump_buffer, 1);
}

int camera_media_save_jpeg(const char * path,
                           const lv_color_t * pixels,
                           uint32_t width,
                           uint32_t height,
                           int quality)
{
    struct jpeg_compress_struct compressor = { 0 };
    jpeg_error_handler_t error;
    FILE * output = NULL;
    uint8_t * row = NULL;
    int result = -1;

    if (path == NULL || pixels == NULL || width == 0 || height == 0) {
        return -1;
    }

    output = fopen(path, "wb");
    if (output == NULL) {
        perror("Unable to create photo");
        return -1;
    }

    compressor.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_error_exit;
    if (setjmp(error.jump_buffer)) {
        jpeg_destroy_compress(&compressor);
        goto cleanup;
    }

    jpeg_create_compress(&compressor);
    jpeg_stdio_dest(&compressor, output);
    compressor.image_width = width;
    compressor.image_height = height;
    compressor.input_components = 3;
    compressor.in_color_space = JCS_RGB;
    jpeg_set_defaults(&compressor);
    jpeg_set_quality(&compressor, quality, TRUE);
    jpeg_start_compress(&compressor, TRUE);

    row = malloc((size_t)width * 3);
    if (row == NULL) {
        jpeg_destroy_compress(&compressor);
        goto cleanup;
    }

    while (compressor.next_scanline < compressor.image_height) {
        const lv_color_t * source =
            pixels + (size_t)compressor.next_scanline * width;
        JSAMPROW rows[1] = { row };

        for (uint32_t x = 0; x < width; x++) {
            uint32_t color = lv_color_to32(source[x]);
            row[x * 3] = (uint8_t)(color >> 16);
            row[x * 3 + 1] = (uint8_t)(color >> 8);
            row[x * 3 + 2] = (uint8_t)color;
        }
        jpeg_write_scanlines(&compressor, rows, 1);
    }

    jpeg_finish_compress(&compressor);
    jpeg_destroy_compress(&compressor);
    result = 0;

cleanup:
    free(row);
    fclose(output);
    return result;
}

int camera_media_load_jpeg(const char * path,
                           lv_color_t * pixels,
                           uint32_t width,
                           uint32_t height)
{
    struct jpeg_decompress_struct decompressor = { 0 };
    jpeg_error_handler_t error;
    FILE * input = NULL;
    uint8_t * source_pixels = NULL;
    uint8_t * row = NULL;
    int result = -1;

    if (path == NULL || pixels == NULL || width == 0 || height == 0) {
        return -1;
    }

    input = fopen(path, "rb");
    if (input == NULL) {
        perror("Unable to open photo");
        return -1;
    }

    decompressor.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_error_exit;
    if (setjmp(error.jump_buffer)) {
        jpeg_destroy_decompress(&decompressor);
        goto cleanup;
    }

    jpeg_create_decompress(&decompressor);
    jpeg_stdio_src(&decompressor, input);
    jpeg_read_header(&decompressor, TRUE);
    decompressor.out_color_space = JCS_RGB;
    jpeg_start_decompress(&decompressor);

    source_pixels = malloc((size_t)decompressor.output_width *
                           decompressor.output_height * 3);
    row = malloc((size_t)decompressor.output_width * 3);
    if (source_pixels == NULL || row == NULL) {
        jpeg_destroy_decompress(&decompressor);
        goto cleanup;
    }

    while (decompressor.output_scanline < decompressor.output_height) {
        uint32_t y = decompressor.output_scanline;
        JSAMPROW rows[1] = { row };
        jpeg_read_scanlines(&decompressor, rows, 1);
        memcpy(source_pixels + (size_t)y * decompressor.output_width * 3,
               row,
               (size_t)decompressor.output_width * 3);
    }

    for (uint32_t dy = 0; dy < height; dy++) {
        uint32_t sy = (uint64_t)dy * decompressor.output_height / height;
        for (uint32_t dx = 0; dx < width; dx++) {
            uint32_t sx = (uint64_t)dx * decompressor.output_width / width;
            const uint8_t * rgb =
                source_pixels + ((size_t)sy * decompressor.output_width + sx) * 3;
            pixels[(size_t)dy * width + dx] =
                lv_color_make(rgb[0], rgb[1], rgb[2]);
        }
    }

    jpeg_finish_decompress(&decompressor);
    jpeg_destroy_decompress(&decompressor);
    result = 0;

cleanup:
    free(row);
    free(source_pixels);
    fclose(input);
    return result;
}
