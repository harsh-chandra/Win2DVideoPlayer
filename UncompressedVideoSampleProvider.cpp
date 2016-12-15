//*****************************************************************************
//
//	Copyright 2015 Microsoft Corporation
//
//	Licensed under the Apache License, Version 2.0 (the "License");
//	you may not use this file except in compliance with the License.
//	You may obtain a copy of the License at
//
//	http ://www.apache.org/licenses/LICENSE-2.0
//
//	Unless required by applicable law or agreed to in writing, software
//	distributed under the License is distributed on an "AS IS" BASIS,
//	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//	See the License for the specific language governing permissions and
//	limitations under the License.
//
//*****************************************************************************

#include "pch.h"
#include "UncompressedVideoSampleProvider.h"

extern "C"
{
#include <libavutil/imgutils.h>
}


using namespace FFmpegInterop;

UncompressedVideoSampleProvider::UncompressedVideoSampleProvider(
	FFmpegReader^ reader,
	AVFormatContext* avFormatCtx,
	AVCodecContext* avCodecCtx)
	: MediaSampleProvider(reader, avFormatCtx, avCodecCtx)
	, m_pAvFrame(nullptr)
	, m_pSwsCtx(nullptr)
{
	for (int i = 0; i < 4; i++)
	{
		m_rgVideoBufferLineSize[i] = 0;
		m_rgVideoBufferData[i] = nullptr;
	}
}

HRESULT UncompressedVideoSampleProvider::AllocateResources()
{
	HRESULT hr = S_OK;
	hr = MediaSampleProvider::AllocateResources();
	if (SUCCEEDED(hr))
	{
		// Setup software scaler to convert any decoder pixel format (e.g. YUV420P) to NV12 that is supported in Windows & Windows Phone MediaElement
		m_pSwsCtx = sws_getContext(
			m_pAvCodecCtx->width,
			m_pAvCodecCtx->height,
			m_pAvCodecCtx->pix_fmt,
			m_pAvCodecCtx->width,
			m_pAvCodecCtx->height,
			AV_PIX_FMT_NV12,
			SWS_BICUBIC,
			NULL,
			NULL,
			NULL);

		if (m_pSwsCtx == nullptr)
		{
			hr = E_OUTOFMEMORY;
		}
	}

	if (SUCCEEDED(hr))
	{
		m_pAvFrame = av_frame_alloc();
		if (m_pAvFrame == nullptr)
		{
			hr = E_OUTOFMEMORY;
		}
	}

	if (SUCCEEDED(hr))
	{
		if (av_image_alloc(m_rgVideoBufferData, m_rgVideoBufferLineSize, m_pAvCodecCtx->width, m_pAvCodecCtx->height, AV_PIX_FMT_NV12, 1) < 0)
		{
			hr = E_FAIL;
		}
	}

	return hr;
}

UncompressedVideoSampleProvider::~UncompressedVideoSampleProvider()
{
	if (m_pAvFrame)
	{
		av_freep(m_pAvFrame);
	}

	if (m_rgVideoBufferData)
	{
		av_freep(m_rgVideoBufferData);
	}
}

HRESULT UncompressedVideoSampleProvider::WriteAVPacketToStream(DataWriter^ dataWriter, AVPacket* avPacket)
{
	// Convert decoded video pixel format to NV12 using FFmpeg software scaler
	if (sws_scale(m_pSwsCtx, (const uint8_t **)(m_pAvFrame->data), m_pAvFrame->linesize, 0, m_pAvCodecCtx->height, m_rgVideoBufferData, m_rgVideoBufferLineSize) < 0)
	{
		return E_FAIL;
	}

	auto YBuffer = ref new Platform::Array<uint8_t>(m_rgVideoBufferData[0], m_rgVideoBufferLineSize[0] * m_pAvCodecCtx->height);
	auto UVBuffer = ref new Platform::Array<uint8_t>(m_rgVideoBufferData[1], m_rgVideoBufferLineSize[1] * m_pAvCodecCtx->height / 2);
	dataWriter->WriteBytes(YBuffer);
	dataWriter->WriteBytes(UVBuffer);
	//
	av_frame_unref(m_pAvFrame);

	return S_OK;
}

HRESULT UncompressedVideoSampleProvider::DecodeAVPacket(DataWriter^ dataWriter, AVPacket* avPacket)
{
	if (avcodec_send_packet(m_pAvCodecCtx, avPacket) < 0)
	{
		DebugMessage(L"DecodeAVPacket Failed\n");
		return S_FALSE;
	}
	else
	{
		if (avcodec_receive_frame(m_pAvCodecCtx, m_pAvFrame) >= 0)
		{
			avPacket->pts = av_frame_get_best_effort_timestamp(m_pAvFrame);
			return S_OK;
		}
	}

	return S_FALSE;
}

AVFrame * UncompressedVideoSampleProvider::GetNextFrame(AVFrame *frameYUV)
{
	// TODO Change this so that it returns the RGB frame.
	uint8_t * res = NULL;
	res = GetRGBAFrame(frameYUV);
	for (int i = 0; i < FRAME_COUNT; i++)
	{
		if (res)
		{
			char filename[MAX_PATH];
			sprintf_s(filename, OUTPUT_FILE_PREFIX, i);
			if (!BMPSave(filename, frameYUV, YUV_frame->width, YUV_frame->height))
			{
				printf("Cannot save file %s\n", filename);
			}
			av_freep(&res[0]);
			av_freep(res);
		}
	}
	return frameYUV;
}


uint8_t * UncompressedVideoSampleProvider::GetRGBAFrame(AVFrame *pFrameYuv)
{
	YUV_frame = pFrameYuv;
	uint8_t * pointers;
	int * lineSizes = pFrameYuv->linesize;
	int width = m_pAvCodecCtx->width;
	int height = m_pAvCodecCtx->height;
	int bufferImgSize = av_image_alloc(&pointers, lineSizes, width, height, AV_PIX_FMT_RGBA, 0);
	//AVFrame *frame = avcodec_alloc_frame();
	uint8_t * buffer = (uint8_t*)av_mallocz(bufferImgSize);
	if (pointers)
	{
		av_image_fill_arrays(&pointers, lineSizes, buffer, AV_PIX_FMT_RGBA, width, height, 0);
		/*frame->width = width;
		frame->height = height;*/
		//frame->data[0] = buffer;

		sws_scale(sws_getContext(m_pAvCodecCtx->width, m_pAvCodecCtx->height, m_pAvCodecCtx->pix_fmt, m_pAvCodecCtx->width,
			m_pAvCodecCtx->height, AV_PIX_FMT_RGBA, SWS_BICUBIC, NULL, NULL, NULL), pFrameYuv->data, pFrameYuv->linesize,
			0, height, &pointers, lineSizes);
	}
	 
	return pointers;
}

bool BMPSave(const char *pFileName, AVFrame * frame, int w, int h)
{
	bool bResult = false;

	FILE *stream;

	if (frame)
	{
		FILE* file = (FILE *)fopen_s(&stream, pFileName, "wb");

		if (file)
		{
			// RGB image
			int imageSizeInBytes = 3 * w * h;
			int headersSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
			int fileSize = headersSize + imageSizeInBytes;

			uint8_t * pData = new uint8_t[headersSize];

			if (pData != NULL)
			{
				BITMAPFILEHEADER& bfHeader = *((BITMAPFILEHEADER *)(pData));

				bfHeader.bfType = 'MB';
				bfHeader.bfSize = fileSize;
				bfHeader.bfOffBits = headersSize;
				bfHeader.bfReserved1 = bfHeader.bfReserved2 = 0;

				BITMAPINFOHEADER& bmiHeader = *((BITMAPINFOHEADER *)(pData + headersSize - sizeof(BITMAPINFOHEADER)));

				bmiHeader.biBitCount = 3 * 8;
				bmiHeader.biWidth = w;
				bmiHeader.biHeight = h;
				bmiHeader.biPlanes = 1;
				bmiHeader.biSize = sizeof(bmiHeader);
				bmiHeader.biCompression = BI_RGB;
				bmiHeader.biClrImportant = bmiHeader.biClrUsed =
					bmiHeader.biSizeImage = bmiHeader.biXPelsPerMeter =
					bmiHeader.biYPelsPerMeter = 0;

				fwrite(pData, headersSize, 1, file);

				uint8_t *pBits = frame->data[0] + frame->linesize[0] * h - frame->linesize[0];
				int nSpan = -frame->linesize[0];

				int numberOfBytesToWrite = 3 * w;

				for (size_t i = 0; i < h; ++i, pBits += nSpan)
				{
					fwrite(pBits, numberOfBytesToWrite, 1, file);
				}

				bResult = true;
				delete[] pData;
			}

			fclose(file);
		}
	}

	return bResult;
}

//
//static void pgm_save(unsigned char *buf, int wrap, int xsize, int ysize,
//	char *filename)
//{
//	FILE *f;
//	int i;
//	f = fopen(filename, "w");
//	fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255);
//	for (i = 0; i < ysize; i++)
//		fwrite(buf + i * wrap, 1, xsize, f);
//	fclose(f);
//}
//static int decode_write_frame(const char *outfilename, AVCodecContext *avctx,
//	AVFrame *frame, int *frame_count, AVPacket *pkt, int last)
//{
//	int len, got_frame;
//	char buf[1024];
//	len = avcodec_decode_video2(avctx, frame, &got_frame, pkt);
//	if (len < 0) {
//		fprintf(stderr, "Error while decoding frame %d\n", *frame_count);
//		return len;
//	}
//	if (got_frame) {
//		printf("Saving %sframe %3d\n", last ? "last " : "", *frame_count);
//		fflush(stdout);
//		/* the picture is allocated by the decoder, no need to free it */
//		snprintf(buf, sizeof(buf), outfilename, *frame_count);
//		pgm_save(frame->data[0], frame->linesize[0],
//			frame->width, frame->height, buf);
//		(*frame_count)++;
//	}
//	if (pkt->data) {
//		pkt->size -= len;
//		pkt->data += len;
//	}
//	return 0;
//}
//static void video_decode_example(const char *outfilename, const char *filename)
//{
//	AVCodec *codec;
//	AVCodecContext *c = NULL;
//	int frame_count;
//	FILE *f;
//	AVFrame *frame;
//	uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
//	AVPacket avpkt;
//	av_init_packet(&avpkt);
//	/* set end of buffer to 0 (this ensures that no overreading happens for damaged mpeg streams) */
//	memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);
//	printf("Decode video file %s to %s\n", filename, outfilename);
//	/* find the mpeg1 video decoder */
//	codec = avcodec_find_decoder(AV_CODEC_ID_MPEG1VIDEO);
//	if (!codec) {
//		fprintf(stderr, "Codec not found\n");
//		exit(1);
//	}
//	c = avcodec_alloc_context3(codec);
//	if (!c) {
//		fprintf(stderr, "Could not allocate video codec context\n");
//		exit(1);
//	}
//	if (codec->capabilities & AV_CODEC_CAP_TRUNCATED)
//		c->flags |= AV_CODEC_FLAG_TRUNCATED; // we do not send complete frames
//											 /* For some codecs, such as msmpeg4 and mpeg4, width and height
//											 MUST be initialized there because this information is not
//											 available in the bitstream. */
//											 /* open it */
//	if (avcodec_open2(c, codec, NULL) < 0) {
//		fprintf(stderr, "Could not open codec\n");
//		exit(1);
//	}
//	f = fopen(filename, "rb");
//	if (!f) {
//		fprintf(stderr, "Could not open %s\n", filename);
//		exit(1);
//	}
//	frame = av_frame_alloc();
//	if (!frame) {
//		fprintf(stderr, "Could not allocate video frame\n");
//		exit(1);
//	}
//	frame_count = 0;
//	for (;;) {
//		avpkt.size = fread(inbuf, 1, INBUF_SIZE, f);
//		if (avpkt.size == 0)
//			break;
//		/* NOTE1: some codecs are stream based (mpegvideo, mpegaudio)
//		and this is the only method to use them because you cannot
//		know the compressed data size before analysing it.
//		BUT some other codecs (msmpeg4, mpeg4) are inherently frame
//		based, so you must call them with all the data for one
//		frame exactly. You must also initialize 'width' and
//		'height' before initializing them. */
//		/* NOTE2: some codecs allow the raw parameters (frame size,
//		sample rate) to be changed at any frame. We handle this, so
//		you should also take care of it */
//		/* here, we use a stream based decoder (mpeg1video), so we
//		feed decoder and see if it could decode a frame */
//		avpkt.data = inbuf;
//		while (avpkt.size > 0)
//			if (decode_write_frame(outfilename, c, frame, &frame_count, &avpkt, 0) < 0)
//				exit(1);
//	}
//	/* some codecs, such as MPEG, transmit the I and P frame with a
//	latency of one frame. You must do the following to have a
//	chance to get the last frame of the video */
//	avpkt.data = NULL;
//	avpkt.size = 0;
//	decode_write_frame(outfilename, c, frame, &frame_count, &avpkt, 1);
//	fclose(f);
//	avcodec_close(c);
//	av_free(c);
//	av_frame_free(&frame);
//	printf("\n");
//}
