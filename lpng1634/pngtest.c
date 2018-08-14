
/* pngtest.c - a simple test program to test libpng

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Windows.h>

#include "png.h"
#include "lodepng\lodepng.h"


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

int writefile(char* filename, int h, int w, char* data)
{   int i,j;
    int pos=0;
	FILE *fpWrite=fopen(filename,"w");  
    if(fpWrite==NULL)  
    {  
        return 0;  
    }  
    for(i=0;i<h;i++)
		for(j=0; j<w;j++)
		{
			
			fprintf(fpWrite,"[%d] %d\n",pos, data[pos++]);
		}
	fprintf_s(fpWrite,data);
    fclose(fpWrite); 
}



void auto_convert_data(LodePNGColorMode* mode_in, LodePNGColorMode* mode_out, int width, int height, png_bytep in, png_bytep* row_pointer_out)
{
   unsigned char* data= 0;/*uncompressed version of the IDAT chunk data*/
   unsigned char* converted;
   int bpp = lodepng_get_bpp(mode_out);
   int linebits = ((width * bpp + 7) / 8) * 8;

   converted = (unsigned char*)malloc((height *width * bpp + 7) / 8);
   lodepng_convert(converted, in, mode_out, mode_in, width, height);

   if(bpp < 8 && width * bpp != linebits)
   {
	   data  = (unsigned char*)malloc(height * ((width * bpp + 7) / 8));
	   addPaddingBits_export(data, converted, linebits, width * bpp, height);
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

void SetIHDR(png_structp png_ptr, png_infop info_ptr, LodePNGColorMode* mode, int width, int hight)
{
	png_set_IHDR(png_ptr, info_ptr, width, hight, mode->bitdepth, mode->colortype,
		PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
}

void SetPLTE(png_structp png_ptr, png_infop info_ptr, LodePNGColorMode* mode, int width, int hight)
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
	if(lodepng_color_mode_equal_export(mode_out, mode_in))
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
	if(mode->colortype == LCT_PALETTE)
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
	free(pic_data);
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