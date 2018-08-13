#include <opencv2\opencv.hpp>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <png.h>
#include <zlib.h>

using namespace cv;

int write_png_file(char *file_name , Mat srcImg, int imgW, int imgH, int channels)
{
     uchar* pImgData=(uchar*)srcImg.data;
     int j, i, temp, pos;
     png_byte color_type;

     png_structp png_ptr;
     png_infop info_ptr; 
     png_bytep * row_pointers;
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

     if(channels == 4) 
     {
         color_type = PNG_COLOR_TYPE_RGB_ALPHA;
     }
     else if(channels==1)
     {
         color_type = PNG_COLOR_TYPE_GRAY;
     }
     else
     {
         color_type = PNG_COLOR_TYPE_RGB;
     }
     
     png_set_IHDR(png_ptr, info_ptr, imgW, imgH,
      8, color_type, PNG_INTERLACE_NONE,
      PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

     png_write_info(png_ptr, info_ptr);

     /* write bytes */
     if (setjmp(png_jmpbuf(png_ptr)))
     {
        printf("[write_png_file] Error during writing bytes");
        return -1;
     }
     if(channels == 4) 
     {
        temp = (4 * imgW);
     }
     else if(channels == 1)
     {
         temp = ( imgW);
     }
     else
     {
         temp = ( 3*imgW);
     }
 
     row_pointers = (png_bytep*)malloc(imgH*sizeof(png_bytep));
     for(i = 0; i < imgH; i++)
     {
          row_pointers[i] = (png_bytep)malloc(sizeof(uchar)*temp);
          for(j = 0; j < imgW; j += 1)
          {
               if(channels==4) 
               {
                   row_pointers[i][j*3+0]  = pImgData[i*imgW*3+ j*3+0]; // blue
                   row_pointers[i][j*3+1] = pImgData[i*imgW*3+ j*3+1]; // green
                   row_pointers[i][j*3+2] = pImgData[i*imgW*3+ j*3+2];  // red
                   row_pointers[i][j*3+3] = pImgData[i*imgW*3+ j*3+3];  // alpha
               }
               else if(channels==1)
               {
                   row_pointers[i][j]  = pImgData[i*imgW+ j]; // gray
               }
               else
               {
                   row_pointers[i][j*3+0]  = pImgData[i*imgW*3+ j*3+0]; // blue
                   row_pointers[i][j*3+1] = pImgData[i*imgW*3+ j*3+1]; // green
                   row_pointers[i][j*3+2] = pImgData[i*imgW*3+ j*3+2];  // red
               }
          }
     }
     png_write_image(png_ptr, row_pointers);

     /* end write */
     if (setjmp(png_jmpbuf(png_ptr)))
     {
      printf("[write_png_file] Error during end of write");
      return -1;
     }
     png_write_end(png_ptr, NULL);

        /* cleanup heap allocation */
     for (j=0; j<imgH; j++)
     {
        free(row_pointers[j]);
     }
     free(row_pointers);

    fclose(fp);
    return 0;
}

void main()
{
    Mat img=imread("D:\\1_7.png", 0);
    namedWindow("lena");
    imshow("lena",img);
    waitKey(0);
    char imgName[10]="lena.png";
    int imgWidth=img.cols;
    int imgHeight=img.rows;
    int channels=img.channels();
    write_png_file(imgName , img, imgWidth, imgHeight, channels);

    getchar();
}