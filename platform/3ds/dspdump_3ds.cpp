// DSP firmware extraction (Subsystems sheet: dspdump). NDSP needs sdmc:/3ds/dspfirm.cdc, which users normally
// make with the DSP1 tool. When it is missing, this does what DSP1 does (method from DSP1 by zoogie, MIT
// license; the LZSS decoder follows ctrtool's): read the HOME Menu's ExeFS .code from this console's NAND
// (title content archive 0x2345678A, one title ID per region), undo the reverse-LZSS ExeFS compression, find
// the "DSP1" firmware block and write it to sdmc:/3ds/dspfirm.cdc, where every homebrew looks for it.
// The CIA exheader needs the FileSystemAccess categories of the Build sheet's fs_access row for the NAND read.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char FIRM_PATH[] = "sdmc:/3ds/dspfirm.cdc";

static const u64 HOME_MENU[] = {	// USA, JPN, EUR, CHN, KOR, TWN
	0x0004003000008F02ULL, 0x0004003000008202ULL, 0x0004003000009802ULL,
	0x000400300000A102ULL, 0x000400300000A902ULL, 0x000400300000B102ULL,
};

static u32 LE32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

// ExeFS code compression: LZSS applied from the end of the file backwards. The footer holds the size of the
// compressed region (low 24 bits), the footer length (high 8 bits) and how much the data grows.
static u8 *Decompress(const u8 *in, u32 insize, u32 *outsize)
{
	if (insize < 8)
		return NULL;
	u32 bounds = LE32(in + insize - 8), grow = LE32(in + insize - 4);
	u32 size = insize + grow;
	u8 *out = (u8 *)malloc(size);
	if (out == NULL)
		return NULL;
	memset(out, 0, size);
	memcpy(out, in, insize);
	u32 index = insize - (bounds >> 24), stop = insize - (bounds & 0xFFFFFF), pos = size;
	while (index > stop)
	{
		u8 control = in[--index];
		for (int bit = 0; bit < 8 && index > stop; ++bit, control <<= 1)
		{
			if (control & 0x80)
			{
				if (index < 2)
					goto fail;
				index -= 2;
				u32 seg = in[index] | (in[index + 1] << 8);
				u32 len = ((seg >> 12) & 15) + 3, disp = (seg & 0x0FFF) + 2;
				if (pos < len)
					goto fail;
				for (u32 j = 0; j < len; ++j)
				{
					if (pos + disp >= size)
						goto fail;
					u8 b = out[pos + disp];
					out[--pos] = b;
				}
			}
			else
			{
				if (pos < 1)
					goto fail;
				out[--pos] = in[--index];
			}
		}
	}
	*outsize = size;
	return out;
fail:
	free(out);
	return NULL;
}

static bool DumpFrom(u64 tid)
{
	u32 archive[] = { (u32)tid, (u32)(tid >> 32), MEDIATYPE_NAND, 0 };
	static const u32 file[] = { 0, 0, 2, 0x646F632E, 0x00000065 };	// ExeFS ".code"
	Handle h;
	if (R_FAILED(FSUSER_OpenFileDirectly(&h, (FS_ArchiveID)0x2345678A, (FS_Path){ PATH_BINARY, sizeof(archive), archive },
			(FS_Path){ PATH_BINARY, sizeof(file), file }, FS_OPEN_READ, 0)))
		return false;
	u64 size = 0;
	u32 got = 0;
	u8 *code = NULL;
	bool ok = R_SUCCEEDED(FSFILE_GetSize(h, &size)) && size > 8 && size < 64 * 1024 * 1024 &&
		(code = (u8 *)malloc(size)) != NULL && R_SUCCEEDED(FSFILE_Read(h, &got, 0, code, size)) && got == size;
	FSFILE_Close(h);
	u32 plain = 0;
	u8 *raw = ok ? Decompress(code, size, &plain) : NULL;
	free(code);
	if (raw == NULL)
		return false;
	ok = false;
	u8 *magic = (u8 *)memmem(raw, plain, "DSP1", 4);
	if (magic != NULL && magic - raw >= 0x100)
	{
		u32 len = LE32(magic + 4);	// firmware size, usually 0xC25C; the image starts 0x100 before the magic
		u8 *firm = magic - 0x100;
		if (len >= 0x1000 && len <= 0x100000 && firm + len <= raw + plain)
		{
			mkdir("sdmc:/3ds", 0777);
			if (FILE *f = fopen(FIRM_PATH, "wb"))
			{
				ok = fwrite(firm, 1, len, f) == len;
				ok = fclose(f) == 0 && ok;
				if (!ok)
					remove(FIRM_PATH);
			}
		}
	}
	free(raw);
	return ok;
}

// 1 = firmware already there, 2 = extracted now, 0 = not available.
int I3DS_EnsureDspFirmware()
{
	struct stat st;
	if (stat(FIRM_PATH, &st) == 0 && st.st_size > 0)
		return 1;
	for (size_t i = 0; i < sizeof(HOME_MENU) / sizeof(HOME_MENU[0]); ++i)
		if (DumpFrom(HOME_MENU[i]))
			return 2;
	return 0;
}
