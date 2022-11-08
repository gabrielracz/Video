#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <memory.h>
#include <fcntl.h>   // O_RDONLY
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <linux/videodev2.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/poll.h>

const char* dev_name = "/dev/video0";
static int vfd;

static int w = 480;
static int h = 680;
static bool default_format = false;
static int sensor_format = V4L2_PIX_FMT_MJPEG;
static int frame_count = 1000000;

struct buffer {
	void* start;
	size_t length;
};
struct buffer* buffers;
int num_buffers = 6;

FILE* f_out;

int process_image(void* buf, size_t size) {
	if(buf) {
		fwrite(buf, size, 1, f_out);
	}
	fprintf(stderr, "#");
	fflush(stderr);
	return 0;
}

int read_frame() {
	struct v4l2_buffer buf;
	memset(&buf, 0, sizeof(buf));

	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_USERPTR;
	if( ioctl(vfd, VIDIOC_DQBUF, &buf) == -1) {
		switch(errno) {
			case EAGAIN:
				return 0;
			case EIO:
				// internal error
				break;
			default:
				perror("dequeue buffer on read frame");
		}
	}
	process_image((void*)buf.m.userptr, buf.bytesused);

	if( ioctl(vfd, VIDIOC_QBUF, &buf) == -1 ) {
		perror("queueing buffer in read frame");
		exit(-1);
	}
	return 1;
}

int capture_loop() {
	int rc;
	for(int cnt = 0; cnt < frame_count; cnt++){
		for(;;) {
			struct pollfd poller = {0};
			poller.fd = vfd;
			poller.events = POLLIN;
			rc = poll(&poller, 1, 2000);
			if( rc > 0) {
				if( read_frame() )
					break;
			}
		}
	}
	return 0;
}

int stop_capture() {
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if( ioctl(vfd, VIDIOC_STREAMOFF, &type) == -1) {
		perror("stream off");
		exit(-1);
	}
	return 0;
}

int start_capture() 
{
	for(int i = 0; i < num_buffers; i++) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));

		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_USERPTR; 
		buf.index = i;
		buf.m.userptr = (unsigned long)buffers[i].start;
		buf.length = buffers[i].length;

		if( ioctl(vfd, VIDIOC_QBUF, &buf) == -1) {
			perror("setting v4l2 buffer");
			exit(-1);
		}
	}

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if( ioctl(vfd, VIDIOC_STREAMON, &type) == -1 ) {
		perror("stream on");
		exit(-1);
	}

	return 0;
}

int init_userptr(int size) 
{
	struct v4l2_requestbuffers req;
	memset(&req, 0, sizeof(req));

	req.count = num_buffers; // quintuple buffering
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_USERPTR;

	if( ioctl(vfd, VIDIOC_REQBUFS, &req) == -1 ) {
		if(errno == EINVAL) {
			perror("userptr i/o not supported");
			exit(-1);
		} else {
			perror("request buffers init");
		}
	}

	/*  Allocate  */
	buffers = calloc(num_buffers, sizeof(*buffers));
	if(buffers == NULL) {
		perror("allocating buffer pointers");
		exit(-1);
	}

	for(int i = 0; i < num_buffers; i++) {
		buffers[i].length = size;
		buffers[i].start = malloc(size);
		if(buffers[i].start == NULL) {
			perror("allocating buffers");
			exit(-1);
		}
	}
	return 0;
}

int init_device() 
{
	int rc;
	struct v4l2_cropcap cropcap;
	struct v4l2_crop crop;
	memset(&cropcap, 0, sizeof(cropcap));
	
	/*  Cropping  */
	cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	rc = ioctl(vfd, VIDIOC_CROPCAP, &cropcap);
	if(rc == 0) {
		crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		crop.c = cropcap.defrect;
		if( ioctl(vfd, VIDIOC_S_CROP, &crop) == -1 ) {
			if(errno == EINVAL)
				perror("cropping not supported");
		}
	}

	/*  Format  */
	struct v4l2_format format;
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if(!default_format) {
		format.fmt.pix.width		= w;
		format.fmt.pix.height		= h;
		format.fmt.pix.pixelformat	= sensor_format;
		format.fmt.pix.field		= V4L2_FIELD_INTERLACED;
		if( ioctl(vfd, VIDIOC_S_FMT, &format) < 0 ) {
			perror("setting format");
		}
	}else {
		if( ioctl(vfd, VIDIOC_G_FMT, &format) < 0 ) {
			perror("getting format");
		}
	}

	init_userptr(format.fmt.pix.sizeimage);
	return 0;
}

int check_device() 
{
	int rc;
	struct v4l2_capability cap;
	rc = ioctl(vfd, VIDIOC_QUERYCAP, &cap);
	if(rc < 0) {
		if(errno == EINVAL)
			perror("invalid v4l2 device");
		else
			perror("query video capabilities");
	}

	rc = cap.capabilities & V4L2_CAP_VIDEO_CAPTURE;
	if(rc == 0) {
		perror("device not capable of video capture");
	}

	//userptr approach
	rc = cap.capabilities * V4L2_CAP_STREAMING;
	if(rc == 0) {
		perror("device does not support streaming i/o");
	}
	return 0;
}

int open_device() 
{
	vfd = open(dev_name, O_RDONLY | O_NONBLOCK, 0);
	if(vfd < 0) {
		perror("open video device");
		exit(-1);
	}
	return 0;
}

int main(int argc, char* argv[]) 
{
	if(argc < 2) {
		f_out = stdout;
	}else {
		f_out = fopen(argv[1], "a");
		if(f_out == NULL) {
			perror("open out file");
			return -1;
		}
	}

	open_device();
	check_device();
	init_device();
	start_capture();
	capture_loop();
	stop_capture();
	return 0;
}
