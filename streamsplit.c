/*
 * Copyright © 2010 Matthew Gregan <kinetik@flim.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ogg/ogg.h>
#include <theora/theoradec.h>

void
usage(void)
{
  fprintf(stderr, "usage: streamsplit INTERVAL FILE\n");
  exit(EXIT_FAILURE);
}

void
fatal(void)
{
  perror("fatal");
  exit(EXIT_FAILURE);
}

int
read_page(FILE * fp, ogg_sync_state * state, ogg_page * page)
{
  if (feof(fp)) {
    return ogg_sync_pageout(state, page);
  }

  while (!ogg_sync_pageout(state, page)) {
    char * p = ogg_sync_buffer(state, 1024);
    assert(p);

    size_t read = fread(p, 1, 1024, fp);
    if (read < 1024) {
      return ogg_sync_pageout(state, page);
    }

    int r = ogg_sync_wrote(state, read);
    assert(r == 0);
  }

  return 1;
}

int
read_packet(FILE * fp, ogg_sync_state * state, ogg_stream_state * stream, ogg_packet * packet)
{
  while (!ogg_stream_packetout(stream, packet)) {
    ogg_page page;
    if (!read_page(fp, state, &page)) {
      return 0;
    }

    int r = ogg_stream_pagein(stream, &page);
    assert(r == 0);
  }

  return 1;
}

int
main(int argc, char * argv[])
{
  if (argc != 3)
    usage();

  unsigned long interval = strtoul(argv[1], NULL, 10);
  if ((interval == 0 && errno == EINVAL) || (interval == ULONG_MAX && errno == ERANGE))
    fatal();

  char const * path = argv[2];

  FILE * fp = fopen(path, "rb");
  if (!fp)
    fatal();

  ogg_sync_state state;

  int r = ogg_sync_init(&state);
  assert(r == 0);

  /* Read headers. */
  ogg_page page;
  int got_headers = 0;
  ogg_stream_state stream;
  int serial = -1;

  th_info t_info;
  th_info_init(&t_info);

  th_comment t_comment;
  th_comment_init(&t_comment);

  ogg_stream_state ostream;

  th_setup_info * t_setup = NULL;

  while (!got_headers && read_page(fp, &state, &page)) {
    int sl = ogg_page_serialno(&page);
    if (serial == -1) {
      serial = sl;
      r = ogg_stream_init(&stream, serial);
      assert(r == 0);

      r = ogg_stream_init(&ostream, serial);
      assert(r == 0);
    }
    if (serial != sl) {
      fatal();
    }

    r = ogg_stream_pagein(&stream, &page);
    assert(r == 0);

    ogg_packet packet;
    while (!got_headers && ogg_stream_packetpeek(&stream, &packet)) {
      r = th_decode_headerin(&t_info, &t_comment, &t_setup, &packet);
      if (r == TH_ENOTFORMAT || r < 0) {
        fatal();
      }
      got_headers = r == 0;
      if (!got_headers) {
        r = ogg_stream_packetout(&stream, &packet);
        assert(r == 1);
        r = ogg_stream_packetin(&ostream, &packet);
        assert(r == 0);
      }
    }
  }

  unsigned char * headers = NULL;
  size_t hsize = 0;
  ogg_page opage;
  while (ogg_stream_pageout(&ostream, &opage)) {
    headers = realloc(headers, hsize + opage.header_len + opage.body_len);
    assert(headers);
    memcpy(headers + hsize, opage.header, opage.header_len);
    hsize += opage.header_len;
    memcpy(headers + hsize, opage.body, opage.body_len);
    hsize += opage.body_len;
  }

  r = ogg_stream_flush(&ostream, &opage);
  if (r > 0) {
    headers = realloc(headers, hsize + opage.header_len + opage.body_len);
    assert(headers);
    memcpy(headers + hsize, opage.header, opage.header_len);
    hsize += opage.header_len;
    memcpy(headers + hsize, opage.body, opage.body_len);
    hsize += opage.body_len;
  }

  th_dec_ctx * t_ctx = th_decode_alloc(&t_info, t_setup);
  assert(t_ctx);

  unsigned long kfcount = 0;
  unsigned long count = 0;

  char namebuf[100];
  sprintf(namebuf, "out%04lu.ogv", count);

  FILE * fw = fopen(namebuf, "wb");
  fwrite(headers, 1, hsize, fw);

  ogg_packet packet;
  while (read_packet(fp, &state, &stream, &packet)) {
    if (th_packet_iskeyframe(&packet)) {
      if (kfcount > 0 && kfcount % interval == 0) {
        ogg_page opage;
        while (ogg_stream_pageout(&ostream, &opage)) {
          fwrite(opage.header, 1, opage.header_len, fw);
          fwrite(opage.body, 1, opage.body_len, fw);
        }

        r = ogg_stream_flush(&ostream, &opage);
        if (r > 0) {
          fwrite(opage.header, 1, opage.header_len, fw);
          fwrite(opage.body, 1, opage.body_len, fw);
        }

        fclose(fw);
        count += 1;
        sprintf(namebuf, "out%04lu.ogv", count);

        FILE * fw = fopen(namebuf, "wb");
        fwrite(headers, 1, hsize, fw);
      }
      kfcount += 1;
    }
    r = ogg_stream_packetin(&ostream, &packet);
    assert(r == 0);
  }

  while (ogg_stream_pageout(&ostream, &opage)) {
    fwrite(opage.header, 1, opage.header_len, fw);
    fwrite(opage.body, 1, opage.body_len, fw);
  }

  r = ogg_stream_flush(&ostream, &opage);
  if (r > 0) {
    fwrite(opage.header, 1, opage.header_len, fw);
    fwrite(opage.body, 1, opage.body_len, fw);
  }

  fclose(fw);

  return EXIT_SUCCESS;
}
