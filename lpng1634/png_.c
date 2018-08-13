﻿
/* pngtest.c - a simple test program to test libpng
 *
 * Last changed in libpng 1.6.32 [August 24, 2017]
 * Copyright (c) 1998-2002,2004,2006-2017 Glenn Randers-Pehrson
 * (Version 0.96 Copyright (c) 1996, 1997 Andreas Dilger)
 * (Version 0.88 Copyright (c) 1995, 1996 Guy Eric Schalnat, Group 42, Inc.)
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 *
 * This program reads in a PNG image, writes it out again, and then
 * compares the two files.  If the files are identical, this shows that
 * the basic chunk handling, filtering, and (de)compression code is working
 * properly.  It does not currently test all of the transforms, although
 * it probably should.
 *
 * The program will report "FAIL" in certain legitimate cases:
 * 1) when the compression level or filter selection method is changed.
 * 2) when the maximum IDAT size (PNG_ZBUF_SIZE in pngconf.h) is not 8192.
 * 3) unknown unsafe-to-copy ancillary chunks or unknown critical chunks
 *    exist in the input file.
 * 4) others not listed here...
 * In these cases, it is best to check with another tool such as "pngcheck"
 * to see what the differences between the two files are.
 *
 * If a filename is given on the command-line, then this file is used
 * for the input, rather than the default "pngtest.png".  This allows
 * testing a wide variety of files easily.  You can also test a number
 * of files at once by typing "pngtest -m file1.png file2.png ..."
 */
#define COLORTREE 8
#define _POSIX_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Windows.h>
//#include <vector>
/* Defined so I can write to a file on gui/windowing platforms */
/*  #define STDERR stderr  */
#define STDERR stdout   /* For DOS */

#include "png.h"

/* Known chunks that exist in pngtest.png must be supported or pngtest will fail
 * simply as a result of re-ordering them.  This may be fixed in 1.7
 *
 * pngtest allocates a single row buffer for each row and overwrites it,
 * therefore if the write side doesn't support the writing of interlaced images
 * nothing can be done for an interlaced image (and the code below will fail
 * horribly trying to write extra data after writing garbage).
 */
#if defined PNG_READ_SUPPORTED && /* else nothing can be done */\
   defined PNG_READ_bKGD_SUPPORTED &&\
   defined PNG_READ_cHRM_SUPPORTED &&\
   defined PNG_READ_gAMA_SUPPORTED &&\
   defined PNG_READ_oFFs_SUPPORTED &&\
   defined PNG_READ_pCAL_SUPPORTED &&\
   defined PNG_READ_pHYs_SUPPORTED &&\
   defined PNG_READ_sBIT_SUPPORTED &&\
   defined PNG_READ_sCAL_SUPPORTED &&\
   defined PNG_READ_sRGB_SUPPORTED &&\
   defined PNG_READ_sPLT_SUPPORTED &&\
   defined PNG_READ_tEXt_SUPPORTED &&\
   defined PNG_READ_tIME_SUPPORTED &&\
   defined PNG_READ_zTXt_SUPPORTED &&\
   (defined PNG_WRITE_INTERLACING_SUPPORTED || PNG_LIBPNG_VER >= 10700)

#ifdef PNG_ZLIB_HEADER
#  include PNG_ZLIB_HEADER /* defined by pnglibconf.h from 1.7 */
#else
#  include "zlib.h"
#endif

/* Copied from pngpriv.h but only used in error messages below. */
#ifndef PNG_ZBUF_SIZE
#  define PNG_ZBUF_SIZE 8192
#endif
#define FCLOSE(file) fclose(file)

#ifndef PNG_STDIO_SUPPORTED
typedef FILE                * png_FILE_p;
#endif

/* Makes pngtest verbose so we can find problems. */
#ifndef PNG_DEBUG
#  define PNG_DEBUG 0
#endif

#if PNG_DEBUG > 1
#  define pngtest_debug(m)        ((void)fprintf(stderr, m "\n"))
#  define pngtest_debug1(m,p1)    ((void)fprintf(stderr, m "\n", p1))
#  define pngtest_debug2(m,p1,p2) ((void)fprintf(stderr, m "\n", p1, p2))
#else
#  define pngtest_debug(m)        ((void)0)
#  define pngtest_debug1(m,p1)    ((void)0)
#  define pngtest_debug2(m,p1,p2) ((void)0)
#endif

#if !PNG_DEBUG
#  define SINGLE_ROWBUF_ALLOC  /* Makes buffer overruns easier to nail */
#endif

#ifndef PNG_UNUSED
#  define PNG_UNUSED(param) (void)param;
#endif

/* Turn on CPU timing
#define PNGTEST_TIMING
*/

#ifndef PNG_FLOATING_POINT_SUPPORTED
#undef PNGTEST_TIMING
#endif

#ifdef PNGTEST_TIMING
static float t_start, t_stop, t_decode, t_encode, t_misc;
#include <time.h>
#endif

#ifdef PNG_TIME_RFC1123_SUPPORTED
#define PNG_tIME_STRING_LENGTH 29
static int tIME_chunk_present = 0;
static char tIME_string[PNG_tIME_STRING_LENGTH] = "tIME chunk is not present";

#if PNG_LIBPNG_VER < 10619
#define png_convert_to_rfc1123_buffer(ts, t) tIME_to_str(read_ptr, ts, t)

static int
tIME_to_str(png_structp png_ptr, png_charp ts, png_const_timep t)
{
   png_const_charp str = png_convert_to_rfc1123(png_ptr, t);

   if (str == NULL)
       return 0;

   strcpy(ts, str);
   return 1;
}
#endif /* older libpng */
#endif

static int verbose = 0;
static int strict = 0;
static int relaxed = 0;
static int xfail = 0;
static int unsupported_chunks = 0; /* chunk unsupported by libpng in input */
static int error_count = 0; /* count calls to png_error */
static int warning_count = 0; /* count calls to png_warning */

/* Define png_jmpbuf() in case we are using a pre-1.0.6 version of libpng */
#ifndef png_jmpbuf
#  define png_jmpbuf(png_ptr) png_ptr->jmpbuf
#endif

/* Defines for unknown chunk handling if required. */
#ifndef PNG_HANDLE_CHUNK_ALWAYS
#  define PNG_HANDLE_CHUNK_ALWAYS       3
#endif
#ifndef PNG_HANDLE_CHUNK_IF_SAFE
#  define PNG_HANDLE_CHUNK_IF_SAFE      2
#endif

/* Utility to save typing/errors, the argument must be a name */
#define MEMZERO(var) ((void)memset(&var, 0, sizeof var))

/* Example of using row callbacks to make a simple progress meter */
static int status_pass = 1;
static int status_dots_requested = 0;
static int status_dots = 1;

static void PNGCBAPI
read_row_callback(png_structp png_ptr, png_uint_32 row_number, int pass)
{
   if (png_ptr == NULL || row_number > PNG_UINT_31_MAX)
      return;

   if (status_pass != pass)
   {
      fprintf(stdout, "\n Pass %d: ", pass);
      status_pass = pass;
      status_dots = 31;
   }

   status_dots--;

   if (status_dots == 0)
   {
      fprintf(stdout, "\n         ");
      status_dots=30;
   }

   fprintf(stdout, "r");
}

#ifdef PNG_WRITE_SUPPORTED
static void PNGCBAPI
write_row_callback(png_structp png_ptr, png_uint_32 row_number, int pass)
{
   if (png_ptr == NULL || row_number > PNG_UINT_31_MAX || pass > 7)
      return;

   fprintf(stdout, "w");
}
#endif


#ifdef PNG_READ_USER_TRANSFORM_SUPPORTED
/* Example of using a user transform callback (doesn't do anything at present).
 */
static void PNGCBAPI
read_user_callback(png_structp png_ptr, png_row_infop row_info, png_bytep data)
{
   PNG_UNUSED(png_ptr)
   PNG_UNUSED(row_info)
   PNG_UNUSED(data)
}
#endif

#ifdef PNG_WRITE_USER_TRANSFORM_SUPPORTED
/* Example of using user transform callback (we don't transform anything,
 * but merely count the zero samples)
 */

static png_uint_32 zero_samples;

static void PNGCBAPI
count_zero_samples(png_structp png_ptr, png_row_infop row_info, png_bytep data)
{
   png_bytep dp = data;
   if (png_ptr == NULL)
      return;

   /* Contents of row_info:
    *  png_uint_32 width      width of row
    *  png_uint_32 rowbytes   number of bytes in row
    *  png_byte color_type    color type of pixels
    *  png_byte bit_depth     bit depth of samples
    *  png_byte channels      number of channels (1-4)
    *  png_byte pixel_depth   bits per pixel (depth*channels)
    */

   /* Counts the number of zero samples (or zero pixels if color_type is 3 */

   if (row_info->color_type == 0 || row_info->color_type == 3)
   {
      int pos = 0;
      png_uint_32 n, nstop;

      for (n = 0, nstop=row_info->width; n<nstop; n++)
      {
         if (row_info->bit_depth == 1)
         {
            if (((*dp << pos++ ) & 0x80) == 0)
               zero_samples++;

            if (pos == 8)
            {
               pos = 0;
               dp++;
            }
         }

         if (row_info->bit_depth == 2)
         {
            if (((*dp << (pos+=2)) & 0xc0) == 0)
               zero_samples++;

            if (pos == 8)
            {
               pos = 0;
               dp++;
            }
         }

         if (row_info->bit_depth == 4)
         {
            if (((*dp << (pos+=4)) & 0xf0) == 0)
               zero_samples++;

            if (pos == 8)
            {
               pos = 0;
               dp++;
            }
         }

         if (row_info->bit_depth == 8)
            if (*dp++ == 0)
               zero_samples++;

         if (row_info->bit_depth == 16)
         {
            if ((*dp | *(dp+1)) == 0)
               zero_samples++;
            dp+=2;
         }
      }
   }
   else /* Other color types */
   {
      png_uint_32 n, nstop;
      int channel;
      int color_channels = row_info->channels;
      if (row_info->color_type > 3)
         color_channels--;

      for (n = 0, nstop=row_info->width; n<nstop; n++)
      {
         for (channel = 0; channel < color_channels; channel++)
         {
            if (row_info->bit_depth == 8)
               if (*dp++ == 0)
                  zero_samples++;

            if (row_info->bit_depth == 16)
            {
               if ((*dp | *(dp+1)) == 0)
                  zero_samples++;

               dp+=2;
            }
         }
         if (row_info->color_type > 3)
         {
            dp++;
            if (row_info->bit_depth == 16)
               dp++;
         }
      }
   }
}
#endif /* WRITE_USER_TRANSFORM */

#ifndef PNG_STDIO_SUPPORTED
/* START of code to validate stdio-free compilation */
/* These copies of the default read/write functions come from pngrio.c and
 * pngwio.c.  They allow "don't include stdio" testing of the library.
 * This is the function that does the actual reading of data.  If you are
 * not reading from a standard C stream, you should create a replacement
 * read_data function and use it at run time with png_set_read_fn(), rather
 * than changing the library.
 */

#ifdef PNG_IO_STATE_SUPPORTED
void
pngtest_check_io_state(png_structp png_ptr, png_size_t data_length,
    png_uint_32 io_op);
void
pngtest_check_io_state(png_structp png_ptr, png_size_t data_length,
    png_uint_32 io_op)
{
   png_uint_32 io_state = png_get_io_state(png_ptr);
   int err = 0;

   /* Check if the current operation (reading / writing) is as expected. */
   if ((io_state & PNG_IO_MASK_OP) != io_op)
      png_error(png_ptr, "Incorrect operation in I/O state");

   /* Check if the buffer size specific to the current location
    * (file signature / header / data / crc) is as expected.
    */
   switch (io_state & PNG_IO_MASK_LOC)
   {
   case PNG_IO_SIGNATURE:
      if (data_length > 8)
         err = 1;
      break;
   case PNG_IO_CHUNK_HDR:
      if (data_length != 8)
         err = 1;
      break;
   case PNG_IO_CHUNK_DATA:
      break;  /* no restrictions here */
   case PNG_IO_CHUNK_CRC:
      if (data_length != 4)
         err = 1;
      break;
   default:
      err = 1;  /* uninitialized */
   }
   if (err != 0)
      png_error(png_ptr, "Bad I/O state or buffer size");
}
#endif

static void PNGCBAPI
pngtest_read_data(png_structp png_ptr, png_bytep data, png_size_t length)
{
   png_size_t check = 0;
   png_voidp io_ptr;

   /* fread() returns 0 on error, so it is OK to store this in a png_size_t
    * instead of an int, which is what fread() actually returns.
    */
   io_ptr = png_get_io_ptr(png_ptr);
   if (io_ptr != NULL)
   {
      check = fread(data, 1, length, (png_FILE_p)io_ptr);
   }

   if (check != length)
   {
      png_error(png_ptr, "Read Error");
   }

#ifdef PNG_IO_STATE_SUPPORTED
   pngtest_check_io_state(png_ptr, length, PNG_IO_READING);
#endif
}

#ifdef PNG_WRITE_FLUSH_SUPPORTED
static void PNGCBAPI
pngtest_flush(png_structp png_ptr)
{
   /* Do nothing; fflush() is said to be just a waste of energy. */
   PNG_UNUSED(png_ptr)   /* Stifle compiler warning */
}
#endif

/* This is the function that does the actual writing of data.  If you are
 * not writing to a standard C stream, you should create a replacement
 * write_data function and use it at run time with png_set_write_fn(), rather
 * than changing the library.
 */
static void PNGCBAPI
pngtest_write_data(png_structp png_ptr, png_bytep data, png_size_t length)
{
   png_size_t check;

   check = fwrite(data, 1, length, (png_FILE_p)png_get_io_ptr(png_ptr));

   if (check != length)
   {
      png_error(png_ptr, "Write Error");
   }

#ifdef PNG_IO_STATE_SUPPORTED
   pngtest_check_io_state(png_ptr, length, PNG_IO_WRITING);
#endif
}
#endif /* !STDIO */

/* This function is called when there is a warning, but the library thinks
 * it can continue anyway.  Replacement functions don't have to do anything
 * here if you don't want to.  In the default configuration, png_ptr is
 * not used, but it is passed in case it may be useful.
 */
typedef struct
{
   PNG_CONST char *file_name;
}  pngtest_error_parameters;

static void PNGCBAPI
pngtest_warning(png_structp png_ptr, png_const_charp message)
{
   PNG_CONST char *name = "UNKNOWN (ERROR!)";
   pngtest_error_parameters *test =
      (pngtest_error_parameters*)png_get_error_ptr(png_ptr);

   ++warning_count;

   if (test != NULL && test->file_name != NULL)
      name = test->file_name;

   fprintf(STDERR, "\n%s: libpng warning: %s\n", name, message);
}

/* This is the default error handling function.  Note that replacements for
 * this function MUST NOT RETURN, or the program will likely crash.  This
 * function is used by default, or if the program supplies NULL for the
 * error function pointer in png_set_error_fn().
 */
static void PNGCBAPI
pngtest_error(png_structp png_ptr, png_const_charp message)
{
   ++error_count;

   pngtest_warning(png_ptr, message);
   /* We can return because png_error calls the default handler, which is
    * actually OK in this case.
    */
}

/* END of code to validate stdio-free compilation */

/* START of code to validate memory allocation and deallocation */
#if defined(PNG_USER_MEM_SUPPORTED) && PNG_DEBUG

/* Allocate memory.  For reasonable files, size should never exceed
 * 64K.  However, zlib may allocate more than 64K if you don't tell
 * it not to.  See zconf.h and png.h for more information.  zlib does
 * need to allocate exactly 64K, so whatever you call here must
 * have the ability to do that.
 *
 * This piece of code can be compiled to validate max 64K allocations
 * by setting MAXSEG_64K in zlib zconf.h *or* PNG_MAX_MALLOC_64K.
 */
typedef struct memory_information
{
   png_alloc_size_t          size;
   png_voidp                 pointer;
   struct memory_information *next;
} memory_information;
typedef memory_information *memory_infop;

static memory_infop pinformation = NULL;
static int current_allocation = 0;
static int maximum_allocation = 0;
static int total_allocation = 0;
static int num_allocations = 0;

png_voidp PNGCBAPI png_debug_malloc PNGARG((png_structp png_ptr,
    png_alloc_size_t size));
void PNGCBAPI png_debug_free PNGARG((png_structp png_ptr, png_voidp ptr));

png_voidp
PNGCBAPI png_debug_malloc(png_structp png_ptr, png_alloc_size_t size)
{

   /* png_malloc has already tested for NULL; png_create_struct calls
    * png_debug_malloc directly, with png_ptr == NULL which is OK
    */

   if (size == 0)
      return (NULL);

   /* This calls the library allocator twice, once to get the requested
      buffer and once to get a new free list entry. */
   {
      /* Disable malloc_fn and free_fn */
      memory_infop pinfo;
      png_set_mem_fn(png_ptr, NULL, NULL, NULL);
      pinfo = (memory_infop)png_malloc(png_ptr,
          (sizeof *pinfo));
      pinfo->size = size;
      current_allocation += size;
      total_allocation += size;
      num_allocations ++;

      if (current_allocation > maximum_allocation)
         maximum_allocation = current_allocation;

      pinfo->pointer = png_malloc(png_ptr, size);
      /* Restore malloc_fn and free_fn */

      png_set_mem_fn(png_ptr,
          NULL, png_debug_malloc, png_debug_free);

      if (size != 0 && pinfo->pointer == NULL)
      {
         current_allocation -= size;
         total_allocation -= size;
         png_error(png_ptr,
           "out of memory in pngtest->png_debug_malloc");
      }

      pinfo->next = pinformation;
      pinformation = pinfo;
      /* Make sure the caller isn't assuming zeroed memory. */
      memset(pinfo->pointer, 0xdd, pinfo->size);

      if (verbose != 0)
         printf("png_malloc %lu bytes at %p\n", (unsigned long)size,
             pinfo->pointer);

      return (png_voidp)(pinfo->pointer);
   }
}

/* Free a pointer.  It is removed from the list at the same time. */
void PNGCBAPI
png_debug_free(png_structp png_ptr, png_voidp ptr)
{
   if (png_ptr == NULL)
      fprintf(STDERR, "NULL pointer to png_debug_free.\n");

   if (ptr == 0)
   {
#if 0 /* This happens all the time. */
      fprintf(STDERR, "WARNING: freeing NULL pointer\n");
#endif
      return;
   }

   /* Unlink the element from the list. */
   if (pinformation != NULL)
   {
      memory_infop *ppinfo = &pinformation;

      for (;;)
      {
         memory_infop pinfo = *ppinfo;

         if (pinfo->pointer == ptr)
         {
            *ppinfo = pinfo->next;
            current_allocation -= pinfo->size;
            if (current_allocation < 0)
               fprintf(STDERR, "Duplicate free of memory\n");
            /* We must free the list element too, but first kill
               the memory that is to be freed. */
            memset(ptr, 0x55, pinfo->size);
            free(pinfo);
            pinfo = NULL;
            break;
         }

         if (pinfo->next == NULL)
         {
            fprintf(STDERR, "Pointer %p not found\n", ptr);
            break;
         }

         ppinfo = &pinfo->next;
      }
   }

   /* Finally free the data. */
   if (verbose != 0)
      printf("Freeing %p\n", ptr);

   if (ptr != NULL)
      free(ptr);
   ptr = NULL;
}
#endif /* USER_MEM && DEBUG */
/* END of code to test memory allocation/deallocation */


#ifdef PNG_READ_USER_CHUNKS_SUPPORTED
/* Demonstration of user chunk support of the sTER and vpAg chunks */

/* (sTER is a public chunk not yet known by libpng.  vpAg is a private
chunk used in ImageMagick to store "virtual page" size).  */

static struct user_chunk_data
{
   png_const_infop info_ptr;
   png_uint_32     vpAg_width, vpAg_height;
   png_byte        vpAg_units;
   png_byte        sTER_mode;
   int             location[2];
}
user_chunk_data;

/* Used for location and order; zero means nothing. */
#define have_sTER   0x01
#define have_vpAg   0x02
#define before_PLTE 0x10
#define before_IDAT 0x20
#define after_IDAT  0x40

static void
init_callback_info(png_const_infop info_ptr)
{
   MEMZERO(user_chunk_data);
   user_chunk_data.info_ptr = info_ptr;
}

static int
set_location(png_structp png_ptr, struct user_chunk_data *data, int what)
{
   int location;

   if ((data->location[0] & what) != 0 || (data->location[1] & what) != 0)
      return 0; /* already have one of these */

   /* Find where we are (the code below zeroes info_ptr to indicate that the
    * chunks before the first IDAT have been read.)
    */
   if (data->info_ptr == NULL) /* after IDAT */
      location = what | after_IDAT;

   else if (png_get_valid(png_ptr, data->info_ptr, PNG_INFO_PLTE) != 0)
      location = what | before_IDAT;

   else
      location = what | before_PLTE;

   if (data->location[0] == 0)
      data->location[0] = location;

   else
      data->location[1] = location;

   return 1; /* handled */
}

static int PNGCBAPI
read_user_chunk_callback(png_struct *png_ptr, png_unknown_chunkp chunk)
{
   struct user_chunk_data *my_user_chunk_data =
      (struct user_chunk_data*)png_get_user_chunk_ptr(png_ptr);

   if (my_user_chunk_data == NULL)
      png_error(png_ptr, "lost user chunk pointer");

   /* Return one of the following:
    *    return (-n);  chunk had an error
    *    return (0);  did not recognize
    *    return (n);  success
    *
    * The unknown chunk structure contains the chunk data:
    * png_byte name[5];
    * png_byte *data;
    * png_size_t size;
    *
    * Note that libpng has already taken care of the CRC handling.
    */

   if (chunk->name[0] == 115 && chunk->name[1] ==  84 &&     /* s  T */
       chunk->name[2] ==  69 && chunk->name[3] ==  82)       /* E  R */
      {
         /* Found sTER chunk */
         if (chunk->size != 1)
            return (-1); /* Error return */

         if (chunk->data[0] != 0 && chunk->data[0] != 1)
            return (-1);  /* Invalid mode */

         if (set_location(png_ptr, my_user_chunk_data, have_sTER) != 0)
         {
            my_user_chunk_data->sTER_mode=chunk->data[0];
            return (1);
         }

         else
            return (0); /* duplicate sTER - give it to libpng */
      }

   if (chunk->name[0] != 118 || chunk->name[1] != 112 ||    /* v  p */
       chunk->name[2] !=  65 || chunk->name[3] != 103)      /* A  g */
      return (0); /* Did not recognize */

   /* Found ImageMagick vpAg chunk */

   if (chunk->size != 9)
      return (-1); /* Error return */

   if (set_location(png_ptr, my_user_chunk_data, have_vpAg) == 0)
      return (0);  /* duplicate vpAg */

   my_user_chunk_data->vpAg_width = png_get_uint_31(png_ptr, chunk->data);
   my_user_chunk_data->vpAg_height = png_get_uint_31(png_ptr, chunk->data + 4);
   my_user_chunk_data->vpAg_units = chunk->data[8];

   return (1);
}

#ifdef PNG_WRITE_SUPPORTED
static void
write_sTER_chunk(png_structp write_ptr)
{
   png_byte sTER[5] = {115,  84,  69,  82, '\0'};

   if (verbose != 0)
      fprintf(STDERR, "\n stereo mode = %d\n", user_chunk_data.sTER_mode);

   png_write_chunk(write_ptr, sTER, &user_chunk_data.sTER_mode, 1);
}

static void
write_vpAg_chunk(png_structp write_ptr)
{
   png_byte vpAg[5] = {118, 112,  65, 103, '\0'};

   png_byte vpag_chunk_data[9];

   if (verbose != 0)
      fprintf(STDERR, " vpAg = %lu x %lu, units = %d\n",
          (unsigned long)user_chunk_data.vpAg_width,
          (unsigned long)user_chunk_data.vpAg_height,
          user_chunk_data.vpAg_units);

   png_save_uint_32(vpag_chunk_data, user_chunk_data.vpAg_width);
   png_save_uint_32(vpag_chunk_data + 4, user_chunk_data.vpAg_height);
   vpag_chunk_data[8] = user_chunk_data.vpAg_units;
   png_write_chunk(write_ptr, vpAg, vpag_chunk_data, 9);
}

static void
write_chunks(png_structp write_ptr, int location)
{
   int i;

   /* Notice that this preserves the original chunk order, however chunks
    * intercepted by the callback will be written *after* chunks passed to
    * libpng.  This will actually reverse a pair of sTER chunks or a pair of
    * vpAg chunks, resulting in an error later.  This is not worth worrying
    * about - the chunks should not be duplicated!
    */
   for (i=0; i<2; ++i)
   {
      if (user_chunk_data.location[i] == (location | have_sTER))
         write_sTER_chunk(write_ptr);

      else if (user_chunk_data.location[i] == (location | have_vpAg))
         write_vpAg_chunk(write_ptr);
   }
}
#endif /* WRITE */
#else /* !READ_USER_CHUNKS */
#  define write_chunks(pp,loc) ((void)0)
#endif
/* END of code to demonstrate user chunk support */

/* START of code to check that libpng has the required text support; this only
 * checks for the write support because if read support is missing the chunk
 * will simply not be reported back to pngtest.
 */
#ifdef PNG_TEXT_SUPPORTED
static void
pngtest_check_text_support(png_structp png_ptr, png_textp text_ptr,
    int num_text)
{
   while (num_text > 0)
   {
      switch (text_ptr[--num_text].compression)
      {
         case PNG_TEXT_COMPRESSION_NONE:
            break;

         case PNG_TEXT_COMPRESSION_zTXt:
#           ifndef PNG_WRITE_zTXt_SUPPORTED
               ++unsupported_chunks;
               /* In libpng 1.7 this now does an app-error, so stop it: */
               text_ptr[num_text].compression = PNG_TEXT_COMPRESSION_NONE;
#           endif
            break;

         case PNG_ITXT_COMPRESSION_NONE:
         case PNG_ITXT_COMPRESSION_zTXt:
#           ifndef PNG_WRITE_iTXt_SUPPORTED
               ++unsupported_chunks;
               text_ptr[num_text].compression = PNG_TEXT_COMPRESSION_NONE;
#           endif
            break;

         default:
            /* This is an error */
            png_error(png_ptr, "invalid text chunk compression field");
            break;
      }
   }
}
#endif
/* END of code to check that libpng has the required text support */

/* Test one file */
static int
test_one_file(PNG_CONST char *inname, PNG_CONST char *outname)
{
   static png_FILE_p fpin;
   static png_FILE_p fpout;  /* "static" prevents setjmp corruption */
   pngtest_error_parameters error_parameters;
   png_structp read_ptr;
   png_infop read_info_ptr, end_info_ptr;
#ifdef PNG_WRITE_SUPPORTED
   png_structp write_ptr;
   png_infop write_info_ptr;
   png_infop write_end_info_ptr;
#ifdef PNG_WRITE_FILTER_SUPPORTED
   int interlace_preserved = 1;
#endif /* WRITE_FILTER */
#else /* !WRITE */
   png_structp write_ptr = NULL;
   png_infop write_info_ptr = NULL;
   png_infop write_end_info_ptr = NULL;
#endif /* !WRITE */
   png_bytep row_buf;
   png_uint_32 y;
   png_uint_32 width, height;
   volatile int num_passes;
   int pass;
   int bit_depth, color_type;

   row_buf = NULL;
   error_parameters.file_name = inname;

   if ((fpin = fopen(inname, "rb")) == NULL)
   {
      fprintf(STDERR, "Could not find input file %s\n", inname);
      return (1);
   }

   if ((fpout = fopen(outname, "wb")) == NULL)
   {
      fprintf(STDERR, "Could not open output file %s\n", outname);
      FCLOSE(fpin);
      return (1);
   }

   pngtest_debug("Allocating read and write structures");
#if defined(PNG_USER_MEM_SUPPORTED) && PNG_DEBUG
   read_ptr =
       png_create_read_struct_2(PNG_LIBPNG_VER_STRING, NULL,
       NULL, NULL, NULL, png_debug_malloc, png_debug_free);
#else
   read_ptr =
       png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
#endif
   png_set_error_fn(read_ptr, &error_parameters, pngtest_error,
       pngtest_warning);

#ifdef PNG_WRITE_SUPPORTED
#if defined(PNG_USER_MEM_SUPPORTED) && PNG_DEBUG
   write_ptr =
       png_create_write_struct_2(PNG_LIBPNG_VER_STRING, NULL,
       NULL, NULL, NULL, png_debug_malloc, png_debug_free);
#else
   write_ptr =
       png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
#endif
   png_set_error_fn(write_ptr, &error_parameters, pngtest_error,
       pngtest_warning);
#endif
   pngtest_debug("Allocating read_info, write_info and end_info structures");
   read_info_ptr = png_create_info_struct(read_ptr);
   end_info_ptr = png_create_info_struct(read_ptr);
#ifdef PNG_WRITE_SUPPORTED
   write_info_ptr = png_create_info_struct(write_ptr);
   write_end_info_ptr = png_create_info_struct(write_ptr);
#endif

#ifdef PNG_READ_USER_CHUNKS_SUPPORTED
   init_callback_info(read_info_ptr);
   png_set_read_user_chunk_fn(read_ptr, &user_chunk_data,
       read_user_chunk_callback);
#endif

#ifdef PNG_SETJMP_SUPPORTED
   pngtest_debug("Setting jmpbuf for read struct");
   if (setjmp(png_jmpbuf(read_ptr)))
   {
      fprintf(STDERR, "%s -> %s: libpng read error\n", inname, outname);
      png_free(read_ptr, row_buf);
      row_buf = NULL;
      if (verbose != 0)
        fprintf(STDERR, "   destroy read structs\n");
      png_destroy_read_struct(&read_ptr, &read_info_ptr, &end_info_ptr);
#ifdef PNG_WRITE_SUPPORTED
      if (verbose != 0)
        fprintf(STDERR, "   destroy write structs\n");
      png_destroy_info_struct(write_ptr, &write_end_info_ptr);
      png_destroy_write_struct(&write_ptr, &write_info_ptr);
#endif
      FCLOSE(fpin);
      FCLOSE(fpout);
      return (1);
   }

#ifdef PNG_WRITE_SUPPORTED
   pngtest_debug("Setting jmpbuf for write struct");

   if (setjmp(png_jmpbuf(write_ptr)))
   {
      fprintf(STDERR, "%s -> %s: libpng write error\n", inname, outname);
      if (verbose != 0)
        fprintf(STDERR, "   destroying read structs\n");
      png_destroy_read_struct(&read_ptr, &read_info_ptr, &end_info_ptr);
      if (verbose != 0)
        fprintf(STDERR, "   destroying write structs\n");
      png_destroy_info_struct(write_ptr, &write_end_info_ptr);
      png_destroy_write_struct(&write_ptr, &write_info_ptr);
      FCLOSE(fpin);
      FCLOSE(fpout);
      return (1);
   }
#endif
#endif

#ifdef PNG_BENIGN_ERRORS_SUPPORTED
   if (strict != 0)
   {
      /* Treat png_benign_error() as errors on read */
      png_set_benign_errors(read_ptr, 0);

# ifdef PNG_WRITE_SUPPORTED
      /* Treat them as errors on write */
      png_set_benign_errors(write_ptr, 0);
# endif

      /* if strict is not set, then app warnings and errors are treated as
       * warnings in release builds, but not in unstable builds; this can be
       * changed with '--relaxed'.
       */
   }

   else if (relaxed != 0)
   {
      /* Allow application (pngtest) errors and warnings to pass */
      png_set_benign_errors(read_ptr, 1);

      /* Turn off CRC checking while reading */
      png_set_crc_action(read_ptr, PNG_CRC_QUIET_USE, PNG_CRC_QUIET_USE);

#ifdef PNG_IGNORE_ADLER32
      /* Turn off ADLER32 checking while reading */
      png_set_option(read_ptr, PNG_IGNORE_ADLER32, PNG_OPTION_ON);
#endif

# ifdef PNG_WRITE_SUPPORTED
      png_set_benign_errors(write_ptr, 1);
# endif

   }
#endif /* BENIGN_ERRORS */

   pngtest_debug("Initializing input and output streams");
#ifdef PNG_STDIO_SUPPORTED
   png_init_io(read_ptr, fpin);
#  ifdef PNG_WRITE_SUPPORTED
   png_init_io(write_ptr, fpout);
#  endif
#else
   png_set_read_fn(read_ptr, (png_voidp)fpin, pngtest_read_data);
#  ifdef PNG_WRITE_SUPPORTED
   png_set_write_fn(write_ptr, (png_voidp)fpout,  pngtest_write_data,
#    ifdef PNG_WRITE_FLUSH_SUPPORTED
       pngtest_flush);
#    else
       NULL);
#    endif
#  endif
#endif

   if (status_dots_requested == 1)
   {
#ifdef PNG_WRITE_SUPPORTED
      png_set_write_status_fn(write_ptr, write_row_callback);
#endif
      png_set_read_status_fn(read_ptr, read_row_callback);
   }

   else
   {
#ifdef PNG_WRITE_SUPPORTED
      png_set_write_status_fn(write_ptr, NULL);
#endif
      png_set_read_status_fn(read_ptr, NULL);
   }

#ifdef PNG_READ_USER_TRANSFORM_SUPPORTED
   png_set_read_user_transform_fn(read_ptr, read_user_callback);
#endif
#ifdef PNG_WRITE_USER_TRANSFORM_SUPPORTED
   zero_samples = 0;
   png_set_write_user_transform_fn(write_ptr, count_zero_samples);
#endif

#ifdef PNG_SET_UNKNOWN_CHUNKS_SUPPORTED
   /* Preserve all the unknown chunks, if possible.  If this is disabled then,
    * even if the png_{get,set}_unknown_chunks stuff is enabled, we can't use
    * libpng to *save* the unknown chunks on read (because we can't switch the
    * save option on!)
    *
    * Notice that if SET_UNKNOWN_CHUNKS is *not* supported read will discard all
    * unknown chunks and write will write them all.
    */
#ifdef PNG_SAVE_UNKNOWN_CHUNKS_SUPPORTED
   png_set_keep_unknown_chunks(read_ptr, PNG_HANDLE_CHUNK_ALWAYS,
       NULL, 0);
#endif
#ifdef PNG_WRITE_UNKNOWN_CHUNKS_SUPPORTED
   png_set_keep_unknown_chunks(write_ptr, PNG_HANDLE_CHUNK_ALWAYS,
       NULL, 0);
#endif
#endif

   pngtest_debug("Reading info struct");
   png_read_info(read_ptr, read_info_ptr);

#ifdef PNG_READ_USER_CHUNKS_SUPPORTED
   /* This is a bit of a hack; there is no obvious way in the callback function
    * to determine that the chunks before the first IDAT have been read, so
    * remove the info_ptr (which is only used to determine position relative to
    * PLTE) here to indicate that we are after the IDAT.
    */
   user_chunk_data.info_ptr = NULL;
#endif

   pngtest_debug("Transferring info struct");
   {
      int interlace_type, compression_type, filter_type;

      if (png_get_IHDR(read_ptr, read_info_ptr, &width, &height, &bit_depth,
          &color_type, &interlace_type, &compression_type, &filter_type) != 0)
      {
         png_set_IHDR(write_ptr, write_info_ptr, width, height, bit_depth,
             color_type, interlace_type, compression_type, filter_type);
         /* num_passes may not be available below if interlace support is not
          * provided by libpng for both read and write.
          */
         switch (interlace_type)
         {
            case PNG_INTERLACE_NONE:
               num_passes = 1;
               break;

            case PNG_INTERLACE_ADAM7:
               num_passes = 7;
               break;

            default:
               png_error(read_ptr, "invalid interlace type");
               /*NOT REACHED*/
         }
      }

      else
         png_error(read_ptr, "png_get_IHDR failed");
   }
#ifdef PNG_FIXED_POINT_SUPPORTED
#ifdef PNG_cHRM_SUPPORTED
   {
      png_fixed_point white_x, white_y, red_x, red_y, green_x, green_y, blue_x,
          blue_y;

      if (png_get_cHRM_fixed(read_ptr, read_info_ptr, &white_x, &white_y,
          &red_x, &red_y, &green_x, &green_y, &blue_x, &blue_y) != 0)
      {
         png_set_cHRM_fixed(write_ptr, write_info_ptr, white_x, white_y, red_x,
             red_y, green_x, green_y, blue_x, blue_y);
      }
   }
#endif
#ifdef PNG_gAMA_SUPPORTED
   {
      png_fixed_point gamma;

      if (png_get_gAMA_fixed(read_ptr, read_info_ptr, &gamma) != 0)
         png_set_gAMA_fixed(write_ptr, write_info_ptr, gamma);
   }
#endif
#else /* Use floating point versions */
#ifdef PNG_FLOATING_POINT_SUPPORTED
#ifdef PNG_cHRM_SUPPORTED
   {
      double white_x, white_y, red_x, red_y, green_x, green_y, blue_x,
          blue_y;

      if (png_get_cHRM(read_ptr, read_info_ptr, &white_x, &white_y, &red_x,
          &red_y, &green_x, &green_y, &blue_x, &blue_y) != 0)
      {
         png_set_cHRM(write_ptr, write_info_ptr, white_x, white_y, red_x,
             red_y, green_x, green_y, blue_x, blue_y);
      }
   }
#endif
#ifdef PNG_gAMA_SUPPORTED
   {
      double gamma;

      if (png_get_gAMA(read_ptr, read_info_ptr, &gamma) != 0)
         png_set_gAMA(write_ptr, write_info_ptr, gamma);
   }
#endif
#endif /* Floating point */
#endif /* Fixed point */
#ifdef PNG_iCCP_SUPPORTED
   {
      png_charp name;
      png_bytep profile;
      png_uint_32 proflen;
      int compression_type;

      if (png_get_iCCP(read_ptr, read_info_ptr, &name, &compression_type,
          &profile, &proflen) != 0)
      {
         png_set_iCCP(write_ptr, write_info_ptr, name, compression_type,
             profile, proflen);
      }
   }
#endif
#ifdef PNG_sRGB_SUPPORTED
   {
      int intent;

      if (png_get_sRGB(read_ptr, read_info_ptr, &intent) != 0)
         png_set_sRGB(write_ptr, write_info_ptr, intent);
   }
#endif
   {
      png_colorp palette;
      int num_palette;

      if (png_get_PLTE(read_ptr, read_info_ptr, &palette, &num_palette) != 0)
         png_set_PLTE(write_ptr, write_info_ptr, palette, num_palette);
   }
#ifdef PNG_bKGD_SUPPORTED
   {
      png_color_16p background;

      if (png_get_bKGD(read_ptr, read_info_ptr, &background) != 0)
      {
         png_set_bKGD(write_ptr, write_info_ptr, background);
      }
   }
#endif
#ifdef PNG_READ_eXIf_SUPPORTED
   {
      png_bytep exif=NULL;
      png_uint_32 exif_length;

      if (png_get_eXIf_1(read_ptr, read_info_ptr, &exif_length, &exif) != 0)
      {
         if (exif_length > 1)
            fprintf(STDERR," eXIf type %c%c, %lu bytes\n",exif[0],exif[1],
               (unsigned long)exif_length);
# ifdef PNG_WRITE_eXIf_SUPPORTED
         png_set_eXIf_1(write_ptr, write_info_ptr, exif_length, exif);
# endif
      }
   }
#endif
#ifdef PNG_hIST_SUPPORTED
   {
      png_uint_16p hist;

      if (png_get_hIST(read_ptr, read_info_ptr, &hist) != 0)
         png_set_hIST(write_ptr, write_info_ptr, hist);
   }
#endif
#ifdef PNG_oFFs_SUPPORTED
   {
      png_int_32 offset_x, offset_y;
      int unit_type;

      if (png_get_oFFs(read_ptr, read_info_ptr, &offset_x, &offset_y,
          &unit_type) != 0)
      {
         png_set_oFFs(write_ptr, write_info_ptr, offset_x, offset_y, unit_type);
      }
   }
#endif
#ifdef PNG_pCAL_SUPPORTED
   {
      png_charp purpose, units;
      png_charpp params;
      png_int_32 X0, X1;
      int type, nparams;

      if (png_get_pCAL(read_ptr, read_info_ptr, &purpose, &X0, &X1, &type,
          &nparams, &units, &params) != 0)
      {
         png_set_pCAL(write_ptr, write_info_ptr, purpose, X0, X1, type,
             nparams, units, params);
      }
   }
#endif
#ifdef PNG_pHYs_SUPPORTED
   {
      png_uint_32 res_x, res_y;
      int unit_type;

      if (png_get_pHYs(read_ptr, read_info_ptr, &res_x, &res_y,
          &unit_type) != 0)
         png_set_pHYs(write_ptr, write_info_ptr, res_x, res_y, unit_type);
   }
#endif
#ifdef PNG_sBIT_SUPPORTED
   {
      png_color_8p sig_bit;

      if (png_get_sBIT(read_ptr, read_info_ptr, &sig_bit) != 0)
         png_set_sBIT(write_ptr, write_info_ptr, sig_bit);
   }
#endif
#ifdef PNG_sCAL_SUPPORTED
#if defined(PNG_FLOATING_POINT_SUPPORTED) && \
   defined(PNG_FLOATING_ARITHMETIC_SUPPORTED)
   {
      int unit;
      double scal_width, scal_height;

      if (png_get_sCAL(read_ptr, read_info_ptr, &unit, &scal_width,
          &scal_height) != 0)
      {
         png_set_sCAL(write_ptr, write_info_ptr, unit, scal_width, scal_height);
      }
   }
#else
#ifdef PNG_FIXED_POINT_SUPPORTED
   {
      int unit;
      png_charp scal_width, scal_height;

      if (png_get_sCAL_s(read_ptr, read_info_ptr, &unit, &scal_width,
           &scal_height) != 0)
      {
         png_set_sCAL_s(write_ptr, write_info_ptr, unit, scal_width,
             scal_height);
      }
   }
#endif
#endif
#endif

#ifdef PNG_sPLT_SUPPORTED
   {
       png_sPLT_tp entries;

       int num_entries = (int) png_get_sPLT(read_ptr, read_info_ptr, &entries);
       if (num_entries)
       {
           png_set_sPLT(write_ptr, write_info_ptr, entries, num_entries);
       }
   }
#endif

#ifdef PNG_TEXT_SUPPORTED
   {
      png_textp text_ptr;
      int num_text;

      if (png_get_text(read_ptr, read_info_ptr, &text_ptr, &num_text) > 0)
      {
         pngtest_debug1("Handling %d iTXt/tEXt/zTXt chunks", num_text);

         pngtest_check_text_support(read_ptr, text_ptr, num_text);

         if (verbose != 0)
         {
            int i;

            fprintf(STDERR,"\n");
            for (i=0; i<num_text; i++)
            {
               fprintf(STDERR,"   Text compression[%d]=%d\n",
                   i, text_ptr[i].compression);
            }
         }

         png_set_text(write_ptr, write_info_ptr, text_ptr, num_text);
      }
   }
#endif
#ifdef PNG_tIME_SUPPORTED
   {
      png_timep mod_time;

      if (png_get_tIME(read_ptr, read_info_ptr, &mod_time) != 0)
      {
         png_set_tIME(write_ptr, write_info_ptr, mod_time);
#ifdef PNG_TIME_RFC1123_SUPPORTED
         if (png_convert_to_rfc1123_buffer(tIME_string, mod_time) != 0)
            tIME_string[(sizeof tIME_string) - 1] = '\0';

         else
         {
            strncpy(tIME_string, "*** invalid time ***", (sizeof tIME_string));
            tIME_string[(sizeof tIME_string) - 1] = '\0';
         }

         tIME_chunk_present++;
#endif /* TIME_RFC1123 */
      }
   }
#endif
#ifdef PNG_tRNS_SUPPORTED
   {
      png_bytep trans_alpha;
      int num_trans;
      png_color_16p trans_color;

      if (png_get_tRNS(read_ptr, read_info_ptr, &trans_alpha, &num_trans,
          &trans_color) != 0)
      {
         int sample_max = (1 << bit_depth);
         /* libpng doesn't reject a tRNS chunk with out-of-range samples */
         if (!((color_type == PNG_COLOR_TYPE_GRAY &&
             (int)trans_color->gray > sample_max) ||
             (color_type == PNG_COLOR_TYPE_RGB &&
             ((int)trans_color->red > sample_max ||
             (int)trans_color->green > sample_max ||
             (int)trans_color->blue > sample_max))))
            png_set_tRNS(write_ptr, write_info_ptr, trans_alpha, num_trans,
               trans_color);
      }
   }
#endif
#ifdef PNG_WRITE_UNKNOWN_CHUNKS_SUPPORTED
   {
      png_unknown_chunkp unknowns;
      int num_unknowns = png_get_unknown_chunks(read_ptr, read_info_ptr,
          &unknowns);

      if (num_unknowns != 0)
      {
         png_set_unknown_chunks(write_ptr, write_info_ptr, unknowns,
             num_unknowns);
#if PNG_LIBPNG_VER < 10600
         /* Copy the locations from the read_info_ptr.  The automatically
          * generated locations in write_end_info_ptr are wrong prior to 1.6.0
          * because they are reset from the write pointer (removed in 1.6.0).
          */
         {
            int i;
            for (i = 0; i < num_unknowns; i++)
              png_set_unknown_chunk_location(write_ptr, write_info_ptr, i,
                  unknowns[i].location);
         }
#endif
      }
   }
#endif

#ifdef PNG_WRITE_SUPPORTED
   pngtest_debug("Writing info struct");

   /* Write the info in two steps so that if we write the 'unknown' chunks here
    * they go to the correct place.
    */
   png_write_info_before_PLTE(write_ptr, write_info_ptr);

   write_chunks(write_ptr, before_PLTE); /* before PLTE */

   png_write_info(write_ptr, write_info_ptr);

   write_chunks(write_ptr, before_IDAT); /* after PLTE */

   png_write_info(write_ptr, write_end_info_ptr);

   write_chunks(write_ptr, after_IDAT); /* after IDAT */

#ifdef PNG_COMPRESSION_COMPAT
   /* Test the 'compatibility' setting here, if it is available. */
   png_set_compression(write_ptr, PNG_COMPRESSION_COMPAT);
#endif
#endif

#ifdef SINGLE_ROWBUF_ALLOC
   pngtest_debug("Allocating row buffer...");
   row_buf = (png_bytep)png_malloc(read_ptr,
       png_get_rowbytes(read_ptr, read_info_ptr));

   pngtest_debug1("\t0x%08lx", (unsigned long)row_buf);
#endif /* SINGLE_ROWBUF_ALLOC */
   pngtest_debug("Writing row data");

#if defined(PNG_READ_INTERLACING_SUPPORTED) &&\
   defined(PNG_WRITE_INTERLACING_SUPPORTED)
   /* Both must be defined for libpng to be able to handle the interlace,
    * otherwise it gets handled below by simply reading and writing the passes
    * directly.
    */
   if (png_set_interlace_handling(read_ptr) != num_passes)
      png_error(write_ptr,
          "png_set_interlace_handling(read): wrong pass count ");
   if (png_set_interlace_handling(write_ptr) != num_passes)
      png_error(write_ptr,
          "png_set_interlace_handling(write): wrong pass count ");
#else /* png_set_interlace_handling not called on either read or write */
#  define calc_pass_height
#endif /* not using libpng interlace handling */

#ifdef PNGTEST_TIMING
   t_stop = (float)clock();
   t_misc += (t_stop - t_start);
   t_start = t_stop;
#endif
   for (pass = 0; pass < num_passes; pass++)
   {
#     ifdef calc_pass_height
         png_uint_32 pass_height;

         if (num_passes == 7) /* interlaced */
         {
            if (PNG_PASS_COLS(width, pass) > 0)
               pass_height = PNG_PASS_ROWS(height, pass);

            else
               pass_height = 0;
         }

         else /* not interlaced */
            pass_height = height;
#     else
#        define pass_height height
#     endif

      pngtest_debug1("Writing row data for pass %d", pass);
      for (y = 0; y < pass_height; y++)
      {
#ifndef SINGLE_ROWBUF_ALLOC
         pngtest_debug2("Allocating row buffer (pass %d, y = %u)...", pass, y);

         row_buf = (png_bytep)png_malloc(read_ptr,
             png_get_rowbytes(read_ptr, read_info_ptr));

         pngtest_debug2("\t0x%08lx (%lu bytes)", (unsigned long)row_buf,
             (unsigned long)png_get_rowbytes(read_ptr, read_info_ptr));

#endif /* !SINGLE_ROWBUF_ALLOC */
         png_read_rows(read_ptr, (png_bytepp)&row_buf, NULL, 1);

#ifdef PNG_WRITE_SUPPORTED
#ifdef PNGTEST_TIMING
         t_stop = (float)clock();
         t_decode += (t_stop - t_start);
         t_start = t_stop;
#endif
         png_write_rows(write_ptr, (png_bytepp)&row_buf, 1);
#ifdef PNGTEST_TIMING
         t_stop = (float)clock();
         t_encode += (t_stop - t_start);
         t_start = t_stop;
#endif
#endif /* WRITE */

#ifndef SINGLE_ROWBUF_ALLOC
         pngtest_debug2("Freeing row buffer (pass %d, y = %u)", pass, y);
         png_free(read_ptr, row_buf);
         row_buf = NULL;
#endif /* !SINGLE_ROWBUF_ALLOC */
      }
   }

#ifdef PNG_STORE_UNKNOWN_CHUNKS_SUPPORTED
#  ifdef PNG_READ_UNKNOWN_CHUNKS_SUPPORTED
      png_free_data(read_ptr, read_info_ptr, PNG_FREE_UNKN, -1);
#  endif
#  ifdef PNG_WRITE_UNKNOWN_CHUNKS_SUPPORTED
      png_free_data(write_ptr, write_info_ptr, PNG_FREE_UNKN, -1);
#  endif
#endif

   pngtest_debug("Reading and writing end_info data");

   png_read_end(read_ptr, end_info_ptr);
#ifdef PNG_TEXT_SUPPORTED
   {
      png_textp text_ptr;
      int num_text;

      if (png_get_text(read_ptr, end_info_ptr, &text_ptr, &num_text) > 0)
      {
         pngtest_debug1("Handling %d iTXt/tEXt/zTXt chunks", num_text);

         pngtest_check_text_support(read_ptr, text_ptr, num_text);

         if (verbose != 0)
         {
            int i;

            fprintf(STDERR,"\n");
            for (i=0; i<num_text; i++)
            {
               fprintf(STDERR,"   Text compression[%d]=%d\n",
                   i, text_ptr[i].compression);
            }
         }

         png_set_text(write_ptr, write_end_info_ptr, text_ptr, num_text);
      }
   }
#endif
#ifdef PNG_READ_eXIf_SUPPORTED
   {
      png_bytep exif=NULL;
      png_uint_32 exif_length;

      if (png_get_eXIf_1(read_ptr, end_info_ptr, &exif_length, &exif) != 0)
      {
         if (exif_length > 1)
            fprintf(STDERR," eXIf type %c%c, %lu bytes\n",exif[0],exif[1],
               (unsigned long)exif_length);
# ifdef PNG_WRITE_eXIf_SUPPORTED
         png_set_eXIf_1(write_ptr, write_end_info_ptr, exif_length, exif);
# endif
      }
   }
#endif
#ifdef PNG_tIME_SUPPORTED
   {
      png_timep mod_time;

      if (png_get_tIME(read_ptr, end_info_ptr, &mod_time) != 0)
      {
         png_set_tIME(write_ptr, write_end_info_ptr, mod_time);
#ifdef PNG_TIME_RFC1123_SUPPORTED
         if (png_convert_to_rfc1123_buffer(tIME_string, mod_time) != 0)
            tIME_string[(sizeof tIME_string) - 1] = '\0';

         else
         {
            strncpy(tIME_string, "*** invalid time ***", sizeof tIME_string);
            tIME_string[(sizeof tIME_string)-1] = '\0';
         }

         tIME_chunk_present++;
#endif /* TIME_RFC1123 */
      }
   }
#endif
#ifdef PNG_WRITE_UNKNOWN_CHUNKS_SUPPORTED
   {
      png_unknown_chunkp unknowns;
      int num_unknowns = png_get_unknown_chunks(read_ptr, end_info_ptr,
          &unknowns);

      if (num_unknowns != 0)
      {
         png_set_unknown_chunks(write_ptr, write_end_info_ptr, unknowns,
             num_unknowns);
#if PNG_LIBPNG_VER < 10600
         /* Copy the locations from the read_info_ptr.  The automatically
          * generated locations in write_end_info_ptr are wrong prior to 1.6.0
          * because they are reset from the write pointer (removed in 1.6.0).
          */
         {
            int i;
            for (i = 0; i < num_unknowns; i++)
              png_set_unknown_chunk_location(write_ptr, write_end_info_ptr, i,
                  unknowns[i].location);
         }
#endif
      }
   }
#endif

#ifdef PNG_WRITE_SUPPORTED
#ifdef PNG_WRITE_CUSTOMIZE_ZTXT_COMPRESSION_SUPPORTED
   /* Normally one would use Z_DEFAULT_STRATEGY for text compression.
    * This is here just to make pngtest replicate the results from libpng
    * versions prior to 1.5.4, and to test this new API.
    */
   png_set_text_compression_strategy(write_ptr, Z_FILTERED);
#endif

   /* When the unknown vpAg/sTER chunks are written by pngtest the only way to
    * do it is to write them *before* calling png_write_end.  When unknown
    * chunks are written by libpng, however, they are written just before IEND.
    * There seems to be no way round this, however vpAg/sTER are not expected
    * after IDAT.
    */
   write_chunks(write_ptr, after_IDAT);

   png_write_end(write_ptr, write_end_info_ptr);
#endif

#ifdef PNG_EASY_ACCESS_SUPPORTED
   if (verbose != 0)
   {
      png_uint_32 iwidth, iheight;
      iwidth = png_get_image_width(write_ptr, write_info_ptr);
      iheight = png_get_image_height(write_ptr, write_info_ptr);
      fprintf(STDERR, "\n Image width = %lu, height = %lu\n",
          (unsigned long)iwidth, (unsigned long)iheight);
   }
#endif

   pngtest_debug("Destroying data structs");
#ifdef SINGLE_ROWBUF_ALLOC
   pngtest_debug("destroying row_buf for read_ptr");
   png_free(read_ptr, row_buf);
   row_buf = NULL;
#endif /* SINGLE_ROWBUF_ALLOC */
   pngtest_debug("destroying read_ptr, read_info_ptr, end_info_ptr");
   png_destroy_read_struct(&read_ptr, &read_info_ptr, &end_info_ptr);
#ifdef PNG_WRITE_SUPPORTED
   pngtest_debug("destroying write_end_info_ptr");
   png_destroy_info_struct(write_ptr, &write_end_info_ptr);
   pngtest_debug("destroying write_ptr, write_info_ptr");
   png_destroy_write_struct(&write_ptr, &write_info_ptr);
#endif
   pngtest_debug("Destruction complete.");

   FCLOSE(fpin);
   FCLOSE(fpout);

   /* Summarize any warnings or errors and in 'strict' mode fail the test.
    * Unsupported chunks can result in warnings, in that case ignore the strict
    * setting, otherwise fail the test on warnings as well as errors.
    */
   if (error_count > 0)
   {
      /* We don't really expect to get here because of the setjmp handling
       * above, but this is safe.
       */
      fprintf(STDERR, "\n  %s: %d libpng errors found (%d warnings)",
          inname, error_count, warning_count);

      if (strict != 0)
         return (1);
   }

#  ifdef PNG_WRITE_SUPPORTED
      /* If there is no write support nothing was written! */
      else if (unsupported_chunks > 0)
      {
         fprintf(STDERR, "\n  %s: unsupported chunks (%d)%s",
             inname, unsupported_chunks, strict ? ": IGNORED --strict!" : "");
      }
#  endif

   else if (warning_count > 0)
   {
      fprintf(STDERR, "\n  %s: %d libpng warnings found",
          inname, warning_count);

      if (strict != 0)
         return (1);
   }

   pngtest_debug("Opening files for comparison");
   if ((fpin = fopen(inname, "rb")) == NULL)
   {
      fprintf(STDERR, "Could not find file %s\n", inname);
      return (1);
   }

   if ((fpout = fopen(outname, "rb")) == NULL)
   {
      fprintf(STDERR, "Could not find file %s\n", outname);
      FCLOSE(fpin);
      return (1);
   }

#if defined (PNG_WRITE_SUPPORTED) /* else nothing was written */ &&\
    defined (PNG_WRITE_FILTER_SUPPORTED)
   if (interlace_preserved != 0) /* else the files will be changed */
   {
      for (;;)
      {
         static int wrote_question = 0;
         png_size_t num_in, num_out;
         char inbuf[256], outbuf[256];

         num_in = fread(inbuf, 1, sizeof inbuf, fpin);
         num_out = fread(outbuf, 1, sizeof outbuf, fpout);

         if (num_in != num_out)
         {
            fprintf(STDERR, "\nFiles %s and %s are of a different size\n",
                inname, outname);

            if (wrote_question == 0 && unsupported_chunks == 0)
            {
               fprintf(STDERR,
                   "   Was %s written with the same maximum IDAT"
                   " chunk size (%d bytes),",
                   inname, PNG_ZBUF_SIZE);
               fprintf(STDERR,
                   "\n   filtering heuristic (libpng default), compression");
               fprintf(STDERR,
                   " level (zlib default),\n   and zlib version (%s)?\n\n",
                   ZLIB_VERSION);
               wrote_question = 1;
            }

            FCLOSE(fpin);
            FCLOSE(fpout);

            if (strict != 0 && unsupported_chunks == 0)
              return (1);

            else
              return (0);
         }

         if (num_in == 0)
            break;

         if (memcmp(inbuf, outbuf, num_in))
         {
            fprintf(STDERR, "\nFiles %s and %s are different\n", inname,
                outname);

            if (wrote_question == 0 && unsupported_chunks == 0)
            {
               fprintf(STDERR,
                   "   Was %s written with the same maximum"
                   " IDAT chunk size (%d bytes),",
                    inname, PNG_ZBUF_SIZE);
               fprintf(STDERR,
                   "\n   filtering heuristic (libpng default), compression");
               fprintf(STDERR,
                   " level (zlib default),\n   and zlib version (%s)?\n\n",
                 ZLIB_VERSION);
               wrote_question = 1;
            }

            FCLOSE(fpin);
            FCLOSE(fpout);

            /* NOTE: the unsupported_chunks escape is permitted here because
             * unsupported text chunk compression will result in the compression
             * mode being changed (to NONE) yet, in the test case, the result
             * can be exactly the same size!
             */
            if (strict != 0 && unsupported_chunks == 0)
              return (1);

            else
              return (0);
         }
      }
   }
#endif /* WRITE && WRITE_FILTER */

   FCLOSE(fpin);
   FCLOSE(fpout);

   return (0);
}

/* Input and output filenames */
#ifdef RISCOS
static PNG_CONST char *inname = "pngtest/png";
static PNG_CONST char *outname = "pngout/png";
#else
static PNG_CONST char *inname = "pngtest.png";
static PNG_CONST char *outname = "pngout.png";
#endif


typedef struct _pic_data pic_data;
struct _pic_data
{
 int width, height;
 int bit_depth; 
 int color_type;
 int can_gray;
 int only_black_or_white;
 png_bytep* row_pointers;
 unsigned numcolors;
 unsigned char palette[1024];
};

typedef struct _auto_pic_data auto_pic_data;
struct _auto_pic_data
{
   int width;
   int height;
   unsigned char* row_pointer;
   int size;
   int src_size;
};

#define PNG_BYTES_TO_CHECK 4
#define HAVE_ALPHA 1
#define NO_ALPHA 0

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
            //************* add my tesing************************
            //printf("%d : r=%d, g=%d ,b=%d, a=%d\n", profile->numcolors,r,g,b,a);
          }
          ++profile->numcolors;
          numcolors_done = profile->numcolors >= maxnumcolors;
        }
        //printf("r=%d, g=%d ,b=%d, a=%d\n",r,g,b,a);
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

/*Information about the PNG image, except pixels, width and height.*/
typedef struct LodePNGInfo
{
  /*header (IHDR), palette (PLTE) and transparency (tRNS) chunks*/
  unsigned compression_method;/*compression method of the original file. Always 0.*/
  unsigned filter_method;     /*filter method of the original file*/
  unsigned interlace_method;  /*interlace method of the original file*/
  LodePNGColorMode color;     /*color type and bits, palette and transparency of the PNG file*/
} LodePNGInfo;


void lodepng_info_cleanup(LodePNGInfo* info)
{
  lodepng_color_mode_cleanup(&info->color);
}

typedef enum LodePNGFilterStrategy
{
  /*every filter at zero*/
  LFS_ZERO,
  /*Use filter that gives minimum sum, as described in the official PNG filter heuristic.*/
  LFS_MINSUM,
  /*Use the filter type that gives smallest Shannon entropy for this scanline. Depending
  on the image, this is better or worse than minsum.*/
  LFS_ENTROPY,
  /*
  Brute-force-search PNG filters by compressing each filter for each scanline.
  Experimental, very slow, and only rarely gives better compression than MINSUM.
  */
  LFS_BRUTE_FORCE,
  /*use predefined_filters buffer: you specify the filter type for each scanline*/
  LFS_PREDEFINED
} LodePNGFilterStrategy;



/*Settings for the encoder.*/
typedef struct LodePNGEncoderSettings
{
  //LodePNGCompressSettings zlibsettings; /*settings for the zlib encoder, such as window size, ...*/

  unsigned auto_convert; /*automatically choose output PNG color type. Default: true*/

  /*If true, follows the official PNG heuristic: if the PNG uses a palette or lower than
  8 bit depth, set all filters to zero. Otherwise use the filter_strategy. Note that to
  completely follow the official PNG heuristic, filter_palette_zero must be true and
  filter_strategy must be LFS_MINSUM*/
  unsigned filter_palette_zero;
  /*Which filter strategy to use when not using zeroes due to filter_palette_zero.
  Set filter_palette_zero to 0 to ensure always using your chosen strategy. Default: LFS_MINSUM*/
  LodePNGFilterStrategy filter_strategy;
  /*used if filter_strategy is LFS_PREDEFINED. In that case, this must point to a buffer with
  the same length as the amount of scanlines in the image, and each value must <= 5. You
  have to cleanup this buffer, LodePNG will never free it. Don't forget that filter_palette_zero
  must be set to 0 to ensure this is also used on palette or low bitdepth images.*/
  const unsigned char* predefined_filters;

  /*force creating a PLTE chunk if colortype is 2 or 6 (= a suggested palette).
  If colortype is 3, PLTE is _always_ created.*/
  unsigned force_palette;
} LodePNGEncoderSettings;

typedef struct LodePNGState
{
  LodePNGEncoderSettings encoder; /*the encoding settings*/

  LodePNGColorMode info_raw; /*specifies the format in which you would like to get the raw pixel buffer*/
  LodePNGInfo info_png; /*info of the PNG image obtained after decoding*/
  unsigned error;
#ifdef LODEPNG_COMPILE_CPP
  /* For the lodepng::State subclass. */
  virtual ~LodePNGState(){}
#endif
} LodePNGState;

void lodepng_state_cleanup(LodePNGState* state)
{
  lodepng_color_mode_cleanup(&state->info_raw);
  lodepng_info_cleanup(&state->info_png);
}

void lodepng_info_init(LodePNGInfo* info)
{
  lodepng_color_mode_init(&info->color);
  info->interlace_method = 0;
  info->compression_method = 0;
  info->filter_method = 0;
}

#define DEFAULT_WINDOWSIZE 2048

void lodepng_encoder_settings_init(LodePNGEncoderSettings* settings)
{
  //lodepng_compress_settings_init(&settings->zlibsettings);
  settings->auto_convert = 1;
  settings->force_palette = 0;
  settings->predefined_filters = 0;
}

void lodepng_state_init(LodePNGState* state)
{
  lodepng_encoder_settings_init(&state->encoder);
  lodepng_color_mode_init(&state->info_raw);
  lodepng_info_init(&state->info_png);
  state->error = 1;
}

/*Try the code, if it returns error, also return the error.*/
#define CERROR_TRY_RETURN(call)\
{\
  unsigned error = call;\
  if(error) return error;\
}

unsigned lodepng_info_copy(LodePNGInfo* dest, const LodePNGInfo* source)
{
  lodepng_info_cleanup(dest);
  *dest = *source;
  lodepng_color_mode_init(&dest->color);
  CERROR_TRY_RETURN(lodepng_color_mode_copy(&dest->color, &source->color));

#ifdef LODEPNG_COMPILE_ANCILLARY_CHUNKS
  CERROR_TRY_RETURN(LodePNGText_copy(dest, source));
  CERROR_TRY_RETURN(LodePNGIText_copy(dest, source));

  LodePNGUnknownChunks_init(dest);
  CERROR_TRY_RETURN(LodePNGUnknownChunks_copy(dest, source));
#endif /*LODEPNG_COMPILE_ANCILLARY_CHUNKS*/
  return 0;
}

/*dynamic vector of unsigned chars*/
typedef struct ucvector
{
  unsigned char* data;
  size_t size; /*used size*/
  size_t allocsize; /*allocated size*/
} ucvector;


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

unsigned lodepng_encode(unsigned char** out, size_t* outsize,
                        const unsigned char* image, unsigned w, unsigned h,
                        LodePNGState* state)
{
 return 0;
}

unsigned lodepng_encode_memory(unsigned char** out, size_t* outsize, const unsigned char* image,
                               unsigned w, unsigned h, LodePNGColorType colortype, unsigned bitdepth)
{
  unsigned error;
  LodePNGState state;
  lodepng_state_init(&state);
  state.info_raw.colortype = colortype;
  state.info_raw.bitdepth = bitdepth;
  state.info_png.color.colortype = colortype;
  state.info_png.color.bitdepth = bitdepth;
  lodepng_encode(out, outsize, image, w, h, &state);
  error = state.error;
  lodepng_state_cleanup(&state);
  return error;
}



unsigned lodepng_save_file(const unsigned char* buffer, size_t buffersize, const char* filename)
{
  FILE* file;
  file = fopen(filename, "wb" );
  if(!file) return 79;
  fwrite((char*)buffer , 1 , buffersize, file);
  fclose(file);
  return 0;
}

unsigned lodepng_encode_file(const char* filename, const unsigned char* image, unsigned w, unsigned h,
                             LodePNGColorType colortype, unsigned bitdepth)
{
  unsigned char* buffer;
  size_t buffersize;
  unsigned error = lodepng_encode_memory(&buffer, &buffersize, image, w, h, colortype, bitdepth);
  if(!error) error = lodepng_save_file(buffer, buffersize, filename);
  lodepng_free(buffer);
  return error;
}

unsigned lodepng_encode32_file(const char* filename, const unsigned char* image, unsigned w, unsigned h)
{
  return lodepng_encode_file(filename, image, w, h, LCT_RGBA, 8);
}

unsigned lodepng_encode32(unsigned char** out, size_t* outsize, const unsigned char* image, unsigned w, unsigned h)
{
  return lodepng_encode_memory(out, outsize, image, w, h, LCT_RGBA, 8);
}

unsigned lodepng_encode24(unsigned char** out, size_t* outsize, const unsigned char* image, unsigned w, unsigned h)
{
  return lodepng_encode_memory(out, outsize, image, w, h, LCT_RGB, 8);
}

/*shared values used by multiple Adam7 related functions*/

static const unsigned ADAM7_IX[7] = { 0, 4, 0, 2, 0, 1, 0 }; /*x start values*/
static const unsigned ADAM7_IY[7] = { 0, 0, 4, 0, 2, 0, 1 }; /*y start values*/
static const unsigned ADAM7_DX[7] = { 8, 8, 4, 4, 2, 2, 1 }; /*x delta values*/
static const unsigned ADAM7_DY[7] = { 8, 8, 8, 4, 4, 2, 2 }; /*y delta values*/

static void Adam7_getpassvalues(unsigned passw[7], unsigned passh[7], size_t filter_passstart[8],
                                size_t padded_passstart[8], size_t passstart[8], unsigned w, unsigned h, unsigned bpp)
{
  /*the passstart values have 8 values: the 8th one indicates the byte after the end of the 7th (= last) pass*/
  unsigned i;

  /*calculate width and height in pixels of each pass*/
  for(i = 0; i != 7; ++i)
  {
    passw[i] = (w + ADAM7_DX[i] - ADAM7_IX[i] - 1) / ADAM7_DX[i];
    passh[i] = (h + ADAM7_DY[i] - ADAM7_IY[i] - 1) / ADAM7_DY[i];
    if(passw[i] == 0) passh[i] = 0;
    if(passh[i] == 0) passw[i] = 0;
  }

  filter_passstart[0] = padded_passstart[0] = passstart[0] = 0;
  for(i = 0; i != 7; ++i)
  {
    /*if passw[i] is 0, it's 0 bytes, not 1 (no filtertype-byte)*/
    filter_passstart[i + 1] = filter_passstart[i]
                            + ((passw[i] && passh[i]) ? passh[i] * (1 + (passw[i] * bpp + 7) / 8) : 0);
    /*bits padded if needed to fill full byte at end of each scanline*/
    padded_passstart[i + 1] = padded_passstart[i] + passh[i] * ((passw[i] * bpp + 7) / 8);
    /*only padded at end of reduced image*/
    passstart[i + 1] = passstart[i] + (passh[i] * passw[i] * bpp + 7) / 8;
  }
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

typedef struct LodePNGCompressSettings LodePNGCompressSettings;
struct LodePNGCompressSettings /*deflate = compress*/
{
  /*LZ77 related settings*/
  unsigned btype; /*the block type for LZ (0, 1, 2 or 3, see zlib standard). Should be 2 for proper compression.*/
  unsigned use_lz77; /*whether or not to use LZ77. Should be 1 for proper compression.*/
  unsigned windowsize; /*must be a power of two <= 32768. higher compresses more but is slower. Default value: 2048.*/
  unsigned minmatch; /*mininum lz77 length. 3 is normally best, 6 can be better for some PNGs. Default: 0*/
  unsigned nicematch; /*stop searching if >= this length found. Set to 258 for best compression. Default: 128*/
  unsigned lazymatching; /*use lazy matching: better compression but a bit slower. Default: true*/

  /*use custom zlib encoder instead of built in one (default: null)*/
  unsigned (*custom_zlib)(unsigned char**, size_t*,
                          const unsigned char*, size_t,
                          const LodePNGCompressSettings*);
  /*use custom deflate encoder instead of built in one (default: null)
  if custom_zlib is used, custom_deflate is ignored since only the built in
  zlib function will call custom_deflate*/
  unsigned (*custom_deflate)(unsigned char**, size_t*,
                             const unsigned char*, size_t,
                             const LodePNGCompressSettings*);

  const void* custom_context; /*optional custom settings for custom functions*/
};

/* log2 approximation. A slight bit faster than std::log. */
static float flog2(float f)
{
  float result = 0;
  while(f > 32) { result += 4; f /= 16; }
  while(f > 2) { ++result; f /= 2; }
  return result + 1.442695f * (f * f * f / 3 - 3 * f * f / 2 + 3 * f - 1.83333f);
}

static unsigned filter(unsigned char* out, const unsigned char* in, unsigned w, unsigned h,
                       const LodePNGColorMode* info, const LodePNGEncoderSettings* settings)
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
  LodePNGFilterStrategy strategy = settings->filter_strategy;

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
  if(settings->filter_palette_zero &&
     (info->colortype == LCT_PALETTE || info->bitdepth < 8)) strategy = LFS_ZERO;

  if(bpp == 0) return 31; /*error: invalid color type*/

  if(strategy == LFS_ZERO)
  {
    for(y = 0; y != h; ++y)
    {
      size_t outindex = (1 + linebytes) * y; /*the extra filterbyte added to each row*/
      size_t inindex = linebytes * y;
      out[outindex] = 0; /*filter type byte*/
      filterScanline(&out[outindex + 1], &in[inindex], prevline, linebytes, bytewidth, 0);
      prevline = &in[inindex];
    }
  }
  else if(strategy == LFS_MINSUM)
  {
    /*adaptive filtering*/
    size_t sum[5];
    unsigned char* attempt[5]; /*five filtering attempts, one for each filter type*/
    size_t smallest = 0;
    unsigned char type, bestType = 0;

    for(type = 0; type != 5; ++type)
    {
      attempt[type] = (unsigned char*)lodepng_malloc(linebytes);
      if(!attempt[type]) return 83; /*alloc fail*/
    }

    if(!error)
    {
      for(y = 0; y != h; ++y)
      {
        /*try the 5 filter types*/
        for(type = 0; type != 5; ++type)
        {
          filterScanline(attempt[type], &in[y * linebytes], prevline, linebytes, bytewidth, type);

          /*calculate the sum of the result*/
          sum[type] = 0;
          if(type == 0)
          {
            for(x = 0; x != linebytes; ++x) sum[type] += (unsigned char)(attempt[type][x]);
          }
          else
          {
            for(x = 0; x != linebytes; ++x)
            {
              /*For differences, each byte should be treated as signed, values above 127 are negative
              (converted to signed char). Filtertype 0 isn't a difference though, so use unsigned there.
              This means filtertype 0 is almost never chosen, but that is justified.*/
              unsigned char s = attempt[type][x];
              sum[type] += s < 128 ? s : (255U - s);
            }
          }

          /*check if this is smallest sum (or if type == 0 it's the first case so always store the values)*/
          if(type == 0 || sum[type] < smallest)
          {
            bestType = type;
            smallest = sum[type];
          }
        }

        prevline = &in[y * linebytes];

        /*now fill the out values*/
        out[y * (linebytes + 1)] = bestType; /*the first byte of a scanline will be the filter type*/
        for(x = 0; x != linebytes; ++x) out[y * (linebytes + 1) + 1 + x] = attempt[bestType][x];
      }
    }

    for(type = 0; type != 5; ++type) lodepng_free(attempt[type]);
  }
  else if(strategy == LFS_ENTROPY)
  {
    float sum[5];
    unsigned char* attempt[5]; /*five filtering attempts, one for each filter type*/
    float smallest = 0;
    unsigned type, bestType = 0;
    unsigned count[256];

    for(type = 0; type != 5; ++type)
    {
      attempt[type] = (unsigned char*)lodepng_malloc(linebytes);
      if(!attempt[type]) return 83; /*alloc fail*/
    }

    for(y = 0; y != h; ++y)
    {
      /*try the 5 filter types*/
      for(type = 0; type != 5; ++type)
      {
        filterScanline(attempt[type], &in[y * linebytes], prevline, linebytes, bytewidth, type);
        for(x = 0; x != 256; ++x) count[x] = 0;
        for(x = 0; x != linebytes; ++x) ++count[attempt[type][x]];
        ++count[type]; /*the filter type itself is part of the scanline*/
        sum[type] = 0;
        for(x = 0; x != 256; ++x)
        {
          float p = count[x] / (float)(linebytes + 1);
          sum[type] += count[x] == 0 ? 0 : flog2(1 / p) * p;
        }
        /*check if this is smallest sum (or if type == 0 it's the first case so always store the values)*/
        if(type == 0 || sum[type] < smallest)
        {
          bestType = type;
          smallest = sum[type];
        }
      }

      prevline = &in[y * linebytes];

      /*now fill the out values*/
      out[y * (linebytes + 1)] = bestType; /*the first byte of a scanline will be the filter type*/
      for(x = 0; x != linebytes; ++x) out[y * (linebytes + 1) + 1 + x] = attempt[bestType][x];
    }

    for(type = 0; type != 5; ++type) lodepng_free(attempt[type]);
  }
  else if(strategy == LFS_PREDEFINED)
  {
    for(y = 0; y != h; ++y)
    {
      size_t outindex = (1 + linebytes) * y; /*the extra filterbyte added to each row*/
      size_t inindex = linebytes * y;
      unsigned char type = settings->predefined_filters[y];
      out[outindex] = type; /*filter type byte*/
      filterScanline(&out[outindex + 1], &in[inindex], prevline, linebytes, bytewidth, type);
      prevline = &in[inindex];
    }
  }
  else if(strategy == LFS_BRUTE_FORCE)
  {
    ;
  }
  else return 88; /* unknown filter strategy */

  return error;
}


static unsigned preProcessScanlines(unsigned char** out, size_t* outsize, const unsigned char* in,
                                    unsigned w, unsigned h,
                                    const LodePNGColorMode* color, const LodePNGEncoderSettings* settings)
{
  /*
  This function converts the pure 2D image with the PNG's colortype, into filtered-padded-interlaced data. Steps:
  *) if no Adam7: 1) add padding bits (= posible extra bits per scanline if bpp < 8) 2) filter
  *) if adam7: 1) Adam7_interlace 2) 7x add padding bits 3) 7x filter
  */
  unsigned bpp = lodepng_get_bpp(color);
  unsigned error = 0;


    *outsize = h + (h * ((w * bpp + 7) / 8)); /*image size plus an extra byte per scanline + possible padding bits*/
    *out = (unsigned char*)lodepng_malloc(*outsize);
    if(!(*out) && (*outsize)) error = 83; /*alloc fail*/

    if(!error)
    {
      /*non multiple of 8 bits per scanline, padding bits needed per scanline*/
      if(bpp < 8 && w * bpp != ((w * bpp + 7) / 8) * 8)
      {
        unsigned char* padded = (unsigned char*)lodepng_malloc(h * ((w * bpp + 7) / 8));
        if(!padded) error = 83; /*alloc fail*/
        if(!error)
        {
          addPaddingBits(padded, in, ((w * bpp + 7) / 8) * 8, w * bpp, h);
          error = filter(*out, padded, w, h, color, settings);
        }
        lodepng_free(padded);
      }
      else
      {
        /*we can immediately filter into the out buffer, no other steps needed*/
        error = filter(*out, in, w, h, color, settings);
      }
    }

  return error;
}



/*#define MAXDIF 50
int isgray(int r, int g, int b)
{
   int max, min;
   if(abs(r-g)>MAXDIF ||abs(r-b)>MAXDIF || abs(g-b)>MAXDIF)
      return 0;
   else
      return 1;
}*/


//#include <string>
//#include <iostream>
//#include <fstream>

//using namespace std;




/*vector<png_color> ver_color;

void ifexistcolor(png_color color)
{
   int i;
   int find = 0;
   vector<png_color>::iterator it;
   //it=find(ver_color.begin,ver_color.end(),color);
   for(i=0;i<ver_color.size();i++)
   {
         if(ver_color[i].blue == color.blue && ver_color[i].green ==color.green && ver_color[i].red == color.red)
         {
            find=1;
            break;
         }
   }
   if(find=0)
      ver_color.push_back(color);

}*/

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

int auto_detect_png(char *filepath, LodePNGColorMode* modein, LodePNGColorMode* modeout, auto_pic_data* pic_data)
{
 png_structp png_ptr;
 png_infop   info_ptr;
 char        buf[PNG_BYTES_TO_CHECK];
 int         temp;
 FILE *pic_fp;
 int i,j;
 int pos=0;
 int channel;
 png_byte color_type;

 png_bytep* row_pointers;
 int width;
 int height;
 
 unsigned char* putin;
 ColorTree tree;
 color_tree_init(&tree);
 
 pic_fp = fopen(filepath, "rb");
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
 pic_data->height = height;
 pic_data->width = width;
 pic_data->src_size = lodepng_filesize(filepath);
 pic_data->size =pic_data->src_size;

 if(color_type ==PNG_COLOR_TYPE_RGB)
 {
   modein->colortype=LCT_RGB;
 }
 else if(color_type ==PNG_COLOR_TYPE_RGBA)
 {   
   modein->colortype=LCT_RGBA;
 }
 else if(color_type==PNG_COLOR_TYPE_PALETTE)
 {
    modein->colortype=LCT_PALETTE;
 }
 else if(color_type==PNG_COLOR_TYPE_GRAY)
 {
    modein->colortype =LCT_GREY;
 }
 else 
 {
    modein->colortype=LCT_GREY_ALPHA;
 }

 if(modein->colortype !=LCT_RGB && modein->colortype !=LCT_RGBA)
 {
    png_destroy_read_struct(&png_ptr, &info_ptr, 0);
    fclose(pic_fp);
    return -1;
 }
 
 modein->bitdepth= png_get_bit_depth(png_ptr, info_ptr);
 channel=png_get_channels(png_ptr, info_ptr);

 row_pointers = png_get_rows(png_ptr, info_ptr);


 putin =(unsigned char*)malloc(sizeof(unsigned char)*width*height*channel);

 for(i=0; i<height; i++)
 {
    for(j=0; j<channel*width; j++)
       putin[pos++]= row_pointers[i][j];
 }
 pic_data->row_pointer = putin;
 lodepng_auto_choose_color(modeout,(unsigned char*)putin,width,height, modein);

 png_destroy_read_struct(&png_ptr, &info_ptr, 0);
 fclose(pic_fp);
 return 0;
}



png_bytepp return_row_pointers;
int detect_png(char *filepath, pic_data *out)
//int detect_png(char *filepath)
{
 int sisi;
 png_structp png_ptr;
 png_infop   info_ptr;
  char        buf[PNG_BYTES_TO_CHECK];
 int         temp;
 FILE *pic_fp;
 int color_type,channels;
 int i,j;
 int size;
 int pos = 0;
 unsigned char* putin;

     
  FILE *fpWrite;
 // string outfilename("color.txt");
  //ofstream fout(outfilename.c_str());

 size_t num;
   png_bytep* row_pointers;
   int bit_depth;
   int width;
   int height;
   unsigned numcolors_done = 0;
   int step;
   unsigned char haha;

   unsigned char high;

   ColorTree tree;
   unsigned maxnumcolors = 257;
 //
   int r;
   int g;
   int b;
   int a;

 LodePNGColorMode* modeout = (LodePNGColorMode*)malloc(sizeof(LodePNGColorMode));
 LodePNGColorMode* modein =(LodePNGColorMode*)malloc(sizeof(LodePNGColorMode));
 lodepng_color_mode_init(modeout);
 lodepng_color_mode_init(modein);

 modein->bitdepth=8;
 modein->colortype=LCT_RGBA;

 color_tree_init(&tree);
 pic_fp = fopen(filepath, "rb");
 if(pic_fp == NULL) 
  return -1;
 
 fpWrite=fopen("D:\\data1.txt","w");
 if(fpWrite == NULL) 
  return -1;


 
 png_ptr  = png_create_read_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
 info_ptr = png_create_info_struct(png_ptr);
 
 setjmp(png_jmpbuf(png_ptr)); 
 
 temp = fread(buf,1,PNG_BYTES_TO_CHECK,pic_fp);
 temp = png_sig_cmp((void*)buf, (png_size_t)0, PNG_BYTES_TO_CHECK);
 
 if (temp!=0) return 1;
 
 rewind(pic_fp);

 png_init_io(png_ptr, pic_fp);

 png_read_png(png_ptr, info_ptr, PNG_TRANSFORM_EXPAND, 0);


 //png_read_info(png_ptr, info_ptr);
 channels       = png_get_channels(png_ptr, info_ptr); 
 bit_depth = png_get_bit_depth(png_ptr, info_ptr);
 color_type     = png_get_color_type(png_ptr, info_ptr);
 //png_get_IHDR(png_ptr,info_ptr,width,height,bit_depth,color_type,interlace_method,compression_method,filter_method);


 row_pointers = png_get_rows(png_ptr, info_ptr);
 width = png_get_image_width(png_ptr, info_ptr);
 height = png_get_image_height(png_ptr, info_ptr);

 
 putin =(unsigned char*)malloc(sizeof( unsigned char)*width*height*4);

 for(i=0;i<height;i++)
 {
    for(j=0;j<4*width;j++)
       putin[pos++]= row_pointers[i][j];
 }

 lodepng_auto_choose_color(modeout,(unsigned char*)putin,width,height,modein);

 //out->can_gray = modeout->


 //png_const_color_8p


 fclose(fpWrite);

 png_destroy_read_struct(&png_ptr, &info_ptr, 0); 

 out->bit_depth = modeout->bitdepth;
 out->color_type =modeout->colortype;
 out->height =height;
 out->row_pointers = return_row_pointers;
 out->width =width;

 return 0;
}

 png_colorp palette;
int detect_palette_png(char *filepath)
{
 int sisi;
 png_structp png_ptr;
 png_infop   info_ptr;
 //png_colorp palette;
 int num_palette;
 char        buf[PNG_BYTES_TO_CHECK];
 int         temp;
 FILE *pic_fp;
 int color_type,channels;
 int i,j;
 int size;
 int pos = 0;
 size_t num;
   png_bytep* row_pointers;
   int bit_depth;
   png_uint_32 width, height;
   png_sPLT_tp entry;

   int rowbytes;
   int interlace_method,compression_method,filter_method;

   int step;
   int r;
   int g;
   int b;
   int a;
 pic_fp = fopen(filepath, "rb");
 if(pic_fp == NULL) 
  return -1;
 



 
 png_ptr  = png_create_read_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
 info_ptr = png_create_info_struct(png_ptr);
 
 setjmp(png_jmpbuf(png_ptr)); 
 
 temp = fread(buf,1,PNG_BYTES_TO_CHECK,pic_fp);
 temp = png_sig_cmp((void*)buf, (png_size_t)0, PNG_BYTES_TO_CHECK);
 
 if (temp!=0) return 1;
 
 rewind(pic_fp);

 png_init_io(png_ptr, pic_fp);

 //png_read_png(png_ptr, info_ptr, PNG_TRANSFORM_EXPAND, 0);
 png_read_info(png_ptr,info_ptr);


 //png_read_info(png_ptr, info_ptr);
 //channels       = png_get_channels(png_ptr, info_ptr); 
 //bit_depth = png_get_bit_depth(png_ptr, info_ptr);
 //color_type     = png_get_color_type(png_ptr, info_ptr);
 row_pointers = png_get_rows(png_ptr,info_ptr);

 png_get_IHDR(png_ptr,info_ptr,&width,&height, &bit_depth,&color_type,&interlace_method, &compression_method, &filter_method);


 //row_pointers = png_get_rows(png_ptr, info_ptr);
 //width = png_get_image_width(png_ptr, info_ptr);
 //height = png_get_image_height(png_ptr, info_ptr);
 png_get_sPLT(png_ptr,info_ptr,&entry);

 if(color_type ==PNG_COLOR_TYPE_PALETTE)
 {
    png_get_PLTE(png_ptr,info_ptr,&palette,&num_palette);
    for(i=0;i<num_palette;i++)
    {
       printf("%d, %d, %d\n", palette[i].red, palette[i].green,palette[i].blue);
    }
 }


 //rowbytes = png_get_rowbytes(png_ptr,info_ptr);
 row_pointers = (png_bytep*) malloc(sizeof(unsigned char)*width*height*3);
 row_pointers = png_get_rows(png_ptr,info_ptr);

  /* for(i = 0; i < height; i++)
  {
   for(j = 0; j < width; j++)
   {
      printf("%d, %d, %d", row_pointers[i][j],row_pointers[i][j+1],row_pointers[i][j+2]);
   }
  }*/
   

 
  /*if(out->only_black_or_white)
   printf("%s\n",filepath);*/
  size = (sizeof(unsigned char))*width*height;

  
  return_row_pointers=(png_bytepp)malloc(size*step); 

  if(return_row_pointers == NULL)
  {
   fclose(pic_fp);
   puts("error");
   return 1;
  }

 png_destroy_read_struct(&png_ptr, &info_ptr, 0); 

 return 0;
}


int write_png_palette(char *file_name)
{
   int i;
// int j, i, temp, pos;
   //int temp,
 int pos,j;
 png_byte color_type; 

 png_structp png_ptr;
 png_infop info_ptr; 
 png_bytepp testing;
 //png_bytep * row_pointers;
 /* create file */
 FILE *fp = fopen(file_name, "wb");
 if (!fp)
 {
  printf("[write_png_file] File %s could not be opened for writing", file_name);
  return -1;
 }


 /* initialize stuff */
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


 /* write header */
 if (setjmp(png_jmpbuf(png_ptr)))
 {
  printf("[write_png_file] Error during writing header");
  return -1;
 }

 png_set_IHDR(png_ptr, info_ptr, 100, 100, 8, PNG_COLOR_TYPE_PALETTE, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

 
 /*palette = (png_colorp)png_malloc(png_ptr, PNG_MAX_PALETTE_LENGTH
             * (sizeof (png_color)));*/

 png_set_PLTE(png_ptr, info_ptr, palette, 256);


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

 return_row_pointers=(png_bytepp)malloc(sizeof(unsigned char)*100*100); 

 for(i =0;i <100;i++)
 {
    return_row_pointers[i] = (png_bytep)malloc(sizeof(unsigned char)*100);
    for( j=0;j<100;j++)
    { if(j<50)
      return_row_pointers[i][j]=255;
    else
    {
       return_row_pointers[i][j]=0;
    }
    }
 }

 png_write_image(png_ptr, (png_bytepp)return_row_pointers);
 png_write_end(png_ptr, NULL);

 //png_free(png_ptr, palette);
 //palette = NULL;
 png_destroy_write_struct(&png_ptr, &info_ptr);

    fclose(fp);
    return 0;
}

int write_png_file(char *file_name, pic_data *in)
{
// int j, i, temp, pos;
   //int temp,
 int pos,j;
 png_byte color_type; 

 png_structp png_ptr;
 png_infop info_ptr; 
 //png_bytep * row_pointers;
 /* create file */
 FILE *fp = fopen(file_name, "wb");
 if (!fp)
 {
  printf("[write_png_file] File %s could not be opened for writing", file_name);
  return -1;
 }


 /* initialize stuff */
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


 /* write header */
 if (setjmp(png_jmpbuf(png_ptr)))
 {
  printf("[write_png_file] Error during writing header");
  return -1;
 }

 /*if(graph->flag == HAVE_ALPHA) color_type = PNG_COLOR_TYPE_RGB_ALPHA;
 else color_type = PNG_COLOR_TYPE_RGB;*/
 if(in->only_black_or_white)
 {
    //printf("1 bit, gray\n %s\n",file_name);
    png_set_IHDR(png_ptr, info_ptr, in->width, in->height,
    4, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE,
  PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
 }

 else if(in->can_gray)
 {
    //printf("8 bit, gray\n %s\n",file_name);
    png_set_IHDR(png_ptr, info_ptr, in->width, in->height,
    8, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE,
  PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
 }
 else
 {
   //printf("8 bit, grb\n %s\n",file_name);
   png_set_IHDR(png_ptr, info_ptr, in->width, in->height,
    8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
  PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
 }
 
 



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

 png_write_image(png_ptr, (png_bytepp)(in->row_pointers));
 png_write_end(png_ptr, NULL);


    fclose(fp);
    return 0;
}



#include <io.h>
struct _pngexportinfo
{
   int grey_bit1_num;
   int grey_bit1_size;
   int grey_bit1_src_size;

   int grey_bit2_num;
   int grey_bit2_size;
   int grey_bit2_src_size;

   int grey_bit4_num;
   int grey_bit4_size;
   int grey_bit4_src_size;

   int grey_bit8_num;
   int grey_bit8_size;
   int grey_bit8_src_size;
   
   int palette_bit1_num;
   int palette_bit1_size;
   int palette_bit1_src_size;

   int palette_bit2_num;
   int palette_bit2_size;
   int palette_bit2_src_size;
   
   int palette_bit4_num;
   int palette_bit4_size;
   int palette_bit4_src_size;
   
   int palette_bit8_num;
   int palette_bit8_size;
   int palette_bit8_src_size;
   
   int rgb_bit8_num;
   int rgb_bit8_size;
   int rgb_bit8_src_size;
   
   int rgba_bit8_num;
   int rgba_bit8_size;
   int rgba_bit8_src_size;
};
typedef struct _pngexportinfo pngexportinfo;

static pngexportinfo info;

void InitExportInfo()
{
   info.grey_bit1_num =0;
   info.grey_bit1_size =0;
   info.grey_bit1_src_size=0;

   info.grey_bit2_num =0;
   info.grey_bit2_size =0;
   info.grey_bit2_src_size=0;
   
   info.grey_bit4_num =0;
   info.grey_bit4_size =0;
   info.grey_bit4_src_size=0;

   info.grey_bit8_num =0;
   info.grey_bit8_size =0;
   info.grey_bit8_src_size=0;
   
   info.palette_bit1_num =0;
   info.palette_bit1_size =0;
   info.palette_bit1_src_size=0;

   info.palette_bit2_num =0;
   info.palette_bit2_size =0;
   info.palette_bit2_src_size=0;

   info.palette_bit4_num =0;
   info.palette_bit4_size =0;
   info.palette_bit4_src_size=0;

   info.palette_bit8_num =0;
   info.palette_bit8_size=0;
   info.palette_bit8_src_size=0;

   info.rgb_bit8_num =0;
   info.rgb_bit8_size=0;
   info.rgb_bit8_src_size=0;
   
   info.rgba_bit8_num=0;
   info.rgba_bit8_size=0;
   info.rgba_bit8_src_size=0;
}

/*void updateInfo(pic_data* data)
{
   if(data->color_type == PNG_COLOR_TYPE_GRAY)
   {
      info.grey_num++;
   }
   else if(data->color_type == PNG_COLOR_TYPE_PALETTE)
      info.palette_num++;
   else if(data->color_type ==PNG_COLOR_TYPE_RGB)
      info.rgb_num++;
}*/

/*void displayInfo()
{
   printf("Total number of PNG: %d\n", info.grey_num + info.palette_num + info.rgb_num);
   printf("Grey : %d\n",info.grey_num);
   printf("Palette : %d\n", info.palette_num);
   printf("RBA : %d\n", info.rgb_num);
}*/

void autodisplayInfo()
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

   float Grey=0;
   float Palette=0;
   float RGB=0;
   grey_num =info.grey_bit1_num+info.grey_bit2_num+info.grey_bit4_num+info.grey_bit8_num;
   grey_size=info.grey_bit1_size+info.grey_bit2_size+info.grey_bit4_size+info.grey_bit8_size;
   grey_src_size=info.grey_bit1_src_size+info.grey_bit2_src_size+info.grey_bit4_src_size+info.grey_bit8_src_size;

   palette_num =info.palette_bit1_num+info.palette_bit2_num+info.palette_bit4_num+info.palette_bit8_num;
   palette_size= info.palette_bit1_size+info.palette_bit2_size+info.palette_bit4_size+info.palette_bit8_size;
   palette_src_size= info.palette_bit1_src_size+info.palette_bit2_src_size+info.palette_bit4_src_size+info.palette_bit8_src_size;
   
   total_num = grey_num+palette_num+info.rgb_bit8_num+info.rgba_bit8_num;
   total_size = grey_size+palette_size+info.rgb_bit8_size+info.rgba_bit8_size;
   total_src_size= grey_src_size+ palette_src_size+info.rgb_bit8_src_size+info.rgba_bit8_src_size;

   printf("Total PNG: %3d   size = %d source_size = %d compress = %3f\n\n",total_num , total_size, total_src_size, (float)total_size/total_src_size);
   printf("All Grey    : %3d   size = %9d percent = %3f, compress = %3f\n",grey_num, grey_size, (float)grey_size/total_size, (float)grey_size/grey_src_size);
   printf("  Grey 1    : %3d   size = %9d percent = %3f, compress = %3f\n",info.grey_bit1_num, info.grey_bit1_size, (float)info.grey_bit1_size/total_size,(float)info.grey_bit1_size/info.grey_bit1_src_size);
   printf("  Grey 2    : %3d   size = %9d percent = %3f, compress = %3f\n",info.grey_bit2_num, info.grey_bit2_size, (float)info.grey_bit2_size/total_size,(float)info.grey_bit2_size/info.grey_bit2_src_size);
   printf("  Grey 4    : %3d   size = %9d percent = %3f, compress = %3f\n",info.grey_bit4_num, info.grey_bit4_size, (float)info.grey_bit4_size/total_size,(float)info.grey_bit4_size/info.grey_bit4_src_size);
   printf("  Grey 8    : %3d   size = %9d percent = %3f, compress = %3f\n\n",info.grey_bit8_num, info.grey_bit8_size, (float)info.grey_bit8_size/total_size,(float)info.grey_bit8_size/info.grey_bit8_src_size);
   printf("All Pal     : %3d   size = %9d percent = %3f, compress = %3f\n",palette_num, palette_size, (float)palette_size/total_size, (float)palette_size/palette_src_size);
   printf("  Palette 1 : %3d   size = %9d percent = %3f, compress = %3f\n",info.palette_bit1_num, info.palette_bit1_size, (float)info.palette_bit1_size/total_size,(float)info.palette_bit1_size/info.palette_bit1_src_size);
   printf("  Palette 2 : %3d   size = %9d percent = %3f, compress = %3f\n",info.palette_bit2_num, info.palette_bit2_size, (float)info.palette_bit2_size/total_size,(float)info.palette_bit2_size/info.palette_bit2_src_size);
   printf("  Palette 4 : %3d   size = %9d percent = %3f, compress = %3f\n",info.palette_bit4_num, info.palette_bit4_size, (float)info.palette_bit4_size/total_size,(float)info.palette_bit4_size/info.palette_bit4_src_size);
   printf("  Palette 8 : %3d   size = %9d percent = %3f, compress = %3f\n\n",info.palette_bit8_num, info.palette_bit8_size, (float)info.palette_bit8_size/total_size,(float)info.palette_bit8_size/info.palette_bit8_src_size);
   printf("Total RGB   : %3d   size = %9d percent = %3f, compress = %3f\n\n",info.rgb_bit8_num, info.rgb_bit8_size, (float)info.rgb_bit8_size/total_size,(float)info.rgb_bit8_size/info.rgba_bit8_src_size);
   //printf("Total RGBA       : %3d   size = %d\n",info.rgba_bit8_num, info.rgba_bit8_size);
}

void ConvertFile(char* src_path)
{
   pic_data *data=(pic_data*)malloc(sizeof(pic_data));

   detect_png(src_path,data);
   
   write_png_file(src_path, data);
   //updateInfo(data);
}

void autoupdateInfo(char* src_path, LodePNGColorMode* mode, auto_pic_data* data)
{
   if(mode->colortype == PNG_COLOR_TYPE_GRAY)
   {
      if(mode->bitdepth ==1)
      {
         info.grey_bit1_num++;
         info.grey_bit1_size += data->size;
         info.grey_bit1_src_size +=data->src_size;
      }
      else if(mode->bitdepth==2)
      {
         info.grey_bit2_num++;
         info.grey_bit2_size += data->size;
         info.grey_bit2_src_size +=data->src_size;
      }
      else if(mode->bitdepth==4)
      {
         info.grey_bit4_num++;
         info.grey_bit4_size +=data->size;
         info.grey_bit4_src_size +=data->src_size;
      }
      else if(mode->bitdepth==8)
      {
         info.grey_bit8_num++;
         info.grey_bit8_size +=data->size;
         info.grey_bit8_src_size +=data->src_size;
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
         info.palette_bit1_num++;
         info.palette_bit1_size +=data->size;
         info.palette_bit1_src_size +=data->src_size;
      }
      else if(mode->bitdepth==2)
      {
         info.palette_bit2_num++;
         info.palette_bit2_size +=data->size;
         info.palette_bit2_src_size +=data->src_size;
      }
      else if(mode->bitdepth==4)
      {
         info.palette_bit4_num++;
         info.palette_bit4_size +=data->size;
         info.palette_bit4_src_size +=data->src_size;
      }
      else if(mode->bitdepth==8)
      {
         info.palette_bit8_num++;
         info.palette_bit8_size +=data->size;
         info.palette_bit8_src_size +=data->src_size;
      }
      else
      {
         printf("palette color bitdepth error\n");
      }
   }
   else if(mode->colortype ==PNG_COLOR_TYPE_RGB)
   {
      info.rgb_bit8_num++;
      info.rgb_bit8_size +=data->size;
      info.rgba_bit8_src_size +=data->src_size;
   }
   else if(mode->colortype ==PNG_COLOR_TYPE_RGB_ALPHA)
   {
      info.rgba_bit8_num++;
      info.rgba_bit8_size +=data->size;
      info.rgba_bit8_src_size +=data->src_size;
   }
   //printf("%s type = %d bit= %d\n",src_path, mode->colortype, mode->bitdepth);
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

/*Try the code, if it returns error, also return the error.*/
#define CERROR_TRY_RETURN(call)\
{\
  unsigned error = call;\
  if(error) return error;\
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


void autoCovertFile(char* src_path)
{
 auto_pic_data* pic_data =(auto_pic_data*)malloc(sizeof(auto_pic_data));
 LodePNGColorMode* modeout = (LodePNGColorMode*)malloc(sizeof(LodePNGColorMode));
 LodePNGColorMode* modein =(LodePNGColorMode*)malloc(sizeof(LodePNGColorMode));
 lodepng_color_mode_init(modeout);
 lodepng_color_mode_init(modein);

 if(auto_detect_png(src_path, modein, modeout,pic_data)==0)
 {
   autoWriteFile(src_path,modein,modeout,pic_data);
   autoupdateInfo(src_path, modeout, pic_data);
 }
 else
 {
    autoupdateInfo(src_path,modein, pic_data);
 }
}


static void ucvector_init(ucvector* p)
{
  p->data = NULL;
  p->size = p->allocsize = 0;
}

static unsigned zlib_compress(unsigned char** out, size_t* outsize, const unsigned char* in,
                              size_t insize, const LodePNGCompressSettings* settings)
{
  if(!settings->custom_zlib) return 87; /*no custom zlib function provided */
  return settings->custom_zlib(out, outsize, in, insize, settings);
}

/*chunkName must be string of 4 characters*/
static unsigned addChunk(ucvector* out, const char* chunkName, const unsigned char* data, size_t length)
{
  CERROR_TRY_RETURN(lodepng_chunk_create(&out->data, &out->size, (unsigned)length, chunkName, data));
  out->allocsize = out->size; /*fix the allocsize again*/
  return 0;
}

static void ucvector_cleanup(void* p)
{
  ((ucvector*)p)->size = ((ucvector*)p)->allocsize = 0;
  lodepng_free(((ucvector*)p)->data);
  ((ucvector*)p)->data = NULL;
}


/*static unsigned addChunk_IDAT(ucvector* out, const unsigned char* data, size_t datasize,
                              LodePNGCompressSettings* zlibsettings)*/
static unsigned addChunk_IDAT(ucvector* out, const unsigned char* data, size_t datasize,
                              LodePNGCompressSettings* zlibsettings)
{
  ucvector zlibdata;
  unsigned error = 0;

  /*compress with the Zlib compressor*/
  ucvector_init(&zlibdata);
  //error = zlib_compress(&zlibdata.data, &zlibdata.size, data, datasize, zlibsettings);
  //if(!error) error = addChunk(out, "IDAT", zlibdata.data, zlibdata.size);
  addChunk(out,"IDAT",data, datasize);
  ucvector_cleanup(&zlibdata);

  return error;
}

static void lodepng_set32bitInt(unsigned char* buffer, unsigned value)
{
  buffer[0] = (unsigned char)((value >> 24) & 0xff);
  buffer[1] = (unsigned char)((value >> 16) & 0xff);
  buffer[2] = (unsigned char)((value >>  8) & 0xff);
  buffer[3] = (unsigned char)((value      ) & 0xff);
}

unsigned lodepng_read32bitInt(const unsigned char* buffer)
{
  return (unsigned)((buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3]);
}

static unsigned lodepng_crc32_table[256] = {
           0u, 1996959894u, 3993919788u, 2567524794u,  124634137u, 1886057615u, 3915621685u, 2657392035u,
   249268274u, 2044508324u, 3772115230u, 2547177864u,  162941995u, 2125561021u, 3887607047u, 2428444049u,
   498536548u, 1789927666u, 4089016648u, 2227061214u,  450548861u, 1843258603u, 4107580753u, 2211677639u,
   325883990u, 1684777152u, 4251122042u, 2321926636u,  335633487u, 1661365465u, 4195302755u, 2366115317u,
   997073096u, 1281953886u, 3579855332u, 2724688242u, 1006888145u, 1258607687u, 3524101629u, 2768942443u,
   901097722u, 1119000684u, 3686517206u, 2898065728u,  853044451u, 1172266101u, 3705015759u, 2882616665u,
   651767980u, 1373503546u, 3369554304u, 3218104598u,  565507253u, 1454621731u, 3485111705u, 3099436303u,
   671266974u, 1594198024u, 3322730930u, 2970347812u,  795835527u, 1483230225u, 3244367275u, 3060149565u,
  1994146192u,   31158534u, 2563907772u, 4023717930u, 1907459465u,  112637215u, 2680153253u, 3904427059u,
  2013776290u,  251722036u, 2517215374u, 3775830040u, 2137656763u,  141376813u, 2439277719u, 3865271297u,
  1802195444u,  476864866u, 2238001368u, 4066508878u, 1812370925u,  453092731u, 2181625025u, 4111451223u,
  1706088902u,  314042704u, 2344532202u, 4240017532u, 1658658271u,  366619977u, 2362670323u, 4224994405u,
  1303535960u,  984961486u, 2747007092u, 3569037538u, 1256170817u, 1037604311u, 2765210733u, 3554079995u,
  1131014506u,  879679996u, 2909243462u, 3663771856u, 1141124467u,  855842277u, 2852801631u, 3708648649u,
  1342533948u,  654459306u, 3188396048u, 3373015174u, 1466479909u,  544179635u, 3110523913u, 3462522015u,
  1591671054u,  702138776u, 2966460450u, 3352799412u, 1504918807u,  783551873u, 3082640443u, 3233442989u,
  3988292384u, 2596254646u,   62317068u, 1957810842u, 3939845945u, 2647816111u,   81470997u, 1943803523u,
  3814918930u, 2489596804u,  225274430u, 2053790376u, 3826175755u, 2466906013u,  167816743u, 2097651377u,
  4027552580u, 2265490386u,  503444072u, 1762050814u, 4150417245u, 2154129355u,  426522225u, 1852507879u,
  4275313526u, 2312317920u,  282753626u, 1742555852u, 4189708143u, 2394877945u,  397917763u, 1622183637u,
  3604390888u, 2714866558u,  953729732u, 1340076626u, 3518719985u, 2797360999u, 1068828381u, 1219638859u,
  3624741850u, 2936675148u,  906185462u, 1090812512u, 3747672003u, 2825379669u,  829329135u, 1181335161u,
  3412177804u, 3160834842u,  628085408u, 1382605366u, 3423369109u, 3138078467u,  570562233u, 1426400815u,
  3317316542u, 2998733608u,  733239954u, 1555261956u, 3268935591u, 3050360625u,  752459403u, 1541320221u,
  2607071920u, 3965973030u, 1969922972u,   40735498u, 2617837225u, 3943577151u, 1913087877u,   83908371u,
  2512341634u, 3803740692u, 2075208622u,  213261112u, 2463272603u, 3855990285u, 2094854071u,  198958881u,
  2262029012u, 4057260610u, 1759359992u,  534414190u, 2176718541u, 4139329115u, 1873836001u,  414664567u,
  2282248934u, 4279200368u, 1711684554u,  285281116u, 2405801727u, 4167216745u, 1634467795u,  376229701u,
  2685067896u, 3608007406u, 1308918612u,  956543938u, 2808555105u, 3495958263u, 1231636301u, 1047427035u,
  2932959818u, 3654703836u, 1088359270u,  936918000u, 2847714899u, 3736837829u, 1202900863u,  817233897u,
  3183342108u, 3401237130u, 1404277552u,  615818150u, 3134207493u, 3453421203u, 1423857449u,  601450431u,
  3009837614u, 3294710456u, 1567103746u,  711928724u, 3020668471u, 3272380065u, 1510334235u,  755167117u
};

unsigned lodepng_crc32(const unsigned char* data, size_t length)
{
  unsigned r = 0xffffffffu;
  size_t i;
  for(i = 0; i < length; ++i)
  {
    r = lodepng_crc32_table[(r ^ data[i]) & 0xff] ^ (r >> 8);
  }
  return r ^ 0xffffffffu;
}

unsigned lodepng_chunk_length(const unsigned char* chunk)
{
  return lodepng_read32bitInt(&chunk[0]);
}

void lodepng_chunk_generate_crc(unsigned char* chunk)
{
  unsigned length = lodepng_chunk_length(chunk);
  unsigned CRC = lodepng_crc32(&chunk[4], length + 4);
  lodepng_set32bitInt(chunk + 8 + length, CRC);
}

unsigned lodepng_chunk_create(unsigned char** out, size_t* outlength, unsigned length,
                              const char* type, const unsigned char* data)
{
  unsigned i;
  unsigned char *chunk, *new_buffer;
  size_t new_length = (*outlength) + length + 12;
  if(new_length < length + 12 || new_length < (*outlength)) return 77; /*integer overflow happened*/
  new_buffer = (unsigned char*)lodepng_realloc(*out, new_length);
  if(!new_buffer) return 83; /*alloc fail*/
  (*out) = new_buffer;
  (*outlength) = new_length;
  chunk = &(*out)[(*outlength) - length - 12];

  /*1: length*/
  lodepng_set32bitInt(chunk, (unsigned)length);

  /*2: chunk name (4 letters)*/
  chunk[4] = (unsigned char)type[0];
  chunk[5] = (unsigned char)type[1];
  chunk[6] = (unsigned char)type[2];
  chunk[7] = (unsigned char)type[3];

  /*3: the data*/
  for(i = 0; i != length; ++i) chunk[8 + i] = data[i];

  /*4: CRC (of the chunkname characters and the data)*/
  lodepng_chunk_generate_crc(chunk);

  return 0;
}


int autoWriteFile(char* file_name, LodePNGColorMode* modein,  LodePNGColorMode* modeout, auto_pic_data* pic_data)
{
 int w,h;
 unsigned char* data = 0; /*uncompressed version of the IDAT chunk data*/
 size_t datasize = 0;
 int pos,i,j;
 png_structp png_ptr;
 png_infop info_ptr; 
 LodePNGEncoderSettings *setting;
 unsigned char* converted;
 ucvector outv;
 png_bytepp bytepp;
 int n;
 int step;
 int bpp;

 FILE *fp;
 //LodePNGCompressSettings lodepng_default_compress_settings;
 //png_bytep * row_pointers;
 /* create file */

 if(lodepng_color_mode_equal(modein, modeout))
 {
    pic_data->size=pic_data->src_size;
    return 0; 
 }

 fp = fopen(file_name, "wb");
 if (!fp)
 {
  printf("[write_png_file] File %s could not be opened for writing", file_name);
  return -1;
 }

 /* initialize stuff */
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


 /* write header */
 if (setjmp(png_jmpbuf(png_ptr)))
 {
  printf("[write_png_file] Error during writing header");
  return -1;
 }

 h=pic_data->height;
 w=pic_data->width;


 setting=(LodePNGEncoderSettings *) malloc(sizeof(LodePNGEncoderSettings));
 lodepng_encoder_settings_init(setting);
 ucvector_init(&outv);



  //if(!lodepng_color_mode_equal(modein, modeout))
 {
    //unsigned char* converted;
    size_t size = (w*h * lodepng_get_bpp(modeout) + 7) / 8;

    converted = (unsigned char*)lodepng_malloc(size);
    if(!converted && size) 
       //state->error = 83; /*alloc fail*/
          return ;
    //if(!state->error)
    {
       //state->error = 
       lodepng_convert(converted, pic_data->row_pointer, modeout, modein, w,h);
    }
    preProcessScanlines(&data, &datasize, converted, w, h,modeout, setting);
    //lodepng_free(converted);
  }
  //else 
  {
     //preProcessScanlines(&data, &datasize, pic_data->row_pointer, w, h, modeout, setting);
  }

 switch (modeout->colortype)
 {
 case PNG_COLOR_TYPE_GRAY:
        png_set_IHDR(png_ptr, info_ptr, w, h,
       modeout->bitdepth, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE,
  PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    break;
 case PNG_COLOR_TYPE_PALETTE:
        png_set_IHDR(png_ptr, info_ptr, w, h,
       modeout->bitdepth, PNG_COLOR_TYPE_PALETTE, PNG_INTERLACE_NONE,
  PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    break;
 case PNG_COLOR_TYPE_RGB:
        png_set_IHDR(png_ptr, info_ptr, w, h,
       modeout->bitdepth, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
  PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    break;

 default:
    break;
 }
 
 if(modeout->colortype ==PNG_COLOR_TYPE_PALETTE)
 {
    png_colorp palette=(png_colorp)malloc(sizeof(png_color)*modeout->palettesize);
    for(i=0; i<modeout->palettesize;i++)
    {
       palette[i].red= modeout->palette[4*i];
       palette[i].green = modeout->palette[4*i+1];
       palette[i].blue = modeout->palette[4*i+2];
    }
    png_set_PLTE(png_ptr, info_ptr,palette,modeout->palettesize);
 }
 pos=0;


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

 bpp= lodepng_get_bpp(modeout);
 n = (w*h*bpp + 7) / 8;
 bytepp =(png_bytepp)malloc(n*sizeof(png_bytep));

 pos=0;
 for(i =0; i<h;i++)
 {
    bytepp[i] =(png_bytep)malloc((bpp*w+7)/8);
    for(j=0;j<w;j++)
    {
       if(bpp==24)
       {
          bytepp[i][3*j]=converted[pos++];
          bytepp[i][3*j+1]=converted[pos++];
          bytepp[i][3*j+2]=converted[pos++];
       }
       else if (bpp==8)
       {
          bytepp[i][j]=converted[pos++];
       }
       else if(bpp<8)
       {
          if(j%(8/bpp)==0)
             bytepp[i][j/(8/bpp)]=converted[pos++];
       }
    }
 }


 //png_write_image(png_ptr, (png_bytepp)(in->row_pointers));
 //png_write_image(png_ptr,&outv.data);
 png_write_image(png_ptr,bytepp);
 //for (i = 0; i < h; i++)
    //png_write_rows(png_ptr, &data, 1);
 //png_write_image(png_ptr,&converted);
 png_write_end(png_ptr, NULL);
 
 fclose(fp);

 pic_data->size=lodepng_filesize(file_name);
 return 0;

}




void ConvertFolder(const char * dir)
{
     //char destfile[100];
    //static pngfileinfo info;
    long handle;
    struct _finddata_t FileInfo;
    char dirNew[200];
    pic_data *data=(pic_data*)malloc(sizeof(pic_data));
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

            //cout << findData.name << "\t<dir>\n";
            
            strcpy(dirNew, dir);
            strcat(dirNew, "\\");
            strcat(dirNew, FileInfo.name);
            //printf("%s\n",dirNew);
            ConvertFolder(dirNew);
        }
        else
        {
            //cout << findData.name << "\t" << findData.size << " bytes.\n";
            strcpy(dirNew, dir);
            strcat(dirNew, "\\");
            strcat(dirNew, FileInfo.name);
            //printf("%s\n",dirNew);
            //ConvertFile(dirNew);
            autoCovertFile(dirNew);
        }
    } while (_findnext(handle, &FileInfo) == 0);

    _findclose(handle);    // 关闭搜索句柄
}


 int main(int argc, char *argv[])
{
   unsigned char i=15;
   unsigned char j=1;
//start
//   int i;
pic_data *data;
clock_t begin, end;
double cost;
begin =clock();
InitExportInfo();

printf("start\n");
//info.grey_num=0;
//info.palette_num=0;
//info.rgb_num=0;
/* if(argc!=2)
   {
      printf("pngtest.exe path of tiles file folder\n");
      return -1;
  }
 ConvertFolder(argv[1]);*/
//********************covert PNG**************************
//i=i<<4;
//i=i+j;
//printf("%d",i);
//ConverFolder("D:\\PDF\\Output\\7_png\\tmp - Copy\\AB0D.tmp\\tiles_files");
//listFiles("D:\\PDF\\Output\\6_png\\output - Copy\\1\\tiles_files");
//ConvertFile("D:\\Capture.PNG");
//detect_palette_png("D:\\testpngs\\palette-8.png");
//write_png_palette("D:\\haha.png");


//ConvertFile("D:\\3_6.png");
ConvertFolder("D:\\Tool\\tiles_files");
//ConvertFolder("D:\\PDF\\tiles_files");
//ConvertFolder("D:\\PDF\\Output\\benchmark\\tmp\\71D2.tmp\\tiles_files - Copy");
//autoCovertFile("D:\\9_0.png");
//autoCovertFile("D:\\13");
//ConvertFolder("D:\\PDF\\Output\\A1\\tmp\\5463.tmp\\tiles_files - Copy");
//ConvertFile("D:\\12_7.png");
//ConvertFile("D:\\Capture.png");
//ConvertFolder();
//ConvertFile("D:\\Capture.PNG");

   //data=(pic_data*)malloc(sizeof(pic_data));
   //detect_png("D:\\testpngs\\gray-8.png",data);

//listFiles("D:\\PDF\\Output\\4\\output\\1\\tiles_files - Copy_only_black_white");
//***********************end Covert PNG******************
end = clock();
cost = (double)(end - begin)/CLOCKS_PER_SEC;
printf("end\n");
//displayInfo();
autodisplayInfo();
printf("constant CLOCKS_PER_SEC is: %ld, time cost is: %lf secs", CLOCKS_PER_SEC, cost);



return 0;
}

/*static unsigned getValueRequiredBits(unsigned char value)
{
  if(value == 0 || value == 255) return 1;
  /*The scaling of 2-bit and 4-bit values uses multiples of 85 and 17*/
  //if(value % 17 == 0) return value % 85 == 0 ? 2 : 4;
 // return 8;
//}*/

int
main1(int argc, char *argv[])
{
   int multiple = 0;
   int ierror = 0;

   png_structp dummy_ptr;

   fprintf(STDERR, "\n Testing libpng version %s\n", PNG_LIBPNG_VER_STRING);
   fprintf(STDERR, "   with zlib   version %s\n", ZLIB_VERSION);
   fprintf(STDERR, "%s", png_get_copyright(NULL));
   /* Show the version of libpng used in building the library */
   fprintf(STDERR, " library (%lu):%s",
       (unsigned long)png_access_version_number(),
       png_get_header_version(NULL));

   /* Show the version of libpng used in building the application */
   fprintf(STDERR, " pngtest (%lu):%s", (unsigned long)PNG_LIBPNG_VER,
       PNG_HEADER_VERSION_STRING);

   /* Do some consistency checking on the memory allocation settings, I'm
    * not sure this matters, but it is nice to know, the first of these
    * tests should be impossible because of the way the macros are set
    * in pngconf.h
    */
#if defined(MAXSEG_64K) && !defined(PNG_MAX_MALLOC_64K)
      fprintf(STDERR, " NOTE: Zlib compiled for max 64k, libpng not\n");
#endif
   /* I think the following can happen. */
#if !defined(MAXSEG_64K) && defined(PNG_MAX_MALLOC_64K)
      fprintf(STDERR, " NOTE: libpng compiled for max 64k, zlib not\n");
#endif

   if (strcmp(png_libpng_ver, PNG_LIBPNG_VER_STRING))
   {
      fprintf(STDERR,
          "Warning: versions are different between png.h and png.c\n");
      fprintf(STDERR, "  png.h version: %s\n", PNG_LIBPNG_VER_STRING);
      fprintf(STDERR, "  png.c version: %s\n\n", png_libpng_ver);
      ++ierror;
   }

   if (argc > 1)
   {
      if (strcmp(argv[1], "-m") == 0)
      {
         multiple = 1;
         status_dots_requested = 0;
      }

      else if (strcmp(argv[1], "-mv") == 0 ||
               strcmp(argv[1], "-vm") == 0 )
      {
         multiple = 1;
         verbose = 1;
         status_dots_requested = 1;
      }

      else if (strcmp(argv[1], "-v") == 0)
      {
         verbose = 1;
         status_dots_requested = 1;
         inname = argv[2];
      }

      else if (strcmp(argv[1], "--strict") == 0)
      {
         status_dots_requested = 0;
         verbose = 1;
         inname = argv[2];
         strict++;
         relaxed = 0;
         multiple=1;
      }

      else if (strcmp(argv[1], "--relaxed") == 0)
      {
         status_dots_requested = 0;
         verbose = 1;
         inname = argv[2];
         strict = 0;
         relaxed++;
         multiple=1;
      }
      else if (strcmp(argv[1], "--xfail") == 0)
      {
         status_dots_requested = 0;
         verbose = 1;
         inname = argv[2];
         strict = 0;
         xfail++;
         relaxed++;
         multiple=1;
      }

      else
      {
         inname = argv[1];
         status_dots_requested = 0;
      }
   }

   if (multiple == 0 && argc == 3 + verbose)
      outname = argv[2 + verbose];

   if ((multiple == 0 && argc > 3 + verbose) ||
       (multiple != 0 && argc < 2))
   {
      fprintf(STDERR,
          "usage: %s [infile.png] [outfile.png]\n\t%s -m {infile.png}\n",
          argv[0], argv[0]);
      fprintf(STDERR,
          "  reads/writes one PNG file (without -m) or multiple files (-m)\n");
      fprintf(STDERR,
          "  with -m %s is used as a temporary file\n", outname);
      exit(1);
   }

   if (multiple != 0)
   {
      int i;
#if defined(PNG_USER_MEM_SUPPORTED) && PNG_DEBUG
      int allocation_now = current_allocation;
#endif
      for (i=2; i<argc; ++i)
      {
         int kerror;
         fprintf(STDERR, "\n Testing %s:", argv[i]);
#if PNG_DEBUG > 0
         fprintf(STDERR, "\n");
#endif
         kerror = test_one_file(argv[i], outname);
         if (kerror == 0)
         {
#ifdef PNG_WRITE_USER_TRANSFORM_SUPPORTED
            fprintf(STDERR, "\n PASS (%lu zero samples)\n",
                (unsigned long)zero_samples);
#else
            fprintf(STDERR, " PASS\n");
#endif
#ifdef PNG_TIME_RFC1123_SUPPORTED
            if (tIME_chunk_present != 0)
               fprintf(STDERR, " tIME = %s\n", tIME_string);

            tIME_chunk_present = 0;
#endif /* TIME_RFC1123 */
         }

         else
         {
            if (xfail)
              fprintf(STDERR, " XFAIL\n");
            else
            {
              fprintf(STDERR, " FAIL\n");
              ierror += kerror;
            }
         }
#if defined(PNG_USER_MEM_SUPPORTED) && PNG_DEBUG
         if (allocation_now != current_allocation)
            fprintf(STDERR, "MEMORY ERROR: %d bytes lost\n",
                current_allocation - allocation_now);

         if (current_allocation != 0)
         {
            memory_infop pinfo = pinformation;

            fprintf(STDERR, "MEMORY ERROR: %d bytes still allocated\n",
                current_allocation);

            while (pinfo != NULL)
            {
               fprintf(STDERR, " %lu bytes at %p\n",
                   (unsigned long)pinfo->size,
                   pinfo->pointer);
               pinfo = pinfo->next;
            }
         }
#endif
      }
#if defined(PNG_USER_MEM_SUPPORTED) && PNG_DEBUG
         fprintf(STDERR, " Current memory allocation: %10d bytes\n",
             current_allocation);
         fprintf(STDERR, " Maximum memory allocation: %10d bytes\n",
             maximum_allocation);
         fprintf(STDERR, " Total   memory allocation: %10d bytes\n",
             total_allocation);
         fprintf(STDERR, "     Number of allocations: %10d\n",
             num_allocations);
#endif
   }

   else
   {
      int i;
      for (i = 0; i<3; ++i)
      {
         int kerror;
#if defined(PNG_USER_MEM_SUPPORTED) && PNG_DEBUG
         int allocation_now = current_allocation;
#endif
         if (i == 1)
            status_dots_requested = 1;

         else if (verbose == 0)
            status_dots_requested = 0;

         if (i == 0 || verbose == 1 || ierror != 0)
         {
            fprintf(STDERR, "\n Testing %s:", inname);
#if PNG_DEBUG > 0
            fprintf(STDERR, "\n");
#endif
         }

         kerror = test_one_file(inname, outname);

         if (kerror == 0)
         {
            if (verbose == 1 || i == 2)
            {
#ifdef PNG_WRITE_USER_TRANSFORM_SUPPORTED
                fprintf(STDERR, "\n PASS (%lu zero samples)\n",
                    (unsigned long)zero_samples);
#else
                fprintf(STDERR, " PASS\n");
#endif
#ifdef PNG_TIME_RFC1123_SUPPORTED
             if (tIME_chunk_present != 0)
                fprintf(STDERR, " tIME = %s\n", tIME_string);
#endif /* TIME_RFC1123 */
            }
         }

         else
         {
            if (verbose == 0 && i != 2)
            {
               fprintf(STDERR, "\n Testing %s:", inname);
#if PNG_DEBUG > 0
               fprintf(STDERR, "\n");
#endif
            }

            if (xfail)
              fprintf(STDERR, " XFAIL\n");
            else
            {
              fprintf(STDERR, " FAIL\n");
              ierror += kerror;
            }
         }
#if defined(PNG_USER_MEM_SUPPORTED) && PNG_DEBUG
         if (allocation_now != current_allocation)
             fprintf(STDERR, "MEMORY ERROR: %d bytes lost\n",
                 current_allocation - allocation_now);

         if (current_allocation != 0)
         {
             memory_infop pinfo = pinformation;

             fprintf(STDERR, "MEMORY ERROR: %d bytes still allocated\n",
                 current_allocation);

             while (pinfo != NULL)
             {
                fprintf(STDERR, " %lu bytes at %p\n",
                    (unsigned long)pinfo->size, pinfo->pointer);
                pinfo = pinfo->next;
             }
          }
#endif
       }
#if defined(PNG_USER_MEM_SUPPORTED) && PNG_DEBUG
       fprintf(STDERR, " Current memory allocation: %10d bytes\n",
           current_allocation);
       fprintf(STDERR, " Maximum memory allocation: %10d bytes\n",
           maximum_allocation);
       fprintf(STDERR, " Total   memory allocation: %10d bytes\n",
           total_allocation);
       fprintf(STDERR, "     Number of allocations: %10d\n",
           num_allocations);
#endif
   }

#ifdef PNGTEST_TIMING
   t_stop = (float)clock();
   t_misc += (t_stop - t_start);
   t_start = t_stop;
   fprintf(STDERR, " CPU time used = %.3f seconds",
       (t_misc+t_decode+t_encode)/(float)CLOCKS_PER_SEC);
   fprintf(STDERR, " (decoding %.3f,\n",
       t_decode/(float)CLOCKS_PER_SEC);
   fprintf(STDERR, "        encoding %.3f ,",
       t_encode/(float)CLOCKS_PER_SEC);
   fprintf(STDERR, " other %.3f seconds)\n\n",
       t_misc/(float)CLOCKS_PER_SEC);
#endif

   if (ierror == 0)
      fprintf(STDERR, " libpng passes test\n");

   else
      fprintf(STDERR, " libpng FAILS test\n");

   dummy_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
   fprintf(STDERR, " Default limits:\n");
   fprintf(STDERR, "  width_max  = %lu\n",
       (unsigned long) png_get_user_width_max(dummy_ptr));
   fprintf(STDERR, "  height_max = %lu\n",
       (unsigned long) png_get_user_height_max(dummy_ptr));
   if (png_get_chunk_cache_max(dummy_ptr) == 0)
      fprintf(STDERR, "  cache_max  = unlimited\n");
   else
      fprintf(STDERR, "  cache_max  = %lu\n",
          (unsigned long) png_get_chunk_cache_max(dummy_ptr));
   if (png_get_chunk_malloc_max(dummy_ptr) == 0)
      fprintf(STDERR, "  malloc_max = unlimited\n");
   else
      fprintf(STDERR, "  malloc_max = %lu\n",
          (unsigned long) png_get_chunk_malloc_max(dummy_ptr));
   png_destroy_read_struct(&dummy_ptr, NULL, NULL);

   return (int)(ierror != 0);
}
#else
int
main(void)
{
   fprintf(STDERR,
       " test ignored because libpng was not built with read support\n");
   /* And skip this test */
   return PNG_LIBPNG_VER < 10600 ? 0 : 77;
}
#endif

/* Generate a compiler error if there is an old png.h in the search path. */
typedef png_libpng_version_1_6_34 Your_png_h_is_not_version_1_6_34;
