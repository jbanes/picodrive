/*
 * PicoDrive
 * (c) Copyright Dave, 2004
 * (C) notaz, 2006-2009
 * (C) kub, 2020
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */

#include "pico_int.h"
#define NEED_DMA_SOURCE
#include "memory.h"

extern const unsigned char  hcounts_32[], hcounts_40[];
extern const unsigned char  vdpcyc2sl_32_bl[], vdpcyc2sl_40_bl[];
extern const unsigned char  vdpcyc2sl_32[], vdpcyc2sl_40[];
extern const unsigned short vdpsl2cyc_32_bl[], vdpsl2cyc_40_bl[];
extern const unsigned short vdpsl2cyc_32[], vdpsl2cyc_40[];

static int blankline;           // display disabled for this line

unsigned SATaddr, SATmask;      // VRAM addr of sprite attribute table

int (*PicoDmaHook)(unsigned int source, int len, unsigned short **base, unsigned int *mask) = NULL;


/* VDP FIFO implementation
 * 
 * fifo_slot: last slot executed in this scanline
 * fifo_cnt: #slots remaining for active FIFO write (#writes<<#bytep)
 * fifo_total: #total FIFO entries pending
 * fifo_data: last values transferred through fifo
 * fifo_queue: fifo transfer queue (#writes, flags)
 *
 * FIFO states:		empty	total=0
 *			inuse	total>0 && total<4
 *			full	total==4
 *			wait	total>4
 * Conditions:
 * fifo_slot is always behind slot2cyc[cycles]. Advancing it beyond cycles
 * implies blocking the 68k up to that slot.
 *
 * A FIFO write goes to the end of the FIFO queue, but DMA running in background
 * is always the last queue entry (transfers by CPU intervene and come 1st).
 * There can be more pending writes than FIFO slots, but the CPU will be blocked
 * until FIFO level (without background DMA) <= 4.
 * This is only about correct timing, data xfer must be handled by the caller.
 * Blocking the CPU means burning cycles via SekCyclesBurn*(), which is to be
 * executed by the caller.
 *
 * FIFOSync "executes" FIFO write slots up to the given cycle in the current
 * scanline. A queue entry completely executed is removed from the queue.
 * FIFOWrite pushes writes to the transfer queue. If it's a blocking write, 68k
 * is blocked if more than 4 FIFO writes are pending.
 * FIFORead executes a 68k read. 68k is blocked until the next transfer slot.
 */

// NB code assumes fifo_* arrays have size 2^n
static struct VdpFIFO { // XXX this must go into save file!
  // last transferred FIFO data, ...x = index  XXX currently only CPU
  unsigned short fifo_data[4], fifo_dx;

  // queued FIFO transfers, ...x = index, ...l = queue length
  // each entry has 2 values: [n]>>3 = #writes, [n]&7 = flags (FQ_*)
  unsigned int fifo_queue[8], fifo_qx, fifo_ql;
  unsigned int fifo_total;    // total# of pending FIFO entries (w/o BGDMA)

  unsigned short fifo_slot;   // last executed slot in current scanline
  unsigned short fifo_maxslot;// #slots in scanline

  const unsigned char *fifo_cyc2sl;
  const unsigned short *fifo_sl2cyc;
} VdpFIFO;

enum { FQ_BYTE = 1, FQ_BGDMA = 2, FQ_FGDMA = 4 }; // queue flags, NB: BYTE = 1!

// do the FIFO math
static __inline int AdvanceFIFOEntry(struct VdpFIFO *vf, struct PicoVideo *pv, int slots)
{
  int l = slots, b = vf->fifo_queue[vf->fifo_qx] & FQ_BYTE;
  int cnt = pv->fifo_cnt;

  // advance currently active FIFO entry
  if (l > cnt)
    l = cnt;
  if (!(vf->fifo_queue[vf->fifo_qx] & FQ_BGDMA))
    vf->fifo_total -= ((cnt & b) + l) >> b;
  cnt -= l;

  // if entry has been processed...
  if (cnt == 0) {
    // remove entry from FIFO
    if (vf->fifo_ql)
      vf->fifo_qx = (vf->fifo_qx+1) & 7, vf->fifo_ql --;
    // start processing for next entry if there is one
    if (vf->fifo_ql) {
      b = vf->fifo_queue[vf->fifo_qx] & FQ_BYTE;
      cnt = (vf->fifo_queue[vf->fifo_qx] >> 3) << b;
    } else { // FIFO empty
      pv->status &= ~PVS_FIFORUN;
      vf->fifo_total = 0;
    }
  }

  pv->fifo_cnt = cnt;
  return l;
}

static __inline void SetFIFOState(struct VdpFIFO *vf, struct PicoVideo *pv)
{
  unsigned int st = pv->status, cmd = pv->command;
  // release CPU and terminate DMA if FIFO isn't blocking the 68k anymore
  if (vf->fifo_total <= 4) {
    st &= ~PVS_CPUWR;
    if (!(st & (PVS_DMABG|PVS_DMAFILL))) {
      st &= ~SR_DMA;
      cmd &= ~0x80;
    }
  }
  if (pv->fifo_cnt == 0) {
    st &= ~PVS_CPURD;
    // terminate DMA if applicable
    if (!(st & (PVS_FIFORUN|PVS_DMAFILL))) {
      st &= ~(SR_DMA|PVS_DMABG);
      cmd &= ~0x80;
    }
  }
  pv->status = st;
  pv->command = cmd;
}

// sync FIFO to cycles
void PicoVideoFIFOSync(int cycles)
{
  struct VdpFIFO *vf = &VdpFIFO;
  struct PicoVideo *pv = &Pico.video;
  int slots, done;

  // calculate #slots since last executed slot
  slots = vf->fifo_cyc2sl[cycles>>1] - vf->fifo_slot;

  // advance FIFO queue by #done slots
  done = slots;
  while (done > 0 && pv->fifo_cnt) {
    int l = AdvanceFIFOEntry(vf, pv, done);
    vf->fifo_slot += l;
    done -= l;
  }

  if (done != slots)
    SetFIFOState(vf, pv);
}

// drain FIFO, blocking 68k on the way. FIFO must be synced prior to drain.
static int PicoVideoFIFODrain(int level, int cycles, int bgdma)
{
  struct VdpFIFO *vf = &VdpFIFO;
  struct PicoVideo *pv = &Pico.video;
  unsigned ocyc = cycles;
  int burn = 0;
//int osl = fifo_slot;

  // process FIFO entries until low level is reached
  while (vf->fifo_slot < vf->fifo_maxslot && cycles < 488 &&
         ((vf->fifo_total > level) | (vf->fifo_queue[vf->fifo_qx] & bgdma))) {
    int b = vf->fifo_queue[vf->fifo_qx] & FQ_BYTE;
    int cnt = bgdma ? pv->fifo_cnt : ((vf->fifo_total-level)<<b) - (pv->fifo_cnt&b);
    int slot = (pv->fifo_cnt<cnt ? pv->fifo_cnt:cnt) + vf->fifo_slot;

    if (slot > vf->fifo_maxslot) {
      // target slot in later scanline, advance to eol
      slot = vf->fifo_maxslot;
      cycles = 488;
    } else {
      // advance FIFO to target slot and CPU to cycles at that slot
      cycles = vf->fifo_sl2cyc[slot]<<1;
    }
    if (slot > vf->fifo_slot) {
      AdvanceFIFOEntry(vf, pv, slot - vf->fifo_slot);
      vf->fifo_slot = slot;
    }
  }
  if (cycles > ocyc)
    burn = cycles - ocyc;

  SetFIFOState(vf, pv);

  return burn;
}

// read VDP data port
static int PicoVideoFIFORead(void)
{
  struct VdpFIFO *vf = &VdpFIFO;
  struct PicoVideo *pv = &Pico.video;
  int lc = SekCyclesDone()-Pico.t.m68c_line_start;
  int burn = 0;

  if (pv->fifo_cnt) {
    PicoVideoFIFOSync(lc);
    // advance FIFO and CPU until FIFO is empty
    burn = PicoVideoFIFODrain(0, lc, FQ_BGDMA);
    lc += burn;
  }

  if (pv->fifo_cnt)
    pv->status |= PVS_CPURD; // target slot is in later scanline
  else {
    // use next VDP access slot for reading, block 68k until then
    vf->fifo_slot = vf->fifo_cyc2sl[lc>>1] + 1;
    burn += (vf->fifo_sl2cyc[vf->fifo_slot]<<1) - lc;
  }

  return burn;
}
 
// write VDP data port
int PicoVideoFIFOWrite(int count, int flags, unsigned sr_mask,unsigned sr_flags)
{
  struct VdpFIFO *vf = &VdpFIFO;
  struct PicoVideo *pv = &Pico.video;
  int lc = SekCyclesDone()-Pico.t.m68c_line_start;
  int burn = 0;

  if (pv->fifo_cnt)
    PicoVideoFIFOSync(lc);
  pv->status = (pv->status & ~sr_mask) | sr_flags;

  if (count && vf->fifo_ql < 8) {
    // determine queue position for entry
    int x = (vf->fifo_qx + vf->fifo_ql - 1) & 7;
    if (unlikely(vf->fifo_ql && (vf->fifo_queue[x] & FQ_BGDMA))) {
      // CPU FIFO writes have priority over a background DMA Fill/Copy
      // XXX if interrupting a DMA fill, fill data changes
      if (x == vf->fifo_qx) { // overtaking to queue head?
        int f = vf->fifo_queue[x] & 7;
        vf->fifo_queue[(x+1) & 7] = (pv->fifo_cnt >> (f & FQ_BYTE) << 3) | f;
        pv->status &= ~PVS_FIFORUN;
      } else
        // push background DMA back
        vf->fifo_queue[(x+1) & 7] = vf->fifo_queue[x];
      x = (x-1) & 7;
    }

    if ((pv->status & PVS_FIFORUN) && (vf->fifo_queue[x] & 7) == flags) {
      // amalgamate entries if of same type
      vf->fifo_queue[x] += (count << 3);
      if (x == vf->fifo_qx)
        pv->fifo_cnt += count << (flags & FQ_BYTE);
    } else {
      // create new xfer queue entry
      vf->fifo_ql ++;
      x = (x+1) & 7;
      vf->fifo_queue[x] = (count << 3) | flags;
    }

    // update FIFO state if it was empty
    if (!(pv->status & PVS_FIFORUN)) {
      vf->fifo_slot = vf->fifo_cyc2sl[(lc+8)>>1]; // FIFO latency ~3 vdp slots
      pv->status |= PVS_FIFORUN;
      pv->fifo_cnt = count << (flags & FQ_BYTE);
    }
    if (!(flags & FQ_BGDMA))
      vf->fifo_total += count;
  }

  // if CPU is waiting for the bus, advance CPU and FIFO until bus is free
  if (pv->status & PVS_CPUWR)
    burn = PicoVideoFIFODrain(4, lc, 0);

  return burn;
}

// at HINT, advance FIFO to new scanline
int PicoVideoFIFOHint(void)
{
  struct VdpFIFO *vf = &VdpFIFO;
  struct PicoVideo *pv = &Pico.video;
  int burn = 0;

  // reset slot to start of scanline
  vf->fifo_slot = 0;
 
  // if CPU is waiting for the bus, advance CPU and FIFO until bus is free
  if (pv->status & PVS_CPUWR)
    burn = PicoVideoFIFOWrite(0, 0, 0, 0);
  else if (pv->status & PVS_CPURD)
    burn = PicoVideoFIFORead();

  return burn;
}

// switch FIFO mode between active/inactive display
void PicoVideoFIFOMode(int active, int h40)
{
  static const unsigned char *vdpcyc2sl[2][2] =
        { {vdpcyc2sl_32_bl, vdpcyc2sl_40_bl} , {vdpcyc2sl_32, vdpcyc2sl_40} };
  static const unsigned short *vdpsl2cyc[2][2] =
        { {vdpsl2cyc_32_bl, vdpsl2cyc_40_bl} , {vdpsl2cyc_32, vdpsl2cyc_40} };

  struct VdpFIFO *vf = &VdpFIFO;
  struct PicoVideo *pv = &Pico.video;
  int lc = SekCyclesDone() - Pico.t.m68c_line_start;
  active = active && !(pv->status & PVS_VB2);

  if (vf->fifo_maxslot)
    PicoVideoFIFOSync(lc);

  vf->fifo_cyc2sl = vdpcyc2sl[active][h40];
  vf->fifo_sl2cyc = vdpsl2cyc[active][h40];
  // recalculate FIFO slot for new mode
  vf->fifo_slot = vf->fifo_cyc2sl[lc>>1]-1;
  vf->fifo_maxslot = vf->fifo_cyc2sl[488>>1];
}


// VDP memory rd/wr

static __inline void AutoIncrement(void)
{
  Pico.video.addr=(unsigned short)(Pico.video.addr+Pico.video.reg[0xf]);
  if (Pico.video.addr < Pico.video.reg[0xf]) Pico.video.addr_u ^= 1;
}

static NOINLINE void VideoWriteVRAM128(u32 a, u16 d)
{
  // nasty
  u32 b = ((a & 2) >> 1) | ((a & 0x400) >> 9) | (a & 0x3FC) | ((a & 0x1F800) >> 1);

  ((u8 *)PicoMem.vram)[b] = d;
  if (!((u16)(b^SATaddr) & SATmask))
    Pico.est.rendstatus |= PDRAW_DIRTY_SPRITES;

  if (!((u16)(a^SATaddr) & SATmask))
    UpdateSAT(a, d);
}

static void VideoWrite(u16 d)
{
  unsigned int a = Pico.video.addr;

  switch (Pico.video.type)
  {
    case 1: if (a & 1)
              d = (u16)((d << 8) | (d >> 8));
            a |= Pico.video.addr_u << 16;
            VideoWriteVRAM(a, d);
            break;
    case 3: if (PicoMem.cram [(a >> 1) & 0x3f] != d) Pico.m.dirtyPal = 1;
            PicoMem.cram [(a >> 1) & 0x3f] = d & 0xeee; break;
    case 5: PicoMem.vsram[(a >> 1) & 0x3f] = d & 0x7ff; break;
    case 0x81:
            a |= Pico.video.addr_u << 16;
            VideoWriteVRAM128(a, d);
            break;
    //default:elprintf(EL_ANOMALY, "VDP write %04x with bad type %i", d, Pico.video.type); break;
  }

  AutoIncrement();
}

static unsigned int VideoRead(void)
{
  unsigned int a, d = VdpFIFO.fifo_data[(VdpFIFO.fifo_dx+1)&3];

  a=Pico.video.addr; a>>=1;

  SekCyclesBurnRun(PicoVideoFIFORead());
  switch (Pico.video.type)
  {
    case 0: d=PicoMem.vram [a & 0x7fff]; break;
    case 8: d=PicoMem.cram [a & 0x003f] | (d & ~0x0eee); break;
    case 4: if ((a & 0x3f) >= 0x28) a = 0;
            d=PicoMem.vsram [a & 0x003f] | (d & ~0x07ff); break;
    case 12:a=PicoMem.vram [a & 0x7fff]; if (Pico.video.addr&1) a >>= 8;
            d=(a & 0x00ff) | (d & ~0x00ff); break;
    default:elprintf(EL_ANOMALY, "VDP read with bad type %i", Pico.video.type); break;
  }

  AutoIncrement();
  return d;
}

// VDP DMA

static int GetDmaLength(void)
{
  struct PicoVideo *pvid=&Pico.video;
  int len=0;
  // 16-bit words to transfer:
  len =pvid->reg[0x13];
  len|=pvid->reg[0x14]<<8;
  len = ((len - 1) & 0xffff) + 1;
  return len;
}

static void DmaSlow(int len, unsigned int source)
{
  u32 inc = Pico.video.reg[0xf];
  u32 a = Pico.video.addr | (Pico.video.addr_u << 16);
  u16 *r, *base = NULL;
  u32 mask = 0x1ffff;

  elprintf(EL_VDPDMA, "DmaSlow[%i] %06x->%04x len %i inc=%i blank %i [%u] @ %06x",
    Pico.video.type, source, a, len, inc, (Pico.video.status&SR_VB)||!(Pico.video.reg[1]&0x40),
    SekCyclesDone(), SekPc);

  SekCyclesBurnRun(PicoVideoFIFOWrite(len, FQ_FGDMA | (Pico.video.type == 1),
                              PVS_DMABG, SR_DMA | PVS_CPUWR));

  if ((source & 0xe00000) == 0xe00000) { // Ram
    base = (u16 *)PicoMem.ram;
    mask = 0xffff;
  }
  else if (PicoIn.AHW & PAHW_MCD)
  {
    u8 r3 = Pico_mcd->s68k_regs[3];
    elprintf(EL_VDPDMA, "DmaSlow CD, r3=%02x", r3);
    if (source < 0x20000) { // Bios area
      base = (u16 *)Pico_mcd->bios;
    } else if ((source & 0xfc0000) == 0x200000) { // Word Ram
      if (!(r3 & 4)) { // 2M mode
        base = (u16 *)(Pico_mcd->word_ram2M + (source & 0x20000));
      } else {
        if (source < 0x220000) { // 1M mode
          int bank = r3 & 1;
          base = (u16 *)(Pico_mcd->word_ram1M[bank]);
        } else {
          DmaSlowCell(source - 2, a, len, inc);
          return;
        }
      }
      source -= 2;
    } else if ((source & 0xfe0000) == 0x020000) { // Prg Ram
      base = (u16 *)Pico_mcd->prg_ram_b[r3 >> 6];
      source -= 2; // XXX: test
    }
  }
  else
  {
    // if we have DmaHook, let it handle ROM because of possible DMA delay
    u32 source2;
    if (PicoDmaHook && (source2 = PicoDmaHook(source, len, &base, &mask)))
      source = source2;
    else // Rom
      base = m68k_dma_source(source);
  }
  if (!base) {
    elprintf(EL_VDPDMA|EL_ANOMALY, "DmaSlow[%i] %06x->%04x: invalid src", Pico.video.type, source, a);
    return;
  }

  // operate in words
  source >>= 1;
  mask >>= 1;

  switch (Pico.video.type)
  {
    case 1: // vram
      r = PicoMem.vram;
      if (inc == 2 && !(a & 1) && (a & ~0xffff) == ((a + len*2-1) & ~0xffff) &&
          ((a >= SATaddr+0x280) | ((a + len*2-1) < SATaddr)) &&
          (source & ~mask) == ((source + len-1) & ~mask))
      {
        // most used DMA mode
        memcpy((char *)r + a, base + (source & mask), len * 2);
        a += len * 2;
        break;
      }
      for(; len; len--)
      {
        u16 d = base[source++ & mask];
        if(a & 1) d=(d<<8)|(d>>8);
        VideoWriteVRAM(a, d);
        // AutoIncrement
        a = (a+inc) & ~0x20000;
      }
      break;

    case 3: // cram
      Pico.m.dirtyPal = 1;
      r = PicoMem.cram;
      for (; len; len--)
      {
        r[(a / 2) & 0x3f] = base[source++ & mask] & 0xeee;
        // AutoIncrement
        a = (a+inc) & ~0x20000;
      }
      break;

    case 5: // vsram
      r = PicoMem.vsram;
      for (; len; len--)
      {
        r[(a / 2) & 0x3f] = base[source++ & mask] & 0x7ff;
        // AutoIncrement
        a = (a+inc) & ~0x20000;
      }
      break;

    case 0x81: // vram 128k
      for(; len; len--)
      {
        u16 d = base[source++ & mask];
        VideoWriteVRAM128(a, d);
        // AutoIncrement
        a = (a+inc) & ~0x20000;
      }
      break;

    default:
      if (Pico.video.type != 0 || (EL_LOGMASK & EL_VDPDMA))
        elprintf(EL_VDPDMA|EL_ANOMALY, "DMA with bad type %i", Pico.video.type);
      break;
  }
  // remember addr
  Pico.video.addr = a;
  Pico.video.addr_u = a >> 16;
}

static void DmaCopy(int len)
{
  u32 a = Pico.video.addr | (Pico.video.addr_u << 16);
  u8 *vr = (u8 *)PicoMem.vram;
  u8 inc = Pico.video.reg[0xf];
  int source;
  elprintf(EL_VDPDMA, "DmaCopy len %i [%u]", len, SekCyclesDone());

  // XXX implement VRAM 128k? Is this even working? xfer/count still FQ_BYTE?
  SekCyclesBurnRun(PicoVideoFIFOWrite(len, FQ_BGDMA | FQ_BYTE,
                              PVS_CPUWR, SR_DMA | PVS_DMABG));

  source =Pico.video.reg[0x15];
  source|=Pico.video.reg[0x16]<<8;

  for (; len; len--)
  {
    vr[(u16)a] = vr[(u16)(source++)];
    if (!((u16)(a^SATaddr) & SATmask))
      UpdateSAT(a, ((u16 *)vr)[(u16)a >> 1]);
    // AutoIncrement
    a = (a+inc) & ~0x20000;
  }
  // remember addr
  Pico.video.addr = a;
  Pico.video.addr_u = a >> 16;
}

static NOINLINE void DmaFill(int data)
{
  u32 a = Pico.video.addr | (Pico.video.addr_u << 16);
  u8 *vr = (u8 *)PicoMem.vram;
  u8 high = (u8)(data >> 8);
  u8 inc = Pico.video.reg[0xf];
  int source;
  int len, l;

  len = GetDmaLength();
  elprintf(EL_VDPDMA, "DmaFill len %i inc %i [%u]", len, inc, SekCyclesDone());

  SekCyclesBurnRun(PicoVideoFIFOWrite(len, FQ_BGDMA | (Pico.video.type == 1),
                              PVS_CPUWR | PVS_DMAFILL, SR_DMA | PVS_DMABG));

  switch (Pico.video.type)
  {
    case 1: // vram
      if (inc == 1 && (a & ~0xffff) == ((a + len-1) & ~0xffff) &&
          ((a >= SATaddr+0x280) | ((a + len-1) < SATaddr)))
      {
        // most used DMA mode
        memset(vr + (u16)a, high, len);
        a += len;
        break;
      }
      for (l = len; l; l--) {
        // Write upper byte to adjacent address
        // (here we are byteswapped, so address is already 'adjacent')
        vr[(u16)a] = high;
        if (!((u16)(a^SATaddr) & SATmask))
          UpdateSAT(a, ((u16 *)vr)[(u16)a >> 1]);

        // Increment address register
        a = (a+inc) & ~0x20000;
      }
      break;
    case 3:   // cram
      Pico.m.dirtyPal = 1;
      data &= 0xeee;
      for (l = len; l; l--) {
        PicoMem.cram[(a/2) & 0x3f] = data;

        // Increment address register
        a = (a+inc) & ~0x20000;
      }
      break;
    case 5: { // vsram
      data &= 0x7ff;
      for (l = len; l; l--) {
        PicoMem.vsram[(a/2) & 0x3f] = data;

        // Increment address register
        a = (a+inc) & ~0x20000;
      }
      break;
    }
    case 0x81: // vram 128k
      for (l = len; l; l--) {
        VideoWriteVRAM128(a, data);

        // Increment address register
        a = (a+inc) & ~0x20000;
      }
      break;
    default:
      a += len * inc;
      break;
  }

  // remember addr
  Pico.video.addr = a;
  Pico.video.addr_u = a >> 16;
  // register update
  Pico.video.reg[0x13] = Pico.video.reg[0x14] = 0;
  source  = Pico.video.reg[0x15];
  source |= Pico.video.reg[0x16] << 8;
  source += len;
  Pico.video.reg[0x15] = source;
  Pico.video.reg[0x16] = source >> 8;

}

// VDP command handling

static NOINLINE void CommandDma(void)
{
  struct PicoVideo *pvid=&Pico.video;
  u32 len, method;
  u32 source;

  PicoVideoFIFOSync(SekCyclesDone()-Pico.t.m68c_line_start);
  if (pvid->status & SR_DMA) {
    elprintf(EL_VDPDMA, "Dma overlap, left=%d @ %06x",
             VdpFIFO.fifo_total, SekPc);
    pvid->fifo_cnt = VdpFIFO.fifo_total = VdpFIFO.fifo_ql = 0;
    pvid->status &= ~(PVS_FIFORUN|PVS_DMAFILL);
  }

  len = GetDmaLength();
  source =Pico.video.reg[0x15];
  source|=Pico.video.reg[0x16] << 8;
  source|=Pico.video.reg[0x17] << 16;

  method=pvid->reg[0x17]>>6;
  if (method < 2)
    DmaSlow(len, source << 1); // 68000 to VDP
  else if (method == 3)
    DmaCopy(len); // VRAM Copy
  else {
    pvid->status |= SR_DMA|PVS_DMAFILL;
    return;
  }
  source += len;
  Pico.video.reg[0x13] = Pico.video.reg[0x14] = 0;
  Pico.video.reg[0x15] = source;
  Pico.video.reg[0x16] = source >> 8;
}

static NOINLINE void CommandChange(struct PicoVideo *pvid)
{
  unsigned int cmd, addr;

  cmd = pvid->command;

  // Get type of transfer 0xc0000030 (v/c/vsram read/write)
  pvid->type = (u8)(((cmd >> 2) & 0xc) | (cmd >> 30));
  if (pvid->type == 1) // vram
    pvid->type |= pvid->reg[1] & 0x80; // 128k

  // Get address 0x3fff0003
  addr  = (cmd >> 16) & 0x3fff;
  addr |= (cmd << 14) & 0xc000;
  pvid->addr = (u16)addr;
  pvid->addr_u = (u8)((cmd >> 2) & 1);
}

// VDP interface
 
static void DrawSync(int skip)
{
  int lines = Pico.video.reg[1]&0x08 ? 240 : 224;
  int last = Pico.m.scanline - (skip || blankline == Pico.m.scanline);

  if (last < lines && !(PicoIn.opt & POPT_ALT_RENDERER) &&
      !PicoIn.skipFrame && Pico.est.DrawScanline <= last) {
    //elprintf(EL_ANOMALY, "sync");
    if (blankline >= 0 && blankline < last) {
      PicoDrawSync(blankline, 1);
      blankline = -1;
    }
    PicoDrawSync(last, 0);
  }
}

PICO_INTERNAL_ASM void PicoVideoWrite(unsigned int a,unsigned short d)
{
  struct PicoVideo *pvid=&Pico.video;

  //elprintf(EL_STATUS, "PicoVideoWrite [%06x] %04x [%u] @ %06x",
  //  a, d, SekCyclesDone(), SekPc);

  a &= 0x1c;
  switch (a)
  {
  case 0x00: // Data port 0 or 2
    // try avoiding the sync..
    if (Pico.m.scanline < (pvid->reg[1]&0x08 ? 240 : 224) && (pvid->reg[1]&0x40) &&
        !(!pvid->pending &&
          ((pvid->command & 0xc00000f0) == 0x40000010 && PicoMem.vsram[pvid->addr>>1] == (d & 0x7ff)))
       )
      DrawSync(0); // XXX  it's unclear when vscroll data is fetched from vsram?

    if (pvid->pending) {
      CommandChange(pvid);
      pvid->pending=0;
    }

    if (!(PicoIn.opt&POPT_DIS_VDP_FIFO))
    {
      VdpFIFO.fifo_data[++VdpFIFO.fifo_dx&3] = d;
      SekCyclesBurnRun(PicoVideoFIFOWrite(1, pvid->type == 1, 0, PVS_CPUWR));

      elprintf(EL_ASVDP, "VDP data write: [%04x] %04x [%u] {%i} @ %06x",
        Pico.video.addr, d, SekCyclesDone(), Pico.video.type, SekPc);
    }
    VideoWrite(d);

    // start DMA fill on write. NB VSRAM and CRAM fills use wrong FIFO data.
    if (pvid->status & PVS_DMAFILL)
      DmaFill(VdpFIFO.fifo_data[(VdpFIFO.fifo_dx + !!(pvid->type&~0x81))&3]);

    break;

  case 0x04: // Control (command) port 4 or 6
    if (pvid->status & SR_DMA)
      SekCyclesBurnRun(PicoVideoFIFORead()); // kludge, flush out running DMA
    if (pvid->pending)
    {
      // Low word of command:
      if (!(pvid->reg[1]&0x10))
        d = (d&~0x80)|(pvid->command&0x80);
      pvid->command &= 0xffff0000;
      pvid->command |= d;
      pvid->pending = 0;
      CommandChange(pvid);
      // Check for dma:
      if (d & 0x80) {
        DrawSync(SekCyclesDone() - Pico.t.m68c_line_start <= 488-390);
        CommandDma();
      }
    }
    else
    {
      if ((d&0xc000)==0x8000)
      {
        // Register write:
        int num=(d>>8)&0x1f;
        int dold=pvid->reg[num];
        pvid->type=0; // register writes clear command (else no Sega logo in Golden Axe II)
        if (num > 0x0a && !(pvid->reg[1]&4)) {
          elprintf(EL_ANOMALY, "%02x written to reg %02x in SMS mode @ %06x", d, num, SekPc);
          return;
        }

        if (num == 0 && !(pvid->reg[0]&2) && (d&2))
          pvid->hv_latch = PicoVideoRead(0x08);
        if (num == 1 && ((pvid->reg[1]^d)&0x40)) {
          PicoVideoFIFOMode(d & 0x40, pvid->reg[12]&1);
          // handle line blanking before line rendering
          if (SekCyclesDone() - Pico.t.m68c_line_start <= 488-390)
            blankline = d&0x40 ? -1 : Pico.m.scanline;
        }
        if (num == 12 && ((pvid->reg[12]^d)&0x01))
          PicoVideoFIFOMode(pvid->reg[1]&0x40, d & 1);
        DrawSync(SekCyclesDone() - Pico.t.m68c_line_start <= 488-390);
        pvid->reg[num]=(unsigned char)d;
        switch (num)
        {
          case 0x00:
            elprintf(EL_INTSW, "hint_onoff: %i->%i [%u] pend=%i @ %06x", (dold&0x10)>>4,
                    (d&0x10)>>4, SekCyclesDone(), (pvid->pending_ints&0x10)>>4, SekPc);
            goto update_irq;
          case 0x01:
            elprintf(EL_INTSW, "vint_onoff: %i->%i [%u] pend=%i @ %06x", (dold&0x20)>>5,
                    (d&0x20)>>5, SekCyclesDone(), (pvid->pending_ints&0x20)>>5, SekPc);
            if (!(pvid->status & PVS_VB2))
              pvid->status &= ~SR_VB;
            pvid->status |= ((d >> 3) ^ SR_VB) & SR_VB; // forced blanking
            goto update_irq;
          case 0x05:
          case 0x06:
            if (d^dold) Pico.est.rendstatus |= PDRAW_SPRITES_MOVED;
            break;
          case 0x0c:
            // renderers should update their palettes if sh/hi mode is changed
            if ((d^dold)&8) Pico.m.dirtyPal = 1;
            break;
          default:
            return;
        }
        SATaddr = ((pvid->reg[5]&0x7f) << 9) | ((pvid->reg[6]&0x20) << 11);
        SATmask = ~0x1ff;
        if (Pico.video.reg[12]&1)
          SATaddr &= ~0x200, SATmask &= ~0x200; // H40, zero lowest SAT bit
        //elprintf(EL_STATUS, "spritep moved to %04x", SATaddr);
        return;

update_irq:
#ifndef EMU_CORE_DEBUG
        // update IRQ level
        if (!SekShouldInterrupt()) // hack
        {
          int lines, pints, irq = 0;
          lines = (pvid->reg[1] & 0x20) | (pvid->reg[0] & 0x10);
          pints = pvid->pending_ints & lines;
               if (pints & 0x20) irq = 6;
          else if (pints & 0x10) irq = 4;
          SekInterrupt(irq); // update line

          // this is broken because cost of current insn isn't known here
          if (irq) SekEndRun(21); // make it delayed
        }
#endif
      }
      else
      {
        // High word of command:
        pvid->command&=0x0000ffff;
        pvid->command|=d<<16;
        pvid->pending=1;
      }
    }
    break;

  // case 0x08: // 08 0a - HV counter - lock up
  // case 0x0c: // 0c 0e - HV counter - lock up
  // case 0x10: // 10 12 - PSG - handled by caller
  // case 0x14: // 14 16 - PSG - handled by caller
  // case 0x18: // 18 1a - no effect?
  case 0x1c: // 1c 1e - debug
    pvid->debug = d;
    pvid->debug_p = 0;
    if (d & (1 << 6)) {
      pvid->debug_p |= PVD_KILL_A | PVD_KILL_B;
      pvid->debug_p |= PVD_KILL_S_LO | PVD_KILL_S_HI;
    }
    switch ((d >> 7) & 3) {
      case 1:
        pvid->debug_p &= ~(PVD_KILL_S_LO | PVD_KILL_S_HI);
        pvid->debug_p |= PVD_FORCE_S;
        break;
      case 2:
        pvid->debug_p &= ~PVD_KILL_A;
        pvid->debug_p |= PVD_FORCE_A;
        break;
      case 3:
        pvid->debug_p &= ~PVD_KILL_B;
        pvid->debug_p |= PVD_FORCE_B;
        break;
    }
    break;
  }
}

static u32 VideoSr(const struct PicoVideo *pv)
{
  unsigned int c, d = pv->status;
  unsigned int hp = pv->reg[12]&1 ? 15*488/210+1 : 15*488/171+1; // HBLANK start
  unsigned int hl = pv->reg[12]&1 ? 37*488/210+1 : 28*488/171+1; // HBLANK len

  c = SekCyclesDone() - Pico.t.m68c_line_start;
  if (c - hp < hl)
    d |= SR_HB;

  PicoVideoFIFOSync(c);
  if (VdpFIFO.fifo_total >= 4)
    d |= SR_FULL;
  else if (!VdpFIFO.fifo_total)
    d |= SR_EMPT;
  return d;
}

PICO_INTERNAL_ASM unsigned int PicoVideoRead(unsigned int a)
{
  a &= 0x1c;

  if (a == 0x04) // control port
  {
    struct PicoVideo *pv = &Pico.video;
    unsigned int d = VideoSr(pv);
    if (pv->pending) {
      CommandChange(pv);
      pv->pending = 0;
    }
    elprintf(EL_SR, "SR read: %04x [%u] @ %06x", d, SekCyclesDone(), SekPc);
    return d;
  }

  // H-counter info (based on Generator):
  // frame:
  //                       |       <- hblank? ->      |
  // start    <416>       hint  <36> hdisplay <38>  end // CPU cycles
  // |---------...---------|------------|-------------|
  // 0                   B6 E4                       FF // 40 cells
  // 0                   93 E8                       FF // 32 cells

  // Gens (?)              v-render
  // start  <hblank=84>   hint    hdisplay <404>      |
  // |---------------------|--------------------------|
  // E4  (hc[0x43]==0)    07                         B1 // 40
  // E8  (hc[0x45]==0)    05                         91 // 32

  // check: Sonic 3D Blast bonus, Cannon Fodder, Chase HQ II, 3 Ninjas kick back, Road Rash 3, Skitchin', Wheel of Fortune
  if ((a&0x1c)==0x08)
  {
    unsigned int d;

    d = (SekCyclesDone() - Pico.t.m68c_line_start) & 0x1ff; // FIXME
    if (Pico.video.reg[0]&2)
         d = Pico.video.hv_latch;
    else if (Pico.video.reg[12]&1)
         d = hcounts_40[d/2] | (Pico.video.v_counter << 8);
    else d = hcounts_32[d/2] | (Pico.video.v_counter << 8);

    elprintf(EL_HVCNT, "hv: %02x %02x [%u] @ %06x", d, Pico.video.v_counter, SekCyclesDone(), SekPc);
    return d;
  }

  if (a==0x00) // data port
  {
    return VideoRead();
  }

  return 0;
}

unsigned char PicoVideoRead8DataH(void)
{
  return VideoRead() >> 8;
}

unsigned char PicoVideoRead8DataL(void)
{
  return VideoRead();
}

unsigned char PicoVideoRead8CtlH(void)
{
  struct PicoVideo *pv = &Pico.video;
  u8 d = VideoSr(pv) >> 8;
  if (pv->pending) {
    CommandChange(pv);
    pv->pending = 0;
  }
  elprintf(EL_SR, "SR read (h): %02x @ %06x", d, SekPc);
  return d;
}

unsigned char PicoVideoRead8CtlL(void)
{
  struct PicoVideo *pv = &Pico.video;
  u8 d = VideoSr(pv);
  if (pv->pending) {
    CommandChange(pv);
    pv->pending = 0;
  }
  elprintf(EL_SR, "SR read (l): %02x @ %06x", d, SekPc);
  return d;
}

unsigned char PicoVideoRead8HV_H(void)
{
  elprintf(EL_HVCNT, "vcounter: %02x [%u] @ %06x", Pico.video.v_counter, SekCyclesDone(), SekPc);
  return Pico.video.v_counter;
}

// FIXME: broken
unsigned char PicoVideoRead8HV_L(void)
{
  u32 d = (SekCyclesDone() - Pico.t.m68c_line_start) & 0x1ff; // FIXME
  if (Pico.video.reg[0]&2)
       d = Pico.video.hv_latch;
  else if (Pico.video.reg[12]&1)
       d = hcounts_40[d/2];
  else d = hcounts_32[d/2];
  elprintf(EL_HVCNT, "hcounter: %02x [%u] @ %06x", d, SekCyclesDone(), SekPc);
  return d;
}

void PicoVideoSave(void)
{
  struct VdpFIFO *vf = &VdpFIFO;
  struct PicoVideo *pv = &Pico.video;
  int l, x;

  // account for all outstanding xfers XXX kludge, entry attr's not saved
  for (l = vf->fifo_ql, x = vf->fifo_qx + l-1; l > 1; l--, x--)
    pv->fifo_cnt += (vf->fifo_queue[x&7] >> 3) << (vf->fifo_queue[x&7] & FQ_BYTE);
}

void PicoVideoLoad(void)
{
  struct VdpFIFO *vf = &VdpFIFO;
  struct PicoVideo *pv = &Pico.video;
  int l;

  // convert former dma_xfers (why was this in PicoMisc anyway?)
  if (Pico.m.dma_xfers) {
    pv->status = SR_DMA|PVS_FIFORUN;
    pv->fifo_cnt = Pico.m.dma_xfers * (pv->type == 1 ? 2 : 1);
    vf->fifo_total = Pico.m.dma_xfers;
    Pico.m.dma_xfers = 0;
  }

  SATaddr = ((pv->reg[5]&0x7f) << 9) | ((pv->reg[6]&0x20) << 11);
  SATmask = ~0x1ff;
  if (pv->reg[12]&1)
    SATaddr &= ~0x200, SATmask &= ~0x200; // H40, zero lowest SAT bit

  // rebuild SAT cache XXX wrong since cache and memory can differ
  for (l = 0; l < 80; l++) {
    *((u16 *)VdpSATCache + 2*l  ) = PicoMem.vram[(SATaddr>>1) + l*4    ];
    *((u16 *)VdpSATCache + 2*l+1) = PicoMem.vram[(SATaddr>>1) + l*4 + 1];
  }
}

// vim:shiftwidth=2:ts=2:expandtab
