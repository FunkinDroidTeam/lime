#define WUFFS_IMPLEMENTATION

extern "C" {

	#include <png.h>
	#include <pngstruct.h>
	#define PNG_SIG_SIZE 8
}

#define MAX_PNG_SIZE (64 * 1024 * 1024)  // 64 MB
#define MAX_DIMENSION 4096

#include "wuffs-v0.3.c"
#include <stdio.h> // For printf
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <setjmp.h>
#include <graphics/format/PNG.h>
#include <graphics/ImageBuffer.h>
#include <system/System.h>
#include <utils/Bytes.h>
#include <utils/QuickVec.h>


namespace lime {


	struct ReadBuffer {

		ReadBuffer (const unsigned char* data, int length) : data (data), length (length), position (0) {}

		bool Read (unsigned char* out, int count) {

			if (position >= length) return false;

			if (count > length - position) {

				memcpy (out, data + position, length - position);
				position = length;

			} else {

				memcpy (out, data + position, count);
				position += count;

			}

			return true;

		}

		char unused; // the first byte gets corrupted when passed to libpng?
		const unsigned char* data;
		int length;
		int position;

	};

	static void user_error_fn (png_structp png_ptr, png_const_charp error_msg) {

			longjmp (png_ptr->jmp_buf_local, 1);

		}


	static void user_read_data_fn (png_structp png_ptr, png_bytep data, png_size_t length) {

		ReadBuffer* buffer = (ReadBuffer*)png_get_io_ptr (png_ptr);
		if (!buffer->Read (data, length)) {
			png_error (png_ptr, "Read Error");
		}

	}

	static void user_warning_fn (png_structp png_ptr, png_const_charp warning_msg) {}


	void user_write_data (png_structp png_ptr, png_bytep data, png_size_t length) {

		QuickVec<unsigned char> *buffer = (QuickVec<unsigned char> *)png_get_io_ptr (png_ptr);
		buffer->append ((unsigned char *)data,(int)length);

	}

	void user_flush_data (png_structp png_ptr) {}

	bool ReadInput(Resource* resource, wuffs_base__io_buffer* io_buffer) {
    size_t data_size = 0;
    uint8_t* data = nullptr;

    if (resource->path) {
        FILE_HANDLE* file = lime::fopen(resource->path, "rb");
        if (!file) {
            return false;
        }

        // Assuming lime::fseek, lime::ftell, and lime::fread exist
        lime::fseek(file, 0, SEEK_END);
        data_size = lime::ftell(file);
        lime::fseek(file, 0, SEEK_SET);

        if (data_size > MAX_PNG_SIZE) {
            lime::fclose(file);
            return false;
        }

        data = (uint8_t*)malloc(data_size);
        if (!data) {
            lime::fclose(file);
            return false;
        }

        if (lime::fread(data, 1, data_size, file) != data_size) {
            free(data);
            lime::fclose(file);
            return false;
        }

        lime::fclose(file);
    } else if (resource->data && resource->data->length > 0) {
        data_size = resource->data->length;
        if (data_size > MAX_PNG_SIZE) {
            return false;
        }

        data = (uint8_t*)malloc(data_size);
        if (!data) {
            return false;
        }

        memcpy(data, resource->data->b, data_size);
    } else {
        return false;
    }

    io_buffer->data.ptr = data;
    io_buffer->data.len = data_size;
    io_buffer->meta.wi = data_size;
    io_buffer->meta.ri = 0;
    io_buffer->meta.pos = 0;
    io_buffer->meta.closed = true;

    return true;
	}

	bool AllocateBuffers(wuffs_png__decoder* decoder,
                     wuffs_base__image_config* ic,
                     uint8_t** pixel_buffer,
                     uint8_t** workbuf,
                     size_t* workbuf_len) {
    uint32_t width = wuffs_base__pixel_config__width(&ic->pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic->pixcfg);

    if (width > MAX_DIMENSION || height > MAX_DIMENSION) {
        return false;
    }

    size_t pixel_buffer_size = width * height * 4;  // 4 bytes per pixel for BGRA_PREMUL
    *pixel_buffer = (uint8_t*)malloc(pixel_buffer_size);
    if (!*pixel_buffer) {
        return false;
    }

    wuffs_base__range_ii_u64 workbuf_len_range = wuffs_png__decoder__workbuf_len(decoder);
    *workbuf_len = workbuf_len_range.max_incl;
    *workbuf = (uint8_t*)malloc(*workbuf_len);
    if (!*workbuf) {
        free(*pixel_buffer);
        *pixel_buffer = nullptr;
        return false;
    }

    return true;
	}
	bool PNG::Decode(Resource* resource, ImageBuffer* imageBuffer, bool decodeData) {
    class MyCallbacks : public wuffs_aux::DecodeImageCallbacks {
    public:
        wuffs_base__pixel_format SelectPixfmt(const wuffs_base__image_config& image_config) override {
            return wuffs_base__make_pixel_format(WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL);
        }
    };

    MyCallbacks callbacks;
    wuffs_aux::sync_io::MemoryInput input(resource->data->b, resource->data->length);

    wuffs_aux::DecodeImageResult result = wuffs_aux::DecodeImage(
        callbacks,
        input,
        wuffs_aux::DecodeImageArgQuirks::DefaultValue(),
        wuffs_aux::DecodeImageArgFlags::DefaultValue(),
        wuffs_aux::DecodeImageArgPixelBlend::DefaultValue(),
        wuffs_aux::DecodeImageArgBackgroundColor::DefaultValue(),
        wuffs_aux::DecodeImageArgMaxInclDimension::DefaultValue(),
        wuffs_aux::DecodeImageArgMaxInclMetadataLength::DefaultValue()
    );

    if (!result.error_message.empty()) {
        printf("Failed to decode image: %s\n", result.error_message.c_str());
        return false;
    }

    // Update ImageBuffer dimensions
    imageBuffer->width = result.pixbuf.pixcfg.width();
    imageBuffer->height = result.pixbuf.pixcfg.height();

    if (decodeData) {
        // Resize ImageBuffer
        imageBuffer->Resize(imageBuffer->width, imageBuffer->height, 32);

        // Copy decoded data to ImageBuffer
        wuffs_base__table_u8 pixels = result.pixbuf.plane(0);
        size_t bytes_per_row = imageBuffer->width * 4;  // 4 bytes per pixel for RGBA
        for (uint32_t y = 0; y < imageBuffer->height; ++y) {
            memcpy(imageBuffer->data->buffer->b + (y * bytes_per_row),
                   pixels.ptr + (y * pixels.stride),
                   bytes_per_row);
        }

        // No need for color correction if the format already matches your needs
    }

    return true;
	}

	bool PNG::Encode (ImageBuffer *imageBuffer, Bytes* bytes) {

		png_structp png_ptr = png_create_write_struct (PNG_LIBPNG_VER_STRING, NULL, user_error_fn, user_warning_fn);

		if (!png_ptr) {

			return false;

		}

		png_infop info_ptr = png_create_info_struct (png_ptr);

		if (!info_ptr) {

			return false;

		}

		if (setjmp (png_jmpbuf (png_ptr))) {

			png_destroy_write_struct (&png_ptr, &info_ptr);
			return false;

		}

		QuickVec<unsigned char> out_buffer;

		png_set_write_fn (png_ptr, &out_buffer, user_write_data, user_flush_data);

		int w = imageBuffer->width;
		int h = imageBuffer->height;

		int bit_depth = 8;
		//int color_type = (inSurface->Format () & pfHasAlpha) ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB;
		int color_type = PNG_COLOR_TYPE_RGB_ALPHA;
		png_set_IHDR (png_ptr, info_ptr, w, h, bit_depth, color_type, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

		png_write_info (png_ptr, info_ptr);

		bool do_alpha = (color_type == PNG_COLOR_TYPE_RGBA);
		unsigned char* imageData = imageBuffer->data->buffer->b;
		int stride = imageBuffer->Stride ();

		{
			QuickVec<unsigned char> row_data (w * 4);
			png_bytep row = &row_data[0];

			for (int y = 0; y < h; y++) {

				unsigned char *buf = &row_data[0];
				const unsigned char *src = (const unsigned char *)(imageData + (stride * y));

				for (int x = 0; x < w; x++) {

					buf[0] = src[0];
					buf[1] = src[1];
					buf[2] = src[2];
					src += 3;
					buf += 3;

					if (do_alpha) {

						*buf++ = *src;

					}

					src++;

				}

				png_write_rows (png_ptr, &row, 1);

			}

		}

		png_write_end (png_ptr, NULL);

		int size = out_buffer.size ();

		if (size > 0) {

			bytes->Resize (size);
			memcpy (bytes->b, &out_buffer[0], size);

		}

		return true;

	}


}
