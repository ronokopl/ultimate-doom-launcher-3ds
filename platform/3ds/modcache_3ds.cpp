// On-device mod cache (Subsystems sheet: modcache; rules: ModCache sheet).
// Mods are only known at runtime, so the build-time speedups of Brutal Doom 3DS can't be baked in. Instead,
// the first launch of a mod converts its slow-to-load content with the engine's own decoders and writes it to
// cache/<mod>-<key>.pk3 (stored zip); the launcher then loads that pk3 right after the mod:
//   image_png / image_jpeg:   sprites, textures, patches and graphics become Doom patches built from the
//                             engine texture (pixels + spans): bit-exact with what the renderer made of them
//   sfx_ogg:                  OGG sound effects become the 16-bit mono WAV that ndspsound.cpp builds
// The key covers the mod file, the effective PLAYPAL and the cache format (Build sheet cache_version).
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <zlib.h>
#include <algorithm>
#include <deque>
#include <string>
#include <vector>

#include "doomtype.h"
#include "templates.h"
#include "w_wad.h"
#include "files.h"
#include "textures/textures.h"
#include "s_sound.h"
#include "sound/i_sound.h"
#include "sound/i_musicinterns.h"
#include "c_console.h"
#include "m_argv.h"
#include "bd3ds_sheets.h"
#include "i_3ds.h"

// ---- cache naming (shared with the launcher; no engine state) -----------------------------------------

static bool ReadAt(FILE *f, long pos, void *buf, size_t len)
{
	return fseek(f, pos, SEEK_SET) == 0 && fread(buf, 1, len, f) == len;
}

static unsigned LE32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
static unsigned LE16(const unsigned char *p) { return p[0] | (p[1] << 8); }

// Last PLAYPAL in a WAD or zip/pk3 (root entry), first 768 bytes; false if the file has none.
static bool FindPlaypal(const char *path, unsigned char pal[768])
{
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return false;
	bool found = false;
	unsigned char h[12];
	if (ReadAt(f, 0, h, 12) && (!memcmp(h, "IWAD", 4) || !memcmp(h, "PWAD", 4)))
	{
		unsigned n = LE32(h + 4), dir = LE32(h + 8);
		std::vector<unsigned char> d(n * 16);
		if (n && ReadAt(f, dir, &d[0], d.size()))
			for (unsigned i = 0; i < n; ++i)
				if (!strncasecmp((const char *)&d[i * 16 + 8], "PLAYPAL", 8) && LE32(&d[i * 16 + 4]) >= 768)
					found = ReadAt(f, LE32(&d[i * 16]), pal, 768);
	}
	else if (!memcmp(h, "PK\3\4", 4))
	{
		fseek(f, 0, SEEK_END);
		long size = ftell(f), tail = size < 65557 ? size : 65557;
		std::vector<unsigned char> t(tail);
		if (ReadAt(f, size - tail, &t[0], tail))
			for (long i = tail - 22; i >= 0; --i)
				if (!memcmp(&t[i], "PK\5\6", 4))
				{
					unsigned n = LE16(&t[i + 10]), cdsize = LE32(&t[i + 12]), cd = LE32(&t[i + 16]);
					std::vector<unsigned char> c(cdsize);
					if (!cdsize || !ReadAt(f, cd, &c[0], cdsize))
						break;
					for (unsigned k = 0, p = 0; k < n && p + 46 <= cdsize; ++k)
					{
						unsigned method = LE16(&c[p + 10]), csize = LE32(&c[p + 20]), usize = LE32(&c[p + 24]);
						unsigned nl = LE16(&c[p + 28]), xl = LE16(&c[p + 30]), cl = LE16(&c[p + 32]), lho = LE32(&c[p + 42]);
						std::string name((const char *)&c[p + 46], nl);
						p += 46 + nl + xl + cl;
						size_t dot = name.find('.');
						if (name.find('/') != std::string::npos || usize < 768 ||
							strcasecmp(name.substr(0, dot).c_str(), "PLAYPAL") != 0)
							continue;
						unsigned char lh[30];
						if (!ReadAt(f, lho, lh, 30))
							continue;
						std::vector<unsigned char> raw(csize), out(usize);
						if (!csize || !ReadAt(f, lho + 30 + LE16(lh + 26) + LE16(lh + 28), &raw[0], csize))
							continue;
						if (method == 0)
							memcpy(&out[0], &raw[0], usize < csize ? usize : csize);
						else if (method == 8)
						{
							z_stream z = {};
							z.next_in = &raw[0]; z.avail_in = csize; z.next_out = &out[0]; z.avail_out = usize;
							if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
								continue;
							inflate(&z, Z_FINISH);
							inflateEnd(&z);
						}
						else
							continue;
						memcpy(pal, &out[0], 768);
						found = true;
					}
					break;
				}
	}
	fclose(f);
	return found;
}

// Entry names of a WAD (lump names) or zip/pk3 (full paths); false if the file is neither.
static bool ListEntries(const char *path, std::vector<std::string> &names)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return false;
	bool ok = false;
	unsigned char h[12];
	if (ReadAt(f, 0, h, 12) && (!memcmp(h, "IWAD", 4) || !memcmp(h, "PWAD", 4)))
	{
		unsigned n = LE32(h + 4), dir = LE32(h + 8);
		std::vector<unsigned char> d(n * 16 + 1);
		if (ReadAt(f, dir, &d[0], n * 16))
		{
			for (unsigned i = 0; i < n; ++i)
				names.push_back(std::string((const char *)&d[i * 16 + 8], strnlen((const char *)&d[i * 16 + 8], 8)));
			ok = true;
		}
	}
	else if (!memcmp(h, "PK\3\4", 4))
	{
		fseek(f, 0, SEEK_END);
		long size = ftell(f), tail = size < 65557 ? size : 65557;
		std::vector<unsigned char> t(tail);
		if (ReadAt(f, size - tail, &t[0], tail))
			for (long i = tail - 22; i >= 0; --i)
				if (!memcmp(&t[i], "PK\5\6", 4))
				{
					unsigned n = LE16(&t[i + 10]), cdsize = LE32(&t[i + 12]), cd = LE32(&t[i + 16]);
					std::vector<unsigned char> c(cdsize + 1);
					if (cdsize && ReadAt(f, cd, &c[0], cdsize))
					{
						for (unsigned k = 0, p = 0; k < n && p + 46 <= cdsize; ++k)
						{
							unsigned nl = LE16(&c[p + 28]), xl = LE16(&c[p + 30]), cl = LE16(&c[p + 32]);
							names.push_back(std::string((const char *)&c[p + 46], nl));
							p += 46 + nl + xl + cl;
						}
						ok = true;
					}
					break;
				}
	}
	fclose(f);
	return ok;
}

// What a mod's actor code is written in: 0 = DECORATE/ACS or no actors, 1 = ZScript and DECORATE (ZDoom 2.8.1
// ignores the ZScript part), 2 = ZScript only (made for GZDoom 3.0+, cannot run on this engine).
int I3DS_ModScriptKind(const char *path)
{
	std::vector<std::string> names;
	if (!ListEntries(path, names))
		return 0;
	bool zs = false, dec = false;
	for (size_t i = 0; i < names.size(); ++i)
	{
		std::string n = names[i];
		for (size_t k = 0; k < n.size(); ++k)
			n[k] = tolower(n[k]);
		size_t slash = n.rfind('/'), dot = n.rfind('.');
		std::string base = n.substr(slash == std::string::npos ? 0 : slash + 1);
		std::string stem = base.substr(0, base.find('.'));
		std::string ext = dot != std::string::npos && (slash == std::string::npos || dot > slash) ? n.substr(dot) : "";
		if (stem == "zscript" || ext == ".zs" || ext == ".zsc" || ext == ".zc")
			zs = true;
		else if (stem == "decorate" || n.compare(0, 9, "decorate/") == 0)
			dec = true;
	}
	return zs ? (dec ? 1 : 2) : 0;
}

unsigned I3DS_PaletteKey(const std::vector<std::string> &files)
{
	unsigned char pal[768];
	unsigned crc = 0;
	for (size_t i = 0; i < files.size(); ++i)
		if (FindPlaypal(files[i].c_str(), pal))
			crc = crc32(0, pal, 768);
	return crc;
}

unsigned I3DS_ModKey(const char *path, unsigned palkey)
{
	unsigned key = crc32(0, NULL, 0), v[3] = { 0, palkey, BD3DS_CACHE_VERSION };
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return 0;
	fseek(f, 0, SEEK_END);
	long size = ftell(f), tail = size < 65536 ? size : 65536;
	v[0] = (unsigned)size;
	std::vector<unsigned char> t(tail ? tail : 1);
	if (tail && ReadAt(f, size - tail, &t[0], tail))	// zip central directory / WAD directory live at the end
		key = crc32(key, &t[0], tail);
	fclose(f);
	return crc32(key, (const unsigned char *)v, sizeof(v));
}

std::string I3DS_ModCacheFile(const char *modpath, unsigned key, const char *ext)
{
	const char *base = strrchr(modpath, '/');
	char name[400];
	snprintf(name, sizeof(name), "%s%s-%08x.%s", BD3DS_PATH_CACHE_DIR, base ? base + 1 : modpath, key, ext);
	return name;
}

extern __thread const BYTE *I3DS_PngMemory;	// pngtexture.cpp: PNG bytes MakeTexture reads on this thread
extern __thread long I3DS_PngMemoryLength;
FTexture *PNGTexture_TryCreate(FileReader &data, int lumpnum);

// ---- zip writer ---------------------------------------------------------------------------------

namespace {

struct ZipEntry { std::string name; unsigned crc, size, csize, method, offset; };

struct ZipWriter
{
	FILE *f;
	std::vector<ZipEntry> entries;

	static void Put16(std::vector<unsigned char> &b, unsigned v) { b.push_back(v & 255); b.push_back(v >> 8); }
	static void Put32(std::vector<unsigned char> &b, unsigned v) { Put16(b, v & 0xffff); Put16(b, v >> 16); }

	std::vector<unsigned char> packed;

	// Build sheet cache_deflate: 0 = entries stored (no compression time; the engine then reads them straight from
	// the file), 1 = deflated (fastest level) when that saves at least 10% (smaller cache files).
	bool Add(const std::string &name, const unsigned char *data, unsigned size)
	{
		ZipEntry e = { name, (unsigned)crc32(0, data, size), size, size, 0, (unsigned)ftell(f) };
		const unsigned char *body = data;
		packed.resize(size + size / 1000 + 64);
		z_stream z = {};
		if (BD3DS_CACHE_DEFLATE && size > 256 && deflateInit2(&z, Z_BEST_SPEED, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK)
		{
			z.next_in = (Bytef *)data; z.avail_in = size;
			z.next_out = &packed[0]; z.avail_out = packed.size();
			if (deflate(&z, Z_FINISH) == Z_STREAM_END && z.total_out < size - size / 10)
			{
				e.csize = z.total_out;
				e.method = 8;
				body = &packed[0];
			}
			deflateEnd(&z);
		}
		std::vector<unsigned char> h;
		Put32(h, 0x04034b50); Put16(h, 20); Put16(h, 0); Put16(h, e.method); Put16(h, 0); Put16(h, 0x21);
		Put32(h, e.crc); Put32(h, e.csize); Put32(h, size); Put16(h, name.size()); Put16(h, 0);
		h.insert(h.end(), name.begin(), name.end());
		entries.push_back(e);
		return fwrite(&h[0], 1, h.size(), f) == h.size() && fwrite(body, 1, e.csize, f) == e.csize;
	}

	bool Finish()
	{
		std::vector<unsigned char> c;
		for (size_t i = 0; i < entries.size(); ++i)
		{
			const ZipEntry &e = entries[i];
			Put32(c, 0x02014b50); Put16(c, 20); Put16(c, 20); Put16(c, 0); Put16(c, e.method); Put16(c, 0); Put16(c, 0x21);
			Put32(c, e.crc); Put32(c, e.csize); Put32(c, e.size); Put16(c, e.name.size()); Put16(c, 0); Put16(c, 0);
			Put16(c, 0); Put16(c, 0); Put32(c, 0); Put32(c, e.offset);
			c.insert(c.end(), e.name.begin(), e.name.end());
		}
		unsigned cd = (unsigned)ftell(f), cdsize = (unsigned)c.size();
		Put32(c, 0x06054b50); Put16(c, 0); Put16(c, 0); Put16(c, entries.size()); Put16(c, entries.size());
		Put32(c, cdsize); Put32(c, cd); Put16(c, 0);
		return fwrite(&c[0], 1, c.size(), f) == c.size();
	}
};

// ---- sprite -> Doom patch ------------------------------------------------------------------------------

// Post encoding as FPatchTexture::MakeTexture reads it: topdelta > previous top is absolute, otherwise it is
// added to the previous top (DeePsea tall patches); zero-length posts step past row 254.
struct ColumnWriter
{
	std::vector<unsigned char> &out;
	int top;

	void Post(int delta, const BYTE *pix, int len)
	{
		out.push_back(delta); out.push_back(len); out.push_back(0);
		if (len)
			out.insert(out.end(), pix, pix + len);
		out.push_back(0);
	}

	void Emit(int start, const BYTE *pix, int len)
	{
		for (;;)
		{
			if (start < 255 && start > top)
			{
				Post(start, pix, len);
				top = start;
				return;
			}
			int d = start - top;
			if (d >= 0 && d <= top && d < 255)
			{
				Post(d, pix, len);
				top = start;
				return;
			}
			if (top < 254)
			{
				Post(254, NULL, 0);
				top = 254;
			}
			else
			{
				int step = MIN(MIN(top, 254), d);
				Post(step, NULL, 0);
				top += step;
			}
		}
	}
};

static bool MakePatch(FTexture *tex, std::vector<unsigned char> &out)
{
	int w = tex->GetWidth(), h = tex->GetHeight();
	if (w <= 0 || h <= 0 || w > 2048 || h > 2048)
		return false;
	out.clear();
	short hdr[4] = { (short)w, (short)h, tex->LeftOffset, tex->TopOffset };
	out.insert(out.end(), (unsigned char *)hdr, (unsigned char *)hdr + 8);
	out.resize(8 + 4 * w);
	for (int x = 0; x < w; ++x)
	{
		unsigned ofs = out.size();
		memcpy(&out[8 + 4 * x], &ofs, 4);
		const FTexture::Span *spans;
		const BYTE *col = tex->GetColumn(x, &spans);
		ColumnWriter cw = { out, -1 };
		for (; spans->Length != 0; ++spans)
			for (int s = spans->TopOffset, left = spans->Length; left > 0; )
			{
				int len = MIN(left, 254);
				cw.Emit(s, col + s, len);
				s += len;
				left -= len;
			}
		out.push_back(0xff);
	}
	return true;
}

// ---- OGG sfx -> WAV ------------------------------------------------------------------------------------

// SoundRenderer::CreateDecoder is protected; a pointer-to-member taken in a derived class may be called on
// any SoundRenderer (GSnd), which gives the engine's decoder list (tremor, WAV, ...) without a new patch.
struct DecoderAccess : SoundRenderer
{
	static SoundDecoder *Create(SoundRenderer *r, FileReader *f)
	{
		return (r->*(&DecoderAccess::CreateDecoder))(f);
	}
};

// Same transform as NDSPSoundRenderer::MakeSample: 16-bit mono, rate halved (pair average) when >= 32 kHz.
static bool MakeWav(const unsigned char *data, int length, std::vector<unsigned char> &wav)
{
	if (GSnd == NULL)
		return false;
	MemoryReader reader((const char *)data, length);
	SoundDecoder *decoder = DecoderAccess::Create(GSnd, &reader);
	if (decoder == NULL)
		return false;
	int rate; ChannelConfig chans; SampleType type;
	decoder->getInfo(&rate, &chans, &type);
	TArray<char> raw = decoder->readAll();
	delete decoder;
	int channels = chans == ChannelConfig_Stereo ? 2 : 1, bits = type == SampleType_UInt8 ? 8 : 16, bytesper = bits / 8;
	if (rate <= 0 || raw.Size() < (unsigned)(bytesper * channels))
		return false;
	unsigned frames = raw.Size() / (bytesper * channels);
	int step = rate >= 32000 ? 2 : 1;
	unsigned outframes = frames / step;
	if (outframes == 0)
		return false;
	const BYTE *src = (const BYTE *)&raw[0];
	wav.resize(44 + outframes * 2);
	for (unsigned f = 0; f < outframes; ++f)
	{
		int acc = 0;
		for (int k = 0; k < step; ++k)
		{
			const BYTE *p = src + ((f * step + k) * channels) * bytesper;
			for (int c = 0; c < channels; ++c)
				acc += bits == 8 ? ((int)p[c] - 128) << 8 : (int)(SWORD)(p[c * 2] | (p[c * 2 + 1] << 8));
		}
		short s = (short)(acc / (step * channels));
		memcpy(&wav[44 + f * 2], &s, 2);
	}
	unsigned outrate = rate / step, datasize = outframes * 2, riff = 36 + datasize, byterate = outrate * 2;
	unsigned char *h = &wav[0];
	memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8);
	unsigned fmtsize = 16; unsigned short fmt[2] = { 1, 1 }; unsigned short align[2] = { 2, 16 };
	memcpy(h + 16, &fmtsize, 4); memcpy(h + 20, fmt, 4); memcpy(h + 24, &outrate, 4); memcpy(h + 28, &byterate, 4);
	memcpy(h + 32, align, 4); memcpy(h + 36, "data", 4); memcpy(h + 40, &datasize, 4);
	return true;
}

// Background writer: compressing + writing cache entries, decoding OGG sounds and converting part of the PNG images
// run on the New 3DS's third core while the main thread converts the other images (WAD reads stay on the main
// thread; a PNG job carries the lump bytes and the engine decodes them from memory, pngtexture.cpp patch). Without
// that core (Old 3DS) every job runs inline.
enum { JOB_ENTRY, JOB_OGG, JOB_PNG };
struct CacheJob { std::string name; std::vector<unsigned char> data; int kind; int lump; int usetype; };

class CacheWriter
{
public:
	explicit CacheWriter(ZipWriter &z) : zip(z)
	{
		LightLock_Init(&lock);
		LightEvent_Init(&work, RESET_ONESHOT);
		LightEvent_Init(&room, RESET_ONESHOT);
		s32 prio = 0x30;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		thread = threadCreate(Main, this, 64 * 1024, prio, 2, false);
	}

	size_t Pending() const { return pending; }
	bool Threaded() const { return thread != NULL; }
	unsigned QueuedImages() const { return queuedImages; }

	void Push(CacheJob &job)
	{
		if (thread == NULL)
		{
			Process(job);
			return;
		}
		LightLock_Lock(&lock);
		while (pending > Limit && !jobs.empty())
		{
			LightLock_Unlock(&lock);
			LightEvent_Wait(&room);
			LightLock_Lock(&lock);
		}
		pending += job.data.size();
		if (job.kind == JOB_PNG)
			++queuedImages;
		jobs.push_back(CacheJob());
		jobs.back().name.swap(job.name);
		jobs.back().data.swap(job.data);
		jobs.back().kind = job.kind;
		jobs.back().lump = job.lump;
		jobs.back().usetype = job.usetype;
		LightLock_Unlock(&lock);
		LightEvent_Signal(&work);
	}

	// Waits for the queue to drain (answering HOME / sleep meanwhile); false if a write failed.
	bool Finish()
	{
		if (thread != NULL)
		{
			LightLock_Lock(&lock);
			closing = true;
			LightLock_Unlock(&lock);
			LightEvent_Signal(&work);
			while (R_FAILED(threadJoin(thread, 50 * 1000000ULL)))
				I3DS_PumpApt();
			threadFree(thread);
			thread = NULL;
		}
		return ok;
	}

	unsigned soundsMade = 0, imagesMade = 0;

private:
	static const size_t Limit = 8 << 20;	// queued bytes before the main thread waits
	ZipWriter &zip;
	LightLock lock;
	LightEvent work, room;
	std::deque<CacheJob> jobs;
	size_t pending = 0;
	unsigned queuedImages = 0;
	bool closing = false, ok = true;
	Thread thread = NULL;
	std::vector<unsigned char> out;

	void Process(CacheJob &job)
	{
		if (job.kind == JOB_OGG)
		{
			if (!MakeWav(&job.data[0], (int)job.data.size(), out))
				return;
			ok = zip.Add(job.name, &out[0], out.size()) && ok;
			++soundsMade;
		}
		else if (job.kind == JOB_PNG)
		{
			MemoryReader reader((const char *)&job.data[0], (long)job.data.size());
			I3DS_PngMemory = &job.data[0];
			I3DS_PngMemoryLength = (long)job.data.size();
			FTexture *tex = PNGTexture_TryCreate(reader, job.lump);
			if (tex != NULL)
				tex->UseType = job.usetype;	// as FTexture::CreateTexture does
			bool made = tex != NULL && MakePatch(tex, out);
			delete tex;
			I3DS_PngMemory = NULL;
			if (made)
			{
				ok = zip.Add(job.name, &out[0], out.size()) && ok;
				++imagesMade;
			}
		}
		else
			ok = zip.Add(job.name, &job.data[0], job.data.size()) && ok;
	}

	static void Main(void *arg)
	{
		CacheWriter *w = (CacheWriter *)arg;
		for (;;)
		{
			LightLock_Lock(&w->lock);
			while (w->jobs.empty() && !w->closing)
			{
				LightLock_Unlock(&w->lock);
				LightEvent_Wait(&w->work);
				LightLock_Lock(&w->lock);
			}
			if (w->jobs.empty())
			{
				LightLock_Unlock(&w->lock);
				return;
			}
			CacheJob job;
			job.name.swap(w->jobs.front().name);
			job.data.swap(w->jobs.front().data);
			job.kind = w->jobs.front().kind;
			job.lump = w->jobs.front().lump;
			job.usetype = w->jobs.front().usetype;
			w->jobs.pop_front();
			LightLock_Unlock(&w->lock);
			size_t size = job.data.size();
			w->Process(job);
			LightLock_Lock(&w->lock);
			w->pending -= size;
			if (job.kind == JOB_PNG)
				--w->queuedImages;
			LightLock_Unlock(&w->lock);
			LightEvent_Signal(&w->room);
		}
	}
};

// Namespaces whose images load through FTexture::CreateTexture with the patch format as a valid choice, and the
// pk3 folder that puts a cache entry back into the same namespace. Flats are left alone: their raw format is
// detected by size, which a patch could match by accident.
static const char *ImageDir(int ns)
{
	switch (ns)
	{
	case ns_sprites: return "sprites/";
	case ns_newtextures: return "textures/";
	case ns_patches: return "patches/";
	case ns_graphics: return "graphics/";
	default: return NULL;
	}
}

static bool LumpStartsWith(int lump, const char *magic, int len)
{
	if (Wads.LumpLength(lump) < len)
		return false;
	FWadLump r = Wads.OpenLumpNum(lump);
	char buf[8];
	return r.Read(buf, len) == len && memcmp(buf, magic, len) == 0;
}

// First bytes of zip entries without the engine: opening a deflated lump through the engine inflates all of it,
// which made the format scan decompress every sprite of a mod once just to read 4 bytes.
struct ZipPeek
{
	struct Entry { unsigned method, csize, lho; };
	FILE *f;
	std::vector<std::pair<std::string, Entry> > entries;	// lower-case full names, sorted

	explicit ZipPeek(const char *path) : f(fopen(path, "rb"))
	{
		unsigned char h[4];
		if (f == NULL || !ReadAt(f, 0, h, 4) || memcmp(h, "PK\3\4", 4) != 0)
			return;
		fseek(f, 0, SEEK_END);
		long size = ftell(f), tail = size < 65557 ? size : 65557;
		std::vector<unsigned char> t(tail);
		if (!ReadAt(f, size - tail, &t[0], tail))
			return;
		for (long i = tail - 22; i >= 0; --i)
			if (!memcmp(&t[i], "PK\5\6", 4))
			{
				unsigned n = LE16(&t[i + 10]), cdsize = LE32(&t[i + 12]), cd = LE32(&t[i + 16]);
				std::vector<unsigned char> c(cdsize + 1);
				if (!cdsize || !ReadAt(f, cd, &c[0], cdsize))
					return;
				for (unsigned k = 0, p = 0; k < n && p + 46 <= cdsize; ++k)
				{
					unsigned nl = LE16(&c[p + 28]), xl = LE16(&c[p + 30]), cl = LE16(&c[p + 32]);
					std::string name((const char *)&c[p + 46], nl);
					for (size_t j = 0; j < name.size(); ++j)
						name[j] = tolower(name[j]);
					Entry e = { LE16(&c[p + 10]), LE32(&c[p + 20]), LE32(&c[p + 42]) };
					entries.push_back(std::make_pair(name, e));
					p += 46 + nl + xl + cl;
				}
				std::sort(entries.begin(), entries.end(),
					[](const std::pair<std::string, Entry> &a, const std::pair<std::string, Entry> &b) { return a.first < b.first; });
				return;
			}
	}
	~ZipPeek() { if (f) fclose(f); }

	// 1 = starts with magic, 0 = does not, -1 = unknown here (not a zip entry: ask the engine)
	int StartsWith(const char *fullname, const char *magic, int len)
	{
		if (entries.empty() || fullname == NULL)
			return -1;
		std::string key(fullname);
		for (size_t j = 0; j < key.size(); ++j)
			key[j] = tolower(key[j]);
		auto it = std::lower_bound(entries.begin(), entries.end(), key,
			[](const std::pair<std::string, Entry> &a, const std::string &k) { return a.first < k; });
		if (it == entries.end() || it->first != key)
			return -1;
		const Entry &e = it->second;
		unsigned char lh[30], raw[128], out[16];
		if (!ReadAt(f, e.lho, lh, 30))
			return -1;
		unsigned n = e.csize < sizeof(raw) ? e.csize : sizeof(raw);
		if (n == 0 || !ReadAt(f, e.lho + 30 + LE16(lh + 26) + LE16(lh + 28), raw, n))
			return 0;
		const unsigned char *head = raw;
		unsigned have = n;
		if (e.method == 8)
		{
			z_stream z = {};
			z.next_in = raw; z.avail_in = n; z.next_out = out; z.avail_out = sizeof(out);
			if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
				return -1;
			inflate(&z, Z_SYNC_FLUSH);
			have = z.total_out;
			inflateEnd(&z);
			head = out;
		}
		else if (e.method != 0)
			return -1;
		return have >= (unsigned)len && memcmp(head, magic, len) == 0;
	}
};

} // namespace

// Cache files of the same mod with another key (mod updated, other palette, older cache_version).
static void RemoveStaleCaches(const char *modname, const char *keep)
{
	DIR *d = opendir(BD3DS_PATH_CACHE_DIR);
	if (d == NULL)
		return;
	size_t n = strlen(modname);
	std::vector<std::string> stale;
	while (dirent *e = readdir(d))
		if (!strncmp(e->d_name, modname, n) && e->d_name[n] == '-' && strcmp(e->d_name, keep) != 0)
			stale.push_back(std::string(BD3DS_PATH_CACHE_DIR) + e->d_name);
	closedir(d);
	for (size_t i = 0; i < stale.size(); ++i)
		remove(stale[i].c_str());
}

// ---- build (engine hook after R_Init, d_main.cpp patch) ---------------------------------------------------

void I3DS_ModCacheBuild()
{
	if (Args->CheckParm("-nomodcache"))	// launcher Options: mod caching off
		return;
	int pallump = Wads.CheckNumForName("PLAYPAL");
	if (pallump < 0 || Wads.LumpLength(pallump) < 768)
		return;
	unsigned char pal[768];
	{
		FWadLump r = Wads.OpenLumpNum(pallump);
		r.Read(pal, 768);
	}
	unsigned palkey = crc32(0, pal, 768);
	const size_t moddir = strlen(BD3DS_PATH_MOD_DIR);
	mkdir(BD3DS_PATH_CACHE_DIR, 0777);

	for (int file = 0; file < Wads.GetNumWads(); ++file)
	{
		const char *path = Wads.GetWadFullName(file);
		if (path == NULL || strncasecmp(path, BD3DS_PATH_MOD_DIR, moddir) != 0)
			continue;
		unsigned key = I3DS_ModKey(path, palkey);
		std::string pk3 = I3DS_ModCacheFile(path, key, "pk3"), none = I3DS_ModCacheFile(path, key, "none");
		struct stat st;
		if (stat(pk3.c_str(), &st) == 0 || stat(none.c_str(), &st) == 0)
			continue;

		u64 t0 = osGetTime();
		ZipPeek peek(path);
		auto startsWith = [&](int lump, const char *magic, int len)
		{
			int r = peek.StartsWith(Wads.GetLumpFullName(lump), magic, len);
			return r >= 0 ? r == 1 : LumpStartsWith(lump, magic, len);
		};
		std::vector<int> images, sounds;
		std::vector<bool> isPng;
		for (int lump = 0; lump < Wads.GetNumLumps(); ++lump)
		{
			I3DS_PumpApt();	// sleep / HOME / close while caching
			if (Wads.GetLumpFile(lump) != file || ImageDir(Wads.GetLumpNamespace(lump)) == NULL)
				continue;
			bool png = BD3DS_MODCACHE_IMAGE_PNG && startsWith(lump, "\x89PNG", 4);
			if (png || (BD3DS_MODCACHE_IMAGE_JPEG && startsWith(lump, "\xff\xd8\xff", 3)))
			{
				images.push_back(lump);
				isPng.push_back(png);
			}
		}
		std::vector<bool> seen(Wads.GetNumLumps(), false);
		for (unsigned i = 1; BD3DS_MODCACHE_SFX_OGG && i < S_sfx.Size(); ++i)
		{
			I3DS_PumpApt();
			int lump = S_sfx[i].lumpnum;
			if (lump >= 0 && !seen[lump] && Wads.GetLumpFile(lump) == file && startsWith(lump, "OggS", 4))
			{
				seen[lump] = true;
				sounds.push_back(lump);
			}
		}
		u64 tScan = osGetTime() - t0, tImages = 0, tSounds = 0;
		const char *base = strrchr(path, '/') + 1;
		if (images.empty() && sounds.empty())
		{
			FILE *m = fopen(none.c_str(), "wb");
			if (m) fclose(m);
			continue;
		}
		Printf("Mod cache: %s (%u images, %u sounds)\n", base, (unsigned)images.size(), (unsigned)sounds.size());
		std::string tmp = pk3 + ".tmp";
		ZipWriter zip = { fopen(tmp.c_str(), "wb") };
		if (zip.f == NULL)
		{
			Printf("Mod cache: cannot write %s\n", tmp.c_str());
			continue;
		}
		setvbuf(zip.f, NULL, _IOFBF, 64 * 1024);
		CacheWriter writer(zip);
		unsigned done = 0, total = images.size() + sounds.size();
		size_t nextSound = 0;
		char name[9];
		// OGG sounds go to the writer core as raw data (read here: WAD access stays on the main thread); keep a few
		// queued so that core always has decoding to do while this one converts images.
		auto pushSound = [&]()
		{
			FMemLump lump = Wads.ReadLump(sounds[nextSound]);
			CacheJob job;
			job.data.assign((const unsigned char *)lump.GetMem(), (const unsigned char *)lump.GetMem() + Wads.LumpLength(sounds[nextSound]));
			memset(name, 0, sizeof(name));
			Wads.GetLumpName(name, sounds[nextSound]);
			job.name = std::string("sounds/") + name + ".wav";
			job.kind = JOB_OGG;
			writer.Push(job);
			++nextSound;
			++done;
		};
		for (size_t i = 0; i < images.size(); ++i, ++done)
		{
			I3DS_PumpApt();
			while (nextSound < sounds.size() && writer.Pending() < (2u << 20))
				pushSound();
			u64 t = osGetTime();
			int ns = Wads.GetLumpNamespace(images[i]);
			int usetype = ns == ns_sprites ? FTexture::TEX_Sprite : ns == ns_newtextures ? FTexture::TEX_Wall : FTexture::TEX_MiscPatch;
			memset(name, 0, sizeof(name));
			Wads.GetLumpName(name, images[i]);
			CacheJob job;
			job.name = std::string(ImageDir(ns)) + name + ".lmp";
			job.lump = images[i];
			job.usetype = usetype;
			if (writer.Threaded() && isPng[i] && writer.QueuedImages() < 2)
			{	// the writer core is short of work: it converts this PNG from its bytes
				FMemLump lump = Wads.ReadLump(images[i]);
				job.data.assign((const unsigned char *)lump.GetMem(), (const unsigned char *)lump.GetMem() + Wads.LumpLength(images[i]));
				job.kind = JOB_PNG;
				writer.Push(job);
				tImages += osGetTime() - t;
				if (done % 100 == 0)
					Printf("  %u / %u\n", done, total);
				continue;
			}
			// An open reader keeps a deflated lump's data cached, so creating the texture and reading its pixels
			// share one inflate instead of decompressing the lump twice.
			FWadLump keep = Wads.OpenLumpNum(images[i]);
			FTexture *tex = FTexture::CreateTexture(images[i], usetype);
			bool made = tex != NULL && MakePatch(tex, job.data);
			delete tex;
			tImages += osGetTime() - t;
			if (made)
			{
				job.kind = JOB_ENTRY;
				writer.Push(job);
			}
			if (done % 100 == 0)
				Printf("  %u / %u\n", done, total);
		}
		while (nextSound < sounds.size())
		{
			I3DS_PumpApt();
			pushSound();
		}
		u64 t = osGetTime();
		bool ok = writer.Finish();
		tSounds = osGetTime() - t;	// left over for the writer core after the last image
		ok = ok && zip.Finish();
		ok = (fclose(zip.f) == 0) && ok;
		Printf("Mod cache timing: scan %llu ms, images %llu ms (%u on the writer core), writer tail %llu ms, %u/%u sounds, total %llu ms\n",
			(unsigned long long)tScan, (unsigned long long)tImages, writer.imagesMade, (unsigned long long)tSounds,
			writer.soundsMade, (unsigned)sounds.size(), (unsigned long long)(osGetTime() - t0));
		if (ok && rename(tmp.c_str(), pk3.c_str()) == 0)
		{
			Printf("Mod cache: wrote %s (%u entries) - used from the next launch\n", strrchr(pk3.c_str(), '/') + 1,
				(unsigned)zip.entries.size());
			RemoveStaleCaches(base, strrchr(pk3.c_str(), '/') + 1);
		}
		else
		{
			remove(tmp.c_str());
			Printf("Mod cache: failed for %s (SD card full?)\n", base);
		}
	}
}
