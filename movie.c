/*  SSNES - A Super Nintendo Entertainment System (SNES) Emulator frontend for libsnes.
 *  Copyright (C) 2010-2012 - Hans-Kristian Arntzen
 *
 *  Some code herein may be based on code found in BSNES.
 * 
 *  SSNES is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  SSNES is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with SSNES.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "movie.h"
#include "hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "general.h"
#include "dynamic.h"

struct bsv_movie
{
   FILE *file;
   uint8_t *state;
   size_t state_size;

   size_t *frame_pos; // A ring buffer keeping track of positions in the file for each frame.
   size_t frame_mask;
   size_t frame_ptr;

   bool playback;
   size_t min_file_pos;

   bool first_rewind;
   bool did_rewind;
};

#define BSV_MAGIC 0x42535631

#define MAGIC_INDEX 0
#define SERIALIZER_INDEX 1
#define CRC_INDEX 2
#define STATE_SIZE_INDEX 3

// Convert to big-endian if needed
static inline uint32_t swap_if_big32(uint32_t val)
{
   if (is_little_endian()) // Little-endian
      return val;
   else
      return (val >> 24) | ((val >> 8) & 0xFF00) | ((val << 8) & 0xFF0000) | (val << 24);
}

static inline uint32_t swap_if_little32(uint32_t val)
{
   if (!is_little_endian()) // Big-endian
      return val;
   else
      return (val >> 24) | ((val >> 8) & 0xFF00) | ((val << 8) & 0xFF0000) | (val << 24);
}

static inline uint16_t swap_if_big16(uint16_t val)
{
   if (is_little_endian())
      return val;
   else
      return (val >> 8) | (val << 8);
}

static bool init_playback(bsv_movie_t *handle, const char *path)
{
   handle->playback = true;
   handle->file = fopen(path, "rb");
   if (!handle->file)
   {
      SSNES_ERR("Couldn't open BSV file \"%s\" for playback.\n", path);
      return false;
   }

   uint32_t header[4] = {0};
   if (fread(header, sizeof(uint32_t), 4, handle->file) != 4)
   {
      SSNES_ERR("Couldn't read movie header.\n");
      return false;
   }

   // Compatibility with old implementation that used incorrect documentation.
   if (swap_if_little32(header[MAGIC_INDEX]) != BSV_MAGIC && swap_if_big32(header[MAGIC_INDEX]) != BSV_MAGIC)
   {
      SSNES_ERR("Movie file is not a valid BSV1 file.\n");
      return false;
   }

   if (swap_if_big32(header[CRC_INDEX]) != g_extern.cart_crc)
   {
      SSNES_ERR("Cart CRC32s differ. Cannot play back.\n");
      return false;
   }

   uint32_t state_size = swap_if_big32(header[STATE_SIZE_INDEX]);

   if (state_size)
   {
      handle->state = (uint8_t*)malloc(state_size);
      handle->state_size = state_size;
      if (!handle->state)
         return false;

      if (fread(handle->state, 1, state_size, handle->file) != state_size)
      {
         SSNES_ERR("Couldn't read state from movie.\n");
         return false;
      }

      if (psnes_serialize_size() == state_size)
         psnes_unserialize(handle->state, state_size);
      else
         SSNES_WARN("Movie format seems to have a different serializer version. Will most likely fail.\n");
   }

   handle->min_file_pos = sizeof(header) + state_size;

   return true;
}

static bool init_record(bsv_movie_t *handle, const char *path)
{
   handle->file = fopen(path, "wb");
   if (!handle->file)
   {
      SSNES_ERR("Couldn't open BSV \"%s\" for recording.\n", path);
      return false;
   }

   uint32_t header[4] = {0};

   // This value is supposed to show up as BSV1 in a HEX editor, big-endian.
   header[MAGIC_INDEX] = swap_if_little32(BSV_MAGIC);

   header[CRC_INDEX] = swap_if_big32(g_extern.cart_crc);

   uint32_t state_size = psnes_serialize_size();

   header[STATE_SIZE_INDEX] = swap_if_big32(state_size);
   fwrite(header, 4, sizeof(uint32_t), handle->file);

   handle->min_file_pos = sizeof(header) + state_size;
   handle->state_size = state_size;

   if (state_size)
   {
      handle->state = (uint8_t*)malloc(state_size);
      if (!handle->state)
         return false;

      psnes_serialize(handle->state, state_size);
      fwrite(handle->state, 1, state_size, handle->file);
   }

   return true;
}

void bsv_movie_free(bsv_movie_t *handle)
{
   if (handle)
   {
      if (handle->file)
         fclose(handle->file);
      free(handle->state);
      free(handle->frame_pos);
      free(handle);
   }
}

bool bsv_movie_get_input(bsv_movie_t *handle, int16_t *input)
{
   if (fread(input, sizeof(int16_t), 1, handle->file) != 1)
      return false;

   *input = swap_if_big16(*input);
   return true;
}

void bsv_movie_set_input(bsv_movie_t *handle, int16_t input)
{
   input = swap_if_big16(input);
   fwrite(&input, sizeof(int16_t), 1, handle->file);
}

bsv_movie_t *bsv_movie_init(const char *path, enum ssnes_movie_type type)
{
   bsv_movie_t *handle = (bsv_movie_t*)calloc(1, sizeof(*handle));
   if (!handle)
      return NULL;

   if (type == SSNES_MOVIE_PLAYBACK)
   {
      if (!init_playback(handle, path))
         goto error;
   }
   else if (!init_record(handle, path))
      goto error;

   // Just pick something really large :D ~1 million frames rewind should do the trick.
   if (!(handle->frame_pos = (size_t*)calloc((1 << 20), sizeof(size_t))))
      goto error; 

   handle->frame_pos[0] = handle->min_file_pos;
   handle->frame_mask = (1 << 20) - 1;

   return handle;

error:
   bsv_movie_free(handle);
   return NULL;
}

void bsv_movie_set_frame_start(bsv_movie_t *handle)
{
   handle->frame_pos[handle->frame_ptr] = ftell(handle->file);
}

void bsv_movie_set_frame_end(bsv_movie_t *handle)
{
   handle->frame_ptr = (handle->frame_ptr + 1) & handle->frame_mask;

   handle->first_rewind = !handle->did_rewind;
   handle->did_rewind = false;
}

void bsv_movie_frame_rewind(bsv_movie_t *handle)
{
   handle->did_rewind = true;

   // If we're at the beginning ... :)
   if ((handle->frame_ptr <= 1) && (handle->frame_pos[0] == handle->min_file_pos))
   {
      handle->frame_ptr = 0;
      fseek(handle->file, handle->min_file_pos, SEEK_SET);
   }
   else
   {
      // First time rewind is performed, the old frame is simply replayed.
      // However, playing back that frame caused us to read data, and push data to the ring buffer.
      // Sucessively rewinding frames, we need to rewind past the read data, plus another.
      handle->frame_ptr = (handle->frame_ptr - (handle->first_rewind ? 1 : 2)) & handle->frame_mask;
      fseek(handle->file, handle->frame_pos[handle->frame_ptr], SEEK_SET);
   }

   // We rewound past the beginning. :O
   if (ftell(handle->file) <= (long)handle->min_file_pos)
   {
      // If recording, we simply reset the starting point. Nice and easy.
      if (!handle->playback)
      {
         fseek(handle->file, 4 * sizeof(uint32_t), SEEK_SET);
         psnes_serialize(handle->state, handle->state_size);
         fwrite(handle->state, 1, handle->state_size, handle->file);
      }
      else
         fseek(handle->file, handle->min_file_pos, SEEK_SET);
   }
}

uint32_t *bsv_header_generate(size_t *size, uint32_t magic)
{
   uint32_t bsv_header[4] = {0};
   unsigned serialize_size = psnes_serialize_size();
   size_t header_size = sizeof(bsv_header) + serialize_size;
   *size = header_size;

   uint32_t *header = (uint32_t*)malloc(header_size);
   if (!header)
      return NULL;

   bsv_header[MAGIC_INDEX] = swap_if_little32(BSV_MAGIC);
   bsv_header[SERIALIZER_INDEX] = swap_if_big32(magic);
   bsv_header[CRC_INDEX] = swap_if_big32(g_extern.cart_crc);
   bsv_header[STATE_SIZE_INDEX] = swap_if_big32(serialize_size);

   if (serialize_size && !psnes_serialize((uint8_t*)header + sizeof(bsv_header), serialize_size))
   {
      free(header);
      return NULL;
   }

   memcpy(header, bsv_header, sizeof(bsv_header));
   return header;
}

bool bsv_parse_header(const uint32_t *header, uint32_t magic)
{
   uint32_t in_bsv = swap_if_little32(header[MAGIC_INDEX]);
   if (in_bsv != BSV_MAGIC)
   {
      SSNES_ERR("BSV magic mismatch, got 0x%x, expected 0x%x.\n",
            in_bsv, BSV_MAGIC);
      return false;
   }

   uint32_t in_magic = swap_if_big32(header[SERIALIZER_INDEX]);
   if (in_magic != magic)
   {
      SSNES_ERR("Magic mismatch, got 0x%x, expected 0x%x.\n", in_magic, magic);
      return false;
   }

   uint32_t in_crc = swap_if_big32(header[CRC_INDEX]);
   if (in_crc != g_extern.cart_crc)
   {
      SSNES_ERR("CRC32 mismatch, got 0x%x, expected 0x%x.\n", in_crc, g_extern.cart_crc);
      return false;
   }

   uint32_t in_state_size = swap_if_big32(header[STATE_SIZE_INDEX]);
   if (in_state_size != psnes_serialize_size())
   {
      SSNES_ERR("Serialization size mismatch, got 0x%x, expected 0x%x.\n",
            in_state_size, psnes_serialize_size());
      return false;
   }

   return true;
}

