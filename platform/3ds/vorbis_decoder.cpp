// 3DS sound decoders (replace sndfile/mpg123). Subsystems sheet: decoder; Audio sheet: sfx_ogg, sfx_wav, music_ogg.
// OGG via tremor (libvorbisidec), WAV via a minimal RIFF parser. Both plug into SoundRenderer::CreateDecoder.
#include <string.h>
#include <tremor/ivorbisfile.h>

#include "doomtype.h"
#include "files.h"
#include "i_soundinternal.h"

// ---- OGG Vorbis (tremor) --------------------------------------------------------

class VorbisDecoder : public SoundDecoder
{
public:
	VorbisDecoder() : Reader(NULL), Opened(false) {}
	~VorbisDecoder()
	{
		if (Opened)
			ov_clear(&File);
	}
	void getInfo(int *samplerate, ChannelConfig *chans, SampleType *type)
	{
		vorbis_info *vi = ov_info(&File, -1);
		*samplerate = vi->rate;
		*chans = vi->channels == 1 ? ChannelConfig_Mono : ChannelConfig_Stereo;
		*type = SampleType_Int16;
	}
	size_t read(char *buffer, size_t bytes)
	{
		size_t total = 0;
		while (total < bytes)
		{
			int section;
			long got = ov_read(&File, buffer + total, (int)(bytes - total), &section);
			if (got <= 0)
				break;
			total += got;
		}
		return total;
	}
	bool seek(size_t ms_offset)
	{
		return ov_time_seek(&File, (ogg_int64_t)ms_offset) == 0;
	}
	size_t getSampleOffset()
	{
		return (size_t)ov_pcm_tell(&File);
	}
	size_t getSampleLength()
	{
		ogg_int64_t len = ov_pcm_total(&File, -1);
		return len > 0 ? (size_t)len : 0;
	}

protected:
	bool open(FileReader *reader)
	{
		char magic[4];
		long start = reader->Tell();
		if (reader->Read(magic, 4) != 4 || memcmp(magic, "OggS", 4) != 0)
			return false;
		reader->Seek(start, SEEK_SET);
		Reader = reader;
		ov_callbacks cb = { ReadCB, SeekCB, CloseCB, TellCB };
		if (ov_open_callbacks(this, &File, NULL, 0, cb) != 0)
			return false;
		Opened = true;
		vorbis_info *vi = ov_info(&File, -1);
		return vi != NULL && (vi->channels == 1 || vi->channels == 2);
	}

private:
	static size_t ReadCB(void *ptr, size_t size, size_t nmemb, void *src)
	{
		VorbisDecoder *self = (VorbisDecoder *)src;
		long got = self->Reader->Read(ptr, (long)(size * nmemb));
		return got > 0 ? got / size : 0;
	}
	static int SeekCB(void *src, ogg_int64_t offset, int whence)
	{
		VorbisDecoder *self = (VorbisDecoder *)src;
		return self->Reader->Seek((long)offset, whence) == 0 ? 0 : -1;
	}
	static int CloseCB(void *src)
	{
		return 0;
	}
	static long TellCB(void *src)
	{
		return ((VorbisDecoder *)src)->Reader->Tell();
	}

	FileReader *Reader;
	OggVorbis_File File;
	bool Opened;
};

// ---- WAV (PCM 8/16-bit, mono/stereo) ----------------------------------------------

class WavDecoder : public SoundDecoder
{
public:
	WavDecoder() : Reader(NULL), DataStart(0), DataLen(0), Pos(0), Rate(0), Channels(0), Bits(0) {}
	void getInfo(int *samplerate, ChannelConfig *chans, SampleType *type)
	{
		*samplerate = Rate;
		*chans = Channels == 1 ? ChannelConfig_Mono : ChannelConfig_Stereo;
		*type = Bits == 8 ? SampleType_UInt8 : SampleType_Int16;
	}
	size_t read(char *buffer, size_t bytes)
	{
		size_t left = DataLen - Pos;
		if (bytes > left) bytes = left;
		Reader->Seek(DataStart + Pos, SEEK_SET);
		long got = Reader->Read(buffer, (long)bytes);
		if (got <= 0) return 0;
		Pos += got;
		return got;
	}
	bool seek(size_t ms_offset)
	{
		size_t frame = Channels * Bits / 8;
		size_t p = (size_t)((unsigned long long)ms_offset * Rate / 1000) * frame;
		if (p > DataLen) return false;
		Pos = p;
		return true;
	}
	size_t getSampleOffset() { return Pos / (Channels * Bits / 8); }
	size_t getSampleLength() { return DataLen / (Channels * Bits / 8); }

protected:
	bool open(FileReader *reader)
	{
		Reader = reader;
		long base = reader->Tell();
		char hdr[12];
		if (reader->Read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
			return false;
		bool gotfmt = false;
		for (;;)
		{
			char id[4]; DWORD len;
			if (reader->Read(id, 4) != 4 || reader->Read(&len, 4) != 4)
				return false;
			len = LittleLong(len);
			long next = reader->Tell() + len + (len & 1);
			if (memcmp(id, "fmt ", 4) == 0 && len >= 16)
			{
				WORD format, chans, bits, align; DWORD rate, bps;
				reader->Read(&format, 2); reader->Read(&chans, 2); reader->Read(&rate, 4);
				reader->Read(&bps, 4); reader->Read(&align, 2); reader->Read(&bits, 2);
				if (LittleShort(format) != 1)
					return false;	// PCM only
				Channels = LittleShort(chans); Rate = LittleLong(rate); Bits = LittleShort(bits);
				if ((Channels != 1 && Channels != 2) || (Bits != 8 && Bits != 16))
					return false;
				gotfmt = true;
			}
			else if (memcmp(id, "data", 4) == 0)
			{
				if (!gotfmt)
					return false;
				DataStart = reader->Tell();
				DataLen = len;
				Pos = 0;
				(void)base;
				return true;
			}
			reader->Seek(next, SEEK_SET);
		}
	}

private:
	FileReader *Reader;
	long DataStart;
	size_t DataLen, Pos;
	int Rate, Channels, Bits;
};

SoundDecoder *I3DS_CreateVorbisDecoder()
{
	return new VorbisDecoder;
}

SoundDecoder *I3DS_CreateWavDecoder()
{
	return new WavDecoder;
}
