#include "std.h"

#include "emul.h"
#include "vars.h"
#include "tape.h"
#include "memory.h"

#include "util.h"
#include "png/zlib.h"

#define Z80FQ 3500000

// ������� �� ��������� ������ ����� (����� �������� �������� �� ����� 256, ������ ������������)
// -1 (UINT_MAX) - ����������� �������� - ������� �����
// ��� ������� ���� - ����� (��� � ����� 0x19 TZX 1.20):
// 00: opposite to the current level (make an edge, as usual) - default
// 01: same as the current level (no edge - prolongs the previous pulse)
// 10: force low level
// 11: force high level
u32 tape_pulse[0x100];
static unsigned max_pulses = 0;
static unsigned tape_err = 0;

// ������ ������ �� ������� (������ ������������), ������ ����� ������ - ����� ���������� �������
// ��������� ���������� ������������� (������� ������� �������)
u8 *tape_image = nullptr;
static unsigned tape_imagesize = 0;

TAPEINFO *tapeinfo;
unsigned tape_infosize;

static unsigned appendable;

typedef int (__cdecl *inflateInit__ptr)(z_streamp strm, const char *version, int stream_size);
typedef int (__cdecl *inflate_ptr)(z_streamp strm, int flush);
typedef int (__cdecl *inflateEnd_ptr)(z_streamp strm);

static inflateInit__ptr inflateInit__p = nullptr;
static inflate_ptr inflate_p = nullptr;
static inflateEnd_ptr inflateEnd_p = nullptr;

static HMODULE ZlibDll = nullptr;

bool ZlibInit()
{
    ZlibDll = LoadLibrary("zlib1.dll");
    if(!ZlibDll)
    {
        return false;
    }
    inflateInit__p = (inflateInit__ptr)GetProcAddress(ZlibDll, "inflateInit_");
    if(!inflateInit__p)
    {
        return false;
    }
    inflate_p = (inflate_ptr)GetProcAddress(ZlibDll,"inflate");
    if(!inflate_p)
    {
        return false;
    }
    inflateEnd_p = (inflateEnd_ptr)GetProcAddress(ZlibDll,"inflateEnd");
    if(!inflateEnd_p)
    {
        return false;
    }
    return true;
}

void ZlibDone()
{
    if(ZlibDll)
    {
        FreeLibrary(ZlibDll);
    }
}

// ���� ����� �������� � �������� � ���������� ������ ���������� ��������
// ���� ������� � ����� ������� �� ������, �� ����������� � ����� � ������������ ��� ������
// tape image contains indexes in tape_pulse[]
static u8 find_pulse(u32 t, unsigned Flags = 0)
{
    if(max_pulses < _countof(tape_pulse))
    {
        double e = 0.10 * t; // ������� 10%
        for(unsigned i = 0; i < max_pulses; i++)
        {
            u32 pulse = tape_pulse[i] & tape_pulse_mask;
            unsigned fl = (tape_pulse[i] >> 30) & 3;
            if(fl == (Flags & 3) && (t - e) < pulse && pulse <= (t + e))
            {
                return u8(i);
            }
        }
        tape_pulse[max_pulses] = t | ((Flags & 3) << 30);
        return u8(max_pulses++);
    }
    if(!tape_err)
    {
        errmsg("pulse table full");
        tape_err = 1;
    }
    unsigned nearest = 0; int delta = INT_MAX;
    for(unsigned i = 0; i < _countof(tape_pulse); i++)
    {
        u32 pulse = tape_pulse[i] & tape_pulse_mask;
        unsigned fl = (tape_pulse[i] >> 30) & 3;
        if(fl == (Flags & 3) && delta > abs((int)t - (int)pulse))
        {
            nearest = i;
            delta = abs((int)t - (int)pulse);
        }
    }
    return u8(nearest);
}

void find_tape_index()
{
    for(unsigned i = 0; i < tape_infosize; i++)
    {
        if(comp.tape.play_pointer >= tape_image + tapeinfo[i].pos)
        {
            comp.tape.index = i;
        }
    }
    temp.led.tape_started = -600 * Z80FQ;
}

static void find_tape_sizes()
{
    for(unsigned i = 0; i < tape_infosize; i++)
    {
        unsigned end = (i == tape_infosize - 1) ? tape_imagesize : tapeinfo[i + 1].pos;

        unsigned sz = 0;
        for(unsigned j = tapeinfo[i].pos; j < end; j++)
        {
            sz += tape_pulse[tape_image[j]] & tape_pulse_mask;
        }
        tapeinfo[i].t_size = sz;
    }
}

void stop_tape()
{
    // ������� ��� ������ �� ������� tape_bit()
    comp.tape.edge_change = LLONG_MAX;
    comp.tape.tape_bit = -1U;

    if(comp.tape.stopped) // ���� ����� ��� �����������, �� ������ �� ������
    {
        return;
    }

    comp.tape.stopped = true;

    find_tape_index();

    const char *msg = "tape stopped";
    if(comp.tape.play_pointer == comp.tape.end_of_tape) // ��� ���������� ����� ����� �� ������ �������������
    {
        msg = "end of tape";
    }
    else
    {
        comp.tape.play_pointer = nullptr;
    }
    strcpy(statusline, msg); statcnt = 40;
}

void reset_tape()
{
    comp.tape.index = 0;
    comp.tape.stopped = true;
    comp.tape.play_pointer = nullptr;
    comp.tape.edge_change = LLONG_MAX;
    comp.tape.tape_bit = -1U;
}

void start_tape()
{
    if(!tape_image)
    {
        return;
    }

    comp.tape.play_pointer = tape_image + tapeinfo[comp.tape.index].pos;
    comp.tape.end_of_tape = tape_image + tape_imagesize;

    if(comp.tape.play_pointer >= comp.tape.end_of_tape) // ����� �����, ���������� ������
    {
        return;
    }
    comp.tape.stopped = false;
    comp.tape.edge_change = comp.t_states + cpu.t;
    temp.led.tape_started = -600 * Z80FQ;
    comp.tape.tape_bit = -1U;
    strcpy(statusline, "tape started"); statcnt = 40;
}

void closetape()
{
    if(tape_image)
    {
        free(tape_image);
        tape_image = nullptr;
    }
    if(tapeinfo)
    {
        free(tapeinfo);
        tapeinfo = nullptr;
    }
    comp.tape.play_pointer = nullptr; // stop tape
    comp.tape.index = 0; // rewind tape
    tape_err = max_pulses = tape_imagesize = tape_infosize = 0;
    comp.tape.edge_change = LLONG_MAX;
    comp.tape.tape_bit = -1U;
}

static void reserve(unsigned datasize)
{
    const unsigned blocksize = 16384;
    unsigned newsize = align_by(datasize + tape_imagesize + 1, blocksize);
    if(!tape_image)
    {
        tape_image = (unsigned char*)malloc(newsize);
    }
    if(align_by(tape_imagesize, blocksize) < newsize)
    {
        tape_image = (unsigned char*)realloc(tape_image, newsize);
    }
}

static void makeblock(const unsigned char *data, unsigned size, unsigned pilot_t,
    unsigned s1_t, unsigned s2_t, unsigned zero_t, unsigned one_t,
    unsigned pilot_len, unsigned pause, unsigned char last = 8)
{
    reserve(size * 16 + pilot_len + 3);
    if(pilot_len != -1U)
    {
        u8 t = find_pulse(pilot_t);
        for(unsigned i = 0; i < pilot_len; i++)
            tape_image[tape_imagesize++] = t;
        tape_image[tape_imagesize++] = find_pulse(s1_t);
        tape_image[tape_imagesize++] = find_pulse(s2_t);
    }
    u8 t0 = find_pulse(zero_t), t1 = find_pulse(one_t);
    for(; size > 1; size--, data++)
    {
        for(unsigned char j = 0x80; j; j >>= 1)
        {
            tape_image[tape_imagesize++] = (*data & j) ? t1 : t0;
            tape_image[tape_imagesize++] = (*data & j) ? t1 : t0;
        }
    }
    for(unsigned char j = 0x80; j != (unsigned char)(0x80 >> last); j >>= 1) // last byte
    {
        tape_image[tape_imagesize++] = (*data & j) ? t1 : t0;
        tape_image[tape_imagesize++] = (*data & j) ? t1 : t0;
    }
    if(pause)
    {
        tape_image[tape_imagesize++] = find_pulse(pause*(Z80FQ / 1000));
    }
}

static void desc(const unsigned char *data, unsigned size, char *dst)
{
    unsigned char crc = 0;
    char prg[11];
    unsigned i; //Alone Coder 0.36.7
    for(/*unsigned*/ i = 0; i < size; i++)
    {
        crc ^= data[i];
    }

    if(!*data && size == 19 && (data[1] == 0 || data[1] == 3))
    {
        for(i = 0; i < 10; i++)
        {
            prg[i] = (data[i + 2] < ' ' || data[i + 2] >= 0x80) ? '?' : char(data[i + 2]);
        }
        prg[10] = 0;
        for(i = 9; i && prg[i] == ' '; prg[i--] = 0);
        sprintf(dst, "%s: \"%s\" %d,%d", data[1] ? "Bytes" : "Program", prg,
            *(const unsigned short*)(data + 14), *(const unsigned short*)(data + 12));
    }
    else if(*data == 0xFF)
    {
        sprintf(dst, "data block, %u bytes", size - 2);
    }
    else
    {
        sprintf(dst, "#%02X block, %u bytes", *data, size - 2);
    }
    sprintf(dst + strlen(dst), ", crc %s", crc ? "bad" : "ok");
}

static bool alloc_infocell()
{
    TAPEINFO *tmp = (TAPEINFO*)realloc(tapeinfo, (tape_infosize + 1) * sizeof(TAPEINFO));
    if(!tmp)
    {
        return false;
    }
    tapeinfo = tmp;
    tapeinfo[tape_infosize].pos = tape_imagesize;
    appendable = 0;
    return true;
}

static bool named_cell(const void *nm, unsigned sz = 0)
{
    if(!alloc_infocell())
        return false;

    const size_t n = _countof(tapeinfo[tape_infosize].desc) - 1;
    size_t len = min(n, sz ? sz : strlen((const char*)nm));
    strncpy(tapeinfo[tape_infosize].desc, (const char*)nm, len);
    tapeinfo[tape_infosize].desc[len] = 0;
    tape_infosize++;
    return true;
}

int readTAP()
{
    unsigned char *ptr = snbuf;
    closetape();
    while(ptr < snbuf + snapsize)
    {
        unsigned size = *(unsigned short*)ptr; ptr += 2;
        if(!size)
        {
            break;
        }
        alloc_infocell();
        desc(ptr, size, tapeinfo[tape_infosize].desc);
        tape_infosize++;
        makeblock(ptr, size, 2168U, 667U, 735U, 855U, 1710U, (*ptr < 4) ? 8064U : 3220U, 1000U);
        ptr += size;
    }
    find_tape_sizes();
    return (ptr == snbuf + snapsize);
}

#pragma pack(push, 1)
struct TCswHdr
{
    char Signature[22];
    u8 Term;
    u8 VerMajor;
    u8 VerMinor;
    union
    {
        struct
        {
            u16 SampleRate;
            u8 CompressionType;
            u8 Flags;
            u8 Reserved[3];
            u8 Data[];
        } Ver1;

        struct
        {
            u32 SampleRate;
            u32 PulsesAfterDecompression;
            u8 CompressionType;
            u8 Flags;
            u8 HeaderExtLen;
            char EncAppDesc[16];
            u8 ExtHdr[];
        } Ver2;
    };
};
#pragma pack(pop)

int readCSW()
{
    closetape();
    named_cell("CSW tape image");

    const TCswHdr *CswHdr = (TCswHdr *)snbuf;
    u8 CompType;
    u32 SampleRate;
    u8 Flags;
    u32 DataOffset;
    u32 PulsesCount;
    switch(CswHdr->VerMajor)
    {
    case 1:
        CompType = CswHdr->Ver1.CompressionType;
        SampleRate = CswHdr->Ver1.SampleRate;
        Flags = CswHdr->Ver1.Flags;
        DataOffset = offsetof(TCswHdr, Ver1.Data);
        PulsesCount = snapsize - 0x18; // ���������� ���������
        break;

    case 2:
        CompType = CswHdr->Ver2.CompressionType;
        SampleRate = CswHdr->Ver2.SampleRate;
        Flags = CswHdr->Ver2.Flags;
        DataOffset = offsetof(TCswHdr, Ver2.ExtHdr) + CswHdr->Ver2.HeaderExtLen;
        PulsesCount = CswHdr->Ver2.PulsesAfterDecompression;
        break;

    default: // unknown csw version
        return 0;
    }

    u32 UncompressedSize = snapsize;
    switch(CompType)
    {
    case 2: // Z-RLE
    {
        if(!temp.ZlibSupport)
            return 0;
        static const size_t out_sz = sizeof(snbuf);
        void *out = malloc(out_sz);
        if(!out)
            return 0;

        z_stream strm;
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        strm.avail_in = snapsize - DataOffset;
        strm.next_in = snbuf + DataOffset;
        int ret = inflateInit__p(&strm, ZLIB_VERSION, sizeof(strm));
        if(ret != Z_OK)
        {
            free(out);
            return 0;
        }

        strm.avail_out = out_sz;
        strm.next_out = (Byte *)out;
        ret = inflate_p(&strm, Z_FINISH);
        if(ret < 0)
        {
            free(out);
            inflateEnd_p(&strm);
            return 0;
        }
        UncompressedSize = out_sz - strm.avail_out;
        memcpy(snbuf + DataOffset, out, UncompressedSize);
        UncompressedSize += DataOffset;
        free(out);
        inflateEnd_p(&strm);
    }
    case 1: // RLE
        break;

    default:
        return 0; // unknown compression type
    }

    unsigned rate = Z80FQ / SampleRate; // usually 3.5mhz / 44khz
    if(!rate)
        return 0;

    if(!(Flags & 1))
    {
        PulsesCount++;
    }
    PulsesCount++; // �������������� �������� (�����) � ����� CSW

    reserve(PulsesCount);

    if(!(Flags & 1))
        tape_image[tape_imagesize++] = find_pulse(1);

    unsigned i = 0;
    for(unsigned char *ptr = snbuf + DataOffset; ptr < snbuf + UncompressedSize; i++)
    {
        unsigned len = *ptr++ * rate;
        if(!len)
        {
            len = *(unsigned*)ptr * rate;
            ptr += 4;
        }
        tape_image[tape_imagesize++] = find_pulse(len);
    }

    tape_image[tape_imagesize++] = find_pulse(Z80FQ / 10);
    find_tape_sizes();
    return 1;
}

static void create_appendable_block()
{
    if(!tape_infosize || appendable)
    {
        return;
    }
    named_cell("set of pulses");
    appendable = 1;
}

static void parse_hardware(const unsigned char *ptr)
{
    unsigned n = *ptr++;
    if(!n)
    {
        return;
    }
    named_cell("- HARDWARE TYPE ");
    static char ids[] =
        "computer\0"
        "ZX Spectrum 16k\0"
        "ZX Spectrum 48k, Plus\0"
        "ZX Spectrum 48k ISSUE 1\0"
        "ZX Spectrum 128k (Sinclair)\0"
        "ZX Spectrum 128k +2 (Grey case)\0"
        "ZX Spectrum 128k +2A, +3\0"
        "Timex Sinclair TC-2048\0"
        "Timex Sinclair TS-2068\0"
        "Pentagon 128\0"
        "Sam Coupe\0"
        "Didaktik M\0"
        "Didaktik Gama\0"
        "ZX-81 or TS-1000 with  1k RAM\0"
        "ZX-81 or TS-1000 with 16k RAM or more\0"
        "ZX Spectrum 128k, Spanish version\0"
        "ZX Spectrum, Arabic version\0"
        "TK 90-X\0"
        "TK 95\0"
        "Byte\0"
        "Elwro\0"
        "ZS Scorpion\0"
        "Amstrad CPC 464\0"
        "Amstrad CPC 664\0"
        "Amstrad CPC 6128\0"
        "Amstrad CPC 464+\0"
        "Amstrad CPC 6128+\0"
        "Jupiter ACE\0"
        "Enterprise\0"
        "Commodore 64\0"
        "Commodore 128\0"
        "\0"
        "ext. storage\0"
        "Microdrive\0"
        "Opus Discovery\0"
        "Disciple\0"
        "Plus-D\0"
        "Rotronics Wafadrive\0"
        "TR-DOS (BetaDisk)\0"
        "Byte Drive\0"
        "Watsford\0"
        "FIZ\0"
        "Radofin\0"
        "Didaktik disk drives\0"
        "BS-DOS (MB-02)\0"
        "ZX Spectrum +3 disk drive\0"
        "JLO (Oliger) disk interface\0"
        "FDD3000\0"
        "Zebra disk drive\0"
        "Ramex Millenia\0"
        "Larken\0"
        "\0"
        "ROM/RAM type add-on\0"
        "Sam Ram\0"
        "Multiface\0"
        "Multiface 128k\0"
        "Multiface +3\0"
        "MultiPrint\0"
        "MB-02 ROM/RAM expansion\0"
        "\0"
        "sound device\0"
        "Classic AY hardware\0"
        "Fuller Box AY sound hardware\0"
        "Currah microSpeech\0"
        "SpecDrum\0"
        "AY ACB stereo; Melodik\0"
        "AY ABC stereo\0"
        "\0"
        "joystick\0"
        "Kempston\0"
        "Cursor, Protek, AGF\0"
        "Sinclair 2\0"
        "Sinclair 1\0"
        "Fuller\0"
        "\0"
        "mice\0"
        "AMX mouse\0"
        "Kempston mouse\0"
        "\0"
        "other controller\0"
        "Trickstick\0"
        "ZX Light Gun\0"
        "Zebra Graphics Tablet\0"
        "\0"
        "serial port\0"
        "ZX Interface 1\0"
        "ZX Spectrum 128k\0"
        "\0"
        "parallel port\0"
        "Kempston S\0"
        "Kempston E\0"
        "ZX Spectrum 128k +2A, +3\0"
        "Tasman\0"
        "DK'Tronics\0"
        "Hilderbay\0"
        "INES Printerface\0"
        "ZX LPrint Interface 3\0"
        "MultiPrint\0"
        "Opus Discovery\0"
        "Standard 8255 chip with ports 31,63,95\0"
        "\0"
        "printer\0"
        "ZX Printer, Alphacom 32 & compatibles\0"
        "Generic Printer\0"
        "EPSON Compatible\0"
        "\0"
        "modem\0"
        "VTX 5000\0"
        "T/S 2050 or Westridge 2050\0"
        "\0"
        "digitaiser\0"
        "RD Digital Tracer\0"
        "DK'Tronics Light Pen\0"
        "British MicroGraph Pad\0"
        "\0"
        "network adapter\0"
        "ZX Interface 1\0"
        "\0"
        "keyboard / keypad\0"
        "Keypad for ZX Spectrum 128k\0"
        "\0"
        "AD/DA converter\0"
        "Harley Systems ADC 8.2\0"
        "Blackboard Electronics\0"
        "\0"
        "EPROM Programmer\0"
        "Orme Electronics\0"
        "\0"
        "\0";
    for(unsigned i = 0; i < n; i++)
    {
        unsigned char type_n = *ptr++;
        unsigned char id_n = *ptr++;
        unsigned char value_n = *ptr++;
        const char *type = ids, *id, *value;
        unsigned j; //Alone Coder 0.36.7
        for(/*unsigned*/ j = 0; j < type_n; j++)
        {
            if(!*type)
            {
                break;
            }
            while(*(const short*)type)
            {
                type++;
            }
            type += 2;
        }
        if(!*type)
        {
            type = id = "??";
        }
        else
        {
            id = type + strlen(type) + 1;
            for(j = 0; j < id_n; j++)
            {
                if(!*id)
                {
                    id = "??";
                    break;
                }
                id += strlen(id) + 1;
            }
        }
        switch(value_n)
        {
        case 0: value = "compatible with"; break;
        case 1: value = "uses"; break;
        case 2: value = "compatible, but doesn't use"; break;
        case 3: value = "incompatible with"; break;
        default: value = "??";
        }
        char bf[512];
        sprintf(bf, "%s %s: %s", value, type, id);
        named_cell(bf);
    }
    named_cell("-");
}

static inline bool IsPowerOfTwo(unsigned x)
{
    // x will check if x == 0 and !(x & (x - 1)) will check if x is a power of 2 or not
    return x != 0 && (x & (x - 1)) == 0;
}

int readTZX()
{
    unsigned char *ptr = snbuf;
    closetape();
    u32 pause;
    unsigned t0;
    unsigned char pl, last;
    unsigned char *end;
    char *p;
    unsigned loop_n = 0, loop_p = 0;
    unsigned TzxVer = (unsigned(ptr[8]) << 8U) | unsigned(ptr[9]);
    char nm[512];

    ptr += 10; // ������� ���������

    while(ptr < snbuf + snapsize)
    {
        unsigned BlkId = *ptr++;
        switch(BlkId)
        {
        case 0x10: // normal block
        {
            alloc_infocell();
            unsigned size = *(unsigned short*)(ptr + 2);
            pause = *(unsigned short*)ptr;
            ptr += 4;
            desc(ptr, size, tapeinfo[tape_infosize].desc);
            tape_infosize++;
            makeblock(ptr, size, 2168U, 667U, 735U, 855U, 1710U,
                (*ptr < 4) ? 8064U : 3220U, pause);
            ptr += size;
        }
            break;
        case 0x11: // turbo block
        {
            alloc_infocell();
            unsigned size = 0xFFFFFF & *(unsigned*)(ptr + 0x0F);
            desc(ptr + 0x12, size, tapeinfo[tape_infosize].desc);
            tape_infosize++;
            makeblock(ptr + 0x12, size,
                *(unsigned short*)ptr, *(unsigned short*)(ptr + 2),
                *(unsigned short*)(ptr + 4), *(unsigned short*)(ptr + 6),
                *(unsigned short*)(ptr + 8), *(unsigned short*)(ptr + 10),
                *(unsigned short*)(ptr + 13), ptr[12]);
            // todo: test used bits - ptr+12
            ptr += size + 0x12;
        }
            break;
        case 0x12: // pure tone
        {
            create_appendable_block();
            pl = u8(find_pulse(*(unsigned short*)ptr));
            unsigned n = *(unsigned short*)(ptr + 2);
            reserve(n);
            for(unsigned i = 0; i < n; i++)
            {
                tape_image[tape_imagesize++] = pl;
            }
            ptr += 4;
        }
            break;
        case 0x13: // sequence of pulses of different lengths
        {
            create_appendable_block();
            unsigned n = *ptr++;
            reserve(n);
            for(unsigned i = 0; i < n; i++, ptr += 2)
            {
                tape_image[tape_imagesize++] = find_pulse(*(unsigned short*)ptr);
            }
        }
            break;
        case 0x14: // pure data block
        {
            create_appendable_block();
            unsigned size = 0xFFFFFF & *(unsigned*)(ptr + 7);
            makeblock(ptr + 0x0A, size, 0, 0, 0, *(unsigned short*)ptr,
                *(unsigned short*)(ptr + 2), -1U, *(unsigned short*)(ptr + 5), ptr[4]);
            ptr += size + 0x0A;
        }
            break;
        case 0x15: // direct recording
        {
            unsigned size = 0xFFFFFF & *(unsigned*)(ptr + 5);
            t0 = *(unsigned short*)ptr;
            pause = *(unsigned short*)(ptr + 2);
            last = ptr[4];
            named_cell("direct recording");
            ptr += 8;
            pl = 0;
            unsigned n = 0;
            for(unsigned i = 0; i < size; i++) // count number of pulses
            {
                for(unsigned j = 0x80; j; j >>= 1)
                {
                    if((ptr[i] ^ pl) & j)
                    {
                        n++;
                        pl ^= -1;
                    }
                }
            }
            unsigned t = 0;
            pl = 0;
            reserve(n + 2);
            for(unsigned i = 1; i < size; i++, ptr++) // find pulses
            {
                for(unsigned j = 0x80; j; j >>= 1)
                {
                    t += t0;
                    if((*ptr ^ pl) & j)
                    {
                        tape_image[tape_imagesize++] = find_pulse(t);
                        pl ^= -1; t = 0;
                    }
                }
            }
            // find pulses - last byte
            for(unsigned j = 0x80; j != (unsigned char)(0x80 >> last); j >>= 1)
            {
                t += t0;
                if((*ptr ^ pl) & j)
                {
                    tape_image[tape_imagesize++] = find_pulse(t);
                    pl ^= -1; t = 0;
                }
            }
            ptr++;
            tape_image[tape_imagesize++] = find_pulse(t); // last pulse ???
            if(pause)
            {
                tape_image[tape_imagesize++] = find_pulse(pause*(Z80FQ / 1000));
            }
        }
            break;

        case 0x19: // generalized data block
        {
#pragma pack(push, 1)
            struct TTzxBlk19
            {
                u32 Size;
                u16 Pause; // Pause after this block(ms)
                u32 Totp; // Total number of symbols in pilot/sync block (can be 0)
                u8 Npp; // Maximum number of pulses per pilot/sync symbol
                u8 Asp; // Number of pilot/sync symbols in the alphabet table (0=256)
                u32 Totd; // Total number of symbols in data stream(can be 0)
                u8 Npd; // Maximum number of pulses per data symbol
                u8 Asd; // Number of data symbols in the alphabet table (0=256)
            };
            struct TSymDef
            {
                u8 Flags;
                u16 PulseLen[]; // Array of pulse lengths (Npp / Npd)
            };
            struct TPrle
            {
                u8 Symbol; // Symbol to be represented
                u16 RepCnt; // Number of repetitions
            };
#pragma pack(pop)
            auto Blk = (const TTzxBlk19 *)ptr;
            const unsigned Asp = Blk->Asp == 0 ? 256U : Blk->Asp;
            const unsigned Asd = Blk->Asd == 0 ? 256U : Blk->Asd;

            named_cell("generalized data block");
            auto Pause = Blk->Pause;

            auto SymDefp = (const TSymDef *)(Blk + 1); // ������� ����� ����
            const auto SympSize = sizeof(TSymDef) + Blk->Npp * sizeof(*SymDefp->PulseLen); // ������ ������� � �������� ����� ����
            auto Prle = (const TPrle *)(((const u8 *)SymDefp) + Asp * SympSize); // ������� ����� ����

            unsigned n = 0;

            // ������� ����� ��������� � ����� ����
            for(unsigned i = 0; i < Blk->Totp; i++)
            {
                auto Sym = Prle[i].Symbol; // ������ ��������
                assert(Sym < Asp);

                auto RepCnt = Prle[i].RepCnt; // ����� �������� �������
                auto Symp = (const TSymDef *)(((const u8 *)SymDefp) + Sym * SympSize); // �������� ������� � ��������

                unsigned PulseCnt = 0; // ����� ��������� � ����� ������� ��������
                for(unsigned j = 0; j < Blk->Npp && Symp->PulseLen[j] != 0; j++)
                {
                    PulseCnt++;
                }
                n += PulseCnt * RepCnt;
            }

            auto Np = n;

            auto SymDefd = (const TSymDef *)(Prle + Blk->Totp); // ������� ������
            const auto SymdSize = sizeof(TSymDef) + Blk->Npd * sizeof(*SymDefd->PulseLen); // ������ ������� � �������� ������
            auto Data = (const u8 *)(((const u8 *)SymDefd) + Asd * SymdSize); // ������

            const auto Nb = unsigned(ceil(log2(float(Asd))));
            assert(IsPowerOfTwo(Nb)); // ��������� ������ ����������� �� �������� ������ (1,2,4,8 ��� �� ������)

            unsigned BitMask = (1U << Nb) - 1U;

            const auto Ds = unsigned(ceil(Nb * Blk->Totd / 8));

            // ������� ����� ��������� � ������
            for(unsigned i = 0; i < Ds; i++)
            {
                auto Bits = Data[i];
                unsigned PulseCnt = 0; // ����� ��������� � ����� ������� ��������

                // �������������� ����� � �������
                for(unsigned k = 0; k <= 8 - Nb; k++)
                {
                    Bits = rol8(Bits, u8(Nb));
                    auto Sym = unsigned(Bits & BitMask); // ������ ��������
                    assert(Sym < Asd);

                    auto Symd = (const TSymDef *)(((const u8 *)SymDefd) + Sym * SymdSize); // �������� ������� � ��������

                    for(unsigned j = 0; j < Blk->Npd && Symd->PulseLen[j] != 0; j++)
                    {
                        PulseCnt++;
                    }
                }
                n += PulseCnt;
            }

            if(Pause != 0)
            {
                n++;
            }

            reserve(n); // ��������� ������ ��� n ���������

            const auto MaxTapeImageSize = tape_imagesize + n;

            auto Npp = 0U;
            // ��������� ��������� ����� ����
            for(unsigned i = 0; i < Blk->Totp; i++)
            {
                auto Sym = Prle[i].Symbol; // ������ ��������
                assert(Sym < Asp);

                auto RepCnt = Prle[i].RepCnt; // ����� �������� �������
                auto Symp = (const TSymDef *)(((const u8 *)SymDefp) + Sym * SympSize); // �������� ������� � ��������

                unsigned Flags = Symp->Flags & 3U;
                for(unsigned j = 0; j < Blk->Npp && Symp->PulseLen[j] != 0; j++)
                {
                    auto t = Symp->PulseLen[j];
                    for(unsigned k = 0; k < RepCnt; k++)
                    {
                        tape_image[tape_imagesize++] = find_pulse(t, Flags);
                        Flags = 0;
                        assert(tape_imagesize <= MaxTapeImageSize);
                        Npp++;
                    }
                }
            }

            assert(Npp == Np);

            // ��������� ��������� ������
            for(unsigned i = 0; i < Ds; i++)
            {
                auto Bits = Data[i];

                // �������������� ����� � �������
                for(unsigned k = 0; k <= 8 - Nb; k++)
                {
                    Bits = rol8(Bits, u8(Nb));
                    auto Sym = unsigned(Bits & BitMask); // ������ ��������
                    assert(Sym < Asd);

                    auto Symd = (const TSymDef *)(((const u8 *)SymDefd) + Sym * SymdSize); // �������� ������� � ��������

                    unsigned Flags = Symd->Flags & 3U;
                    for(unsigned j = 0; j < Blk->Npd && Symd->PulseLen[j] != 0; j++)
                    {
                        auto t = Symd->PulseLen[j];
                        tape_image[tape_imagesize++] = find_pulse(t, Flags);
                        Flags = 0;
                        assert(tape_imagesize <= MaxTapeImageSize);
                    }
                }
            }

            if(Pause != 0)
            {
                tape_image[tape_imagesize++] = find_pulse(Pause * (Z80FQ / 1000U));
                assert(tape_imagesize <= MaxTapeImageSize);
            }

            ptr += Blk->Size + sizeof(Blk->Size);
        }
        break;

        case 0x20: // pause (silence) or 'stop the tape' command
            pause = *(unsigned short*)ptr;
            if(pause != 0)
            {
                sprintf(nm, "pause %u ms", unsigned(pause));
            }
            else
            {
                sprintf(nm, "stop the tape");
            }
            named_cell(nm);
            reserve(2); ptr += 2;
            if(!pause)
            { // at least 1ms pulse as specified in TZX 1.13
                tape_image[tape_imagesize++] = find_pulse(Z80FQ / 1000);
                pause = UINT32_MAX;
            }
            else
            {
                pause *= Z80FQ / 1000;
            }
            tape_image[tape_imagesize++] = find_pulse(pause);
            break;
        case 0x21: // group start
        {
            unsigned n = *ptr++;
            named_cell(ptr, n); ptr += n;
            appendable = 1;
        }
            break;
        case 0x22: // group end
            break;
        case 0x23: // jump to block
            named_cell("* jump"); ptr += 2;
            break;
        case 0x24: // loop start
            loop_n = *(unsigned short*)ptr; loop_p = tape_imagesize; ptr += 2;
            break;
        case 0x25: // loop end
        {
            if(!loop_n)
            {
                break;
            }
            unsigned size = tape_imagesize - loop_p;
            reserve((loop_n - 1) * size);
            for(unsigned i = 1; i < loop_n; i++)
            {
                memcpy(tape_image + loop_p + i * size, tape_image + loop_p, size);
            }
            tape_imagesize += (loop_n - 1) * size;
            loop_n = 0;
        }
            break;
        case 0x26: // call
            named_cell("* call"); ptr += 2 + 2 * *(unsigned short*)ptr;
            break;
        case 0x27: // ret
            named_cell("* return");
            break;
        case 0x28: // select block
        {
            int l = _countof(nm);
            l -= sprintf(nm, "* choice: ");
            unsigned n = ptr[2];
            p = (char*)ptr + 3;

            for(unsigned i = 0; i < n; i++)
            {
                unsigned size = *(unsigned char*)(p + 2);
                l -= size;
                if(i)
                {
                    l -= 4;
                }

                if(l >= 0)
                {
                    if(i)
                    {
                        strcat(nm, " / ");
                    }

                    char *q = nm + strlen(nm);
                    memcpy(q, p + 3, size);
                    q[size] = 0;
                }

                p += size + 3;
            }
            named_cell(nm);
            ptr += 2 + *(unsigned short*)ptr;
        }
        break;
        case 0x2A: // stop if 48k
            named_cell("* stop if 48K");
            ptr += 4 + *(unsigned*)ptr;
            break;
        case 0x30: // text description
        {
            unsigned n = *ptr++;
            named_cell(ptr, n); ptr += n;
            appendable = 1;
        }
            break;
        case 0x31: // message block
            named_cell("- MESSAGE BLOCK ");
            end = ptr + 2 + ptr[1]; pl = *end; *end = 0;
            for(p = (char*)ptr + 2; p < (const char*)end; p++)
            {
                if(*p == 0x0D)
                {
                    *p = 0;
                }
            }
            for(p = (char*)ptr + 2; p < (const char*)end; p += strlen(p) + 1)
            {
                named_cell(p);
            }
            *end = pl; ptr = end;
            named_cell("-");
            break;
        case 0x32: // archive info
            named_cell("- ARCHIVE INFO ");
            p = (char*)ptr + 3;
            for(unsigned i = 0; i < ptr[2]; i++)
            {
                const char *info;
                switch(*p++)
                {
                case 0: info = "Title"; break;
                case 1: info = "Publisher"; break;
                case 2: info = "Author"; break;
                case 3: info = "Year"; break;
                case 4: info = "Language"; break;
                case 5: info = "Type"; break;
                case 6: info = "Price"; break;
                case 7: info = "Protection"; break;
                case 8: info = "Origin"; break;
                case -1:info = "Comment"; break;
                default:info = "info"; break;
                }
                unsigned size = *(BYTE*)p + 1U;
                char tmp = p[size]; p[size] = 0;
                sprintf(nm, "%s: %s", info, p + 1);
                p[size] = tmp; p += size;
                named_cell(nm);
            }
            named_cell("-");
            ptr += 2 + *(unsigned short*)ptr;
            break;
        case 0x33: // hardware type
            parse_hardware(ptr);
            ptr += 1 + 3 * *ptr;
            break;
        case 0x34: // emulation info
            named_cell("* emulation info"); ptr += 8;
            break;
        case 0x35: // custom info
            if(!memcmp(ptr, "POKEs           ", 16))
            {
                named_cell("- POKEs block ");
                named_cell(ptr + 0x15, ptr[0x14]);
                p = (char*)ptr + 0x15 + ptr[0x14];
                unsigned n = *(unsigned char*)p++;
                for(unsigned i = 0; i < n; i++)
                {
                    named_cell(p + 1, *(unsigned char*)p);
                    p += *p + 1;
                    unsigned t = *(unsigned char*)p++;
                    strcpy(nm, "POKE ");
                    for(unsigned j = 0; j < t; j++)
                    {
                        sprintf(nm + strlen(nm), "%d,", *(unsigned short*)(p + 1));
                        if(*p & 0x10)
                        {
                            sprintf(nm + strlen(nm), "nn");
                        }
                        else
                        {
                            sprintf(nm + strlen(nm), "%d", *(unsigned char*)(p + 3));
                        }
                        if(!(*p & 0x08))
                        {
                            sprintf(nm + strlen(nm), "(page %d)", *p & 7);
                        }
                        strcat(nm, "; "); p += 5;
                    }
                    named_cell(nm);
                }
                *(unsigned*)nm = '-';
            }
            else
            {
                sprintf(nm, "* custom info: %s", ptr);
                nm[15 + 16] = 0;
            }
            named_cell(nm);
            ptr += 0x14 + *(unsigned*)(ptr + 0x10);
            break;
        case 0x40: // snapshot
            named_cell("* snapshot"); ptr += 4 + (0xFFFFFF & *(unsigned*)(ptr + 1));
            break;
        default:
            if(TzxVer >= 0x10A)
            {
                // � ������ ������� �������������� ����� ���� ��� �����
                sprintf(nm, "* unknown id: 0x%X", ptr[-1]);
                named_cell(nm);
                u32 Skip = *(u32 *)ptr;
                ptr += Skip + sizeof(u32);
            }
            else
            {
                ptr += snapsize;
            }
        }
    }
    for(unsigned i = 0; i < tape_infosize; i++)
    {
        if(*(short*)tapeinfo[i].desc == WORD2('*', ' '))
        {
            if(strlen(tapeinfo[i].desc) < _countof(tapeinfo[i].desc) - sizeof(" [UNSUPPORTED]"))
            {
                strcat(tapeinfo[i].desc, " [UNSUPPORTED]");
            }
            else
            {
                strcpy(tapeinfo[i].desc + _countof(tapeinfo[i].desc) - sizeof(" [UNSUPPORTED]"), " [UNSUPPORTED]");
            }
        }
        if(*tapeinfo[i].desc == '-')
        {
            while(strlen(tapeinfo[i].desc) < sizeof(tapeinfo[i].desc) - 1)
            {
                strcat(tapeinfo[i].desc, "-");
            }
        }
    }
    if(tape_imagesize && tape_pulse[tape_image[tape_imagesize - 1]] < Z80FQ / 10)
    {
        // small pause [rqd for 3ddeathchase]
        reserve(1);
        tape_image[tape_imagesize++] = find_pulse(Z80FQ / 10);
    }
    find_tape_sizes();
    return ptr == snbuf + snapsize;
}

unsigned char tape_bit() // used in io.cpp & sound.cpp
{
    __int64 cur = comp.t_states + cpu.t;
    if(cur < comp.tape.edge_change)
    {
        return (unsigned char)comp.tape.tape_bit;
    }
    while(comp.tape.edge_change < cur)
    {
        if(!temp.sndblock)
        {
            unsigned t = (unsigned)(comp.tape.edge_change - comp.t_states - temp.cpu_t_at_frame_start);
            if((int)t >= 0)
            {
                unsigned tape_in = unsigned(conf.sound.micin_vol) & comp.tape.tape_bit;
                comp.tape.sound.update(t, tape_in, tape_in);
            }
        }
        unsigned pulse;
        if(comp.tape.play_pointer == comp.tape.end_of_tape ||
            (pulse = tape_pulse[*comp.tape.play_pointer++]) == UINT32_MAX)
        {
            stop_tape();
        }
        else
        {
            unsigned Flags = (pulse >> 30) & 3;
            switch(Flags)
            {
            case 0: comp.tape.tape_bit ^= -1U; break; // opposite to the current level(make an edge, as usual) - default
            case 1: break; // same as the current level(no edge - prolongs the previous pulse)
            case 2: comp.tape.tape_bit = 0; break; // force low level
            case 3: comp.tape.tape_bit |= -1U; break; // force high level
            }
            comp.tape.edge_change += pulse & tape_pulse_mask;
        }
    }
    return (unsigned char)comp.tape.tape_bit;
}

void fast_tape()
{
    unsigned char *ptr = am_r(cpu.pc);
    unsigned p = *(unsigned*)ptr;
    if(p == WORD4(0x3D, 0x20, 0xFD, 0xA7))
    { // dec a:jr nz,$-1
        cpu.t += ((unsigned char)(cpu.a - 1)) * 16; cpu.a = 1;
        return;
    }
    if((unsigned short)p == WORD2(0x10, 0xFE))
    { // djnz $
        cpu.t += ((unsigned char)(cpu.b - 1)) * 13; cpu.b = 1;
        return;
    }
    if((unsigned short)p == WORD2(0x3D, 0xC2) && (cpu.pc & 0xFFFF) == (p >> 16))
    { // dec a:jp nz,$-1
        cpu.t += ((unsigned char)(cpu.a - 1)) * 14; cpu.a = 1;
        return;
    }
    if((p | WORD4(0, 0, 0, 0xFF)) == WORD4(0x04, 0xC8, 0x3E, 0xFF))
    {
        if(*(unsigned*)(ptr + 4) == WORD4(0xDB, 0xFE, 0x1F, 0xD0) &&
            *(unsigned*)(ptr + 8) == WORD4(0xA9, 0xE6, 0x20, 0x28) && ptr[12] == 0xF3)
        { // find edge (rom routine)
            for(;;)
            {
                if(cpu.b == 0xFF)
                {
                    return;
                }
                if((tape_bit() ? 0x20 : 0) ^ (cpu.c & 0x20))
                {
                    return;
                }
                cpu.b++; cpu.t += 59;
            }
        }
        if(*(unsigned*)(ptr + 4) == WORD4(0xDB, 0xFE, 0xCB, 0x1F) &&
            *(unsigned*)(ptr + 8) == WORD4(0xA9, 0xE6, 0x20, 0x28) && ptr[12] == 0xF3)
        { // rra,ret nc => rr a (popeye2)
            for(;;)
            {
                if(cpu.b == 0xFF)
                {
                    return;
                }
                if((tape_bit() ^ cpu.c) & 0x20)
                {
                    return;
                }
                cpu.b++; cpu.t += 58;
            }
        }
        if(*(unsigned*)(ptr + 4) == WORD4(0xDB, 0xFE, 0x1F, 0x00) &&
            *(unsigned*)(ptr + 8) == WORD4(0xA9, 0xE6, 0x20, 0x28) && ptr[12] == 0xF3)
        { // ret nc nopped (some bleep loaders)
            for(;;)
            {
                if(cpu.b == 0xFF)
                {
                    return;
                }
                if((tape_bit() ^ cpu.c) & 0x20)
                {
                    return;
                }
                cpu.b++; cpu.t += 58;
            }
        }
        if(*(unsigned*)(ptr + 4) == WORD4(0xDB, 0xFE, 0xA9, 0xE6) &&
            *(unsigned*)(ptr + 8) == WORD4(0x40, 0xD8, 0x00, 0x28) && ptr[12] == 0xF3)
        { // no rra, no break check (rana rama)
            for(;;)
            {
                if(cpu.b == 0xFF)
                {
                    return;
                }
                if((tape_bit() ^ cpu.c) & 0x40)
                {
                    return;
                }
                cpu.b++; cpu.t += 59;
            }
        }
        if(*(unsigned*)(ptr + 4) == WORD4(0xDB, 0xFE, 0x1F, 0xA9) &&
            *(unsigned*)(ptr + 8) == WORD4(0xE6, 0x20, 0x28, 0xF4))
        { // ret nc skipped: routine without BREAK checking (ZeroMusic & JSW)
            for(;;)
            {
                if(cpu.b == 0xFF)
                {
                    return;
                }
                if((tape_bit() ^ cpu.c) & 0x20)
                {
                    return;
                }
                cpu.b++; cpu.t += 54;
            }
        }
    }
    if((p | WORD4(0, 0, 0, 0xFF)) == WORD4(0x04, 0x20, 0x03, 0xFF) &&
        ptr[6] == 0xDB && *(unsigned*)(ptr + 8) == WORD4(0x1F, 0xC8, 0xA9, 0xE6) &&
        (*(unsigned*)(ptr + 0x0C) | WORD4(0, 0, 0, 0xFF)) == WORD4(0x20, 0x28, 0xF1, 0xFF))
    { // find edge from Donkey Kong
        for(;;)
        {
            if(cpu.b == 0xFF)
            {
                return;
            }
            if((tape_bit() ^ cpu.c) & 0x20)
            {
                return;
            }
            cpu.b++; cpu.t += 59;
        }
    }
    if((p | WORD4(0, 0xFF, 0, 0)) == WORD4(0x3E, 0xFF, 0xDB, 0xFE) &&
        *(unsigned*)(ptr + 4) == WORD4(0xA9, 0xE6, 0x40, 0x20) &&
        (*(unsigned*)(ptr + 8) | WORD4(0xFF, 0, 0, 0)) == WORD4(0xFF, 0x05, 0x20, 0xF4))
    { // lode runner
        for(;;)
        {
            if(cpu.b == 1)
            {
                return;
            }
            if((tape_bit() ^ cpu.c) & 0x40)
            {
                return;
            }
            cpu.t += 52; cpu.b--;
        }
    }
}

// �������� ����� �� ������ 0x56B (��� ��������� ������������� �����������)
void tape_traps()
{
    u32 pulse;

    // ���� (��� ������ 0x0556, ��� 0x56B �������� ��� ���������):
    // A - �������� ���� (������ ��������� � �������� ������ ���������� �� �����)
    // IX - ����� ��������
    // DE - ����� ������������ �����
    // CF - 0 - verify, 1 - load
    //
    // �����:
    // CF - 0 - ������ ���, 1 - ������ ��������

    /*
    LD_BYTES 0x0556 inc    d                   ; �� ������ CF
                    ex     af, af'             ; A � CF ����������� � af'
                    dec    d
                    di
                    ld     a,0xF
                    out    (0xFE),a
                    ld     hl, 0x53F           ; SA_LD_RET = 0x53F
                    push   hl
                    in     a,(0xFE)
             0x0564 rra
                    and    0x20
                    or     2
             0x056A ld     c,a
    LD_BREAK 0x056B ret    nz
    */

    bool IsLoad = ((cpu.alt.f & CF) != 0);
    u8 Flag = cpu.alt.a;

    // ��� ����������� ������� �� �������� ����� ����� 0x0564 �������� �������������� ������ �����
    // ������ ����� ����������� - ���������� �� Bill Gilbert (������������� ������������� ����� ������� �
    // ������ ������� �� 0x056A)
    if(!comp.tape.play_pointer)
    {
        start_tape();
    }

    do
    {
        if(comp.tape.play_pointer >= comp.tape.end_of_tape ||
            (pulse = tape_pulse[*comp.tape.play_pointer++]) == UINT32_MAX)
        {
            stop_tape();
            return;
        }
    } while((pulse & tape_pulse_mask) > 770);
    comp.tape.play_pointer++;

    // loading flag byte
    cpu.l = 0;
    cpu.h = 0;
    for(unsigned bit = 0x80; bit; bit >>= 1)
    {
        if(comp.tape.play_pointer >= comp.tape.end_of_tape ||
            (pulse = tape_pulse[*comp.tape.play_pointer++]) == UINT32_MAX)
        {
            stop_tape();
            cpu.pc = 0x05DF;
            return;
        }
        cpu.l |= ((pulse & tape_pulse_mask) > 1240) ? bit : 0;
        comp.tape.play_pointer++;
    }

    if(cpu.l != Flag) // ��� ������������ ��������� ����� ���������� ���� ������
    {
        IsLoad = false;
    }

    cpu.h ^= cpu.l;

    // loading data
    do
    {
        cpu.l = 0;
        for(unsigned bit = 0x80; bit; bit >>= 1)
        {
            if(comp.tape.play_pointer >= comp.tape.end_of_tape ||
                (pulse = tape_pulse[*comp.tape.play_pointer++]) == UINT32_MAX)
            {
                stop_tape();
                cpu.pc = 0x05DF;
                return;
            }
            cpu.l |= ((pulse & tape_pulse_mask) > 1240) ? bit : 0;
            comp.tape.play_pointer++;
        }
        cpu.h ^= cpu.l;
        if(IsLoad)
        {
            cpu.DbgMemIf->wm(cpu.ix, cpu.l);
        }
        cpu.ix++;
        cpu.de--;
    } while(cpu.de & 0xFFFF);

    // loading CRC
    cpu.l = 0;
    for(unsigned bit = 0x80; bit; bit >>= 1)
    {
        if(comp.tape.play_pointer >= comp.tape.end_of_tape ||
            (pulse = tape_pulse[*comp.tape.play_pointer++]) == UINT32_MAX)
        {
            stop_tape();
            cpu.pc = 0x05DF;
            return;
        }
        cpu.l |= ((pulse & tape_pulse_mask) > 1240) ? bit : 0;
        comp.tape.play_pointer++;
    }
    cpu.h ^= cpu.l;
    cpu.pc = 0x05DF; // ld a,h / cp 01 / ret
    cpu.bc = 0xB001;

    comp.tape.play_pointer++;
    stop_tape();

    /*cpu.pc=0x0604; // the old one
    unsigned pulse;
    pulse = tape_pulse[*comp.tape.play_pointer++];
    if(pulse == -1) stop_tape();
    else{
      comp.t_states+=pulse;
      comp.tape.edge_change = comp.t_states + cpu.t + 520;

      cpu.b+=(pulse-520)/56;
      cpu.f|=CF;
    }*/
}
