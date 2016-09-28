#include "../../../../main/main.h"
#include "../../../../main/rom.h"
#include "../../../../memory/memory.h"
#include "../../../../rsp/rsp_core.h"
#include "../../../cached_interp.h"
#include "../../../cp0_private.h"
#include "../../../cp1_private.h"
#include "../../../interupt.h"
#include "../../../ops.h"
#include "../../../r4300.h"
#include "../../../recomp.h"
#include "../../../recomph.h" //include for function prototypes
#include "../../../tlb.h"
#include "../../new_dynarec.h"
#include "n64.h"

#include <retro_assert.h>

extern u_int using_tlb;
extern int literalcount;
extern int expirep;

#ifdef __cplusplus
extern "C" {
#endif

void read_nomem_new(void);
void read_nomemb_new(void);
void read_nomemh_new(void);
void read_nomemd_new(void);
void write_nomem_new(void);
void write_nomemb_new(void);
void write_nomemh_new(void);
void write_nomemd_new(void);
void write_rdram_new(void);
void write_rdramb_new(void);
void write_rdramh_new(void);
void write_rdramd_new(void);

extern char *copy;
extern ALIGN(16, char shadow[2097152]);
extern ALIGN(16, u_int hash_table[65536][4]);
extern u_char restore_candidate[512];
extern u_int stop_after_jal;
extern uint64_t readmem_dword;
extern u_int memory_map[1048576];

extern u_int mini_ht[32][2];
extern precomp_instr fake_pc;

#ifdef __cplusplus
}
#endif

void tlb_hacks(void)
{
   // Goldeneye hack
   if (strncmp((char *) ROM_HEADER.Name, "GOLDENEYE",9) == 0)
   {
      u_int addr;
      int n;
      switch (ROM_HEADER.destination_code&0xFF) 
      {
         case 0x45: // U
            addr=0x34b30;
            break;                   
         case 0x4A: // J 
            addr=0x34b70;    
            break;    
         case 0x50: // E 
            addr=0x329f0;
            break;                        
         default: 
            // Unknown country code
            addr=0;
            break;
      }
      u_int rom_addr=(u_int)g_rom;
#ifdef ROM_COPY
      // Since memory_map is 32-bit, on 64-bit systems the rom needs to be
      // in the lower 4G of memory to use this hack.  Copy it if necessary.
      if((void *)g_rom>(void *)0xffffffff) {
         munmap(ROM_COPY, 67108864);
         if(mmap(ROM_COPY, 12582912,
                  PROT_READ | PROT_WRITE,
                  MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                  -1, 0) <= 0) {DebugMessage(M64MSG_ERROR, "mmap() failed");}
         memcpy(ROM_COPY,g_rom,12582912);
         rom_addr=(u_int)ROM_COPY;
      }
#endif
      if(addr) {
         for(n=0x7F000;n<0x80000;n++) {
            memory_map[n]=(((u_int)(rom_addr+addr-0x7F000000))>>2)|0x40000000;
         }
      }
   }
}

void n64_init(void)
{
   rdword=&readmem_dword;
   fake_pc.f.r.rs=(long long int *)&readmem_dword;
   fake_pc.f.r.rt=(long long int *)&readmem_dword;
   fake_pc.f.r.rd=(long long int *)&readmem_dword;
   int n;
   for(n=0x80000;n<0x80800;n++)
      invalid_code[n]=1;
   for(n=0;n<65536;n++)
      hash_table[n][0]=hash_table[n][2]=-1;
   memset(mini_ht,-1,sizeof(mini_ht));
   memset(restore_candidate,0,sizeof(restore_candidate));
   copy=shadow;
   expirep=16384; // Expiry pointer, +2 blocks
   pending_exception=0;
   literalcount=0;
#ifdef HOST_IMM8
   // Copy this into local area so we don't have to put it in every literal pool
   invc_ptr=invalid_code;
#endif
   stop_after_jal=0;

   // TLB
   using_tlb=0;
   for(n=0;n<524288;n++) // 0 .. 0x7FFFFFFF
      memory_map[n]=-1;
   for(n=524288;n<526336;n++) // 0x80000000 .. 0x807FFFFF
      memory_map[n]=((u_int)g_rdram-0x80000000)>>2;
   for(n=526336;n<1048576;n++) // 0x80800000 .. 0xFFFFFFFF
      memory_map[n]=-1;

   for(n=0;n<0x8000;n++)
   {
      // 0 .. 0x7FFFFFFF
      writemem[n] = write_nomem_new;
      writememb[n] = write_nomemb_new;
      writememh[n] = write_nomemh_new;
      writememd[n] = write_nomemd_new;
      readmem[n] = read_nomem_new;
      readmemb[n] = read_nomemb_new;
      readmemh[n] = read_nomemh_new;
      readmemd[n] = read_nomemd_new;
   }
   for(n=0x8000;n<0x8080;n++)
   {
      // 0x80000000 .. 0x807FFFFF
      writemem[n] = write_rdram_new;
      writememb[n] = write_rdramb_new;
      writememh[n] = write_rdramh_new;
      writememd[n] = write_rdramd_new;
   }
   for(n=0xC000;n<0x10000;n++)
   {
      // 0xC0000000 .. 0xFFFFFFFF
      writemem[n] = write_nomem_new;
      writememb[n] = write_nomemb_new;
      writememh[n] = write_nomemh_new;
      writememd[n] = write_nomemd_new;
      readmem[n] = read_nomem_new;
      readmemb[n] = read_nomemb_new;
      readmemh[n] = read_nomemh_new;
      readmemd[n] = read_nomemd_new;
   }
   tlb_hacks();
}

void TLBWI_new(void)
{
  unsigned int i;
  /* Remove old entries */
  unsigned int old_start_even=tlb_e[g_cp0_regs[CP0_INDEX_REG]&0x3F].start_even;
  unsigned int old_end_even=tlb_e[g_cp0_regs[CP0_INDEX_REG]&0x3F].end_even;
  unsigned int old_start_odd=tlb_e[g_cp0_regs[CP0_INDEX_REG]&0x3F].start_odd;
  unsigned int old_end_odd=tlb_e[g_cp0_regs[CP0_INDEX_REG]&0x3F].end_odd;
  for (i=old_start_even>>12; i<=old_end_even>>12; i++)
  {
    if(i<0x80000||i>0xBFFFF)
    {
      invalidate_block(i);
      memory_map[i]=-1;
    }
  }
  for (i=old_start_odd>>12; i<=old_end_odd>>12; i++)
  {
    if(i<0x80000||i>0xBFFFF)
    {
      invalidate_block(i);
      memory_map[i]=-1;
    }
  }
  cached_interpreter_table.TLBWI();
  //DebugMessage(M64MSG_VERBOSE, "TLBWI: index=%d",g_cp0_regs[CP0_INDEX_REG]);
  //DebugMessage(M64MSG_VERBOSE, "TLBWI: start_even=%x end_even=%x phys_even=%x v=%d d=%d",tlb_e[g_cp0_regs[CP0_INDEX_REG]&0x3F].start_even,tlb_e[g_cp0_regs[CP0_INDEX_REG]&0x3F].end_even,tlb_e[g_cp0_regs[CP0_INDEX_REG]&0x3F].phys_even,tlb_e[g_cp0_regs[CP0_INDEX_REG]&0x3F].v_even,tlb_e[g_cp0_regs[CP0_INDEX_REG]&0x3F].d_even);
  //DebugMessage(M64MSG_VERBOSE, "TLBWI: start_odd=%x end_odd=%x phys_odd=%x v=%d d=%d",tlb_e[g_cp0_regs[CP0_INDEX_REG]&0x3F].start_odd,tlb_e[g_cp0_regs[CP0_INDEX_REG]&0x3F].end_odd,tlb_e[g_cp0_regs[CP0_INDEX_REG]&0x3F].phys_odd,tlb_e[g_cp0_regs[CP0_INDEX_REG]&0x3F].v_odd,tlb_e[g_cp0_regs[CP0_INDEX_REG]&0x3F].d_odd);
  /* Combine tlb_LUT_r, tlb_LUT_w, and invalid_code into a single table
     for fast look up. */
  for (i=tlb_e[g_cp0_regs[CP0_INDEX_REG]&0x3F].start_even>>12; i<=tlb_e[g_cp0_regs[CP0_INDEX_REG]&0x3F].end_even>>12; i++)
  {
    //DebugMessage(M64MSG_VERBOSE, "%x: r:%8x w:%8x",i,tlb_LUT_r[i],tlb_LUT_w[i]);
    if(i<0x80000||i>0xBFFFF)
    {
      if(tlb_LUT_r[i]) {
        memory_map[i]=((tlb_LUT_r[i]&0xFFFFF000)-(i<<12)+(unsigned int)g_rdram-0x80000000)>>2;
        // FIXME: should make sure the physical page is invalid too
        if(!tlb_LUT_w[i]||!invalid_code[i]) {
          memory_map[i]|=0x40000000; // Write protect
        }else{
          assert(tlb_LUT_r[i]==tlb_LUT_w[i]);
        }
        if(!using_tlb) DebugMessage(M64MSG_VERBOSE, "Enabled TLB");
        // Tell the dynamic recompiler to generate tlb lookup code
        using_tlb=1;
      }
      else memory_map[i]=-1;
    }
    //DebugMessage(M64MSG_VERBOSE, "memory_map[%x]: %8x (+%8x)",i,memory_map[i],memory_map[i]<<2);
  }
  for (i=tlb_e[g_cp0_regs[CP0_INDEX_REG]&0x3F].start_odd>>12; i<=tlb_e[g_cp0_regs[CP0_INDEX_REG]&0x3F].end_odd>>12; i++)
  {
    //DebugMessage(M64MSG_VERBOSE, "%x: r:%8x w:%8x",i,tlb_LUT_r[i],tlb_LUT_w[i]);
    if(i<0x80000||i>0xBFFFF)
    {
      if(tlb_LUT_r[i]) {
        memory_map[i]=((tlb_LUT_r[i]&0xFFFFF000)-(i<<12)+(unsigned int)g_rdram-0x80000000)>>2;
        // FIXME: should make sure the physical page is invalid too
        if(!tlb_LUT_w[i]||!invalid_code[i]) {
          memory_map[i]|=0x40000000; // Write protect
        }else{
          assert(tlb_LUT_r[i]==tlb_LUT_w[i]);
        }
        if(!using_tlb) DebugMessage(M64MSG_VERBOSE, "Enabled TLB");
        // Tell the dynamic recompiler to generate tlb lookup code
        using_tlb=1;
      }
      else memory_map[i]=-1;
    }
    //DebugMessage(M64MSG_VERBOSE, "memory_map[%x]: %8x (+%8x)",i,memory_map[i],memory_map[i]<<2);
  }
}

void TLBWR_new(void)
{
  unsigned int i;
  g_cp0_regs[CP0_RANDOM_REG] = (g_cp0_regs[CP0_COUNT_REG]/2 % (32 - g_cp0_regs[CP0_WIRED_REG])) + g_cp0_regs[CP0_WIRED_REG];
  /* Remove old entries */
  unsigned int old_start_even=tlb_e[g_cp0_regs[CP0_RANDOM_REG]&0x3F].start_even;
  unsigned int old_end_even=tlb_e[g_cp0_regs[CP0_RANDOM_REG]&0x3F].end_even;
  unsigned int old_start_odd=tlb_e[g_cp0_regs[CP0_RANDOM_REG]&0x3F].start_odd;
  unsigned int old_end_odd=tlb_e[g_cp0_regs[CP0_RANDOM_REG]&0x3F].end_odd;
  for (i=old_start_even>>12; i<=old_end_even>>12; i++)
  {
    if(i<0x80000||i>0xBFFFF)
    {
      invalidate_block(i);
      memory_map[i]=-1;
    }
  }
  for (i=old_start_odd>>12; i<=old_end_odd>>12; i++)
  {
    if(i<0x80000||i>0xBFFFF)
    {
      invalidate_block(i);
      memory_map[i]=-1;
    }
  }
  cached_interpreter_table.TLBWR();
  /* Combine tlb_LUT_r, tlb_LUT_w, and invalid_code into a single table
     for fast look up. */
  for (i=tlb_e[g_cp0_regs[CP0_RANDOM_REG]&0x3F].start_even>>12; i<=tlb_e[g_cp0_regs[CP0_RANDOM_REG]&0x3F].end_even>>12; i++)
  {
    //DebugMessage(M64MSG_VERBOSE, "%x: r:%8x w:%8x",i,tlb_LUT_r[i],tlb_LUT_w[i]);
    if(i<0x80000||i>0xBFFFF)
    {
      if(tlb_LUT_r[i]) {
        memory_map[i]=((tlb_LUT_r[i]&0xFFFFF000)-(i<<12)+(unsigned int)g_rdram-0x80000000)>>2;
        // FIXME: should make sure the physical page is invalid too
        if(!tlb_LUT_w[i]||!invalid_code[i]) {
          memory_map[i]|=0x40000000; // Write protect
        }else{
          assert(tlb_LUT_r[i]==tlb_LUT_w[i]);
        }
        if(!using_tlb) DebugMessage(M64MSG_VERBOSE, "Enabled TLB");
        // Tell the dynamic recompiler to generate tlb lookup code
        using_tlb=1;
      }
      else memory_map[i]=-1;
    }
    //DebugMessage(M64MSG_VERBOSE, "memory_map[%x]: %8x (+%8x)",i,memory_map[i],memory_map[i]<<2);
  }
  for (i=tlb_e[g_cp0_regs[CP0_RANDOM_REG]&0x3F].start_odd>>12; i<=tlb_e[g_cp0_regs[CP0_RANDOM_REG]&0x3F].end_odd>>12; i++)
  {
    //DebugMessage(M64MSG_VERBOSE, "%x: r:%8x w:%8x",i,tlb_LUT_r[i],tlb_LUT_w[i]);
    if(i<0x80000||i>0xBFFFF)
    {
      if(tlb_LUT_r[i]) {
        memory_map[i]=((tlb_LUT_r[i]&0xFFFFF000)-(i<<12)+(unsigned int)g_rdram-0x80000000)>>2;
        // FIXME: should make sure the physical page is invalid too
        if(!tlb_LUT_w[i]||!invalid_code[i]) {
          memory_map[i]|=0x40000000; // Write protect
        }else{
          assert(tlb_LUT_r[i]==tlb_LUT_w[i]);
        }
        if(!using_tlb) DebugMessage(M64MSG_VERBOSE, "Enabled TLB");
        // Tell the dynamic recompiler to generate tlb lookup code
        using_tlb=1;
      }
      else memory_map[i]=-1;
    }
    //DebugMessage(M64MSG_VERBOSE, "memory_map[%x]: %8x (+%8x)",i,memory_map[i],memory_map[i]<<2);
  }
}
