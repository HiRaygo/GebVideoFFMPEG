// AForge FFMPEG Library
// AForge.NET framework
// http://www.aforgenet.com/framework/
//
// Copyright ?AForge.NET, 2009-2012
// contacts@aforgenet.com
//

#include "StdAfx.h"
#include "VideoFileReader.h"

namespace libffmpeg
{
	extern "C"
	{
		// disable warnings about badly formed documentation from FFmpeg, which we don't need at all
		#pragma warning(disable:4635) 
		// disable warning about conversion int64 to int32
		#pragma warning(disable:4244) 

		#include "libavformat\avformat.h"
		#include "libavformat\avio.h"
		#include "libavcodec\avcodec.h"
		#include "libswscale\swscale.h"
	}
}

namespace Geb { namespace Video { namespace FFMPEG
{
#pragma region Some private FFmpeg related stuff hidden out of header file

// A structure to encapsulate all FFMPEG related private variable
public ref struct VideoContext
{
public:
	libffmpeg::AVStream*			VideoStream;
	libffmpeg::AVCodecContext*		VideoCodecContext;
	libffmpeg::AVFrame*				VideoFrame;
	struct libffmpeg::SwsContext*	ConvertContext;
	int BytesRemaining;

	VideoContext( )
	{
		VideoStream       = NULL;
		VideoCodecContext      = NULL;
		VideoFrame        = NULL;
		ConvertContext	  = NULL;
		BytesRemaining = 0;
	}
};

public ref struct AudioContext
{
public:
	libffmpeg::AVStream*			AudioStream;
	short*							AudioFrame;
	libffmpeg::AVCodecContext*		AudioCodecContext;
	int BytesRemaining;

	AudioContext()
	{
		AudioStream		  = NULL;
		AudioCodecContext = NULL;
		AudioFrame = (short*)malloc(192000*100);
		BytesRemaining = 0;
	}

	property int Channels 
	{
		int get()
		{
			return AudioCodecContext->channels;
		}
	}

	property int SampleRate 
	{
		int get()
		{
			return AudioCodecContext->sample_rate;
		}
	}

	property int SampleSize 
	{
		int get()
		{
			switch (AudioCodecContext->sample_fmt)
                {
					case 0:
                        return 8;
                    case 1:
                        return 16;
                    case 2:
                        return 24;
                    case 3:
                        return 32;
                    default:
                        throw gcnew Exception("Unknown sample size.");
                }
		}
	}
};

public ref struct VideoFileContext
{
public:
	libffmpeg::AVFormatContext* FormatContext;

	Queue<IntPtr>^ VideoPackets; 
	Queue<IntPtr>^ AudioPackets;

	int videoStreamIndex;
	int audioStreamIndex;
	int minFrameCacheCount;

	void ClearQueue()
	{
		IntPtr p = IntPtr::Zero;

		while(this->VideoPackets->Count > 0)
		{
			p = this->VideoPackets->Dequeue();
			libffmpeg::AVPacket* packet = (libffmpeg::AVPacket*)(void*)p;
			ClearPacket(packet);
		}

		while(this->AudioPackets->Count > 0)
		{
			p = this->AudioPackets->Dequeue();
			libffmpeg::AVPacket* packet = (libffmpeg::AVPacket*)(void*)p;
			ClearPacket(packet);
		}
	}

	void ClearPacket(libffmpeg::AVPacket* packet)
	{
		if(packet == NULL) return;
		if(packet->data != NULL) libffmpeg::av_free_packet( packet );
		delete packet;
	}

	libffmpeg::AVPacket* NextVideoPacket()
	{
		IntPtr p = IntPtr::Zero;

		while(this->VideoPackets->Count <= this->minFrameCacheCount)
		{
			if(this->ReadNextPacket() == false) break;
		}

		if(this->VideoPackets->Count > 0)
		{
			p = this->VideoPackets->Dequeue();
		}
		else
		{
			while(this->ReadNextPacket() == true)
			{
				if(this->VideoPackets->Count > 0)
				{
					p = this->VideoPackets->Dequeue();
					break;
				}
			}
		}
		
		if(p == IntPtr::Zero) return NULL;
		else return (libffmpeg::AVPacket*)(void*)p;
	}

	libffmpeg::AVPacket* NextAudioPacket(bool onlyReadCacheQuque)
	{
		IntPtr p = IntPtr::Zero;
		if(this->AudioPackets->Count > 0)
		{
			p = this->AudioPackets->Dequeue();
		}
		else if(onlyReadCacheQuque == false)
		{
			while(this->ReadNextPacket() == true)
			{
				if(this->AudioPackets->Count > 0)
				{
					p = this->AudioPackets->Dequeue();
					break;
				}
			}
		}
		
		if(p == IntPtr::Zero) return NULL;
		else return (libffmpeg::AVPacket*)(void*)p;
	}

	bool ReadNextPacket()
	{
		libffmpeg::AVPacket* packet = new libffmpeg::AVPacket( );
		packet->data = NULL;
		int rtn = libffmpeg::av_read_frame( FormatContext, packet );
		if(packet ->data != NULL)
		{
			if(packet->stream_index == videoStreamIndex)
			{
				VideoPackets->Enqueue((IntPtr)packet);
			}
			else if(packet->stream_index == audioStreamIndex)
			{
				AudioPackets->Enqueue((IntPtr)packet);
			}
			else
			{
				libffmpeg::av_free_packet( packet );
				delete packet;
			}
		}
		else
		{
			delete packet;
		}
		return rtn >= 0;
	}

	VideoFileContext()
	{
		FormatContext = NULL;
		VideoPackets = gcnew Queue<IntPtr>();
		AudioPackets = gcnew Queue<IntPtr>();
		minFrameCacheCount = 30;
	}
};


#pragma endregion

// Class constructor
VideoFileReader::VideoFileReader( void ) :
    videoContext( nullptr ), audioContext(nullptr), disposed( false )
{	
	libffmpeg::av_register_all( );
	m_audioBuff = new byte[AVCODEC_MAX_AUDIO_FRAME_SIZE];
}

#pragma managed(push, off)
static libffmpeg::AVFormatContext* open_file( char* fileName )
{
	libffmpeg::AVFormatContext* formatContext;

	if ( libffmpeg::av_open_input_file( &formatContext, fileName, NULL, 0, NULL ) !=0 )
	{
		return NULL;
	}
	return formatContext;
}
#pragma managed(pop)

// Opens the specified video file
void VideoFileReader::Open( String^ fileName )
{
    CheckIfDisposed( );

	// close previous file if any was open
	Close( );

	videoContext = gcnew VideoContext( );
	audioContext = gcnew AudioContext();

	cxt = gcnew VideoFileContext();

	bool success = false;

	// convert specified managed String to UTF8 unmanaged string
	IntPtr ptr = System::Runtime::InteropServices::Marshal::StringToHGlobalUni( fileName );
    wchar_t* nativeFileNameUnicode = (wchar_t*) ptr.ToPointer( );
    int utf8StringSize = WideCharToMultiByte( CP_UTF8, 0, nativeFileNameUnicode, -1, NULL, 0, NULL, NULL );
    char* nativeFileName = new char[utf8StringSize];
    WideCharToMultiByte( CP_UTF8, 0, nativeFileNameUnicode, -1, nativeFileName, utf8StringSize, NULL, NULL );

	try
	{
		// open the specified video file
		cxt->FormatContext = open_file( nativeFileName );

		if ( cxt->FormatContext == NULL )
		{
			throw gcnew System::IO::IOException( "Cannot open the video file." );
		}
		// retrieve stream information
		if ( libffmpeg::av_find_stream_info( cxt->FormatContext ) < 0 )
		{
			throw gcnew Exception( "Cannot find stream information." );
		}

		// search for the first video stream
		for ( unsigned int i = 0; i < cxt->FormatContext->nb_streams; i++ )
		{
			libffmpeg::AVStream* s = cxt->FormatContext->streams[i];
			if( s->codec->codec_type == libffmpeg::AVMEDIA_TYPE_VIDEO )
			{
				// get the pointer to the codec context for the video stream
				videoContext->VideoCodecContext = s->codec;
				videoContext->VideoStream  = s;
				cxt->videoStreamIndex = s->index;
			}
			else if(s->codec->codec_type == libffmpeg::AVMEDIA_TYPE_AUDIO)
			{
				audioContext->AudioCodecContext = s->codec;
				audioContext->AudioStream  = s;
				cxt->audioStreamIndex = s->index;
			}
		}

		if ( videoContext->VideoStream == NULL )
		{
			throw gcnew Exception( "Cannot find video stream in the specified file." );
		}
		
		// find decoder for the video stream
		libffmpeg::AVCodec* codec = libffmpeg::avcodec_find_decoder( videoContext->VideoCodecContext->codec_id );
		if ( codec == NULL )
		{
			throw gcnew Exception( "Cannot find codec to decode the video stream." );
		}

		// open the codec
		if ( libffmpeg::avcodec_open( videoContext->VideoCodecContext, codec ) < 0 )
		{
			throw gcnew Exception( "Cannot open video codec." );
		}

		if(audioContext->AudioCodecContext != NULL)
		{
			libffmpeg::AVCodec* audioCodec = libffmpeg::avcodec_find_decoder(audioContext->AudioCodecContext->codec_id);
			if( audioCodec == NULL )
			{
				throw gcnew Exception( "Cannot find codec to decode the audio stream." );
			}

			if ( libffmpeg::avcodec_open( audioContext->AudioCodecContext, audioCodec ) < 0 )
			{
				throw gcnew Exception( "Cannot open audio codec." );
			}
		}

		// allocate video frame
		videoContext->VideoFrame = libffmpeg::avcodec_alloc_frame( );

		// prepare scaling context to convert RGB image to video format
		videoContext->ConvertContext = libffmpeg::sws_getContext( videoContext->VideoCodecContext->width, videoContext->VideoCodecContext->height, videoContext->VideoCodecContext->pix_fmt,
				videoContext->VideoCodecContext->width, videoContext->VideoCodecContext->height, libffmpeg::PIX_FMT_BGR24,
				SWS_BICUBIC, NULL, NULL, NULL );

		if ( videoContext->ConvertContext == NULL )
		{
			throw gcnew Exception( "Cannot initialize frames conversion context." );
		}

		// get some properties of the video file
		m_width  = videoContext->VideoCodecContext->width;
		m_height = videoContext->VideoCodecContext->height;
		m_frameRate = videoContext->VideoStream->r_frame_rate.num / videoContext->VideoStream->r_frame_rate.den;
		m_codecName = gcnew String( videoContext->VideoCodecContext->codec->name );
		m_framesCount = videoContext->VideoStream->nb_frames;
		success = true;
	}
	finally
	{
		System::Runtime::InteropServices::Marshal::FreeHGlobal( ptr );
        delete [] nativeFileName;

		if ( !success )
		{
			Close( );
		}
	}
}

// Close current video file
void VideoFileReader::Close(  )
{
	if ( audioContext != nullptr )
	{
		if ( audioContext->AudioFrame != NULL )
		{
			delete audioContext->AudioFrame;
		}

		if ( audioContext->AudioCodecContext != NULL )
		{
			libffmpeg::avcodec_close( audioContext->AudioCodecContext );
		}

		audioContext = nullptr;
	}

	if ( videoContext != nullptr )
	{
		if ( videoContext->VideoFrame != NULL )
		{
			libffmpeg::av_free( videoContext->VideoFrame );
		}

		if ( videoContext->VideoCodecContext != NULL )
		{
			libffmpeg::avcodec_close( videoContext->VideoCodecContext );
		}

		if ( videoContext->ConvertContext != NULL )
		{
			libffmpeg::sws_freeContext( videoContext->ConvertContext );
		}

		videoContext = nullptr;
	}

	if ( cxt != nullptr )
	{
		cxt->ClearQueue();
		if ( cxt->FormatContext != NULL )
		{
			libffmpeg::av_close_input_file( cxt->FormatContext );
		}
		cxt = nullptr;
	}
}

// Read next video frame of the current video file
ImageRgb24^ VideoFileReader::ReadVideoFrame(  )
{
    CheckIfDisposed( );

	if ( videoContext == nullptr )
	{
		throw gcnew System::IO::IOException( "Cannot read video frames since video file is not open." );
	}

	int frameFinished;
	videoContext->BytesRemaining = 0;
	int bytesDecoded;
	bool exit = false;
	libffmpeg::AVPacket* packet = NULL;

	while ( true )
	{
		// ��ȡ��һ����Ƶpacket
		cxt->ClearPacket(packet);
		packet = cxt->NextVideoPacket();
		if(packet == NULL) break;

		// ���� packet
		videoContext->BytesRemaining = packet->size;
		while (videoContext->BytesRemaining > 0 )
		{
			bytesDecoded = libffmpeg::avcodec_decode_video2( videoContext->VideoCodecContext, videoContext->VideoFrame, &frameFinished, packet );

			if ( bytesDecoded < 0 )
				throw gcnew Exception( "Error while decoding frame." );

			videoContext->BytesRemaining -= bytesDecoded;
			if ( frameFinished )
			{
				cxt->ClearPacket(packet);
				return DecodeVideoFrame( );
			}
		}
	}

	cxt->ClearPacket(packet);
	return nullptr;
}

// Read next video frame of the current video file
array<Byte>^ VideoFileReader::ReadAudioFrame(  bool onlyCurrentVideoFrame  )
{
    CheckIfDisposed( );

	if ( audioContext == nullptr )
	{
		throw gcnew System::IO::IOException( "Cannot read video frames since video file is not open." );
	}

	int frameFinished;
	audioContext->BytesRemaining = 0;
	int bytesDecoded;
	bool exit = false;
	libffmpeg::AVPacket* packet = NULL;

	while ( true )
	{
		// ��ȡ��һ����Ƶpacket
		cxt->ClearPacket(packet);
		packet = cxt->NextAudioPacket(onlyCurrentVideoFrame);
		if(packet == NULL) break;

		// ���� packet
		audioContext->BytesRemaining = packet->size;
		while (audioContext->BytesRemaining > 0 )
		{
			bytesDecoded = libffmpeg::avcodec_decode_audio3( audioContext->AudioCodecContext, audioContext->AudioFrame, &frameFinished, packet );

			if ( bytesDecoded < 0 )
				throw gcnew Exception( "Error while decoding frame." );

			audioContext->BytesRemaining -= bytesDecoded;
			if ( frameFinished > 0 )
			{
				cxt->ClearPacket(packet);
				return DecodeAudioFrame( frameFinished );
			}
		}
	}

	cxt->ClearPacket(packet);
	return nullptr;
}

int VideoFileReader::Seek(long long frameIndex, Boolean seekKeyFrame)
{
	if(videoContext == nullptr || videoContext->VideoCodecContext == NULL) return -1;
	libffmpeg::AVCodecContext* pCodecCtx = videoContext->VideoCodecContext;
	if(frameIndex < 0 || frameIndex >= this->FrameCount) return -1;
	long long timeBase = (long long(pCodecCtx->time_base.num) * AV_TIME_BASE) / long long(pCodecCtx->time_base.den);
	long long seekTarget = long long(frameIndex) * timeBase;
	int val = libffmpeg::av_seek_frame(cxt->FormatContext, -1, seekTarget, seekKeyFrame ? AVSEEK_FLAG_FRAME : AVSEEK_FLAG_ANY);
	libffmpeg::	avcodec_flush_buffers(pCodecCtx);
	cxt->ClearQueue();
	return val;
}

// Decodes video frame into managed Bitmap
ImageRgb24^ VideoFileReader::DecodeVideoFrame( )
{
	ImageRgb24^ img = gcnew ImageRgb24( videoContext->VideoCodecContext->width, videoContext->VideoCodecContext->height);
	
	libffmpeg::uint8_t* ptr = reinterpret_cast<libffmpeg::uint8_t*>( static_cast<void*>( img->Start ) );

	libffmpeg::uint8_t* srcData[4] = { ptr, NULL, NULL, NULL };
	int srcLinesize[4] = { img->Width * sizeof(Rgb24), 0, 0, 0 };

	// convert video frame to the RGB bitmap
	libffmpeg::sws_scale( videoContext->ConvertContext, videoContext->VideoFrame->data, videoContext->VideoFrame->linesize, 0,
		videoContext->VideoCodecContext->height, srcData, srcLinesize );

	return img;
}

array<Byte>^ VideoFileReader::DecodeAudioFrame( int length )
{
	if(length <= 0) return nullptr;
	array<Byte>^ buff = gcnew array<Byte>( length );
	pin_ptr<unsigned char> ptr = &buff[0];
	memcpy(ptr,audioContext->AudioFrame, length);
	return buff;
}

} } }