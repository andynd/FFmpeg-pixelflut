#include <stdio.h>
#include "libavformat/network.h"
#include "libavutil/imgutils.h"
#include "libavutil/pixdesc.h"
#include "avdevice.h"

typedef struct {
	AVClass *class;
	char* host;
	int port;
	int off_x; /* x offset on pixelflut canvas */
	int off_y; /* y offset on pixelflut canvas */
	int sock;  /* socket for connection to pixelflut host */
	int img_width;
	int img_height;
	int use_udp;
} PixelflutContext;

#define PORT_NUM_LEN_MAX 16
static int pixelflut_write_header(AVFormatContext *s) {
	PixelflutContext *pixelflut = s->priv_data;
	struct addrinfo hints;
	struct addrinfo * result;
	struct addrinfo * rp;
	char port[PORT_NUM_LEN_MAX];
	int gai_s;
	AVCodecParameters *par;

	par = s->streams[0]->codecpar;
	if (   s->nb_streams > 1
	    || par->codec_type != AVMEDIA_TYPE_VIDEO
	    || par->codec_id   != AV_CODEC_ID_RAWVIDEO) {
		av_log(s, AV_LOG_ERROR, "Only supports one rawvideo stream\n");
		return AVERROR(EINVAL);
	}

	if (par->format != AV_PIX_FMT_RGB32) {
		av_log(s, AV_LOG_ERROR, "Pixel format %s is not supported. Must be %s instead.\n",
				av_get_pix_fmt_name(par->format),
				av_get_pix_fmt_name(AV_PIX_FMT_RGB32));
		return AVERROR(EINVAL);
	} /* TODO: work with more pixel formats */
	pixelflut->img_width = par->width;
	pixelflut->img_height = par->height;

	if ( NULL == pixelflut->host ) {
		av_log(s, AV_LOG_ERROR, "pixelflut host not set\n");
		return AVERROR(EINVAL);
	}
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = (pixelflut->use_udp ? SOCK_DGRAM : SOCK_STREAM);
	hints.ai_flags = AI_NUMERICSERV;
	hints.ai_protocol = 0;

	pixelflut->sock = socket(AF_INET6, hints.ai_socktype, 0);
	if (pixelflut->sock == -1) {
		av_log(s, AV_LOG_ERROR, "could not create socket\n");
		return AVERROR(errno);
	}

	snprintf(port, PORT_NUM_LEN_MAX, "%d", pixelflut->port);
	gai_s = getaddrinfo(pixelflut->host, port, &hints, &result);
	if(gai_s != 0) {
		av_log(s, AV_LOG_ERROR, "error in name resolution: %s\n", gai_strerror(gai_s));
		return AVERROR(ESRCH);
	}
	for (rp = result; rp != NULL; rp = rp->ai_next) {
		pixelflut->sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (pixelflut->sock == -1) {
			continue;
		}

		if (connect(pixelflut->sock, rp->ai_addr, rp->ai_addrlen) != -1) {
			break;  /* Success */
		}

		close(pixelflut->sock);
	}
	if (rp == NULL) {
		av_log(s, AV_LOG_ERROR, "could not connect\n");
		return AVERROR(EHOSTUNREACH);
	}
	freeaddrinfo(result);
	return 0;
}

#define PXCMD_MAX_LEN 32
static int pixelflut_send_picture(AVFormatContext *s, uint8_t *data[4], int linesize[4]) {
	PixelflutContext * pixelflut = s->priv_data;
	int x, y;
	uint8_t * line;
	size_t pxcmd_len;
	char pxcmd[PXCMD_MAX_LEN];
	for (y=0; y<pixelflut->img_height; ++y) {
		line = data[0]+4*y*pixelflut->img_width;
		for (x=0; x<pixelflut->img_width; ++x) {
			/* FIXME: Use correct way of accessing image data! */
			pxcmd_len = snprintf(pxcmd, PXCMD_MAX_LEN, "PX %d %d %02X%02X%02X\n",
					x+pixelflut->off_x,
					y+pixelflut->off_y,
					line[4*x+2], line[4*x+1], line[4*x+0]);
			write(pixelflut->sock, pxcmd, pxcmd_len);
		}
	}
	return 0;
}

static int pixelflut_write_packet(AVFormatContext *s, AVPacket *pkt) {
	AVCodecParameters *par = s->streams[0]->codecpar;
	uint8_t *data[4];
	int linesize[4];
	//av_image_fill_arrays(data, linesize, pkt->data, par->format, par->width, par->height, 1);
	av_image_fill_arrays(data, linesize, pkt->data, AV_PIX_FMT_RGB32, par->width, par->height, 1);
	return pixelflut_send_picture(s, data, linesize);
}

static int pixelflut_write_frame(AVFormatContext *s, int stream_index, AVFrame **frame, unsigned flags) {
	return pixelflut_send_picture(s, (*frame)->data, (*frame)->linesize);
}

static int pixelflut_control_message(AVFormatContext *s, int type, void *data, size_t data_size) {
	return AVERROR(ENOSYS); /* we do not implement control messages */
}

static int pixelflut_write_trailer(AVFormatContext *s) {
	PixelflutContext * pixelflut = s->priv_data;
	int status;
	status = close(pixelflut->sock);
	if (status != 0) {
		av_log(s, AV_LOG_ERROR, "error closing socket: %s\n", strerror(errno));
		return AVERROR(errno);
	}
	return 0;
}

#define OFFSET(x) offsetof(PixelflutContext, x)
static const AVOption options[] = {
	{ "host",  "remote host where pixelflut is running",          OFFSET(host),  AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, AV_OPT_FLAG_ENCODING_PARAM },
	{ "port",  "port on the remote host where pixelflut listens", OFFSET(port),  AV_OPT_TYPE_INT, {.i64 = 1234}, 0, 65535, AV_OPT_FLAG_ENCODING_PARAM },
	{ "off_x", "X offset on pixelflut canvas",                    OFFSET(off_x), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM },
	{ "off_y", "Y offset on pixelflut canvas",                    OFFSET(off_y), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM },
	{ "use_udp", "if set, use UDP, otherwise TCP",                OFFSET(use_udp), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, AV_OPT_FLAG_ENCODING_PARAM },
	{ NULL }
};

static const AVClass pixelflut_class = {
	.class_name = "pixelflut outdev",
	.item_name = av_default_item_name,
	.option = options,
	.version = LIBAVUTIL_VERSION_INT,
	.category = AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT,
};

AVOutputFormat ff_pixelflut_muxer = {
	.name = "pixelflut",
	.long_name = NULL_IF_CONFIG_SMALL("pixelflut output device"),
	.priv_data_size = sizeof(PixelflutContext),
	.write_header = pixelflut_write_header,
	.write_packet = pixelflut_write_packet,
	.write_uncoded_frame = pixelflut_write_frame,
	.write_trailer = pixelflut_write_trailer,
	.control_message = pixelflut_control_message,
	.audio_codec = AV_CODEC_ID_NONE,
	.video_codec = AV_CODEC_ID_RAWVIDEO,
	.flags = AVFMT_NOFILE | AVFMT_VARIABLE_FPS | AVFMT_NOTIMESTAMPS,
	.priv_class = &pixelflut_class,
};
