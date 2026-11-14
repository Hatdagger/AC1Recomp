# Memory budget: Xbox-class target

The enhancement phase targets the original Xbox (64 MiB shared by CPU, GPU and
audio) as its ceiling. Content added on top of a PS1 game must fit:

| Pool | Budget | PS1 original | Where it lives |
|---|---|---|---|
| Guest memory (CPU-visible) | 32 MiB | 2 MiB | see the map below |
| Retained texture banks | 24 MiB | 1 MiB VRAM | host GPU (`psx_mod_define_texture_bank`) |
| SPU sample banks | 8 MiB | 512 KiB SPU RAM | host (`psx_mod_spu_sample_bank`) |

The texture and sample bank allocators refuse anything past their budget
(`mod_texture_banks.c`, `spu.c`).

## Guest memory map (physical)

| Range | Size | What | GPU DMA reach |
|---|---|---|---|
| `0x00000000-0x007FFFFF` | 8 MiB | Main RAM (2 MiB mirrored retail; unique with the opt-in `psx.enhancement.8mb-ram`) | yes |
| `0x00800000-0x00FFFFFF` | 8 MiB | GPU DMA aperture (`psx_mod_gpu_dma_memory_alloc`, KSEG0 `0x80800000`) for extra primitive / texture packets | yes |
| `0x01000000-0x01FFFFFF` | 16 MiB | Mod data memory (`psx_mod_memory_alloc`, KSEG0 `0x81000000`) for tables, models and imported content | no |

GPU linked-list tags carry 24 address bits, so the GPU can only walk packets
below 16 MiB. That is why the map keeps the two GPU-reachable pools in the
first 16 MiB and puts plain data above it, instead of growing main RAM to one
32 MiB block. Main RAM keeps its retail mirroring unless the 8 MB mod is on.

Every pool other than retail main RAM is open bus until a trusted plugin
allocates from it, so a game with no enhancement active sees stock hardware.
Mod data memory used to live in Expansion 1 (`0x1F000000`, 1 MiB, at most
8 MiB before scratchpad/MMIO); Expansion 1 is plain open bus again. Savestates
with mod memory record layout version 2; version 1 states are refused.
