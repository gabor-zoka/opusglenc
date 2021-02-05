#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h> /* PRIu64 */
#include <math.h>
#include <regex.h>
#include <assert.h>
#include <string.h>
#include <errno.h>

#include <FLAC/stream_decoder.h>
#include <opusenc.h>



typedef struct {
  /* input params */
  const char*      in_path;
  const char*      out_path;

  /* internal params */
  unsigned         sample_rate;
  unsigned         channels;
  float            scale;
  unsigned         max_blocksize;
  OggOpusComments* comments;
  OggOpusEnc*      enc;
  float*           enc_buffer;
} Client;



FLAC__StreamDecoderWriteStatus
write_cb(const FLAC__StreamDecoder* dec,
         const FLAC__Frame*         frame,
         const FLAC__int32* const   buffer[],
         void*                      client) {
  dec;
  Client* cli = client;

	/* Set up the encoder before we write the first frame */
	if(frame->header.number.sample_number == 0) {
	  if (cli->channels > 2) {
		  fprintf(stderr, "ERROR: Only mono and stereo are supported.\n");
		  return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	  }

    int error;
    cli->enc = ope_encoder_create_file(cli->out_path, cli->comments,
        cli->sample_rate, cli->channels, 0, &error);

    if (!cli->enc) {
      fprintf(stderr, "ERROR: Encoding to file %s: %s\n", cli->out_path, ope_strerror(error));
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    cli->enc_buffer = malloc(cli->channels * cli->max_blocksize * sizeof(float));
    if (!cli->enc_buffer) {
      fprintf(stderr, "ERROR: Memory allocation");
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
  }

  float scale = cli->scale;
  unsigned channels = cli->channels;
  for(unsigned c = 0; c != channels; ++c) {
    float*                   o    = cli->enc_buffer + c;
    const FLAC__int32*       i    = buffer[c];
    const FLAC__int32* const iend = buffer[c] + frame->header.blocksize;

    while(i != iend) {
      *o = scale * *i;

      o += channels;
      i += 1;
    }
  }

  ope_encoder_write_float(cli->enc, cli->enc_buffer, frame->header.blocksize);

	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}



double
read_gain(const char* const   str,
          const regmatch_t    pmatch,
          const Client* const cli) {
  assert(pmatch.rm_so != -1);

  const regoff_t len      = pmatch.rm_eo - pmatch.rm_so;
  char* const    gain_str = malloc(len + 1); assert(gain_str != NULL);
  memcpy(gain_str, str + pmatch.rm_so, len);
  gain_str[len] = '\0';

  double gain = strtod(gain_str, NULL);
  if (errno) {
    fprintf(stderr, "ERROR: Parsing %s of %s\n", str, cli->in_path);
    exit(EXIT_FAILURE);
  }

  free(gain_str);

  return gain;
}



// From https://github.com/Moonbase59/loudgain/blob/master/src/tag.cc
int gain_to_q78num(const double gain) {
  // convert float to Q7.8 number: Q = round(f * 2^8)
  return (int) round(gain * 256.0);    // 2^8 = 256
}



void
add_r128_gain_tag(OggOpusComments* const comments,
                  const char* const      key,
                  const double           gain) {
  // convert float to Q7.8 number: Q = round(f * 2^8)
  // See gain_to_q78num in 
  // https://github.com/Moonbase59/loudgain/blob/master/src/tag.cc
  const int i   = round(gain * 256.0);
  const int len = snprintf(NULL, 0, "%d", i) + 1;
  char*     str = malloc(len); assert(str != NULL);
  snprintf(str, len, "%d", i);

  ope_comments_add(comments, key, str);

  free(str);
}



void
meta_cb(const FLAC__StreamDecoder*  dec,
        const FLAC__StreamMetadata* meta,
        void*                       client) {
	dec;
  Client* cli = client;

	if (meta->type == FLAC__METADATA_TYPE_STREAMINFO) {
    cli->sample_rate     = meta->data.stream_info.sample_rate;
    cli->channels        = meta->data.stream_info.channels;
    cli->scale           = exp(-(meta->data.stream_info.bits_per_sample - 1.0) * M_LN2);
    cli->max_blocksize   = meta->data.stream_info.max_blocksize;
    cli->comments        = ope_comments_create();
	}
  else if (meta->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
    FLAC__StreamMetadata_VorbisComment_Entry* entry     = meta->data.vorbis_comment.comments;
    FLAC__StreamMetadata_VorbisComment_Entry* entry_end = meta->data.vorbis_comment.comments +
      meta->data.vorbis_comment.num_comments;

    regex_t replaygain_re, album_gain_re, track_gain_re;
    assert(regcomp(&replaygain_re, "^REPLAYGAIN_",                    REG_ICASE) == 0);
    assert(regcomp(&album_gain_re, "^REPLAYGAIN_ALBUM_GAIN=\\(.*\\)", REG_ICASE) == 0);
    assert(regcomp(&track_gain_re, "^REPLAYGAIN_TRACK_GAIN=\\(.*\\)", REG_ICASE) == 0);

    regmatch_t pmatch[2];

    double album_gain = NAN;
    double track_gain = NAN;

    while (entry != entry_end) {
      const char* const comment = entry->entry;

      if (regexec(&replaygain_re, comment, 2, pmatch, 0)) {
        // Not REPLAYGAIN_*
        ope_comments_add_string(cli->comments, comment);
      }
      else if (!regexec(&album_gain_re, comment, 2, pmatch, 0)) {
        album_gain = read_gain(comment, pmatch[1], cli);
      }
      else if (!regexec(&track_gain_re, comment, 2, pmatch, 0)) {
        track_gain = read_gain(comment, pmatch[1], cli);
      }

      ++entry;
    }

    if (!isnan(album_gain) && album_gain < 20.0) {
      // album_gain uses -18LUFS, but Opus (and me) wants to use -23LUFS as 
      // target loudness.
      cli->scale *= exp((album_gain - 5.0) / 20.0 * M_LN10);

      if (!isnan(track_gain))
        add_r128_gain_tag(cli->comments, "R128_TRACK_GAIN", track_gain - album_gain);
    }
    else if (!isnan(track_gain))
      add_r128_gain_tag(cli->comments, "R128_TRACK_GAIN", track_gain);
  }
}



void
error_cb(const FLAC__StreamDecoder*     dec,
         FLAC__StreamDecoderErrorStatus status,
         void*                          client) {
	dec, client;

	fprintf(stderr, "ERROR: %s\n", FLAC__StreamDecoderErrorStatusString[status]);
}



int main(int argc, char *argv[])
{
	if(argc != 3) {
		fprintf(stderr, "USAGE: %s infile.flac outfile.opus\n", argv[0]);
		return 1;
	}

	FLAC__StreamDecoder* dec = FLAC__stream_decoder_new();
	if(dec == NULL) {
		fprintf(stderr, "ERROR: Allocating decoder\n");
		return 1;
	}

  FLAC__stream_decoder_set_md5_checking(dec, true);
  FLAC__stream_decoder_set_metadata_respond(dec, FLAC__METADATA_TYPE_VORBIS_COMMENT);

  Client client;
  client.in_path  = argv[1];
  client.out_path = argv[2];

  FLAC__StreamDecoderInitStatus	init_status =
  FLAC__stream_decoder_init_file(dec, argv[1], write_cb, meta_cb, error_cb, &client);
	if(init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		fprintf(stderr, "ERROR: initializing decoder: %s\n", FLAC__StreamDecoderInitStatusString[init_status]);
		return 1;
	}

	FLAC__bool ok = FLAC__stream_decoder_process_until_end_of_stream(dec);

	FLAC__stream_decoder_delete(dec);
  ope_encoder_drain(client.enc);
  ope_encoder_destroy(client.enc);
  ope_comments_destroy(client.comments);
  free(client.enc_buffer);

	return !ok;
}
