/*
 * AWD (Audio Wave Dictionary) loader for Burnout 3
 *
 * Criterion/RenderWare AWD format:
 * - Header: RW type 0x0809, data section offset at +0x08
 * - Entry table: ~0x84-0x88 bytes per entry, names + format + GUID
 * - Data section: raw 16-bit PCM audio data
 *
 * Each entry in the table has:
 * - Cumulative data offset (from data section start)
 * - Name (null-terminated, up to 20 chars)
 * - Sample rate (uint32), channels (uint32), data size (uint32)
 *
 * Entry structure (relative to name position):
 *   -32: [cumul_offset][self_ptr][4][0][prev][next][entry_data][0]
 *   +0:  name (null-terminated ASCII)
 *   After name (4-byte aligned):
 *     +0:  GUID (16 bytes)
 *     +16: link1, link2 (uint32 each)
 *     +24: zero (uint32)
 *     +28: RW version marker 0x100F2E60
 *     +32: sample_rate (uint32)
 *     +36: channels (uint32)
 *     +40: data_size (uint32)
 */

#include "awd_loader.h"
#include "../apu/apu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * AWD file parser
 * ============================================================ */

AWDFile *awd_load(const char *path)
{
    char norm[1024];
    snprintf(norm, sizeof(norm), "%s", path);
    /* Convert Windows backslashes in hard-coded asset paths. */
#if !defined(_WIN32)
    for (char *p = norm; *p; p++) if (*p == '\\') *p = '/';
#endif
    FILE *f = fopen(norm, "rb");
    if (!f) {
        fprintf(stderr, "[AWD] Cannot open: %s\n", norm);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = (uint8_t *)malloc(file_size);
    if (!data) { fclose(f); return NULL; }
    fread(data, 1, file_size, f);
    fclose(f);

    /* Header: data section start at offset 8 */
    uint32_t data_section = *(uint32_t *)(data + 8);

    AWDFile *awd = (AWDFile *)calloc(1, sizeof(AWDFile));
    if (!awd) { free(data); return NULL; }

    /* Extract filename for logging */
    const char *basename = strrchr(path, '/');
    if (!basename) basename = strrchr(path, '\\');
    if (basename) basename++; else basename = path;
    strncpy(awd->filename, basename, sizeof(awd->filename) - 1);

    /* Parse entries by scanning for name strings preceded by a null byte. */
    int found = 0;

    for (uint32_t pos = 1; pos < data_section && pos < (uint32_t)file_size - 64; pos++) {
        /* Name must be preceded by a null byte */
        if (data[pos - 1] != 0) continue;

        /* Check for printable ASCII name (3-20 chars) */
        int name_len = 0;
        while (name_len < 20 && pos + name_len < (uint32_t)file_size &&
               data[pos + name_len] >= 32 && data[pos + name_len] < 127) {
            name_len++;
        }
        if (name_len < 3 || pos + name_len >= (uint32_t)file_size ||
            data[pos + name_len] != 0) {
            continue;
        }

        /* First char must be a letter */
        if (!((data[pos] >= 'A' && data[pos] <= 'Z') ||
              (data[pos] >= 'a' && data[pos] <= 'z'))) {
            continue;
        }

        /* Verify the pre-name pattern: 32 bytes before name should have [x][x][4][0] */
        if (pos < 32) continue;
        uint32_t pre_base = pos - 32;
        uint32_t marker_4 = *(uint32_t *)(data + pre_base + 8);
        uint32_t marker_0 = *(uint32_t *)(data + pre_base + 12);
        if (marker_4 != 4 || marker_0 != 0) continue;

        /* Read cumulative data offset from pre-name */
        uint32_t cumul_data_offset = *(uint32_t *)(data + pre_base);

        /* Post-name format info: aligned end of name + 32 = sample_rate */
        uint32_t name_end = pos + name_len + 1;
        uint32_t name_end_aligned = (name_end + 3) & ~3;
        uint32_t fmt_pos = name_end_aligned + 32;
        if (fmt_pos + 12 >= (uint32_t)file_size) break;

        /* Verify RW version marker at name_end_aligned + 28 */
        uint32_t rw_ver = *(uint32_t *)(data + name_end_aligned + 28);
        if (rw_ver != 0x100F2E60) continue;

        uint32_t sample_rate = *(uint32_t *)(data + fmt_pos);
        uint32_t channels = *(uint32_t *)(data + fmt_pos + 4);
        uint32_t pcm_size = *(uint32_t *)(data + fmt_pos + 8);

        /* Validate */
        if (sample_rate < 4000 || sample_rate > 48000 ||
            channels < 1 || channels > 2 ||
            pcm_size == 0 || pcm_size > (uint32_t)file_size) {
            continue;
        }

        if (found < AWD_MAX_ENTRIES) {
            AWDEntry *e = &awd->entries[found];
            memcpy(e->name, data + pos, name_len);
            e->name[name_len] = 0;
            e->data_offset = cumul_data_offset;
            e->sample_rate = sample_rate;
            e->channels = channels;
            e->mixer_slot = -1;
            e->data_size = pcm_size;

            /* Copy raw 16-bit PCM data */
            uint32_t abs_offset = data_section + cumul_data_offset;
            if (abs_offset + pcm_size <= (uint32_t)file_size) {
                e->pcm_bytes = pcm_size;
                e->pcm_data = (int16_t *)malloc(pcm_size);
                if (e->pcm_data) {
                    memcpy(e->pcm_data, data + abs_offset, pcm_size);
                }
            }

            found++;
        }

        /* Skip past this entry to avoid double-matching */
        pos += name_len;
    }

    awd->num_entries = found;
    free(data);

    fprintf(stderr, "[AWD] Loaded %s: %d entries\n", awd->filename, found);
    for (int i = 0; i < found && i < 10; i++) {
        AWDEntry *e = &awd->entries[i];
        fprintf(stderr, "  [%d] '%s': %u Hz, %u ch, %u bytes PCM\n",
                i, e->name, e->sample_rate, e->channels, e->pcm_bytes);
    }
    if (found > 10) fprintf(stderr, "  ... and %d more\n", found - 10);

    return awd;
}

void awd_free(AWDFile *awd)
{
    if (!awd) return;
    for (int i = 0; i < awd->num_entries; i++) {
        if (awd->entries[i].mixer_slot >= 0) {
            apu_mixer_stop(awd->entries[i].mixer_slot);
            apu_mixer_free_voice(awd->entries[i].mixer_slot);
        }
        free(awd->entries[i].pcm_data);
    }
    free(awd);
}

int awd_play(AWDFile *awd, const char *name, bool looping)
{
    if (!awd) return -1;
    for (int i = 0; i < awd->num_entries; i++) {
        if (strcmp(awd->entries[i].name, name) == 0) {
            awd_play_index(awd, i, looping);
            return i;
        }
    }
    return -1;
}

void awd_play_index(AWDFile *awd, int index, bool looping)
{
    if (!awd || index < 0 || index >= awd->num_entries) return;
    AWDEntry *e = &awd->entries[index];
    if (!e->pcm_data || e->pcm_bytes == 0) return;

    /* Allocate a mixer slot if needed */
    if (e->mixer_slot < 0) {
        e->mixer_slot = apu_mixer_alloc_voice();
    }
    if (e->mixer_slot < 0) return;

    /* Configure the mixer voice */
    APUMixerVoice *v = apu_mixer_get_voice(e->mixer_slot);
    if (!v) return;

    v->pcm_data = e->pcm_data;
    v->pcm_bytes = e->pcm_bytes;
    v->num_channels = e->channels;
    v->sample_rate = e->sample_rate;
    v->volume = 0.5f;  /* 50% default */

    apu_mixer_play(e->mixer_slot, looping ? 1 : 0);
}

void awd_stop(AWDFile *awd, const char *name)
{
    if (!awd) return;
    for (int i = 0; i < awd->num_entries; i++) {
        if (strcmp(awd->entries[i].name, name) == 0 && awd->entries[i].mixer_slot >= 0) {
            apu_mixer_stop(awd->entries[i].mixer_slot);
            break;
        }
    }
}

void awd_stop_all(AWDFile *awd)
{
    if (!awd) return;
    for (int i = 0; i < awd->num_entries; i++) {
        if (awd->entries[i].mixer_slot >= 0) {
            apu_mixer_stop(awd->entries[i].mixer_slot);
        }
    }
}
