//============================================================================//
// Terminal Media Viewer
//============================================================================//

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Licence
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

/*
MIT License

Copyright (c) 2020 Kai Kitagawa-Jones

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Includes
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

#define  _POSIX_C_SOURCE 200809L

//-------- standard libraries ------------------------------------------------//

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <argp.h>

//-------- POSIX libraries ---------------------------------------------------//

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include<sys/wait.h>

//-------- ffmpeg ------------------------------------------------------------//

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

//-------- external libraries ------------------------------------------------//

// reading images <https://github.com/nothings/stb>
#define STB_IMAGE_IMPLEMENTATION
#include "include/stb_image.h"

// audio <https://github.com/dr-soft/miniaudio>
#define CUTE_SOUND_FORCE_SDL
#define CUTE_SOUND_IMPLEMENTATION
#include <include/cute_sound.h>

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Image / Video format lists
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

#include "vFormatList.h"
char vFormats[][25];

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Defines
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

#define DEFAULT_FPS 15
#define TMP_FOLDER "/tmp/tmv"

// number of samples to take when scaling (bigger -> better but slow)
// 1 = nearest neighbor
#define SCALE 5

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Types
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

typedef struct VideoInfo
{
	int width;
	int height;
	int fps;
	int frameCount;
}VideoInfo;

typedef struct Pixel
{
	int r;
    int g;
    int b;
}Pixel;

typedef struct Image
{
	int width;
	int height;
	Pixel *pixels;
}Image;

void freeImage(Image image)
{
	free(image.pixels);
}

Image copyImage(Image image)
{
	Image newImage;
	newImage.width = image.width;
	newImage.height = image.height;

	newImage.pixels = malloc((image.width * image.height) * sizeof(Pixel));

	for(int i = 0; i < image.height; i++)
	{
		for(int j = 0; j < image.width; j++)
		{
			int index = i * image.width + j;
			newImage.pixels[index].r = image.pixels[index].r;
			newImage.pixels[index].g = image.pixels[index].g;
			newImage.pixels[index].b = image.pixels[index].b;
		}
	}
	return(newImage);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Debug
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

int debug(const char *FUNC, const char *FMT, ...)
{
	#ifdef DEBUG
		char msg[4096];
	    va_list args;
	    va_start(args, FMT);
	    vsnprintf(msg, sizeof(msg), FMT, args);
		printf(
			"\e[33mDEBUG\e[39m(\e[32m%s\e[39m) %s\n"
			"\e[90m[press ENTER to continue...]\e[39m",
			FUNC, msg
		);
		getchar();
	    va_end(args);
	    return 0;
	#else
		return 1;
	#endif
}

void error(const char *FUNC, const char *FMT, ...)
{
	char msg[4096];
    va_list args;
    va_start(args, FMT);
    vsnprintf(msg, sizeof(msg), FMT, args);
	printf("\e[31mERROR\e[39m(\e[32m%s\e[39m): %s\n", FUNC, msg);
    va_end(args);
	exit(1);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Argp
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

const char *argp_program_version = "tmv 0.1";

const char *argp_program_bug_address = "<kal390983@gmail.com>";

char doc[] =
	"\nView images and videos without leaving the console.\n"
	"Requires a terminal that supports truecolor and utf-8\n"
	"For more info visit < https://github.com/kal39/TerminalMediaViewer >";

char args_doc[] = "INPUT";

static struct argp_option options[] = {
    {"width", 'w', 0, 0, "Set output width.", 1},
	{"height", 'h', 0, 0, "Set output height.", 1},
	{"fps", 'f', 0, 0, "Set fps. Default 15 fps", 2},
	{"origfps", 'F', 0, 0, "Use original fps from video. Default 15 fps", 2},
	{"no-sound", 's', 0, 0, "disable sound", 3},
	{ 0 }
};

struct args {
	char *input;
	int width;
	int height;
	int fps;
	int fpsFlag;
	int sound;
};

static error_t parse_option(int key, char *arg, struct argp_state *state)
{
	struct args *args = state->input;
	switch(key)
	{
		case ARGP_KEY_ARG:
			if(state->arg_num == 0)
				args->input = arg;
			else
				argp_usage( state );

			break;
		case 'f':
			args->fps = atoi(arg);
			break;
		case 'F':
			args->fpsFlag = 1;
			break;
		case 'w':
			args->width = atoi(arg);
			break;
		case 'h':
			args->height = atoi(arg);
			break;
		case 's':
			args->sound = 0;
			break;
		case ARGP_KEY_END:
			if(args->input == NULL)
				argp_usage( state );
			break;
		default:
			return ARGP_ERR_UNKNOWN;
	}

	return 0;
}

struct argp argp = {
	options,
	parse_option,
	args_doc,
	doc
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Misc
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

float min(float a, float b)
{
	if(a < b) return(a);
	else return(b);
}

float getTime()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	long int time = (long int)((tv.tv_sec) * 1000 + (tv.tv_usec) / 1000);
	return((float)(time % 10000000) / 1000);
}

// get file extension
char *getExtension(const char *TARGET)
{
    char *dot = strrchr(TARGET, '.');
    if(!dot || dot == TARGET) return "";
    else return dot + 1;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Window
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

int getWinWidth()
{
	struct winsize size;
	ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
	return(size.ws_col);
}

int getWinHeight()
{
	struct winsize size;
	ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
	return((size.ws_row) * 2);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Screen
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

void displayImage(Image image)
{
	for(int i = 0; i < image.height - 1; i += 2) // update 2 pixels at once
	{
		for(int j = 0; j < image.width - 1; j++)
		{
			#define pixel1 image.pixels[i * image.width + j]
			#define pixel2 image.pixels[(i + 1) * image.width + j]

			// set foreground and background colors
			printf("\x1b[48;2;%d;%d;%dm", pixel1.r, pixel1.g, pixel1.b);
			printf("\x1b[38;2;%d;%d;%dm", pixel2.r, pixel2.g, pixel2.b);

			printf("▄");

			// reset colors
			printf("\x1b[0m");
		}

		printf("\n");
	}
}

// only updates changed pixels (-> faster than displayImage())
void updateScreen(Image image, Image prevImage)
{
	for(int i = 0; i < image.height - 1; i += 2) // update 2 pixels at once
	{
		for(int j = 0; j < image.width - 1; j++)
		{
			#define cPixel1 image.pixels[i * image.width + j]
			#define cPixel2 image.pixels[(i + 1) * image.width + j]
			#define pPixel1 prevImage.pixels[i * prevImage.width + j]
			#define pPixel2 prevImage.pixels[(i + 1) * prevImage.width + j]

			// draw only if pixel has changed
			if(
				cPixel1.r != pPixel1.r ||
				cPixel1.g != pPixel1.g ||
				cPixel1.b != pPixel1.b ||
				cPixel2.r != pPixel2.r ||
				cPixel2.g != pPixel2.g ||
				cPixel2.b != pPixel2.b
			)
			{
				// move cursor
				printf("\033[%d;%dH", i / 2 + 1, j + 1);

				// set foreground and background colors
				printf("\x1b[48;2;%d;%d;%dm", cPixel1.r, cPixel1.g, cPixel1.b);
				printf("\x1b[38;2;%d;%d;%dm", cPixel2.r, cPixel2.g, cPixel2.b);

				printf("▄");
			}
		}
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Audio
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

void playAudio(const char TARGET[])
{
	
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Image
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

int getImageWidth(const char TARGET[])
{
	int width = -1;
	stbi_load(TARGET, &width, NULL, NULL, 3);
	return(width);
}

int getImageHeight(const char TARGET[])
{
	int height = -1;
	stbi_load(TARGET, NULL, &height, NULL, 3);
	return(height);
}

Image loadImage(const char TARGET[])
{
	Image image;

	unsigned char *imageRaw = stbi_load(
		TARGET, &image.width, &image.height, NULL, 3
	);

	if(imageRaw == NULL)
		error(__func__, "could not open %s (it may be corrupt)", TARGET);

	image.pixels = (Pixel*)malloc((image.width * image.height) * sizeof(Pixel));

	if(image.pixels == NULL)
		error(__func__, "failed to allocate memory for image");

	//debug(__func__, "allocated mempry for image");

	// Convert to "Image" type (easier to use)
	for(int i = 0; i < image.height; i++)
	{
		for(int j = 0; j < image.width; j++)
		{
			image.pixels[i * image.width + j].r
				= imageRaw[i * image.width * 3 + j * 3];
			image.pixels[i * image.width + j].g
				= imageRaw[i * image.width * 3 + j * 3 + 1];
			image.pixels[i * image.width + j].b
				= imageRaw[i * image.width * 3 + j * 3 + 2];
		}
	}

	free(imageRaw);
	return(image);
}

Image scaleImage(Image oldImage, float xZoom, float yZoom)
{
	Image newImage;
	newImage.width = (int)(oldImage.width * xZoom);
	newImage.height = (int)(oldImage.height * yZoom);
	float xPixelWidth = 1 / xZoom;
	float yPixelWidth = 1 / yZoom;

	newImage.pixels
		= (Pixel*)malloc((newImage.width * newImage.height) * sizeof(Pixel));

	if(newImage.pixels == NULL)
		error(__func__, "failed to allocate memory for newImage");

	debug(__func__, "allocated memory for newImage");

	for(int i = 0; i < newImage.height; i++)
	{
		for(int j = 0; j < newImage.width; j++)
		{
			#define pixel newImage.pixels[i * newImage.width + j]
			pixel.r = 0;
			pixel.g = 0;
			pixel.b = 0;
			int count = 0;

			// take the average of all points
			for(float k = 0; k < yPixelWidth; k += yPixelWidth / SCALE)
			{
				for(float l = 0; l < xPixelWidth; l += xPixelWidth / SCALE)
				{
					#define samplePoint oldImage.pixels\
					[(int)(floor(i * yPixelWidth + k) * oldImage.width)\
					 + (int)floor(j * xPixelWidth + l)]

					pixel.r += samplePoint.r;
					pixel.g += samplePoint.g;
					pixel.b += samplePoint.b;
					count++;
				}
			}

			pixel.r = (int)((float)pixel.r / (float)count);
			pixel.g = (int)((float)pixel.g / (float)count);
			pixel.b = (int)((float)pixel.b / (float)count);
		}
	}
	return(newImage);
}

void image(const int WIDTH, const int HEIGHT, const char INPUT[])
{
	debug(__func__, "target image: %s", INPUT);

	Image image = loadImage(INPUT);

	debug(
		__func__, "original image dimensions: %d * %d",
		image.width, image.height
	);

	float xZoom, yZoom;
	if(WIDTH == -1 || HEIGHT == -1)
	{
		xZoom = min(
			(float)getWinWidth() / (float)image.width,
			(float)getWinHeight() / (float)image.height
		);
		yZoom = min(
			(float)getWinWidth() / (float)image.width,
			(float)getWinHeight() / (float)image.height
		);
	}
	else
	{
		xZoom = (float)WIDTH / (float)image.width;
		yZoom = (float)HEIGHT / (float)image.height;
	}

	debug(__func__, "zoom: x: %f, y: %f", xZoom, yZoom);

	displayImage(scaleImage(image, xZoom, yZoom));

	freeImage(image);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Video
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

void playVideo(const VideoInfo INFO, const int SOUND)
{
	debug(__func__, "tmp folder: %s", TMP_FOLDER);

	Image prevImage;
	prevImage.width = INFO.width;
	prevImage.height = INFO.height;

	prevImage.pixels = (Pixel*)malloc((INFO.width * INFO.height) * sizeof(Pixel));

	if(prevImage.pixels == NULL)
		error(__func__, "failed to allocate memory for prevImage");

	debug(__func__, "allocated memory for prevImage");

	// initialize all colors to -1 to force update when calling updateScreen()
	// for the first time
	for(int i = 0; i < INFO.height; i++)
	{
		for(int j = 0; j < INFO.width; j++)
		{
			prevImage.pixels[i * INFO.width + j].r = -1;
			prevImage.pixels[i * INFO.width + j].g = -1;
			prevImage.pixels[i * INFO.width + j].b = -1;
		}
	}

	char audioDir[1000];
	sprintf(audioDir, "%s/audio.wav", TMP_FOLDER);

	debug(
		__func__, "starting audio (%s) and video (%d fps)", audioDir, INFO.fps
	);

	//if(SOUND == 1) playAudio(audioDir);

	float sTime = getTime();
	int currentFrame = 1;
	int prevFrame = 0;

	while(1)
	{
		char file[1000];
		currentFrame = (int)floor(INFO.fps * (getTime() - sTime));
		// frames start from 1
		if(currentFrame < 1)currentFrame = 1;
		sprintf(file, "%s/frame%d.bmp", TMP_FOLDER, currentFrame);

		if(currentFrame != prevFrame) // don't draw same frame twice
		{
			if(access(file, F_OK) != - 1)
			{
				Image currentImage = loadImage(file);
				updateScreen(currentImage, prevImage);
				prevImage = copyImage(currentImage);
				freeImage(currentImage);
				// delete old frames
				remove(file);
			}
			else
			{
				debug(__func__, "next file (%s) not found", file);
				freeImage(prevImage);
				break;
			}
		}
		prevFrame = currentFrame;
	}
	freeImage(prevImage);
}

VideoInfo getVideoInfo(const char TARGET[])
{
	VideoInfo info;

	AVFormatContext *formatCtx = avformat_alloc_context();

	if(formatCtx == NULL)
		error(__func__, "failed to allocate memory for formatCtx");

	debug(__func__, "allocated memory for formatCtx");

	avformat_open_input(&formatCtx, TARGET, NULL, NULL);
	avformat_find_stream_info(formatCtx,  NULL);

	int index = 0;

	for (int i = 0; i < formatCtx->nb_streams; i++)
	{
		AVCodecParameters *codecPram = formatCtx->streams[i]->codecpar;
		if (codecPram->codec_type == AVMEDIA_TYPE_VIDEO) {
			info.width = codecPram->width;
			info.height = codecPram->height;
			index = i;
			break;
		}
	}

	info.fps = formatCtx->streams[index]->r_frame_rate.num;
	info.frameCount = formatCtx->streams[index]->duration;

	debug(
		__func__,
		"got video info: %d * %d, fps = %d, frameCount = %d",
		info.width, info.height, info.fps, info.frameCount
	);

	free(formatCtx);

	return(info);
}

void video(
	const int WIDTH, const int HEIGHT,
	const int FPS, const int FLAG, const char INPUT[], const int SOUND
)
{
	VideoInfo info = getVideoInfo(INPUT);

	float xZoom, yZoom;
	if(WIDTH == -1 || HEIGHT == -1)
	{
		xZoom = min(
			(float)getWinWidth() / (float)info.width,
			(float)getWinHeight() / (float)info.height
		);
		yZoom = min(
			(float)getWinWidth() / (float)info.width,
			(float)getWinHeight() / (float)info.height
		);
	}
	else
	{
		xZoom = (float)WIDTH / (float)info.width;
		yZoom = (float)HEIGHT / (float)info.height;
	}

	debug(__func__, "zoom: x: %f,  y: %f", xZoom, yZoom);

	if(FLAG == 0)
		info.fps = DEFAULT_FPS;

	if(FPS != -1)
		info.fps = FPS;

	char dir[] = TMP_FOLDER;

	debug(__func__, "tmp folder: %s", dir);

	struct stat sb;

	// make /tmp/tmv folder if none exists
	if(stat(dir, &sb) != 0)
	{
		debug(__func__, "could not find tmp folder");
		mkdir(dir, 0700);
		debug(__func__, "created tmp folder: %s", dir);
	}

	// decode video with ffmpeg into audio
	char commandA[1000];
	sprintf(
		commandA,
		"ffmpeg -i %s -f wav %s/audio.wav >>/dev/null 2>>/dev/null",
		INPUT, dir
	);

	// decode video with ffmpeg into bmp files
	char commandB[1000];
	sprintf(
		commandB,
		"ffmpeg -i %s -vf \"fps=%d, scale=%d:%d\" %s/frame%%d.bmp >>/dev/null 2>>/dev/null",
		INPUT, info.fps, (int)(info.width * xZoom), (int)(info.height * yZoom),
		dir
	);

	debug(__func__, "forking");

	// child = plays video, parent = decodes
	int pid = fork();

	if(pid == 0)
	{

		char TARGET[1000];
		sprintf(TARGET, "%s/frame%d.bmp", dir, 1);
		// wait for first image (ffmpeg takes time to start)
		while(access(TARGET, F_OK) == -1){}
		// play the video
		playVideo(info, SOUND);
	}
	else
	{
		// audio first
		system(commandA);
		system(commandB);
		// wait for video to finish
		wait(NULL);
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Cleanup
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

void cleanup()
{
	// move cursor to bottom right and reset colors
	printf("\x1b[0m \033[%d;%dH\n", getWinWidth(), getWinHeight());

	char dirName[] = TMP_FOLDER;

	debug(__func__, "tmp folder: %s", dirName);

	DIR *dir = opendir(dirName);

	if(dir == NULL)
	{
		debug(__func__, "failed to open tmp folder");
		exit(0);
	}

    struct dirent *next_file;
    char filepath[NAME_MAX * 2 + 1];

	int count = 0;

	// delete all images that were left
    while((next_file = readdir(dir)) != NULL)
    {
        sprintf(filepath, "%s/%s", dirName, next_file->d_name);
        remove(filepath);
		count++;
    }
    closedir(dir);

	debug(__func__, "deleted %d images", count);

	exit(0);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Main
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

int main(int argc, char *argv[])
{
	// cleanup on ctr+c
	signal(SIGINT, cleanup);

	// setup argp
	struct args args = {0};

	// default values
	args.fps = -1;
	args.fpsFlag = 0;
	args.width = -1;
	args.height = -1;
	args.sound = 1;

	argp_parse(&argp, argc, argv, 0, 0, &args);

	debug(__func__, "target file: %s", args.input);

	if(access(args.input, F_OK) == -1)
		error(__func__, "%s does not exist", args.input);

	// 1: image, 2: video
	int fileType = 1;

	char *ext = getExtension(args.input);

	debug(__func__, "file extension: %s", ext);

	int i = 0;

	// check if target is video
	while(vFormats[i])
	{
		if(strcmp(ext, vFormats[i]) == 0)
		{
			fileType = 2;
			break;
		}
		else if(strcmp("END", vFormats[i]) == 0)
			break;

		i++;
	}

	int width, height;

	if(args.width != -1)
		width = args.width;
	else
		width = getWinWidth();

	if(args.height != -1)
		height = args.height;
	else
		height = getWinHeight();

	debug(__func__, "display dimentions: %d * %d", width, height);

	if(fileType == 1)
		image(args.width, args.height, args.input);

	else
	{
		video(
			args.width, args.height, args.fps,
			args.fpsFlag, args.input, args.sound
		);
	}

	cleanup();

	return(0);
}
