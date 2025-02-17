#include "std.h"

#include "emul.h"
#include "vars.h"
#include "gs.h"
#include "gsz80.h"
#include "vs1001.h"
#include "sdcard.h"
#include "debug.h"

#include "z80/op_noprefix.h"

#ifdef MOD_GSZ80
namespace z80gs
{
unsigned __int64 gs_t_states; // inc'ed with GSCPUINT every gs int
static unsigned __int64 gscpu_t_at_frame_start; // gs_t_states+gscpu.t when spectrum frame begins

Z80INLINE unsigned char rm(unsigned addr);
u8 __fastcall dbgrm(u32 addr);
Z80INLINE void wm(unsigned addr, unsigned char val);
void __fastcall dbgwm(u32 addr, u8 val);
Z80INLINE u8 *am_r(u32 addr);
Z80INLINE unsigned char m1_cycle(Z80 *cpu);
unsigned char in(unsigned port);
void out(unsigned port, unsigned char val);
// FIXME: ������� ������������� ��������� � ����������� �� ����� gscpu.dbgchk
namespace z80fast
{
Z80INLINE unsigned char xm(unsigned addr);
Z80INLINE unsigned char rm(unsigned addr);
Z80INLINE void wm(unsigned addr, unsigned char val);
}

namespace z80dbg
{
Z80INLINE unsigned char xm(unsigned addr);
Z80INLINE unsigned char rm(unsigned addr);
Z80INLINE void wm(unsigned addr, unsigned char val);
}

u8 __fastcall Xm(u32 addr)
{
    return z80gs::z80fast::xm(addr);
}

u8 __fastcall Rm(u32 addr)
{
    return z80gs::z80fast::rm(addr);
}

void __fastcall Wm(u32 addr, u8 val)
{
    z80gs::z80fast::wm(addr, val);
}

u8 __fastcall DbgXm(u32 addr)
{
    return z80gs::z80dbg::xm(addr);
}

u8 __fastcall DbgRm(u32 addr)
{
    return z80gs::z80dbg::rm(addr);
}

void __fastcall DbgWm(u32 addr, u8 val)
{
    z80gs::z80dbg::wm(addr, val);
}
}

u8 *TGsZ80::DirectMem(unsigned addr) const
{
    return z80gs::am_r(addr);
}

unsigned char TGsZ80::m1_cycle()
{
    return z80gs::m1_cycle(this);
}

unsigned char TGsZ80::in(unsigned port)
{
    return z80gs::in(port);
}

void TGsZ80::out(unsigned port, unsigned char val)
{
    z80gs::out(port, val);
}

void TGsZ80::retn()
{
    nmi_in_progress = false;
}

namespace z80gs
{
#include "z80/op_system.h"

const u8 MPAG   = 0x00;
const u8 MPAGEX = 0x10;

// gs
const u8 VOL1 = 0x06;
const u8 VOL2 = 0x07;
const u8 VOL3 = 0x08;
const u8 VOL4 = 0x09;

// nsg
const u8 VOL5 = 0x16;
const u8 VOL6 = 0x17;
const u8 VOL7 = 0x18;
const u8 VOL8 = 0x19;

const u8 DMA_MOD= 0x1b;
const u8 DMA_HAD= 0x1c;
const u8 DMA_MAD= 0x1d;
const u8 DMA_LAD= 0x1e;
const u8 DMA_CST= 0x1f;

const u8 GSCFG0 = 0x0F;

const u8 M_NOROM = 1;
const u8 M_RAMRO = 2;
const u8 M_EXPAG = 8;

const u8 M_8CHANS = 0x04;
const u8 M_PAN4CH = 0x40;

static u8 *gsbankr[4] = { ROM_GS_M, GSRAM_M + 3 * PAGE, ROM_GS_M, ROM_GS_M + PAGE }; // bank pointers for read
static u8 *gsbankw[4] = { TRASH_M, GSRAM_M + 3 * PAGE, TRASH_M, TRASH_M }; // bank pointers for write

static unsigned gs_v[8];
static unsigned char gsvol[8], gsbyte[8]{ 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 };
static unsigned char gsdata_in, gsdata_out, gspage = 0;
static unsigned char gscmd, gsstat;

static bool GsSilent = false; // ������� ������� ��������� �� ���� �������

static unsigned long long mult_gs, mult_gs2;

// ngs
static u8 ngs_mode_pg1; // page ex number
static u8 ngs_cfg0;
static u8 ngs_s_ctrl;
static u8 ngs_s_stat;
static u8 SdRdVal, SdRdValNew;
static u8 ngs_dmamod;

static u8 ngs_chn_mask = 3;
static int vol_div = 256;

static bool SdDataAvail = false;

const unsigned GSINTFQ = 37500; // hz
static unsigned GSCPUFQI;
const unsigned GSCPUINT = GSCPUFQ/GSINTFQ;
const int MULT_GS_SHIFT = 12; // cpu tick -> gscpu tick precision
void flush_gs_z80();
void reset();
void nmi();

void apply_gs()
{
   GSCPUFQI = GSCPUFQ / conf.intfq;
   mult_gs = (temp.snd_frame_ticks << MULT_C)/GSCPUFQI;
   mult_gs2 = (GSCPUFQI<<MULT_GS_SHIFT)/conf.frame;

   make_gs_volume();
}

static inline void flush_gs_sound()
{
   if (temp.sndblock)
       return;

  unsigned l,r;         //!psb
  l = gs_v[0] + gs_v[1];    //!psb
  r = gs_v[2] + gs_v[3];    //!psb

  // ngs 8ch
  if(ngs_cfg0 & M_8CHANS)
  {
      l += gs_v[4] + gs_v[5];
      r += gs_v[6] + gs_v[7];
  }

   unsigned lv, rv;
   lv = (l + r/2) / 2;
   rv = (r + l/2) / 2;

/*
   if(gs_t_states < gscpu_t_at_frame_start)
   {
       printf("err: gs_t_states = %lld, gscpu_t_at_frame_start=%lld, gscpu.t = %u, t = %lld\n",
           gs_t_states, gscpu_t_at_frame_start, gscpu.t, ((gs_t_states + gscpu.t)  - gscpu_t_at_frame_start));
       fflush(stdout);
   }
*/

//   assert(gs_t_states >= gscpu_t_at_frame_start);

   sound.update(unsigned((gs_t_states + gscpu.t) - gscpu_t_at_frame_start), lv, rv);     //!psb
}

void init_gs_frame()
{
//   printf("%s, gs_t_states = %lld, gscpu.t = %u\n", __FUNCTION__, gs_t_states, gscpu.t);
   assert(gscpu.t < LONG_MAX);
   gscpu_t_at_frame_start = gs_t_states + gscpu.t;
   sound.start_frame();
}

void flush_gs_frame()
{
   flush_gs_z80();

/*   printf("%s, gs_t_states = %lld, gscpu_t_at_frame_start = %lld, gscpu.t = %u, t = %lld\n",
       __FUNCTION__, gs_t_states, gscpu_t_at_frame_start, gscpu.t,
       ((gs_t_states + gscpu.t)  - gscpu_t_at_frame_start));
*/
   sound.end_frame(unsigned((gs_t_states + gscpu.t) - gscpu_t_at_frame_start));

   for(int ch = 0; ch < 8; ch++)
   {
       gsleds[ch].level = abs(int(gsbyte[ch] - 0x80) * gsvol[ch]) / ((128 * 63) / 15);
       gsleds[ch].attrib = 0x0F;
   }
}

void out_gs(unsigned port, u8 val)
{
   port &= 0xFF;

   switch(port)
   {
   case 0x33: // GSCTR
       if(val & 0x80) // reset
       {
           reset();
           flush_gs_z80();
           return;
       }
       if(val & 0x40) // nmi
       {
           nmi();
           flush_gs_z80();
           return;
       }
       return;
   }

   flush_gs_z80();
   switch(port)
   {
   case 0xB3: // GSDAT
        gsdata_out = val;
        gsstat |= 0x80;
   break;
   case 0xBB: // GSCOM
       gscmd = val;
       gsstat |= 0x01;
   break;
   }
}

u8 in_gs(unsigned port)
{
   flush_gs_z80();
   port &= 0xFF;
   switch(port)
   {
   case 0xB3: gsstat &= 0x7F; return gsdata_in;
   case 0xBB: return gsstat | 0x7E;
   }
   return 0xFF;
}

static void gs_byte_to_dac(unsigned addr, unsigned char byte)
{
    if(GsSilent)
    {
        return;
    }

   flush_gs_sound();
   unsigned chan = (addr>>8) & ngs_chn_mask;
   gsbyte[chan] = byte;
//   gs_v[chan] = (gsbyte[chan] * gs_vfx[gsvol[chan]]) >> 8;
   gs_v[chan] = unsigned(((signed char)(gsbyte[chan]-0x80) * (signed)gs_vfx[gsvol[chan]]) / vol_div + int(gs_vfx[33])); //!psb
}

static inline void stepi();

Z80INLINE u8 *am_r(u32 addr)
{
   return &gsbankr[(addr >> 14U) & 3][addr & (PAGE-1)];
}

namespace z80fast
{
   #include "gsz80.inl"
}
namespace z80dbg
{
   #define Z80_DBG
   #include "gsz80.inl"
   #undef Z80_DBG
}

u8 *__fastcall MemDbg(u32 addr);

u8 *__fastcall MemDbg(u32 addr)
{
    return am_r(addr);
}

u8 __fastcall dbgrm(u32 addr)
{
    return z80dbg::rm(addr);
}

void __fastcall dbgwm(u32 addr, u8 val)
{
    *am_r(addr) = val;
}

void __cdecl BankNames(int i, char *Name)
{
    if(gsbankr[i] < GSRAM_M + MAX_GSRAM_PAGES*PAGE)
        sprintf(Name, "RAM%2lX", ULONG((gsbankr[i] - GSRAM_M) / PAGE));
    if((gsbankr[i] - ROM_GS_M) < PAGE*MAX_GSROM_PAGES)
        sprintf(Name, "ROM%2lX", ULONG((gsbankr[i] - ROM_GS_M) / PAGE));
}


Z80INLINE unsigned char m1_cycle(Z80 *cpu)
{
   cpu->r_low++; cpu->t += 4;
   return cpu->MemIf->xm(cpu->pc++);
}

static inline void UpdateMemMapping()
{
    bool RamRo = (ngs_cfg0 & M_RAMRO) != 0;
    bool NoRom = (ngs_cfg0 & M_NOROM) != 0;
    if(NoRom)
    {
        gsbankr[0] = gsbankw[0] = GSRAM_M;
        gsbankr[1] = gsbankw[1] = GSRAM_M + 3 * PAGE;
        gsbankr[2] = gsbankw[2] = GSRAM_M + gspage * PAGE;
        gsbankr[3] = gsbankw[3] = GSRAM_M + ngs_mode_pg1 * PAGE;

        if(RamRo)
        {
            if(gspage == 0 || gspage == 1) // RAM0 or RAM1 in PG2
               gsbankw[2] = TRASH_M;
            if(ngs_mode_pg1 == 0 || ngs_mode_pg1 == 1) // RAM0 or RAM1 in PG3
               gsbankw[3] = TRASH_M;
        }
    }
    else
    {
        gsbankw[0] = gsbankw[2] = gsbankw[3] = TRASH_M;
        gsbankr[0] = ROM_GS_M;                                  // ROM0
        gsbankr[1] = gsbankw[1] = GSRAM_M + 3 * PAGE;           // RAM3
        gsbankr[2] = ROM_GS_M +  (gspage & 0x1F) * PAGE;        // ROMn
        gsbankr[3] = ROM_GS_M +  (ngs_mode_pg1 & 0x1F) * PAGE;  // ROMm
    }
}

void out(unsigned port, unsigned char val)
{
//   printf(__FUNCTION__" port=0x%X, val=0x%X\n", (port & 0xFF), val);
   switch (port & 0xFF)
   {
      case MPAG:
      {
         bool ExtMem = (ngs_cfg0 & M_EXPAG) != 0;

         gspage = rol8(val, 1) & temp.gs_ram_mask & (ExtMem ? 0xFF : 0xFE);

         if(!ExtMem)
             ngs_mode_pg1 = (rol8(val, 1) & temp.gs_ram_mask) | 1;
//         printf(__FUNCTION__"->GSPG, %X, Ro=%d, NoRom=%d, Ext=%d\n", gspage, RamRo, NoRom, ExtMem);
         UpdateMemMapping();
         return;
      }
      case 0x02: gsstat &= 0x7F; return;
      case 0x03: gsstat |= 0x80; gsdata_in = val; return;
      case 0x05: gsstat &= 0xFE; return;

      case VOL1: case VOL2: case VOL3: case VOL4:
      case VOL5: case VOL6: case VOL7: case VOL8:
      {
         if((port & 0x10) && !(ngs_cfg0 & (M_8CHANS | M_PAN4CH)))
         {
             return;
         }

         val &= 0x3F;

         if(GsSilent && val == 0)
         {
             return;
         }

         flush_gs_sound();
         unsigned chan = ((port & 0x10) >> 2U) + (port & 0x0F)-6;
         gsvol[chan] = val;

         auto Chans = (ngs_cfg0 & M_8CHANS) ? 8 : 4;

         auto Silent = true;
         for(auto Ch = 0; Ch < Chans; Ch++)
         {
             if(gsvol[Ch] != 0)
             {
                 Silent = false;
                 break;
             }
         }

         GsSilent = Silent;

//         gs_v[chan] = (gsbyte[chan] * gs_vfx[gsvol[chan]]) >> 8;
         gs_v[chan] = unsigned(((signed char)(gsbyte[chan]-0x80) * (signed)gs_vfx[gsvol[chan]]) /vol_div + int(gs_vfx[33])); //!psb
         return;
      }
      case 0x0A: gsstat = u8((gsstat & 0x7F) | (gspage << 7)); return;
      case 0x0B: gsstat = u8((gsstat & 0xFE) | ((gsvol[0] >> 5) & 1)); return;

   }

//   printf(__FUNCTION__" port=0x%X, val=0x%X\n", (port & 0xFF), val);
   // ngs
   switch (port & 0xFF)
   {
      case GSCFG0:
      {
          ngs_cfg0 = val & 0x3F;
//          printf(__FUNCTION__"->GSCFG0, %X, Ro=%d, NoRom=%d, Ext=%d\n", ngs_cfg0, RamRo, NoRom, ExtMem);

          if(ngs_cfg0 & M_8CHANS)
          {
              ngs_chn_mask = 0x7;
              vol_div = 512;
          }
          else
          {
              ngs_chn_mask = 0x3;
              vol_div = 256;
          }
          UpdateMemMapping();
      }
      break;

      case MPAGEX:
      {
//          assert((ngs_cfg0 & M_EXPAG) != 0);
          ngs_mode_pg1 = rol8(val, 1) & temp.gs_ram_mask;
          UpdateMemMapping();
      }
      break;

      case S_CTRL:
//          printf(__FUNCTION__"->S_CTRL\n");
          if(val & 0x80)
              ngs_s_ctrl |= (val & 0xF);
          else
              ngs_s_ctrl &= ~(val & 0xF);

          if(!(ngs_s_ctrl & _MPXRS))
              Vs1001.Reset();

          Vs1001.SetNcs((ngs_s_ctrl & _MPNCS) != false);
      break;

      case MC_SEND:
          Vs1001.WrCmd(val);
      break;

      case MD_SEND:
          Vs1001.Wr(val);
      break;

      case SD_SEND:
          SdCard.Wr(val);
          SdRdValNew = SdCard.Rd();
          SdDataAvail = true;
      break;

      case DMA_MOD:
          ngs_dmamod = val;
      break;

      case DMA_HAD:
          if (ngs_dmamod == 1)
              temp.gsdmaaddr = (temp.gsdmaaddr&0x0000ffff)|(unsigned(val & 0x1F)<<16); // 5bit only
      break;

      case DMA_MAD:
          if (ngs_dmamod == 1)
              temp.gsdmaaddr = (temp.gsdmaaddr&0x001f00ff)|(unsigned(val)<<8);
      break;

      case DMA_LAD:
          if (ngs_dmamod == 1)
              temp.gsdmaaddr = (temp.gsdmaaddr&0x001fff00)|val;
      break;

      case DMA_CST:
          if (ngs_dmamod == 1)
              temp.gsdmaon = val;
      break;
   }
}

unsigned char in(unsigned port)
{
   switch (port & 0xFF)
   {
      case 0x01: return gscmd;
      case 0x02: gsstat &= 0x7F; return gsdata_out;
      case 0x03: gsstat |= 0x80; gsdata_in = 0xFF; return 0xFF;
      case 0x04: return gsstat;
      case 0x05: gsstat &= 0xFE; return 0xFF;
      case 0x0A: gsstat = u8((gsstat & 0x7F) | (gspage << 7)); return 0xFF;
      case 0x0B: gsstat = u8((gsstat & 0xFE) | (gsvol[0] >> 5)); return 0xFF;


      // ngs
      case GSCFG0:
          return ngs_cfg0;
      case S_CTRL:
          return ngs_s_ctrl;

      case S_STAT:
          if(Vs1001.GetDreq())
              ngs_s_stat |= _MPDRQ;
          else
              ngs_s_stat &= ~_MPDRQ;
          return ngs_s_stat;

      case MC_READ:
          return Vs1001.Rd();

      case SD_READ:
      {
          u8 Tmp = SdRdVal;
          SdRdVal = SdRdValNew;
          return Tmp;
      }
      case SD_RSTR:
          if(SdDataAvail)
          {
              SdDataAvail = false;
              return SdRdValNew;
          }
          return SdCard.Rd();

      case DMA_MOD:
          return ngs_dmamod;

      case DMA_HAD:
          if (ngs_dmamod == 1)
              return (temp.gsdmaaddr>>16) & 0x1F; // 5bit only
      break;

      case DMA_MAD:
          if (ngs_dmamod == 1)
              return (temp.gsdmaaddr>>8) & 0xFF;
      break;

      case DMA_LAD:
          if (ngs_dmamod == 1)
              return temp.gsdmaaddr & 0xFF;
      break;

      case DMA_CST:
          if (ngs_dmamod == 1)
              return temp.gsdmaon;
      break;
   }
   return 0xFF;
}

//#include "z80/cmd.cpp"

static inline void stepi()
{
   u8 opcode = m1_cycle(&gscpu);
   (::normal_opcode[opcode])(&gscpu);
}

void Z80FAST step();

void Z80FAST step()
{
    stepi();
}

void flush_gs_z80()
{
   if(gscpu.dbgchk)
   {
       gscpu.SetDbgMemIf();
       z80gs::z80dbg::z80loop();
   }
   else
   {
       gscpu.SetFastMemIf();
       z80gs::z80fast::z80loop();
   }
}

__int64 __cdecl delta()
{
    return i64(gs_t_states) + gscpu.t - gscpu.debug_last_t;
}

void __cdecl SetLastT()
{
   gscpu.debug_last_t = i64(gs_t_states + gscpu.t);
}

void nmi()
{
   gscpu.sp -= 2;
   z80fast::wm(gscpu.sp, gscpu.pcl);
   z80fast::wm(gscpu.sp+1, gscpu.pch);
   gscpu.pc = 0x66;
   gscpu.iff1 = gscpu.halted = 0;
}

void reset()
{
   gscpu.reset();
   gsbankr[0] = ROM_GS_M; gsbankr[1] = GSRAM_M + 3 * PAGE; gsbankr[2] = ROM_GS_M; gsbankr[3] = ROM_GS_M + PAGE;
   gsbankw[0] = TRASH_M; gsbankw[1] = GSRAM_M + 3 * PAGE; gsbankw[2] = TRASH_M; gsbankw[3] = TRASH_M;

   gscpu.t = 0;
   gs_t_states = 0;
   gscpu_t_at_frame_start = 0;
   ngs_cfg0 = 0;
   ngs_s_stat = u8(u8(rdtsc() & ~7U) | _SDDET | _MPDRQ);
   ngs_s_ctrl = u8(u8(rdtsc() & ~0xFU) | _SDNCS);
   SdRdVal = SdRdValNew = 0xFF;
   SdDataAvail = false;
   Vs1001.Reset();

   for(unsigned i = 0; i < 8; i++)
   {
       gsbyte[i] = 0x80;
       gsvol[i] = 0;
       gs_v[i] = 0;
   }

   ngs_mode_pg1 = 1;
   ngs_dmamod = 0;
   temp.gsdmaaddr = 0;
   temp.gsdmaon = 0;
   SdCard.Reset();
}

} // end of z80gs namespace
#endif
