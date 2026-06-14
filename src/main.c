/*
 * wavezbg - PSP VSH Wave custom colour plugin
 * Copyright (c) 2026 koutsie
 * https://kouts.the-sauna.icu/
 * https://koutsie.github.io/wavezbg/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspdisplay.h>
#include <string.h>
#include <pspmodulemgr.h>
#include "default_wave.h"

PSP_MODULE_INFO("wavezbg", 0x1000, 1, 0);
PSP_MAIN_THREAD_ATTR(0);

typedef struct {
    unsigned int c1;
    unsigned int c2;
    unsigned int c3;
    int num_colours;
} MonthColour;

MonthColour month_colours[35];
char base_path[6] = "ms0:/";

static inline int hex2int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static inline int is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// True only if p[1..6] are all valid hex digits. The loop returns on the first
// non-hex char (the NUL terminator is not hex), so it never reads past the end
// of a NUL-terminated buffer even when '#' is the last character.
static int valid_hex6(const char *p) {
    for (int i = 1; i <= 6; i++) {
        if (!is_hex(p[i])) return 0;
    }
    return 1;
}

unsigned int parse_hex_colour(const char *buf) {
    int r = (hex2int(buf[1]) << 4) | hex2int(buf[2]);
    int g = (hex2int(buf[3]) << 4) | hex2int(buf[4]);
    int b = (hex2int(buf[5]) << 4) | hex2int(buf[6]);
    return (0xFF << 24) | (b << 16) | (g << 8) | r;
}

int parse_config_buffer(const char* buf) {
    // im sure there's a better way to do this
    // honestly just got inspired by Bagieta doing
    // PAF stuff so, enjoy - or dont.
    int valid = 0;
    const char *p = buf;
    while (*p) {
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == ' ') { p++; continue; }
        
        int index = 0;
        while (*p >= '0' && *p <= '9') {
            index = index * 10 + (*p - '0');
            p++;
        }
        while (*p == ' ' || *p == '=') p++;
        
        if (index >= 1 && index <= 35 && *p == '#' && valid_hex6(p)) {
            month_colours[index - 1].c1 = parse_hex_colour(p);
            month_colours[index - 1].num_colours = 1;
            valid++;
            p += 7;
            while (*p == ' ') p++;
            if (*p == '#' && valid_hex6(p)) {
                month_colours[index - 1].c2 = parse_hex_colour(p);
                month_colours[index - 1].num_colours = 2;
                p += 7;
                while (*p == ' ') p++;
                if (*p == '#' && valid_hex6(p)) {
                    month_colours[index - 1].c3 = parse_hex_colour(p);
                    month_colours[index - 1].num_colours = 3;
                    p += 7;
                }
            }
        }
        while (*p && *p != '\n') p++;
    }
    return valid;
}

void read_config() {
    // defaults
    for (int i = 0; i < 35; i++) {
        month_colours[i].c1 = 0xFF0099FF; 
        month_colours[i].num_colours = 1;
    }
    parse_config_buffer(default_wave_txt);

    SceUID fd = sceIoOpen("ef0:/seplugins/wave.txt", PSP_O_RDONLY, 0777);
    if (fd < 0) fd = sceIoOpen("ef0:/wave.txt", PSP_O_RDONLY, 0777);
    if (fd >= 0) {
        base_path[0] = 'e'; base_path[1] = 'f';
    } else {
        fd = sceIoOpen("ms0:/seplugins/wave.txt", PSP_O_RDONLY, 0777);
        if (fd < 0) fd = sceIoOpen("ms0:/wave.txt", PSP_O_RDONLY, 0777);
        base_path[0] = 'm'; base_path[1] = 's';
    }

    if (fd < 0) {
        SceUID fd_out = sceIoOpen("ef0:/seplugins/wave.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd_out >= 0) {
            sceIoWrite(fd_out, default_wave_txt, sizeof(default_wave_txt) - 1);
            sceIoClose(fd_out);
            base_path[0] = 'e'; base_path[1] = 'f';
        } else {
            sceIoMkdir("ms0:/seplugins", 0777);
            fd_out = sceIoOpen("ms0:/seplugins/wave.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
            if (fd_out >= 0) {
                sceIoWrite(fd_out, default_wave_txt, sizeof(default_wave_txt) - 1);
                sceIoClose(fd_out);
            }
            base_path[0] = 'm'; base_path[1] = 's';
        }
    } else {
        // Larger than the shipped wave.txt so user files with extra comments
        // parse fully instead of being silently truncated. Transient: this
        // lives only on the worker thread's stack, freed when the thread exits.
        char buf[4096];
        int bytes = sceIoRead(fd, buf, sizeof(buf) - 1);
        sceIoClose(fd);
        if (bytes > 0) {
            buf[bytes] = '\0';
            if (parse_config_buffer(buf) == 0) {
                char err_path[64];
                strcpy(err_path, base_path);
                strcat(err_path, "wavez_ERROR.txt");
                SceUID fd_err = sceIoOpen(err_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
                if (fd_err >= 0) {
                    // PEBCAK
                    char err_msg[] = "ERROR: wave.txt is empty or improperly formatted...";
                    sceIoWrite(fd_err, err_msg, sizeof(err_msg) - 1);
                    sceIoClose(fd_err);
                }
            }
        }
    }
}

// Little-endian field writers for the BMP header. The header fields land at
// byte offsets that are not 4-byte aligned (the 2-byte "BM" magic shifts
// everything by 2), so writing them with `*(unsigned int *)&buf[off] = x` is an
// unaligned store - illegal on the PSP's MIPS core (Address Error exception)
// and undefined behaviour in C. Byte-by-byte writes are both safe and
// endian-explicit (BMP is little-endian).
static inline void put_u32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static inline void put_u16(unsigned char *p, unsigned short v) {
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
}

void write_bmp(const char *filename, int start_month, int count) {
    char bmp_path[64];
    strcpy(bmp_path, base_path);
    strcat(bmp_path, "wavez_cache/");
    strcat(bmp_path, filename);
    
    SceUID fd = sceIoOpen(bmp_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;

    // The BMP header is identical for every strip (60x34, 24-bit), and the loop
    // below overwrites the entire pixel region each iteration, so build the
    // buffer and header once instead of re-zeroing and rebuilding 34 times.
    // The memset also clears the header's reserved fields and the trailing pad
    // bytes, which the pixel loop never touches.
    unsigned char bmp_data[6176];
    memset(bmp_data, 0, sizeof(bmp_data));

    bmp_data[0] = 'B'; bmp_data[1] = 'M';
    put_u32(&bmp_data[2], 6176);   // file size
    put_u32(&bmp_data[10], 54);    // pixel data offset
    put_u32(&bmp_data[14], 40);    // DIB header size
    put_u32(&bmp_data[18], 60);    // width
    put_u32(&bmp_data[22], 34);    // height
    put_u16(&bmp_data[26], 1);     // colour planes
    put_u16(&bmp_data[28], 24);    // bits per pixel
    put_u32(&bmp_data[34], 6120);  // image data size

    for (int i = 0; i < count; i++) {
        int m = start_month + i;
        if (m >= 35) m = 34;

        int r1 = month_colours[m].c1 & 0xFF;
        int g1 = (month_colours[m].c1 >> 8) & 0xFF;
        int b1 = (month_colours[m].c1 >> 16) & 0xFF;
        
        int r2 = r1, g2 = g1, b2 = b1;
        int r3 = r1, g3 = g1, b3 = b1;

        if (month_colours[m].num_colours >= 2) {
            r2 = month_colours[m].c2 & 0xFF;
            g2 = (month_colours[m].c2 >> 8) & 0xFF;
            b2 = (month_colours[m].c2 >> 16) & 0xFF;
        }
        if (month_colours[m].num_colours == 3) {
            r3 = month_colours[m].c3 & 0xFF;
            g3 = (month_colours[m].c3 >> 8) & 0xFF;
            b3 = (month_colours[m].c3 >> 16) & 0xFF;
        }

        int offset = 54;
        for (int y = 0; y < 34; y++) {
            int cur_r, cur_g, cur_b;
            if (month_colours[m].num_colours == 3) {
                if (y < 17) {
                    cur_r = r1 + (y * (r2 - r1)) / 16;
                    cur_g = g1 + (y * (g2 - g1)) / 16;
                    cur_b = b1 + (y * (b2 - b1)) / 16;
                } else {
                    cur_r = r2 + ((y - 17) * (r3 - r2)) / 16;
                    cur_g = g2 + ((y - 17) * (g3 - g2)) / 16;
                    cur_b = b2 + ((y - 17) * (b3 - b2)) / 16;
                }
            } else {
                cur_r = r1 + (y * (r2 - r1)) / 33;
                cur_g = g1 + (y * (g2 - g1)) / 33;
                cur_b = b1 + (y * (b2 - b1)) / 33;
            }

            for (int x = 0; x < 60; x++) {
                bmp_data[offset++] = cur_b;
                bmp_data[offset++] = cur_g;
                bmp_data[offset++] = cur_r;
            }
        }
        
        sceIoWrite(fd, bmp_data, 6176);
    }
    sceIoClose(fd);
}

void generate_wave_bmp() {
    // pregen so we aint slow
    char cache_path[32];
    strcpy(cache_path, base_path);
    strcat(cache_path, "wavez_cache");
    sceIoMkdir(cache_path, 0777);

    char check_path[64];
    strcpy(check_path, cache_path);
    strcat(check_path, "/w1.bmp");
    SceUID fd = sceIoOpen(check_path, PSP_O_RDONLY, 0777);
    if (fd >= 0) {
        sceIoClose(fd);
        return;
    }

    write_bmp("w1.bmp", 0, 12);
    write_bmp("w2.bmp", 12, 22);
}

// Scan one mapped memory range [addr, end_addr) for the stock flash0 wave
// resource paths and rewrite them in-place to point at our cache. Returns 1 if
// anything was patched. Callers MUST only ever pass a range that belongs to a
// single loaded segment - that is what keeps the scan from wandering into the
// unmapped gaps between segments (the cause of the random crashes).
static int patch_range(char *addr, char *end_addr, const char *replace1, const char *replace2) {
    int patched = 0;
    if (addr == NULL || end_addr <= addr) return 0;

    // Need at least 34 readable bytes for the longest path compare below.
    while (addr + 34 <= end_addr) {
        // Cheap first-byte gate before strncmp. A path we already rewrote no
        // longer starts with "flash", so this stays idempotent across passes.
        if (addr[0] == 'f' && addr[1] == 'l' && addr[2] == 'a' && addr[3] == 's' && addr[4] == 'h') {
            if (strncmp(addr, "flash0:/vsh/resource/01-12.bmp", 30) == 0) {
                memcpy(addr, replace1, 30);
                sceKernelDcacheWritebackInvalidateRange(addr, 30);
                patched = 1;
            }
            else if (strncmp(addr, "flash0:/vsh/resource/01-12_03g.bmp", 34) == 0) {
                memcpy(addr, replace1, 34);
                sceKernelDcacheWritebackInvalidateRange(addr, 34);
                patched = 1;
            }
            else if (strncmp(addr, "flash0:/vsh/resource/13-27.bmp", 30) == 0) {
                memcpy(addr, replace2, 30);
                sceKernelDcacheWritebackInvalidateRange(addr, 30);
                patched = 1;
            }
        }
        addr++;
    }
    return patched;
}

int patch_wave_strings() {
    int patched = 0;

    char replace1[36];
    memset(replace1, 0, sizeof(replace1));
    strcpy(replace1, base_path);
    strcat(replace1, "wavez_cache/w1.bmp");

    char replace2[36];
    memset(replace2, 0, sizeof(replace2));
    strcpy(replace2, base_path);
    strcat(replace2, "wavez_cache/w2.bmp");

    SceUID ids[100];
    int count = 0;
    if (sceKernelGetModuleIdList(ids, sizeof(ids), &count) >= 0) {
        // count is the total module count, which may exceed what fit in ids[].
        int max_ids = (int)(sizeof(ids) / sizeof(ids[0]));
        if (count > max_ids) count = max_ids;

        for (int i = 0; i < count; i++) {
            SceKernelModuleInfo info;
            memset(&info, 0, sizeof(info));
            info.size = sizeof(info);

            if (sceKernelQueryModuleInfo(ids[i], &info) < 0) continue;

            // info.name is a fixed 28-byte field; force a terminator before
            // strstr so a maxed-out name can't run the search past the struct.
            info.name[sizeof(info.name) - 1] = '\0';

            if (strstr(info.name, "system_plugin_bg") == NULL &&
                strstr(info.name, "sysconf_plugin") == NULL &&
                strstr(info.name, "vsh") == NULL) {
                continue;
            }

            // Scan each loaded segment on its own. The old code scanned
            // [text_addr, text_addr + text_size + data_size + bss_size) as one
            // block, but those segments are page-aligned and need not be
            // adjacent - the gaps between them are UNMAPPED, so reading across
            // one raises a TLB-miss exception and crashes the PSP. Whether a
            // module has such a gap depends on its layout, which is why the
            // crashes were random. Per-segment scanning only ever touches
            // memory the loader actually mapped.
            int nseg = info.nsegment;
            if (nseg > 4) nseg = 4;

            if (nseg > 0) {
                for (int s = 0; s < nseg; s++) {
                    char *start = (char *)(unsigned int)info.segmentaddr[s];
                    unsigned int seg_size = (unsigned int)info.segmentsize[s];
                    if (patch_range(start, start + seg_size, replace1, replace2)) {
                        patched = 1;
                    }
                }
            } else if (info.text_addr != 0) {
                // Fallback if the segment table is empty: scan only the text
                // segment (the read-only rodata path strings we target live
                // there). Still a single mapped segment, so still gap-safe.
                char *start = (char *)info.text_addr;
                if (patch_range(start, start + info.text_size, replace1, replace2)) {
                    patched = 1;
                }
            }
        }
    }

    return patched;
}

int main_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    read_config();
    generate_wave_bmp();

    // The target VSH modules aren't loaded yet when module_start runs, so poll
    // until their resource strings are in memory and we can patch them. Bounded
    // at ~60s (600 * 100ms) so that if the patch never lands (unexpected module
    // names, CXMB, etc.) we give up and free this thread instead of spinning
    // forever. The success case breaks out within the first second or two, so
    // the timeout never affects working setups.
    for (int attempts = 0; attempts < 600; attempts++) {
        if (patch_wave_strings()) {
            break;
        }
        sceKernelDelayThread(100000);
    }

    sceKernelExitDeleteThread(0);
    return 0;
}

int module_start(SceSize args, void *argp) {
    // 0x06060110 = 6.61 (ARK), 0x06060010 = 6.60 (PRO-C)
    unsigned int ver = sceKernelDevkitVersion();
    if (ver != 0x06060110 && ver != 0x06060010) {
        return 1;
    }
    // 0x8000 (32KB) stack: peak use is ~6.5KB (the 6176-byte BMP buffer in
    // write_bmp plus call frames), leaving a generous safety margin. The thread
    // deletes itself once patching succeeds, so this is reclaimed at boot.
    SceUID thid = sceKernelCreateThread("wavezbg_thread", main_thread, 0x18, 0x8000, 0, NULL);
    if (thid >= 0) {
        sceKernelStartThread(thid, args, argp);
    }
    return 0;
}

int module_stop(void) {
    return 0;
}
