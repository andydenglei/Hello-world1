
/* pngtest.c - a simple test program to test libpng

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Windows.h>

#include "png.h"


typedef enum LodePNGColorType
{
	LCT_GREY = 0, /*greyscale: 1,2,4,8,16 bit*/
	LCT_RGB = 2, /*RGB: 8,16 bit*/
	LCT_PALETTE = 3, /*palette: 1,2,4,8 bit*/
	LCT_GREY_ALPHA = 4, /*greyscale with alpha: 8,16 bit*/
	LCT_RGBA = 6 /*RGB with alpha: 8,16 bit*/
} LodePNGColorType;

typedef struct LodePNGColorMode
{
	/*header (IHDR)*/
	LodePNGColorType colortype; /*color type, see PNG standard or documentation further in this header file*/
	unsigned bitdepth;  /*bits per sample, see PNG standard or documentation further in this header file*/

	/*
	palette (PLTE and tRNS)

	Dynamically allocated with the colors of the palette, including alpha.
	When encoding a PNG, to store your colors in the palette of the LodePNGColorMode, first use
	lodepng_palette_clear, then for each color use lodepng_palette_add.
	If you encode an image without alpha with palette, don't forget to put value 255 in each A byte of the palette.

	When decoding, by default you can ignore this palette, since LodePNG already
	fills the palette colors in the pixels of the raw RGBA output.

	The palette is only supported for color type 3.
	*/
	unsigned char* palette; /*palette in RGBARGBA... order. When allocated, must be either 0, or have size 1024*/
	size_t palettesize; /*palette size in number of colors (amount of bytes is 4 * palettesize)*/

	/*
	transparent color key (tRNS)

	This color uses the same bit depth as the bitdepth value in this struct, which can be 1-bit to 16-bit.
	For greyscale PNGs, r, g and b will all 3 be set to the same.

	When decoding, by default you can ignore this information, since LodePNG sets
	pixels with this key to transparent already in the raw RGBA output.

	The color key is only supported for color types 0 and 2.
	*/
	unsigned key_defined; /*is a transparent color key given? 0 = false, 1 = true*/
	unsigned key_r;       /*red/greyscale component of color key*/
	unsigned key_g;       /*green component of color key*/
	unsigned key_b;       /*blue component of color key*/
} LodePNGColorMode;

typedef struct LodePNGColorProfile
{
	unsigned colored; /*not greyscale*/
	unsigned key; /*if true, image is not opaque. Only if true and alpha is false, color key is possible.*/
	unsigned short key_r; /*these values are always in 16-bit bitdepth in the profile*/
	unsigned short key_g;
	unsigned short key_b;
	unsigned alpha; /*alpha channel or alpha palette required*/
	unsigned numcolors; /*amount of colors, up to 257. Not valid if bits == 16.*/
	unsigned char palette[1024]; /*Remembers up to the first 256 RGBA colors, in no particular order*/
	unsigned bits; /*bits per channel (not for palette). 1,2 or 4 for greyscale only. 16 if 16-bit per channel required.*/
} LodePNGColorProfile;

typedef struct ColorTree ColorTree;

/*
One node of a color tree
This is the data structure used to count the number of unique colors and to get a palette
index for a color. It's like an octree, but because the alpha channel is used too, each
node has 16 instead of 8 children.
*/
struct ColorTree
{
	ColorTree* children[16]; /*up to 16 pointers to ColorTree of next level*/
	int index; /*the payload. Only has a meaningful value if this is in the last level*/
};

static void* lodepng_malloc(size_t size)
{
	return malloc(size);
}

static void lodepng_free(void* ptr)
{
	free(ptr);
}

static void* lodepng_realloc(void* ptr, size_t new_size)
{
	return realloc(ptr, new_size);
}

/*Try the code, if it returns error, also return the error.*/
#define CERROR_TRY_RETURN(call)\
{\
	unsigned error = call;\
	if(error) return error;\
}

#define CERROR_BREAK(errorvar, code)\
{\
  errorvar = code;\
  break;\
}

/*version of CERROR_BREAK that assumes the common case where the error variable is named "error"*/
#define ERROR_BREAK(code) CERROR_BREAK(error, code)

unsigned lodepng_is_greyscale_type(const LodePNGColorMode* info)
{
	return info->colortype == LCT_GREY || info->colortype == LCT_GREY_ALPHA;
}

unsigned lodepng_is_alpha_type(const LodePNGColorMode* info)
{
	return (info->colortype & 4) != 0; /*4 or 6*/
}

unsigned lodepng_is_palette_type(const LodePNGColorMode* info)
{
	return info->colortype == LCT_PALETTE;
}

unsigned lodepng_has_palette_alpha(const LodePNGColorMode* info)
{
	size_t i;
	for(i = 0; i != info->palettesize; ++i)
	{
		if(info->palette[i * 4 + 3] < 255) return 1;
	}
	return 0;
}

unsigned lodepng_can_have_alpha(const LodePNGColorMode* info)
{
	return info->key_defined
		|| lodepng_is_alpha_type(info)
		|| lodepng_has_palette_alpha(info);
}

static unsigned getNumColorChannels(LodePNGColorType colortype)
{
	switch(colortype)
	{
	case 0: return 1; /*grey*/
	case 2: return 3; /*RGB*/
	case 3: return 1; /*palette*/
	case 4: return 2; /*grey + alpha*/
	case 6: return 4; /*RGBA*/
	}
	return 0; /*unexisting color type*/
}

static unsigned lodepng_get_bpp_lct(LodePNGColorType colortype, unsigned bitdepth)
{
	/*bits per pixel is amount of channels * bits per channel*/
	return getNumColorChannels(colortype) * bitdepth;
}

unsigned lodepng_get_bpp(const LodePNGColorMode* info)
{
	/*calculate bits per pixel out of colortype and bitdepth*/
	return lodepng_get_bpp_lct(info->colortype, info->bitdepth);
}

static void color_tree_init(ColorTree* tree)
{
	int i;
	for(i = 0; i != 16; ++i) tree->children[i] = 0;
	tree->index = -1;
}

/*Get RGBA16 color of pixel with index i (y * width + x) from the raw image with
given color type, but the given color type must be 16-bit itself.*/
static void getPixelColorRGBA16(unsigned short* r, unsigned short* g, unsigned short* b, unsigned short* a,
								const unsigned char* in, size_t i, const LodePNGColorMode* mode)
{
	if(mode->colortype == LCT_GREY)
	{
		*r = *g = *b = 256 * in[i * 2 + 0] + in[i * 2 + 1];
		if(mode->key_defined && 256U * in[i * 2 + 0] + in[i * 2 + 1] == mode->key_r) *a = 0;
		else *a = 65535;
	}
	else if(mode->colortype == LCT_RGB)
	{
		*r = 256u * in[i * 6 + 0] + in[i * 6 + 1];
		*g = 256u * in[i * 6 + 2] + in[i * 6 + 3];
		*b = 256u * in[i * 6 + 4] + in[i * 6 + 5];
		if(mode->key_defined
			&& 256u * in[i * 6 + 0] + in[i * 6 + 1] == mode->key_r
			&& 256u * in[i * 6 + 2] + in[i * 6 + 3] == mode->key_g
			&& 256u * in[i * 6 + 4] + in[i * 6 + 5] == mode->key_b) *a = 0;
		else *a = 65535;
	}
	else if(mode->colortype == LCT_GREY_ALPHA)
	{
		*r = *g = *b = 256u * in[i * 4 + 0] + in[i * 4 + 1];
		*a = 256u * in[i * 4 + 2] + in[i * 4 + 3];
	}
	else if(mode->colortype == LCT_RGBA)
	{
		*r = 256u * in[i * 8 + 0] + in[i * 8 + 1];
		*g = 256u * in[i * 8 + 2] + in[i * 8 + 3];
		*b = 256u * in[i * 8 + 4] + in[i * 8 + 5];
		*a = 256u * in[i * 8 + 6] + in[i * 8 + 7];
	}
}

static unsigned char readBitFromReversedStream(size_t* bitpointer, const unsigned char* bitstream)
{
	unsigned char result = (unsigned char)((bitstream[(*bitpointer) >> 3] >> (7 - ((*bitpointer) & 0x7))) & 1);
	++(*bitpointer);
	return result;
}

static unsigned readBitsFromReversedStream(size_t* bitpointer, const unsigned char* bitstream, size_t nbits)
{
	unsigned result = 0;
	size_t i;
	for(i = nbits - 1; i < nbits; --i)
	{
		result += (unsigned)readBitFromReversedStream(bitpointer, bitstream) << i;
	}
	return result;
}

/*Get RGBA8 color of pixel with index i (y * width + x) from the raw image with given color type.*/
static void getPixelColorRGBA8(unsigned char* r, unsigned char* g,
							   unsigned char* b, unsigned char* a,
							   const unsigned char* in, size_t i,
							   const LodePNGColorMode* mode)
{
	if(mode->colortype == LCT_GREY)
	{
		if(mode->bitdepth == 8)
		{
			*r = *g = *b = in[i];
			if(mode->key_defined && *r == mode->key_r) *a = 0;
			else *a = 255;
		}
		else if(mode->bitdepth == 16)
		{
			*r = *g = *b = in[i * 2 + 0];
			if(mode->key_defined && 256U * in[i * 2 + 0] + in[i * 2 + 1] == mode->key_r) *a = 0;
			else *a = 255;
		}
		else
		{
			unsigned highest = ((1U << mode->bitdepth) - 1U); /*highest possible value for this bit depth*/
			size_t j = i * mode->bitdepth;
			unsigned value = readBitsFromReversedStream(&j, in, mode->bitdepth);
			r = *g = *b = (value * 255) / highest;
			/*r=(value * 255) / highest;
			*g=(value * 255) / highest;
			*b=(value * 255) / highest;*/
			if(mode->key_defined && value == mode->key_r) *a = 0;
			else *a = 255;
		}
	}
	else if(mode->colortype == LCT_RGB)
	{
		if(mode->bitdepth == 8)
		{
			*r = in[i * 3 + 0]; *g = in[i * 3 + 1]; *b = in[i * 3 + 2];
			if(mode->key_defined && *r == mode->key_r && *g == mode->key_g && *b == mode->key_b) *a = 0;
			else *a = 255;
		}
		else
		{
			*r = in[i * 6 + 0];
			*g = in[i * 6 + 2];
			*b = in[i * 6 + 4];
			if(mode->key_defined && 256U * in[i * 6 + 0] + in[i * 6 + 1] == mode->key_r
				&& 256U * in[i * 6 + 2] + in[i * 6 + 3] == mode->key_g
				&& 256U * in[i * 6 + 4] + in[i * 6 + 5] == mode->key_b) *a = 0;
			else *a = 255;
		}
	}
	else if(mode->colortype == LCT_PALETTE)
	{
		unsigned index;
		if(mode->bitdepth == 8) index = in[i];
		else
		{
			size_t j = i * mode->bitdepth;
			index = readBitsFromReversedStream(&j, in, mode->bitdepth);
		}

		if(index >= mode->palettesize)
		{
			/*This is an error according to the PNG spec, but common PNG decoders make it black instead.
			Done here too, slightly faster due to no error handling needed.*/
			*r = *g = *b = 0;
			*a = 255;
		}
		else
		{
			*r = mode->palette[index * 4 + 0];
			*g = mode->palette[index * 4 + 1];
			*b = mode->palette[index * 4 + 2];
			*a = mode->palette[index * 4 + 3];
		}
	}
	else if(mode->colortype == LCT_GREY_ALPHA)
	{
		if(mode->bitdepth == 8)
		{
			*r = *g = *b = in[i * 2 + 0];
			*a = in[i * 2 + 1];
		}
		else
		{
			*r = *g = *b = in[i * 4 + 0];
			*a = in[i * 4 + 2];
		}
	}
	else if(mode->colortype == LCT_RGBA)
	{
		if(mode->bitdepth == 8)
		{
			*r = in[i * 4 + 0];
			*g = in[i * 4 + 1];
			*b = in[i * 4 + 2];
			*a = in[i * 4 + 3];
		}
		else
		{
			*r = in[i * 8 + 0];
			*g = in[i * 8 + 2];
			*b = in[i * 8 + 4];
			*a = in[i * 8 + 6];
		}
	}
}

/*Returns how many bits needed to represent given value (max 8 bit)*/
static unsigned getValueRequiredBits(unsigned char value)
{
	if(value == 0 || value == 255) return 1;
	/*The scaling of 2-bit and 4-bit values uses multiples of 85 and 17*/
	if(value % 17 == 0) return value % 85 == 0 ? 2 : 4;
	return 8;
}

static void color_tree_cleanup(ColorTree* tree)
{
	int i;
	for(i = 0; i != 16; ++i)
	{
		if(tree->children[i])
		{
			color_tree_cleanup(tree->children[i]);
			lodepng_free(tree->children[i]);
		}
	}
}

/*returns -1 if color not present, its index otherwise*/
static int color_tree_get(ColorTree* tree, unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
	int bit = 0;
	for(bit = 0; bit < 8; ++bit)
	{
		int i = 8 * ((r >> bit) & 1) + 4 * ((g >> bit) & 1) + 2 * ((b >> bit) & 1) + 1 * ((a >> bit) & 1);
		if(!tree->children[i]) return -1;
		else tree = tree->children[i];
	}
	return tree ? tree->index : -1;
}

static int color_tree_has(ColorTree* tree, unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
	return color_tree_get(tree, r, g, b, a) >= 0;
}

/*color is not allowed to already exist.
Index should be >= 0 (it's signed to be compatible with using -1 for "doesn't exist")*/
static void color_tree_add(ColorTree* tree,
						   unsigned char r, unsigned char g, unsigned char b, unsigned char a, unsigned index)
{
	int bit;
	for(bit = 0; bit < 8; ++bit)
	{
		int i = 8 * ((r >> bit) & 1) + 4 * ((g >> bit) & 1) + 2 * ((b >> bit) & 1) + 1 * ((a >> bit) & 1);
		if(!tree->children[i])
		{
			tree->children[i] = (ColorTree*)lodepng_malloc(sizeof(ColorTree));
			color_tree_init(tree->children[i]);
		}
		tree = tree->children[i];
	}
	tree->index = (int)index;
}

unsigned lodepng_get_color_profile(LodePNGColorProfile* profile,
								   const unsigned char* in, unsigned w, unsigned h,
								   const LodePNGColorMode* mode)
{
	unsigned error = 0;
	size_t i;
	ColorTree tree;
	size_t numpixels = w * h;

	unsigned colored_done = lodepng_is_greyscale_type(mode) ? 1 : 0;
	unsigned alpha_done = lodepng_can_have_alpha(mode) ? 0 : 1;
	unsigned numcolors_done = 0;
	unsigned bpp = lodepng_get_bpp(mode);
	unsigned bits_done = bpp == 1 ? 1 : 0;
	unsigned maxnumcolors = 257;
	unsigned sixteen = 0;
	if(bpp <= 8) maxnumcolors = bpp == 1 ? 2 : (bpp == 2 ? 4 : (bpp == 4 ? 16 : 256));

	color_tree_init(&tree);

	/*Check if the 16-bit input is truly 16-bit*/
	if(mode->bitdepth == 16)
	{
		unsigned short r, g, b, a;
		for(i = 0; i != numpixels; ++i)
		{
			getPixelColorRGBA16(&r, &g, &b, &a, in, i, mode);
			if((r & 255) != ((r >> 8) & 255) || (g & 255) != ((g >> 8) & 255) ||
				(b & 255) != ((b >> 8) & 255) || (a & 255) != ((a >> 8) & 255)) /*first and second byte differ*/
			{
				sixteen = 1;
				break;
			}
		}
	}

	if(sixteen)
	{
		unsigned short r = 0, g = 0, b = 0, a = 0;
		profile->bits = 16;
		bits_done = numcolors_done = 1; /*counting colors no longer useful, palette doesn't support 16-bit*/

		for(i = 0; i != numpixels; ++i)
		{
			getPixelColorRGBA16(&r, &g, &b, &a, in, i, mode);

			if(!colored_done && (r != g || r != b))
			{
				profile->colored = 1;
				colored_done = 1;
			}

			if(!alpha_done)
			{
				unsigned matchkey = (r == profile->key_r && g == profile->key_g && b == profile->key_b);
				if(a != 65535 && (a != 0 || (profile->key && !matchkey)))
				{
					profile->alpha = 1;
					alpha_done = 1;
					if(profile->bits < 8) profile->bits = 8; /*PNG has no alphachannel modes with less than 8-bit per channel*/
				}
				else if(a == 0 && !profile->alpha && !profile->key)
				{
					profile->key = 1;
					profile->key_r = r;
					profile->key_g = g;
					profile->key_b = b;
				}
				else if(a == 65535 && profile->key && matchkey)
				{
					/* Color key cannot be used if an opaque pixel also has that RGB color. */
					profile->alpha = 1;
					alpha_done = 1;
				}
			}
			if(alpha_done && numcolors_done && colored_done && bits_done) break;
		}

		if(profile->key && !profile->alpha)
		{
			for(i = 0; i != numpixels; ++i)
			{
				getPixelColorRGBA16(&r, &g, &b, &a, in, i, mode);
				if(a != 0 && r == profile->key_r && g == profile->key_g && b == profile->key_b)
				{
					/* Color key cannot be used if an opaque pixel also has that RGB color. */
					profile->alpha = 1;
					alpha_done = 1;
				}
			}
		}
	}
	else /* < 16-bit */
	{
		unsigned char r = 0, g = 0, b = 0, a = 0;
		for(i = 0; i != numpixels; ++i)
		{
			getPixelColorRGBA8(&r, &g, &b, &a, in, i, mode);

			if(!bits_done && profile->bits < 8)
			{
				/*only r is checked, < 8 bits is only relevant for greyscale*/
				unsigned bits = getValueRequiredBits(r);
				if(bits > profile->bits) profile->bits = bits;
			}
			bits_done = (profile->bits >= bpp);

			if(!colored_done && (r != g || r != b))
			{
				profile->colored = 1;
				colored_done = 1;
				if(profile->bits < 8) profile->bits = 8; /*PNG has no colored modes with less than 8-bit per channel*/
			}

			if(!alpha_done)
			{
				unsigned matchkey = (r == profile->key_r && g == profile->key_g && b == profile->key_b);
				if(a != 255 && (a != 0 || (profile->key && !matchkey)))
				{
					profile->alpha = 1;
					alpha_done = 1;
					if(profile->bits < 8) profile->bits = 8; /*PNG has no alphachannel modes with less than 8-bit per channel*/
				}
				else if(a == 0 && !profile->alpha && !profile->key)
				{
					profile->key = 1;
					profile->key_r = r;
					profile->key_g = g;
					profile->key_b = b;
				}
				else if(a == 255 && profile->key && matchkey)
				{
					/* Color key cannot be used if an opaque pixel also has that RGB color. */
					profile->alpha = 1;
					alpha_done = 1;
					if(profile->bits < 8) profile->bits = 8; /*PNG has no alphachannel modes with less than 8-bit per channel*/
				}
			}

			if(!numcolors_done)
			{
				if(!color_tree_has(&tree, r, g, b, a))
				{
					color_tree_add(&tree, r, g, b, a, profile->numcolors);
					if(profile->numcolors < 256)
					{
						unsigned char* p = profile->palette;
						unsigned n = profile->numcolors;
						p[n * 4 + 0] = r;
						p[n * 4 + 1] = g;
						p[n * 4 + 2] = b;
						p[n * 4 + 3] = a;
					}
					++profile->numcolors;
					numcolors_done = profile->numcolors >= maxnumcolors;
				}
			}

			if(alpha_done && numcolors_done && colored_done && bits_done) break;
		}

		if(profile->key && !profile->alpha)
		{
			for(i = 0; i != numpixels; ++i)
			{
				getPixelColorRGBA8(&r, &g, &b, &a, in, i, mode);
				if(a != 0 && r == profile->key_r && g == profile->key_g && b == profile->key_b)
				{
					/* Color key cannot be used if an opaque pixel also has that RGB color. */
					profile->alpha = 1;
					alpha_done = 1;
				}
			}
		}

		/*make the profile's key always 16-bit for consistency - repeat each byte twice*/
		profile->key_r += (profile->key_r << 8);
		profile->key_g += (profile->key_g << 8);
		profile->key_b += (profile->key_b << 8);
	}

	color_tree_cleanup(&tree);
	return error;
}

void lodepng_palette_clear(LodePNGColorMode* info)
{
	if(info->palette) lodepng_free(info->palette);
	info->palette = 0;
	info->palettesize = 0;
}

unsigned lodepng_palette_add(LodePNGColorMode* info,
							 unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
	unsigned char* data;
	/*the same resize technique as C++ std::vectors is used, and here it's made so that for a palette with
	the max of 256 colors, it'll have the exact alloc size*/
	if(!info->palette) /*allocate palette if empty*/
	{
		/*room for 256 colors with 4 bytes each*/
		data = (unsigned char*)lodepng_realloc(info->palette, 1024);
		if(!data) return 83; /*alloc fail*/
		else info->palette = data;
	}
	info->palette[4 * info->palettesize + 0] = r;
	info->palette[4 * info->palettesize + 1] = g;
	info->palette[4 * info->palettesize + 2] = b;
	info->palette[4 * info->palettesize + 3] = a;
	++info->palettesize;
	return 0;
}

void lodepng_color_profile_init(LodePNGColorProfile* profile)
{
	profile->colored = 0;
	profile->key = 0;
	profile->alpha = 0;
	profile->key_r = profile->key_g = profile->key_b = 0;
	profile->numcolors = 0;
	profile->bits = 1;
}

void lodepng_color_mode_cleanup(LodePNGColorMode* info)
{
	lodepng_palette_clear(info);
}

unsigned lodepng_color_mode_copy(LodePNGColorMode* dest, const LodePNGColorMode* source)
{
	size_t i;
	lodepng_color_mode_cleanup(dest);
	*dest = *source;
	if(source->palette)
	{
		dest->palette = (unsigned char*)lodepng_malloc(1024);
		if(!dest->palette && source->palettesize) return 83; /*alloc fail*/
		for(i = 0; i != source->palettesize * 4; ++i) dest->palette[i] = source->palette[i];
	}
	return 0;
}

/*Automatically chooses color type that gives smallest amount of bits in the
output image, e.g. grey if there are only greyscale pixels, palette if there
are less than 256 colors, ...
Updates values of mode with a potentially smaller color model. mode_out should
contain the user chosen color model, but will be overwritten with the new chosen one.*/
unsigned lodepng_auto_choose_color(LodePNGColorMode* mode_out,
								   const unsigned char* image, unsigned w, unsigned h,
								   const LodePNGColorMode* mode_in)
{
	LodePNGColorProfile prof;
	unsigned error = 0;
	unsigned i, n, palettebits, grey_ok, palette_ok;

	lodepng_color_profile_init(&prof);
	error = lodepng_get_color_profile(&prof, image, w, h, mode_in);
	if(error) return error;
	mode_out->key_defined = 0;

	if(prof.key && w * h <= 16)
	{
		prof.alpha = 1; /*too few pixels to justify tRNS chunk overhead*/
		if(prof.bits < 8) prof.bits = 8; /*PNG has no alphachannel modes with less than 8-bit per channel*/
	}
	grey_ok = !prof.colored && !prof.alpha; /*grey without alpha, with potentially low bits*/
	n = prof.numcolors;
	palettebits = n <= 2 ? 1 : (n <= 4 ? 2 : (n <= 16 ? 4 : 8));
	palette_ok = n <= 256 && (n * 2 < w * h) && prof.bits <= 8;
	if(w * h < n * 2) palette_ok = 0; /*don't add palette overhead if image has only a few pixels*/
	if(grey_ok && prof.bits <= palettebits) palette_ok = 0; /*grey is less overhead*/

	if(palette_ok)
	{
		unsigned char* p = prof.palette;
		lodepng_palette_clear(mode_out); /*remove potential earlier palette*/
		for(i = 0; i != prof.numcolors; ++i)
		{
			error = lodepng_palette_add(mode_out, p[i * 4 + 0], p[i * 4 + 1], p[i * 4 + 2], p[i * 4 + 3]);
			if(error) break;
		}

		mode_out->colortype = LCT_PALETTE;
		mode_out->bitdepth = palettebits;

		if(mode_in->colortype == LCT_PALETTE && mode_in->palettesize >= mode_out->palettesize
			&& mode_in->bitdepth == mode_out->bitdepth)
		{
			/*If input should have same palette colors, keep original to preserve its order and prevent conversion*/
			lodepng_color_mode_cleanup(mode_out);
			lodepng_color_mode_copy(mode_out, mode_in);
		}
	}
	else /*8-bit or 16-bit per channel*/
	{
		mode_out->bitdepth = prof.bits;
		mode_out->colortype = prof.alpha ? (prof.colored ? LCT_RGBA : LCT_GREY_ALPHA)
			: (prof.colored ? LCT_RGB : LCT_GREY);

		if(prof.key && !prof.alpha)
		{
			unsigned mask = (1u << mode_out->bitdepth) - 1u; /*profile always uses 16-bit, mask converts it*/
			mode_out->key_r = prof.key_r & mask;
			mode_out->key_g = prof.key_g & mask;
			mode_out->key_b = prof.key_b & mask;
			mode_out->key_defined = 1;
		}
	}

	return error;
}

void lodepng_color_mode_init(LodePNGColorMode* info)
{
	info->key_defined = 0;
	info->key_r = info->key_g = info->key_b = 0;
	info->colortype = LCT_RGBA;
	info->bitdepth = 8;
	info->palette = 0;
	info->palettesize = 0;
}

static unsigned checkColorValidity(LodePNGColorType colortype, unsigned bd) /*bd = bitdepth*/
{
	switch(colortype)
	{
	case 0: if(!(bd == 1 || bd == 2 || bd == 4 || bd == 8 || bd == 16)) return 37; break; /*grey*/
	case 2: if(!(                                 bd == 8 || bd == 16)) return 37; break; /*RGB*/
	case 3: if(!(bd == 1 || bd == 2 || bd == 4 || bd == 8            )) return 37; break; /*palette*/
	case 4: if(!(                                 bd == 8 || bd == 16)) return 37; break; /*grey + alpha*/
	case 6: if(!(                                 bd == 8 || bd == 16)) return 37; break; /*RGBA*/
	default: return 31;
	}
	return 0; /*allowed color type / bits combination*/
}

static int lodepng_color_mode_equal(const LodePNGColorMode* a, const LodePNGColorMode* b)
{
	size_t i;
	if(a->colortype != b->colortype) return 0;
	if(a->bitdepth != b->bitdepth) return 0;
	if(a->key_defined != b->key_defined) return 0;
	if(a->key_defined)
	{
		if(a->key_r != b->key_r) return 0;
		if(a->key_g != b->key_g) return 0;
		if(a->key_b != b->key_b) return 0;
	}
	/*if one of the palette sizes is 0, then we consider it to be the same as the
	other: it means that e.g. the palette was not given by the user and should be
	considered the same as the palette inside the PNG.*/
	if(1/*a->palettesize != 0 && b->palettesize != 0*/) {
		if(a->palettesize != b->palettesize) return 0;
		for(i = 0; i != a->palettesize * 4; ++i)
		{
			if(a->palette[i] != b->palette[i]) return 0;
		}
	}
	return 1;
}

size_t lodepng_get_raw_size(unsigned w, unsigned h, const LodePNGColorMode* color)
{
	/*will not overflow for any color type if roughly w * h < 268435455*/
	size_t bpp = lodepng_get_bpp(color);
	size_t n = w * h;
	return ((n / 8) * bpp) + ((n & 7) * bpp + 7) / 8;
}

/*put a pixel, given its RGBA16 color, into image of any color 16-bitdepth type*/
static void rgba16ToPixel(unsigned char* out, size_t i,
						  const LodePNGColorMode* mode,
						  unsigned short r, unsigned short g, unsigned short b, unsigned short a)
{
	if(mode->colortype == LCT_GREY)
	{
		unsigned short grey = r; /*((unsigned)r + g + b) / 3*/;
		out[i * 2 + 0] = (grey >> 8) & 255;
		out[i * 2 + 1] = grey & 255;
	}
	else if(mode->colortype == LCT_RGB)
	{
		out[i * 6 + 0] = (r >> 8) & 255;
		out[i * 6 + 1] = r & 255;
		out[i * 6 + 2] = (g >> 8) & 255;
		out[i * 6 + 3] = g & 255;
		out[i * 6 + 4] = (b >> 8) & 255;
		out[i * 6 + 5] = b & 255;
	}
	else if(mode->colortype == LCT_GREY_ALPHA)
	{
		unsigned short grey = r; /*((unsigned)r + g + b) / 3*/;
		out[i * 4 + 0] = (grey >> 8) & 255;
		out[i * 4 + 1] = grey & 255;
		out[i * 4 + 2] = (a >> 8) & 255;
		out[i * 4 + 3] = a & 255;
	}
	else if(mode->colortype == LCT_RGBA)
	{
		out[i * 8 + 0] = (r >> 8) & 255;
		out[i * 8 + 1] = r & 255;
		out[i * 8 + 2] = (g >> 8) & 255;
		out[i * 8 + 3] = g & 255;
		out[i * 8 + 4] = (b >> 8) & 255;
		out[i * 8 + 5] = b & 255;
		out[i * 8 + 6] = (a >> 8) & 255;
		out[i * 8 + 7] = a & 255;
	}
}

/*Similar to getPixelColorRGBA8, but with all the for loops inside of the color
mode test cases, optimized to convert the colors much faster, when converting
to RGBA or RGB with 8 bit per cannel. buffer must be RGBA or RGB output with
enough memory, if has_alpha is true the output is RGBA. mode has the color mode
of the input buffer.*/
static void getPixelColorsRGBA8(unsigned char* buffer, size_t numpixels,
								unsigned has_alpha, const unsigned char* in,
								const LodePNGColorMode* mode)
{
	unsigned num_channels = has_alpha ? 4 : 3;
	size_t i;
	if(mode->colortype == LCT_GREY)
	{
		if(mode->bitdepth == 8)
		{
			for(i = 0; i != numpixels; ++i, buffer += num_channels)
			{
				buffer[0] = buffer[1] = buffer[2] = in[i];
				if(has_alpha) buffer[3] = mode->key_defined && in[i] == mode->key_r ? 0 : 255;
			}
		}
		else if(mode->bitdepth == 16)
		{
			for(i = 0; i != numpixels; ++i, buffer += num_channels)
			{
				buffer[0] = buffer[1] = buffer[2] = in[i * 2];
				if(has_alpha) buffer[3] = mode->key_defined && 256U * in[i * 2 + 0] + in[i * 2 + 1] == mode->key_r ? 0 : 255;
			}
		}
		else
		{
			unsigned highest = ((1U << mode->bitdepth) - 1U); /*highest possible value for this bit depth*/
			size_t j = 0;
			for(i = 0; i != numpixels; ++i, buffer += num_channels)
			{
				unsigned value = readBitsFromReversedStream(&j, in, mode->bitdepth);
				buffer[0] = buffer[1] = buffer[2] = (value * 255) / highest;
				if(has_alpha) buffer[3] = mode->key_defined && value == mode->key_r ? 0 : 255;
			}
		}
	}
	else if(mode->colortype == LCT_RGB)
	{
		if(mode->bitdepth == 8)
		{
			for(i = 0; i != numpixels; ++i, buffer += num_channels)
			{
				buffer[0] = in[i * 3 + 0];
				buffer[1] = in[i * 3 + 1];
				buffer[2] = in[i * 3 + 2];
				if(has_alpha) buffer[3] = mode->key_defined && buffer[0] == mode->key_r
					&& buffer[1]== mode->key_g && buffer[2] == mode->key_b ? 0 : 255;
			}
		}
		else
		{
			for(i = 0; i != numpixels; ++i, buffer += num_channels)
			{
				buffer[0] = in[i * 6 + 0];
				buffer[1] = in[i * 6 + 2];
				buffer[2] = in[i * 6 + 4];
				if(has_alpha) buffer[3] = mode->key_defined
					&& 256U * in[i * 6 + 0] + in[i * 6 + 1] == mode->key_r
					&& 256U * in[i * 6 + 2] + in[i * 6 + 3] == mode->key_g
					&& 256U * in[i * 6 + 4] + in[i * 6 + 5] == mode->key_b ? 0 : 255;
			}
		}
	}
	else if(mode->colortype == LCT_PALETTE)
	{
		unsigned index;
		size_t j = 0;
		for(i = 0; i != numpixels; ++i, buffer += num_channels)
		{
			if(mode->bitdepth == 8) index = in[i];
			else index = readBitsFromReversedStream(&j, in, mode->bitdepth);

			if(index >= mode->palettesize)
			{
				/*This is an error according to the PNG spec, but most PNG decoders make it black instead.
				Done here too, slightly faster due to no error handling needed.*/
				buffer[0] = buffer[1] = buffer[2] = 0;
				if(has_alpha) buffer[3] = 255;
			}
			else
			{
				buffer[0] = mode->palette[index * 4 + 0];
				buffer[1] = mode->palette[index * 4 + 1];
				buffer[2] = mode->palette[index * 4 + 2];
				if(has_alpha) buffer[3] = mode->palette[index * 4 + 3];
			}
		}
	}
	else if(mode->colortype == LCT_GREY_ALPHA)
	{
		if(mode->bitdepth == 8)
		{
			for(i = 0; i != numpixels; ++i, buffer += num_channels)
			{
				buffer[0] = buffer[1] = buffer[2] = in[i * 2 + 0];
				if(has_alpha) buffer[3] = in[i * 2 + 1];
			}
		}
		else
		{
			for(i = 0; i != numpixels; ++i, buffer += num_channels)
			{
				buffer[0] = buffer[1] = buffer[2] = in[i * 4 + 0];
				if(has_alpha) buffer[3] = in[i * 4 + 2];
			}
		}
	}
	else if(mode->colortype == LCT_RGBA)
	{
		if(mode->bitdepth == 8)
		{
			for(i = 0; i != numpixels; ++i, buffer += num_channels)
			{
				buffer[0] = in[i * 4 + 0];
				buffer[1] = in[i * 4 + 1];
				buffer[2] = in[i * 4 + 2];
				if(has_alpha) buffer[3] = in[i * 4 + 3];
			}
		}
		else
		{
			for(i = 0; i != numpixels; ++i, buffer += num_channels)
			{
				buffer[0] = in[i * 8 + 0];
				buffer[1] = in[i * 8 + 2];
				buffer[2] = in[i * 8 + 4];
				if(has_alpha) buffer[3] = in[i * 8 + 6];
			}
		}
	}
}

/*index: bitgroup index, bits: bitgroup size(1, 2 or 4), in: bitgroup value, out: octet array to add bits to*/
static void addColorBits(unsigned char* out, size_t index, unsigned bits, unsigned in)
{
	unsigned m = bits == 1 ? 7 : bits == 2 ? 3 : 1; /*8 / bits - 1*/
	/*p = the partial index in the byte, e.g. with 4 palettebits it is 0 for first half or 1 for second half*/
	unsigned p = index & m;
	in &= (1u << bits) - 1u; /*filter out any other bits of the input value*/
	in = in << (bits * (m - p));
	if(p == 0) out[index * bits / 8] = in;
	else out[index * bits / 8] |= in;
}

/*put a pixel, given its RGBA color, into image of any color type*/
static unsigned rgba8ToPixel(unsigned char* out, size_t i,
							 const LodePNGColorMode* mode, ColorTree* tree /*for palette*/,
							 unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
	if(mode->colortype == LCT_GREY)
	{
		unsigned char grey = r; /*((unsigned short)r + g + b) / 3*/;
		if(mode->bitdepth == 8) out[i] = grey;
		else if(mode->bitdepth == 16) out[i * 2 + 0] = out[i * 2 + 1] = grey;
		else
		{
			/*take the most significant bits of grey*/
			grey = (grey >> (8 - mode->bitdepth)) & ((1 << mode->bitdepth) - 1);
			addColorBits(out, i, mode->bitdepth, grey);
		}
	}
	else if(mode->colortype == LCT_RGB)
	{
		if(mode->bitdepth == 8)
		{
			out[i * 3 + 0] = r;
			out[i * 3 + 1] = g;
			out[i * 3 + 2] = b;
		}
		else
		{
			out[i * 6 + 0] = out[i * 6 + 1] = r;
			out[i * 6 + 2] = out[i * 6 + 3] = g;
			out[i * 6 + 4] = out[i * 6 + 5] = b;
		}
	}
	else if(mode->colortype == LCT_PALETTE)
	{
		int index = color_tree_get(tree, r, g, b, a);
		if(index < 0) return 82; /*color not in palette*/
		if(mode->bitdepth == 8) out[i] = index;
		else addColorBits(out, i, mode->bitdepth, (unsigned)index);
	}
	else if(mode->colortype == LCT_GREY_ALPHA)
	{
		unsigned char grey = r; /*((unsigned short)r + g + b) / 3*/;
		if(mode->bitdepth == 8)
		{
			out[i * 2 + 0] = grey;
			out[i * 2 + 1] = a;
		}
		else if(mode->bitdepth == 16)
		{
			out[i * 4 + 0] = out[i * 4 + 1] = grey;
			out[i * 4 + 2] = out[i * 4 + 3] = a;
		}
	}
	else if(mode->colortype == LCT_RGBA)
	{
		if(mode->bitdepth == 8)
		{
			out[i * 4 + 0] = r;
			out[i * 4 + 1] = g;
			out[i * 4 + 2] = b;
			out[i * 4 + 3] = a;
		}
		else
		{
			out[i * 8 + 0] = out[i * 8 + 1] = r;
			out[i * 8 + 2] = out[i * 8 + 3] = g;
			out[i * 8 + 4] = out[i * 8 + 5] = b;
			out[i * 8 + 6] = out[i * 8 + 7] = a;
		}
	}

	return 0; /*no error*/
}

unsigned lodepng_convert(unsigned char* out, const unsigned char* in,
						 const LodePNGColorMode* mode_out, const LodePNGColorMode* mode_in,
						 unsigned w, unsigned h)
{
	size_t i;
	ColorTree tree;
	size_t numpixels = w * h;

	if(lodepng_color_mode_equal(mode_out, mode_in))
	{
		size_t numbytes = lodepng_get_raw_size(w, h, mode_in);
		for(i = 0; i != numbytes; ++i) out[i] = in[i];
		return 0;
	}

	if(mode_out->colortype == LCT_PALETTE)
	{
		size_t palettesize = mode_out->palettesize;
		const unsigned char* palette = mode_out->palette;
		size_t palsize = 1u << mode_out->bitdepth;
		/*if the user specified output palette but did not give the values, assume
		they want the values of the input color type (assuming that one is palette).
		Note that we never create a new palette ourselves.*/
		if(palettesize == 0)
		{
			palettesize = mode_in->palettesize;
			palette = mode_in->palette;
		}
		if(palettesize < palsize) palsize = palettesize;
		color_tree_init(&tree);
		for(i = 0; i != palsize; ++i)
		{
			const unsigned char* p = &palette[i * 4];
			color_tree_add(&tree, p[0], p[1], p[2], p[3], i);
		}
	}

	if(mode_in->bitdepth == 16 && mode_out->bitdepth == 16)
	{
		for(i = 0; i != numpixels; ++i)
		{
			unsigned short r = 0, g = 0, b = 0, a = 0;
			getPixelColorRGBA16(&r, &g, &b, &a, in, i, mode_in);
			rgba16ToPixel(out, i, mode_out, r, g, b, a);
		}
	}
	else if(mode_out->bitdepth == 8 && mode_out->colortype == LCT_RGBA)
	{
		getPixelColorsRGBA8(out, numpixels, 1, in, mode_in);
	}
	else if(mode_out->bitdepth == 8 && mode_out->colortype == LCT_RGB)
	{
		getPixelColorsRGBA8(out, numpixels, 0, in, mode_in);
	}
	else
	{
		unsigned char r = 0, g = 0, b = 0, a = 0;
		for(i = 0; i != numpixels; ++i)
		{
			getPixelColorRGBA8(&r, &g, &b, &a, in, i, mode_in);
			CERROR_TRY_RETURN(rgba8ToPixel(out, i, mode_out, &tree, r, g, b, a));
		}
	}

	if(mode_out->colortype == LCT_PALETTE)
	{
		color_tree_cleanup(&tree);
	}

	return 0; /*no error*/
}

static void setBitOfReversedStream(size_t* bitpointer, unsigned char* bitstream, unsigned char bit)
{
  /*the current bit in bitstream may be 0 or 1 for this to work*/
  if(bit == 0) bitstream[(*bitpointer) >> 3] &=  (unsigned char)(~(1 << (7 - ((*bitpointer) & 0x7))));
  else         bitstream[(*bitpointer) >> 3] |=  (1 << (7 - ((*bitpointer) & 0x7)));
  ++(*bitpointer);
}

static void addPaddingBits(unsigned char* out, const unsigned char* in,
                           size_t olinebits, size_t ilinebits, unsigned h)
{
  /*The opposite of the removePaddingBits function
  olinebits must be >= ilinebits*/
  unsigned y;
  size_t diff = olinebits - ilinebits;
  size_t obp = 0, ibp = 0; /*bit pointers*/
  for(y = 0; y != h; ++y)
  {
    size_t x;
    for(x = 0; x < ilinebits; ++x)
    {
      unsigned char bit = readBitFromReversedStream(&ibp, in);
      setBitOfReversedStream(&obp, out, bit);
    }
    /*obp += diff; --> no, fill in some value in the padding bits too, to avoid
    "Use of uninitialised value of size ###" warning from valgrind*/
    for(x = 0; x != diff; ++x) setBitOfReversedStream(&obp, out, 0);
  }
}

/*
Paeth predicter, used by PNG filter type 4
The parameters are of type short, but should come from unsigned chars, the shorts
are only needed to make the paeth calculation correct.
*/
static unsigned char paethPredictor(short a, short b, short c)
{
  short pa = abs(b - c);
  short pb = abs(a - c);
  short pc = abs(a + b - c - c);

  if(pc < pa && pc < pb) return (unsigned char)c;
  else if(pb < pa) return (unsigned char)b;
  else return (unsigned char)a;
}

static void filterScanline(unsigned char* out, const unsigned char* scanline, const unsigned char* prevline,
                           size_t length, size_t bytewidth, unsigned char filterType)
{
  size_t i;
  switch(filterType)
  {
    case 0: /*None*/
      for(i = 0; i != length; ++i) out[i] = scanline[i];
      break;
    case 1: /*Sub*/
      for(i = 0; i != bytewidth; ++i) out[i] = scanline[i];
      for(i = bytewidth; i < length; ++i) out[i] = scanline[i] - scanline[i - bytewidth];
      break;
    case 2: /*Up*/
      if(prevline)
      {
        for(i = 0; i != length; ++i) out[i] = scanline[i] - prevline[i];
      }
      else
      {
        for(i = 0; i != length; ++i) out[i] = scanline[i];
      }
      break;
    case 3: /*Average*/
      if(prevline)
      {
        for(i = 0; i != bytewidth; ++i) out[i] = scanline[i] - (prevline[i] >> 1);
        for(i = bytewidth; i < length; ++i) out[i] = scanline[i] - ((scanline[i - bytewidth] + prevline[i]) >> 1);
      }
      else
      {
        for(i = 0; i != bytewidth; ++i) out[i] = scanline[i];
        for(i = bytewidth; i < length; ++i) out[i] = scanline[i] - (scanline[i - bytewidth] >> 1);
      }
      break;
    case 4: /*Paeth*/
      if(prevline)
      {
        /*paethPredictor(0, prevline[i], 0) is always prevline[i]*/
        for(i = 0; i != bytewidth; ++i) out[i] = (scanline[i] - prevline[i]);
        for(i = bytewidth; i < length; ++i)
        {
          out[i] = (scanline[i] - paethPredictor(scanline[i - bytewidth], prevline[i], prevline[i - bytewidth]));
        }
      }
      else
      {
        for(i = 0; i != bytewidth; ++i) out[i] = scanline[i];
        /*paethPredictor(scanline[i - bytewidth], 0, 0) is always scanline[i - bytewidth]*/
        for(i = bytewidth; i < length; ++i) out[i] = (scanline[i] - scanline[i - bytewidth]);
      }
      break;
    default: return; /*unexisting filter type given*/
  }
}

static unsigned filter(unsigned char* out, const unsigned char* in, unsigned w, unsigned h,
                       const LodePNGColorMode* info)
{
  /*
  For PNG filter method 0
  out must be a buffer with as size: h + (w * h * bpp + 7) / 8, because there are
  the scanlines with 1 extra byte per scanline
  */

  unsigned bpp = lodepng_get_bpp(info);
  /*the width of a scanline in bytes, not including the filter type*/
  size_t linebytes = (w * bpp + 7) / 8;
  /*bytewidth is used for filtering, is 1 when bpp < 8, number of bytes per pixel otherwise*/
  size_t bytewidth = (bpp + 7) / 8;
  const unsigned char* prevline = 0;
  unsigned x, y;
  unsigned error = 0;

  /*
  There is a heuristic called the minimum sum of absolute differences heuristic, suggested by the PNG standard:
   *  If the image type is Palette, or the bit depth is smaller than 8, then do not filter the image (i.e.
      use fixed filtering, with the filter None).
   * (The other case) If the image type is Grayscale or RGB (with or without Alpha), and the bit depth is
     not smaller than 8, then use adaptive filtering heuristic as follows: independently for each row, apply
     all five filters and select the filter that produces the smallest sum of absolute values per row.
  This heuristic is used if filter strategy is LFS_MINSUM and filter_palette_zero is true.

  If filter_palette_zero is true and filter_strategy is not LFS_MINSUM, the above heuristic is followed,
  but for "the other case", whatever strategy filter_strategy is set to instead of the minimum sum
  heuristic is used.
  */

  if(bpp == 0) return 31; /*error: invalid color type*/

  if(info->colortype == LCT_PALETTE || info->bitdepth < 8)
  {
    for(y = 0; y != h; ++y)
    {
      //size_t outindex = (1 + linebytes) * y; /*the extra filterbyte added to each row*/
      size_t outindex = linebytes *y;
      size_t inindex = linebytes * y;
      out[outindex] = 0; /*filter type byte*/
      //filterScanline(&out[outindex + 1], &in[inindex], prevline, linebytes, bytewidth, 0);
      filterScanline(&out[outindex], &in[inindex], prevline, linebytes, bytewidth, 0);
      prevline = &in[inindex];
    }
  }
  else return 88;

  return error;
}


/*out must be buffer big enough to contain uncompressed IDAT chunk data, and in must contain the full image.
return value is error**/
static unsigned preProcessScanlines(unsigned char** out, size_t* outsize, const unsigned char* in,
                                    unsigned w, unsigned h,
                                    const LodePNGColorMode* color)
{
  /*
  This function converts the pure 2D image with the PNG's colortype, into filtered-padded-interlaced data. Steps:
  *) handle with no Adam7, add padding bits (= posible extra bits per scanline if bpp < 8) 2) filter
  */
  unsigned bpp = lodepng_get_bpp(color);
  unsigned error = 0;
  
  *outsize = (h * ((w * bpp + 7) / 8));
  *out = (unsigned char*)lodepng_malloc(*outsize);
  if(!(*out) && (*outsize)) error = 83; /*alloc fail*/
  
  if(!error)
  {
	   unsigned char* padded = (unsigned char*)lodepng_malloc(h * ((w * bpp + 7) / 8));
       if(!padded) error = 83; /*alloc fail*/
       if(!error)
       {
         addPaddingBits(padded, in, ((w * bpp + 7) / 8) * 8, w * bpp, h);
		 error = filter(*out, padded, w, h, color);
       }       
	   lodepng_free(padded);
  }
  return error;
}

//*************************************Need to vips (Start)***************************************************
void bytep_to_bytepp(const LodePNGColorMode* color, int width, int height, png_bytep in, png_bytepp row_pointer_out)
{
	int i,j;
	int span;
	int pos=0;
	int bpp= lodepng_get_bpp(color);

	for(i = 0; i< height; i++)
	{
		for(j = 0; j < width; j++)
		{
			if(bpp == 32)
			{
				row_pointer_out[i][4*j] = in[pos++];
				row_pointer_out[i][4*j+1] = in[pos++];
				row_pointer_out[i][4*j+2] = in[pos++];
				row_pointer_out[i][4*j+3] = in[pos++];
			}
			else if(bpp == 24)
			{
				row_pointer_out[i][3*j] = in[pos++];
				row_pointer_out[i][3*j+1] = in[pos++];
				row_pointer_out[i][3*j+2] = in[pos++];
			}
			else if(bpp == 8)
			{
				row_pointer_out[i][j] = in[pos++];
			}
			else if(bpp < 8)
			{
				span = 8/bpp;
				if(j%span == 0)
				{
					row_pointer_out[i][j/span] = in[pos++];
				}
			}
		}
	}
}

void bytepp_to_bytep(const LodePNGColorMode* color, int width, int height, png_bytep out, png_bytepp row_pointer_in)
{
	int i, j;
	int pos = 0;
	int size;
	
	int channel = color->colortype == LCT_RGBA ? 4 : 3;
	size = width * channel;
	
	for(i = 0; i < height; i++)
	{
		for(j = 0; j < size; j++)
		{
			out[pos++] = row_pointer_in[i][j];
		}
	}
}

png_bytep malloc_png_bytep(LodePNGColorMode* mode, int width, int height)
{
	png_bytep bytep;
	int channel = mode->colortype == LCT_RGBA ? 4 :3; 
	
	bytep = (png_bytep)malloc(sizeof(png_byte) * width * height * channel);
	return bytep;
}

png_bytepp malloc_png_bytepp(LodePNGColorMode* mode, int width, int height)
{
	int i;
	png_bytepp bytepp;
	int bpp = lodepng_get_bpp(mode);
	bytepp =(png_bytepp) malloc(sizeof(png_bytep) * height);
	for (i=0; i < height; i++)
	{
		bytepp[i] = (png_bytep)malloc(sizeof(png_byte) * (width * bpp + 7)/8);
	}
	return bytepp;
}

void free_png_bytepp(int height, png_bytepp row_pointer)
{
	int i;
    if(row_pointer)
	{
		for(i =0 ; i< height; i++)
		{
			if(row_pointer[i])
				free(row_pointer[i]);
		}
		free(row_pointer);
	}
}

void auto_convert_data(LodePNGColorMode* mode_in, LodePNGColorMode* mode_out, int width, int height, png_bytep in, png_bytep* row_pointer_out)
{
   unsigned char* data= 0;/*uncompressed version of the IDAT chunk data*/
   size_t datasize = 0;
   png_bytep converted;
   int bpp = lodepng_get_bpp(mode_out);
   int size = (width * height * lodepng_get_bpp(mode_out) + 7) / 8;
   int linebits = ((width * bpp + 7) / 8) * 8;
   converted = (png_bytep)malloc(sizeof(png_byte) * size);
   
   lodepng_convert(converted, in, mode_out, mode_in, width, height);
   if(bpp < 8 && width * bpp != linebits)
   {
	   preProcessScanlines(&data, &datasize, converted, width, height, mode_out);
       bytep_to_bytepp(mode_out, width, height, data, row_pointer_out);
	   free(data);
   }
   else
   {
       bytep_to_bytepp(mode_out, width, height, converted, row_pointer_out);
   }


   free(converted);
}

void color_mode_init(LodePNGColorMode* mode, png_byte color_type, png_byte bit_depth)
{
	mode->bitdepth = bit_depth;
	switch(color_type)
	{
	case PNG_COLOR_TYPE_GRAY:
		mode->colortype = LCT_GREY;
		break;
	case PNG_COLOR_TYPE_RGB:
		mode->colortype = LCT_RGB;
		break;
	case PNG_COLOR_TYPE_PALETTE:
		mode->colortype = LCT_PALETTE;
		break;
	case PNG_COLOR_TYPE_GRAY_ALPHA:
		mode->colortype = LCT_GREY_ALPHA;
		break;
	case PNG_COLOR_TYPE_RGBA:
		mode->colortype = LCT_RGBA;
		break;
	}
}

void SetIHDR(png_structp png_ptr, png_infop info_ptr, LodePNGColorMode* mode,int width, int hight)
{
	png_set_IHDR(png_ptr, info_ptr, width, hight, mode->bitdepth, mode->colortype,
		PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
}

void SetPLTE(png_structp png_ptr, png_infop info_ptr, LodePNGColorMode* mode,int width, int hight)
{
	int i;
	if(mode->colortype == LCT_PALETTE)
	{
		png_colorp palette=(png_colorp)malloc(sizeof(png_color)* mode->palettesize);
		for(i=0; i<mode->palettesize; i++)
		{
			palette[i].red= mode->palette[4*i];
			palette[i].green = mode->palette[4*i+1];
			palette[i].blue = mode->palette[4*i+2];
		}
		png_set_PLTE(png_ptr, info_ptr, palette, mode->palettesize);
		free(palette);
	}
}

void SetIDAT(png_structp png_ptr, png_bytepp row_pointers, int hight)
{
	png_write_rows(png_ptr, row_pointers, hight);
}

//************************************End Need to vips (End)***************************************************


//************************************Only for testing (Start)**********************************************
#include <io.h>
#define PNG_BYTES_TO_CHECK 4

typedef struct _auto_pic_data auto_pic_data;
struct _auto_pic_data
{
	int width;
	int height;
	png_bytepp row_pointers;
	int size; //coverted file size
	int src_size; //seed file size
};

typedef struct _file_type_info file_type_info;
struct _file_type_info
{
	int num;
	int size;
	int src_size;
};

void file_type_info_init(file_type_info* information)
{
	information->num = 0;
	information->size = 0;
	information->src_size =0;
}

typedef struct _pngexportinfo pngexportinfo;
struct _pngexportinfo
{
	file_type_info grey_bit1;
	file_type_info grey_bit2;
	file_type_info grey_bit4;
	file_type_info grey_bit8;

	file_type_info palette_bit1;
	file_type_info palette_bit2;
	file_type_info palette_bit4;
	file_type_info palette_bit8;

	file_type_info rgb;
	file_type_info rgba;
};
static pngexportinfo info;

void update_file_type_info(file_type_info* information, auto_pic_data* data)
{
	information->num++;
	information->size += data->size;
	information->src_size +=data->src_size;
}

static long lodepng_filesize(const char* filename)
{
	FILE* file;
	long size;
	file = fopen(filename, "rb");
	if(!file) return -1;

	if(fseek(file, 0, SEEK_END) != 0)
	{
		fclose(file);
		return -1;
	}

	size = ftell(file);
	/* It may give LONG_MAX as directory size, this is invalid for us. */
	if(size == LONG_MAX) size = -1;

	fclose(file);
	return size;
}

void init_info()
{
	file_type_info_init(&info.grey_bit1);
	file_type_info_init(&info.grey_bit2);
	file_type_info_init(&info.grey_bit4);
	file_type_info_init(&info.grey_bit8);

	file_type_info_init(&info.palette_bit1);
	file_type_info_init(&info.palette_bit2);
	file_type_info_init(&info.palette_bit4);
	file_type_info_init(&info.palette_bit8);

	file_type_info_init(&info.rgb);
	file_type_info_init(&info.rgba);
}

void display_info()
{
	int palette_num=0;
	int palette_size=0;
	int palette_src_size=0;

	int grey_num =0;
	int grey_size=0;
	int grey_src_size=0;

	int total_num =0;
	int total_size=0;
	int total_src_size=0;

	grey_num = info.grey_bit1.num + info.grey_bit2.num + info.grey_bit4.num +info.grey_bit8.num;
	grey_size = info.grey_bit1.size + info.grey_bit2.size + info.grey_bit4.size + info.grey_bit8.size;
	grey_src_size = info.grey_bit1.src_size + info.grey_bit2.src_size + info.grey_bit4.src_size + info.grey_bit8.src_size;

	palette_num =info.palette_bit1.num+info.palette_bit2.num+info.palette_bit4.num+info.palette_bit8.num;
	palette_size= info.palette_bit1.size+info.palette_bit2.size+info.palette_bit4.size+info.palette_bit8.size;
	palette_src_size= info.palette_bit1.src_size+info.palette_bit2.src_size+info.palette_bit4.src_size+info.palette_bit8.src_size;

	total_num = grey_num+palette_num+info.rgb.num+info.rgba.num;
	total_size = grey_size+palette_size+info.rgb.size+info.rgba.size;
	total_src_size= grey_src_size+ palette_src_size+info.rgb.src_size+info.rgba.src_size;

	printf("Total PNG: %3d   size = %d source_size = %d compress = %3f\n\n",total_num , total_size, total_src_size, (float)total_size/total_src_size);
	printf("All Grey    : %3d   size = %9d percent = %3f, compress = %3f\n",grey_num, grey_size, (float)grey_size/total_size, (float)grey_size/grey_src_size);
	printf("  Grey 1    : %3d   size = %9d percent = %3f, compress = %3f\n",info.grey_bit1.num, info.grey_bit1.size, (float)info.grey_bit1.size/total_size,(float)info.grey_bit1.size/info.grey_bit1.src_size);
	printf("  Grey 2    : %3d   size = %9d percent = %3f, compress = %3f\n",info.grey_bit2.num, info.grey_bit2.size, (float)info.grey_bit2.size/total_size,(float)info.grey_bit2.size/info.grey_bit2.src_size);
	printf("  Grey 4    : %3d   size = %9d percent = %3f, compress = %3f\n",info.grey_bit4.num, info.grey_bit4.size, (float)info.grey_bit4.size/total_size,(float)info.grey_bit4.size/info.grey_bit4.src_size);
	printf("  Grey 8    : %3d   size = %9d percent = %3f, compress = %3f\n\n",info.grey_bit8.num, info.grey_bit8.size, (float)info.grey_bit8.size/total_size,(float)info.grey_bit8.size/info.grey_bit8.src_size);
	printf("All Pal     : %3d   size = %9d percent = %3f, compress = %3f\n",palette_num, palette_size, (float)palette_size/total_size, (float)palette_size/palette_src_size);
	printf("  Palette 1 : %3d   size = %9d percent = %3f, compress = %3f\n",info.palette_bit1.num, info.palette_bit1.size, (float)info.palette_bit1.size/total_size,(float)info.palette_bit1.size/info.palette_bit1.src_size);
	printf("  Palette 2 : %3d   size = %9d percent = %3f, compress = %3f\n",info.palette_bit2.num, info.palette_bit2.size, (float)info.palette_bit2.size/total_size,(float)info.palette_bit2.size/info.palette_bit2.src_size);
	printf("  Palette 4 : %3d   size = %9d percent = %3f, compress = %3f\n",info.palette_bit4.num, info.palette_bit4.size, (float)info.palette_bit4.size/total_size,(float)info.palette_bit4.size/info.palette_bit4.src_size);
	printf("  Palette 8 : %3d   size = %9d percent = %3f, compress = %3f\n\n",info.palette_bit8.num, info.palette_bit8.size, (float)info.palette_bit8.size/total_size,(float)info.palette_bit8.size/info.palette_bit8.src_size);
	printf("Total RGB   : %3d   size = %9d percent = %3f, compress = %3f\n\n",info.rgb.num, info.rgb.size, (float)info.rgb.size/total_size,(float)info.rgb.size/info.rgb.src_size);
	printf("Total RGBA  : %3d   size = %9d percent = %3f, compress = %3f\n\n",info.rgba.num, info.rgba.size, (float)info.rgba.size/total_size, (float)info.rgba.size/info.rgba.src_size);
}

void update_info(char* file_path, LodePNGColorMode* mode, auto_pic_data* data)
{
	if(mode->colortype == PNG_COLOR_TYPE_GRAY)
	{
		if(mode->bitdepth ==1)
		{
			update_file_type_info(&info.grey_bit1, data);
		}
		else if(mode->bitdepth==2)
		{
			update_file_type_info(&info.grey_bit2, data);
			//printf("P2****** %s",file_path);
		}
		else if(mode->bitdepth==4)
		{
			update_file_type_info(&info.grey_bit4, data);
			//printf("P4****** %s",file_path);
		}
		else if(mode->bitdepth==8)
		{
			update_file_type_info(&info.grey_bit8, data);
		}
		else
		{
			printf("grey color bitdepth error\n");
		}
	}
	else if(mode->colortype == PNG_COLOR_TYPE_PALETTE)
	{
		if(mode->bitdepth ==1)
		{
			update_file_type_info(&info.palette_bit1, data);
		}
		else if(mode->bitdepth==2)
		{
			update_file_type_info(&info.palette_bit2, data);
		}
		else if(mode->bitdepth==4)
		{
			update_file_type_info(&info.palette_bit4, data);
		}
		else if(mode->bitdepth==8)
		{
			update_file_type_info(&info.palette_bit8, data);
		}
		else
		{
			printf("palette color bitdepth error\n");
		}
		
		//printf("palette bit_%d= %s\n", mode->bitdepth, file_path);

	}
	else if(mode->colortype ==PNG_COLOR_TYPE_RGB)
	{
		update_file_type_info(&info.rgb, data);
		printf("%s colortype = RGB\n",file_path);
	}
	else if(mode->colortype == PNG_COLOR_TYPE_RGB_ALPHA)
	{
		update_file_type_info(&info.rgba, data);
		printf("%s colortype = RGBA\n",file_path);
	}
	//printf("%s type = %d bit= %d\n",file_path, mode->colortype, mode->bitdepth);
}

int decode_png(char *file_path, LodePNGColorMode* mode_in, LodePNGColorMode* mode_out, auto_pic_data* pic_data)
{
	png_structp png_ptr;
	png_infop   info_ptr;
	char        buf[PNG_BYTES_TO_CHECK];
	int         temp;
	FILE *pic_fp;

	int width;
	int height;
	png_byte color_type;
	png_byte bit_depth;
   png_byte filter_type;

	//*******************************
    png_byte* in;
	png_bytep* row_pointer_in;
	png_bytep* row_pointer_out;
	//*******************************

	pic_fp = fopen(file_path, "rb");
	if(pic_fp == NULL) 
		return -1;

	png_ptr  = png_create_read_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
	info_ptr = png_create_info_struct(png_ptr);

	setjmp(png_jmpbuf(png_ptr)); 

	temp = fread(buf,1,PNG_BYTES_TO_CHECK,pic_fp);
	temp = png_sig_cmp((void*)buf, (png_size_t)0, PNG_BYTES_TO_CHECK);

	if (temp!=0) 
		return 1;

	rewind(pic_fp);
	png_init_io(png_ptr, pic_fp);
	png_read_png(png_ptr, info_ptr, PNG_TRANSFORM_EXPAND, 0);

	width = png_get_image_width(png_ptr, info_ptr);
	height = png_get_image_height(png_ptr, info_ptr);
	color_type = png_get_color_type(png_ptr,info_ptr);
	bit_depth = png_get_bit_depth(png_ptr, info_ptr);
	row_pointer_in = png_get_rows(png_ptr, info_ptr);
   filter_type = png_get_filter_type(png_ptr, info_ptr);

	pic_data->height = height;
	pic_data->width = width;
	pic_data->src_size = lodepng_filesize(file_path);
	pic_data->size = pic_data->src_size;
	pic_data->row_pointers = NULL;
	//**********************************************************************************
	color_mode_init(mode_in, color_type, bit_depth);
    lodepng_color_mode_cleanup(mode_out);
    lodepng_color_mode_copy(mode_out, mode_in);
	if(mode_in->colortype !=LCT_RGB && mode_in->colortype !=LCT_RGBA)
	{
		png_destroy_read_struct(&png_ptr, &info_ptr, 0);
		fclose(pic_fp);
		return -1;
	}

	in = malloc_png_bytep(mode_in, width, height);
   bytepp_to_bytep(mode_in, width, height, in, row_pointer_in);
	lodepng_auto_choose_color(mode_out, (unsigned char*)in, width, height, mode_in);
	
	//if mode_out equal mode_in, no need do anything.
	if(lodepng_color_mode_equal(mode_out, mode_in))
	{   
		png_destroy_read_struct(&png_ptr, &info_ptr, 0);
		fclose(pic_fp);
		return -2;
	}
	else
	{
		row_pointer_out = malloc_png_bytepp(mode_out, width, height);
        auto_convert_data(mode_in, mode_out, width, height, in, row_pointer_out);
		pic_data->row_pointers = row_pointer_out;
	}
	free(in);
	//***********************************************************************************

	png_destroy_read_struct(&png_ptr, &info_ptr, 0);
	fclose(pic_fp);
	return 0;

}

int encode_png(char* file_name, LodePNGColorMode* mode, auto_pic_data* pic_data)
{
	int w,h;
	png_structp png_ptr;
	png_infop info_ptr; 
	FILE *fp;
	h=pic_data->height;
	w=pic_data->width;

	fp = fopen(file_name, "wb");
	if (!fp)
	{
		printf("[write_png_file] File %s could not be opened for writing", file_name);
		return -1;
	}

	png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png_ptr)
	{
		printf("[write_png_file] png_create_write_struct failed");
		return -1;
	}

	info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr)
	{
		printf("[write_png_file] png_create_info_struct failed");
		return -1;
	}

	if (setjmp(png_jmpbuf(png_ptr)))
	{
		printf("[write_png_file] Error during init_io");
		return -1;
	}
	png_init_io(png_ptr, fp);

	if (setjmp(png_jmpbuf(png_ptr)))
	{
		printf("[write_png_file] Error during writing header");
		return -1;
	}

   png_set_compression_level(png_ptr, 9);

	SetIHDR(png_ptr, info_ptr, mode, w, h);
	SetPLTE(png_ptr, info_ptr, mode, w, h);

	/* write bytes */
	if (setjmp(png_jmpbuf(png_ptr)))
	{
		printf("[write_png_file] Error during writing bytes");
		return -1;
	}

	png_write_info(png_ptr, info_ptr);

	/* end write */
	if (setjmp(png_jmpbuf(png_ptr)))
	{
		printf("[write_png_file] Error during end of write");
		return -1;
	}

	//png_write_image(png_ptr, pic_data->row_pointers);
	SetIDAT(png_ptr, pic_data->row_pointers, h);

	png_write_end(png_ptr, NULL);
	fclose(fp);

	pic_data->size = lodepng_filesize(file_name);
	return 0;
}

void convert_png(char* file_path)
{
	int i,j;
	auto_pic_data* pic_data =(auto_pic_data*)malloc(sizeof(auto_pic_data));
	LodePNGColorMode* mode_out = (LodePNGColorMode*)malloc(sizeof(LodePNGColorMode));
	LodePNGColorMode* mode_in =(LodePNGColorMode*)malloc(sizeof(LodePNGColorMode));
	lodepng_color_mode_init(mode_out);
	lodepng_color_mode_init(mode_in);
    //printf("%s start\n", file_path);

	//Only mode_in is different mode_out, return 0, and encode png file again.
	if(decode_png(file_path, mode_in, mode_out, pic_data) == 0)
	{
		encode_png(file_path, mode_out, pic_data);
	}

	update_info(file_path, mode_out, pic_data);
	//printf("%s end\n", file_path);

	free_png_bytepp(pic_data->height, pic_data->row_pointers);
	lodepng_free(pic_data);
	lodepng_color_mode_cleanup(mode_out);
	lodepng_color_mode_cleanup(mode_in);
}

void convert_folder(const char * dir)
{
	long handle;
	struct _finddata_t FileInfo;
	char dirNew[_MAX_PATH];
	char ext[10];
	strcpy(dirNew, dir);
	strcat(dirNew, "\\*.*");

	handle = _findfirst(dirNew, &FileInfo);
	if (handle == -1)
		return;

	do
	{
		if (FileInfo.attrib & _A_SUBDIR)
		{
			if (strcmp(FileInfo.name, ".") == 0 || strcmp(FileInfo.name, "..") == 0)
				continue;

			strcpy(dirNew, dir);
			strcat(dirNew, "\\");
			strcat(dirNew, FileInfo.name);
			convert_folder(dirNew);
		}
		else
		{
			//cout << findData.name << "\t" << findData.size << " bytes.\n";
			strcpy(dirNew, dir);
			strcat(dirNew, "\\");
			strcat(dirNew, FileInfo.name);

			_splitpath(dirNew,NULL,NULL,NULL,ext);
			if(strnicmp(ext,".png",10) == 0)
			{
				convert_png(dirNew);
				//test_info(dirNew);
			}
		}
	} while (_findnext(handle, &FileInfo) == 0);

	_findclose(handle);    // close handle
}


int test_info(char *file_path)
{
	png_structp png_ptr;
	png_infop   info_ptr;
	char        buf[PNG_BYTES_TO_CHECK];
	int         temp;
	FILE *pic_fp;

	int width;
	int height;
	png_byte color_type;
	png_byte bit_depth;
    png_byte filter_type;
	png_byte channels;


	pic_fp = fopen(file_path, "rb");
	if(pic_fp == NULL) 
		return -1;

	png_ptr  = png_create_read_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
	info_ptr = png_create_info_struct(png_ptr);

	setjmp(png_jmpbuf(png_ptr)); 

	temp = fread(buf,1,PNG_BYTES_TO_CHECK,pic_fp);
	temp = png_sig_cmp((void*)buf, (png_size_t)0, PNG_BYTES_TO_CHECK);

	if (temp!=0) 
		return 1;

	rewind(pic_fp);
	png_init_io(png_ptr, pic_fp);
	png_read_png(png_ptr, info_ptr, PNG_TRANSFORM_EXPAND, 0);

	width = png_get_image_width(png_ptr, info_ptr);
	height = png_get_image_height(png_ptr, info_ptr);
	color_type = png_get_color_type(png_ptr,info_ptr);
	bit_depth = png_get_bit_depth(png_ptr, info_ptr);
    filter_type = png_get_filter_type(png_ptr, info_ptr);
	channels = png_get_channels(png_ptr, info_ptr);

	printf("%s\n", file_path);
	printf("color_type = %d, bit_depth = %d bbp = %d \n\n", color_type, bit_depth, bit_depth*channels);


	png_destroy_read_struct(&png_ptr, &info_ptr, 0);
	fclose(pic_fp);
	return 0;
}

//*************************************Only for testing (End)*********************************************** 


int main(int argc, char *argv[])
{
	unsigned char* data = 0;
    clock_t begin, end;
	double cost;
	begin =clock();
	init_info();
	printf("start\n");

	/*if(argc!=2)
	{
	printf("pngtest.exe path of tiles file folder\n");
	return -1;
	}
	convert_folder(argv[1]);*/

	//********************Covert PNG**************************
	//convert_png("D:\\testing");
	convert_folder("D:\\testing");
	//convert_png("D:\\3_2.png");
     //convert_png("D:\\3_2.png");

	free(data);




	//***********************end Covert PNG******************
	end = clock();
	cost = (double)(end - begin)/CLOCKS_PER_SEC;
	printf("end\n");
	display_info();
	printf("constant CLOCKS_PER_SEC is: %ld, time cost is: %lf secs", CLOCKS_PER_SEC, cost);
	return 0;
}