/* libSceAjm: audio decoder batches. Batches use the SDK chunk format (the
 * builder functions are imports, but games may also build chunks inline), and
 * run synchronously at submission: decoding a batch takes well under a
 * millisecond, so waiting always finds it complete. ATRAC9 is decoded by
 * LibAtrac9 (MIT, third_party/LibAtrac9); MP3/AAC use FFmpeg. */
#define _GNU_SOURCE
#include "runtime.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include "third_party/LibAtrac9/C/src/libatrac9.h"

#define ERR_INVALID_CONTEXT ((int32_t)0x80930002)
#define ERR_INVALID_INSTANCE ((int32_t)0x80930003)
#define ERR_INVALID_BATCH ((int32_t)0x80930004)
#define ERR_INVALID_PARAMETER ((int32_t)0x80930005)
#define ERR_OUT_OF_RESOURCES ((int32_t)0x80930007)
#define ERR_ALREADY_REGISTERED ((int32_t)0x80930009)
#define ERR_NOT_REGISTERED ((int32_t)0x8093000A)
#define ERR_WRONG_REVISION ((int32_t)0x8093000B)
#define ERR_MALFORMED_BATCH ((int32_t)0x80930011)
#define ERR_CANCELLED ((int32_t)0x80930017)
#define RESULT_NOT_INITIALIZED 0x1
#define RESULT_INVALID_PARAMETER 0x4
#define RESULT_PARTIAL_INPUT 0x8
#define RESULT_NOT_ENOUGH_ROOM 0x10
#define RESULT_CODEC_ERROR 0x40000000u
#define RESULT_FATAL 0x80000000u
#define STATISTICS_INSTANCE 0x80000u
#define MAX_INSTANCES 0x3fff
#define MAX_CONTEXTS 8
#define MAX_BATCHES 256
enum { IDENT_JOB=0, IDENT_INPUT_RUN=1, IDENT_INPUT_CONTROL=2, IDENT_CONTROL_FLAGS=3, IDENT_RUN_FLAGS=4,
       IDENT_RETURN_ADDRESS=6, IDENT_INLINE=7, IDENT_OUTPUT_RUN=17, IDENT_OUTPUT_CONTROL=18 };
enum { FORMAT_S16=0, FORMAT_S32=1, FORMAT_FLOAT=2 };
/* Job flags: version:3 codec:8 run:2 control:3 reserved:29 sideband:3 */
#define RUN_CODEC_INFO(f) (((f)>>11)&1)
#define RUN_MULTIPLE_FRAMES(f) (((f)>>12)&1)
#define CONTROL_RESET(f) (((f)>>13)&1)
#define CONTROL_INITIALIZE(f) (((f)>>14)&1)
#define CONTROL_RESAMPLE(f) (((f)>>15)&1)
#define SIDEBAND_GAPLESS(f) (((f)>>45)&1)
#define SIDEBAND_FORMAT(f) (((f)>>46)&1)
#define SIDEBAND_STREAM(f) (((f)>>47)&1)

typedef struct { uint32_t word; uint32_t size; } Chunk;          /* ident:6 payload:20 reserved:6 */
typedef struct { uint32_t word; uint32_t size; void *address; } ChunkBuffer;
typedef struct { int32_t result, internal_result; } SidebandResult;
typedef struct { int32_t input_consumed, output_written; uint64_t total_decoded_samples; } SidebandStream;
typedef struct { uint32_t channels, channel_mask, sample_rate, encoding, bitrate, reserved; } SidebandFormat;
typedef struct { uint32_t total_samples; uint16_t skip_samples, skipped_samples; } SidebandGapless;
typedef struct { uint32_t super_frame_size, frames_in_super_frame, next_frame_size, frame_samples; } At9Info;
typedef struct { int error_code; const void *job_address; uint32_t command_offset; const void *job_return_address; } BatchError;
_Static_assert(sizeof(ChunkBuffer)==16,"Ajm buffer chunk");
_Static_assert(sizeof(SidebandFormat)==24 && sizeof(SidebandGapless)==8 && sizeof(SidebandStream)==16,"Ajm sideband layout");

typedef struct {
    int used, codec, format, channels_hint, gapless_loop;
    uint32_t codec_flags;
    void *handle;
    unsigned char config[4];
    int initialized;
    Atrac9CodecInfo info;
    AVCodecContext *av_context;
    AVCodecParserContext *av_parser;
    AVFrame *av_frame;
    uint32_t superframe_remain, frames;
    SidebandGapless gapless_init, gapless;
    uint64_t total_samples;
} Instance;
typedef struct { int used, registered[24]; Instance instances[MAX_INSTANCES+1]; } Context;
typedef struct { int used, context, canceled; } Batch;

static HostRecursiveMutex lock = HOST_RECURSIVE_MUTEX_INIT;
static Context *contexts[MAX_CONTEXTS+1];
static Batch batches[MAX_BATCHES];
static size_t jobs_run, at9_frames_decoded, ffmpeg_frames_decoded, batches_run;

/* GOW3R_AUDIO_TRACE=1: instance configurations and jobs that return a non-zero result. */
static int trace_left=-1;
static int ajm_trace(void) {
    if (trace_left<0) { const char *e=getenv("GOW3R_AUDIO_TRACE"); trace_left=e && e[0]=='1' ? 400 : 0; }
    return trace_left>0 && trace_left--;
}
static uint32_t ident(uint32_t word) { return word & 0x3f; }
static uint32_t payload(uint32_t word) { return (word>>6) & 0xfffff; }
static uint32_t header(uint32_t id, uint32_t value) { return (id & 0x3f) | ((value & 0xfffff)<<6); }
static uint32_t channel_mask(int channels) {
    switch (channels) { case 1: return 4; case 2: return 3; case 4: return 0x33; case 6: return 0x60f; case 8: return 0x63f; default: return 0; }
}
static int pcm_size(int format) { return format==FORMAT_S16 ? 2 : 4; }

static void decoder_release(Instance *in) {
    if (in->codec==1) {
        if (in->handle) Atrac9ReleaseHandle(in->handle);
        in->handle=NULL;
    }
    if (in->av_parser) av_parser_close(in->av_parser);
    if (in->av_frame) av_frame_free(&in->av_frame);
    if (in->av_context) avcodec_free_context(&in->av_context);
    in->av_parser=NULL;
    in->initialized=0;
}

static void ffmpeg_reset(Instance *in) {
    enum AVCodecID codec_id=in->codec==0 ? AV_CODEC_ID_MP3 : AV_CODEC_ID_AAC;
    const AVCodec *codec=avcodec_find_decoder(codec_id);
    decoder_release(in);
    in->codec=in->codec==0 ? 0 : 2;
    if (!codec) return;
    in->av_context=avcodec_alloc_context3(codec);
    in->av_parser=av_parser_init(codec_id);
    in->av_frame=av_frame_alloc();
    if (!in->av_context || !in->av_parser || !in->av_frame || avcodec_open2(in->av_context,codec,NULL)<0) {
        decoder_release(in);
        return;
    }
    memset(&in->info,0,sizeof(in->info));
    in->initialized=1;
    in->frames=0;
    if (ajm_trace())
        printf("Audio trace: FFmpeg %s decoder initialized, format %d, flags %#x\n",
               in->codec==0 ? "MP3" : "AAC",in->format,in->codec_flags);
}

/* ---- batch builders ---- */
static unsigned char *put_buffer(unsigned char *p, uint32_t id, const void *address, uint64_t size) {
    ChunkBuffer c={header(id,0),(uint32_t)size,(void *)address}; memcpy(p,&c,sizeof(c)); return p+sizeof(c);
}
static unsigned char *put_flags(unsigned char *p, uint32_t id, uint64_t flags) {
    Chunk c={header(id,(uint32_t)(flags>>32)),(uint32_t)flags}; memcpy(p,&c,sizeof(c)); return p+sizeof(c);
}
static void *finish_job(unsigned char *start, unsigned char *end, uint32_t instance) {
    Chunk job={header(IDENT_JOB,instance),(uint32_t)(end-start-sizeof(Chunk))};
    memcpy(start,&job,sizeof(job));
    return end;
}
static ABI void *job_control(void *buffer, uint32_t instance, uint64_t flags, void *in, uint64_t in_size,
                             void *out, uint64_t out_size, void *return_address) {
    unsigned char *p=(unsigned char *)buffer+sizeof(Chunk);
    if (return_address) p=put_buffer(p,IDENT_RETURN_ADDRESS,return_address,0);
    p=put_buffer(p,IDENT_INPUT_CONTROL,in,in_size);
    flags &= instance==STATISTICS_INSTANCE ? UINT64_C(0xC0018007) : UINT64_C(0x60000000E7FF);
    p=put_flags(p,IDENT_CONTROL_FLAGS,flags);
    p=put_buffer(p,IDENT_OUTPUT_CONTROL,out,out_size);
    return finish_job(buffer,p,instance);
}
static ABI void *job_run(void *buffer, uint32_t instance, uint64_t flags, void *in, uint64_t in_size,
                         void *out, uint64_t out_size, void *sideband, uint64_t sideband_size, void *return_address) {
    unsigned char *p=(unsigned char *)buffer+sizeof(Chunk);
    if (return_address) p=put_buffer(p,IDENT_RETURN_ADDRESS,return_address,0);
    p=put_buffer(p,IDENT_INPUT_RUN,in,in_size);
    p=put_flags(p,IDENT_RUN_FLAGS,flags & UINT64_C(0xE00000001FFF));
    p=put_buffer(p,IDENT_OUTPUT_RUN,out,out_size);
    p=put_buffer(p,IDENT_OUTPUT_CONTROL,sideband,sideband_size);
    return finish_job(buffer,p,instance);
}
typedef struct { void *address; uint64_t size; } AjmBuffer;
static ABI void *job_run_split(void *buffer, uint32_t instance, uint64_t flags, const AjmBuffer *in, uint64_t in_count,
                               const AjmBuffer *out, uint64_t out_count, void *sideband, uint64_t sideband_size,
                               void *return_address) {
    unsigned char *p=(unsigned char *)buffer+sizeof(Chunk);
    if (return_address) p=put_buffer(p,IDENT_RETURN_ADDRESS,return_address,0);
    for (uint64_t i=0;i<in_count;++i) p=put_buffer(p,IDENT_INPUT_RUN,in[i].address,in[i].size);
    p=put_flags(p,IDENT_RUN_FLAGS,flags & UINT64_C(0xE00000001FFF));
    for (uint64_t i=0;i<out_count;++i) p=put_buffer(p,IDENT_OUTPUT_RUN,out[i].address,out[i].size);
    p=put_buffer(p,IDENT_OUTPUT_CONTROL,sideband,sideband_size);
    return finish_job(buffer,p,instance);
}
static ABI void *job_inline(void *buffer, const void *data, uint64_t size, const void **address) {
    Chunk c={header(IDENT_INLINE,0),(uint32_t)((size+7)&~UINT64_C(7))};
    memcpy(buffer,&c,sizeof(c));
    unsigned char *p=(unsigned char *)buffer+sizeof(c);
    memcpy(p,data,size);
    if (address) *address=p;
    return p+c.size;
}

/* ---- ATRAC9 decoding ---- */
static void at9_reset(Instance *in) {
    if (in->handle) Atrac9ReleaseHandle(in->handle);
    in->handle=Atrac9GetHandle();
    in->initialized = Atrac9InitDecoder(in->handle,in->config)==0;
    if (in->initialized) Atrac9GetCodecInfo(in->handle,&in->info);
    in->frames=0; in->superframe_remain=(uint32_t)in->info.superframeSize;
    if (ajm_trace())
        printf("Audio trace: ATRAC9 init %s: %d ch, %d Hz, frame %d samples, %d frames/superframe, superframe %d bytes, format %d, flags %#x\n",
               in->initialized ? "ok" : "FAILED",in->info.channels,in->info.samplingRate,in->info.frameSamples,
               in->info.framesInSuperframe,in->info.superframeSize,in->format,in->codec_flags);
}
static void gapless_reset(Instance *in) {
    in->gapless.total_samples=in->gapless_init.total_samples;
    in->gapless.skip_samples=in->gapless_init.skip_samples;
    in->gapless.skipped_samples=0;
}
static int gapless_end(const Instance *in) { return in->gapless_init.total_samples && !in->gapless.total_samples; }
static uint32_t next_frame_bytes(const Instance *in) {
    uint32_t frame=(uint32_t)in->info.frameSamples;
    uint32_t skip=in->gapless.skip_samples<frame ? in->gapless.skip_samples : frame;
    uint32_t samples=frame-skip;
    if (in->gapless_init.total_samples && in->gapless.total_samples<samples) samples=in->gapless.total_samples;
    return samples*(uint32_t)in->info.channels*(uint32_t)pcm_size(in->format);
}
typedef struct { ChunkBuffer *chunks; int count, current; uint32_t offset; } Output;
static uint64_t output_room(const Output *o) {
    uint64_t room=0;
    for (int i=o->current;i<o->count;++i) room+=o->chunks[i].size-(i==o->current ? o->offset : 0);
    return room;
}
static void output_write(Output *o, const unsigned char *data, uint64_t size) {
    while (size && o->current<o->count) {
        ChunkBuffer *c=&o->chunks[o->current];
        uint64_t n=c->size-o->offset; if (n>size) n=size;
        memcpy((unsigned char *)c->address+o->offset,data,n);
        data+=n; size-=n; o->offset+=(uint32_t)n;
        if (o->offset==c->size) { ++o->current; o->offset=0; }
    }
}
/* One frame; returns result flags. */
static uint32_t at9_frame(Instance *in, const unsigned char **input, uint64_t *input_size, Output *out,
                          uint32_t *samples_written, int32_t *internal) {
    int channels=in->info.channels, frame=in->info.frameSamples, used=0;
    int16_t pcm[256*8];
    if (frame*channels>(int)(sizeof(pcm)/sizeof(*pcm))) return RESULT_CODEC_ERROR|RESULT_FATAL;
    int e=Atrac9Decode(in->handle,*input,pcm,&used);
    if (e) { *internal=e; return RESULT_CODEC_ERROR|RESULT_FATAL; }
    *input+=used; *input_size-=(uint64_t)used; in->superframe_remain-=(uint32_t)used;
    ++at9_frames_decoded;
    uint32_t skip=0;
    if (in->gapless.skip_samples) {
        skip=in->gapless.skip_samples<(uint32_t)frame ? in->gapless.skip_samples : (uint32_t)frame;
        in->gapless.skip_samples-=(uint16_t)skip;
    }
    uint32_t samples=(uint32_t)frame-skip;
    if (in->gapless_init.total_samples && samples>in->gapless.total_samples) samples=in->gapless.total_samples;
    uint64_t room=output_room(out)/(uint64_t)(channels*pcm_size(in->format));
    if (samples>room) samples=(uint32_t)room;
    /* Convert (and optionally de-interleave) into the requested encoding. */
    unsigned char converted[256*8*4];
    int non_interleaved=(in->codec_flags>>8)&1;
    for (uint32_t s=0;s<samples;++s) for (int c=0;c<channels;++c) {
        int16_t v=pcm[(skip+s)*(uint32_t)channels+(uint32_t)c];
        size_t index=non_interleaved ? (size_t)c*samples+s : (size_t)s*(size_t)channels+(size_t)c;
        if (in->format==FORMAT_S16) memcpy(converted+index*2,&v,2);
        else if (in->format==FORMAT_S32) { int32_t w=(int32_t)v<<16; memcpy(converted+index*4,&w,4); }
        else { float f=(float)v/32768.0f; memcpy(converted+index*4,&f,4); }
    }
    output_write(out,converted,(uint64_t)samples*(uint64_t)channels*(uint64_t)pcm_size(in->format));
    *samples_written=samples;
    in->gapless.skipped_samples+=(uint16_t)((uint32_t)frame-samples);
    if (in->gapless_init.total_samples) in->gapless.total_samples-=samples;
    if (++in->frames%(uint32_t)in->info.framesInSuperframe==0) {
        uint64_t skip_bytes=in->superframe_remain<*input_size ? in->superframe_remain : *input_size;
        *input+=skip_bytes; *input_size-=skip_bytes;
        in->superframe_remain=(uint32_t)in->info.superframeSize; in->frames=0;
    } else if (gapless_end(in)) {
        /* Drain the rest of the superframe so the next stream starts aligned. */
        while (in->frames%(uint32_t)in->info.framesInSuperframe && *input_size) {
            if (Atrac9Decode(in->handle,*input,pcm,&used)) break;
            *input+=used; *input_size-=(uint64_t)used; in->superframe_remain-=(uint32_t)used; ++in->frames;
        }
        uint64_t skip_bytes=in->superframe_remain<*input_size ? in->superframe_remain : *input_size;
        *input+=skip_bytes; *input_size-=skip_bytes;
        in->superframe_remain=(uint32_t)in->info.superframeSize; in->frames=0;
    }
    return 0;
}

static enum AVSampleFormat ffmpeg_output_format(const Instance *in) {
    int planar=(in->codec_flags>>8)&1;
    if (in->format==FORMAT_S16) return planar ? AV_SAMPLE_FMT_S16P : AV_SAMPLE_FMT_S16;
    if (in->format==FORMAT_S32) return planar ? AV_SAMPLE_FMT_S32P : AV_SAMPLE_FMT_S32;
    return planar ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_FLT;
}

/* Decode one MP3/AAC access unit. The parser is retained by the AJM instance,
 * so split input buffers and partial frames continue correctly on the next job. */
static uint32_t ffmpeg_frame(Instance *in, const unsigned char **input, uint64_t *input_size, Output *out,
                             uint32_t *samples_written, int32_t *internal) {
    uint8_t *packet_data=NULL;
    int packet_size=0;
    while (*input_size && !packet_size) {
        int available=*input_size>(uint64_t)INT_MAX ? INT_MAX : (int)*input_size;
        int used=av_parser_parse2(in->av_parser,in->av_context,&packet_data,&packet_size,
                                  *input,available,AV_NOPTS_VALUE,AV_NOPTS_VALUE,0);
        if (used<0) { *internal=used; return RESULT_CODEC_ERROR|RESULT_FATAL; }
        *input+=(uint32_t)used;
        *input_size-=(uint32_t)used;
        if (!used && !packet_size) return RESULT_PARTIAL_INPUT;
    }
    if (!packet_size) return RESULT_PARTIAL_INPUT;

    AVPacket packet={0};
    packet.data=packet_data;
    packet.size=packet_size;
    int e=avcodec_send_packet(in->av_context,&packet);
    if (e<0 && e!=AVERROR(EAGAIN)) { *internal=e; return RESULT_CODEC_ERROR|RESULT_FATAL; }
    e=avcodec_receive_frame(in->av_context,in->av_frame);
    if (e==AVERROR(EAGAIN) || e==AVERROR_EOF) { *internal=e; return RESULT_PARTIAL_INPUT; }
    if (e<0) { *internal=e; return RESULT_CODEC_ERROR|RESULT_FATAL; }

    AVFrame *frame=in->av_frame;
    AVChannelLayout source_layout={0};
    if (frame->ch_layout.nb_channels>0) av_channel_layout_copy(&source_layout,&frame->ch_layout);
    else av_channel_layout_default(&source_layout,in->channels_hint>0 ? in->channels_hint : 2);
    int channels=source_layout.nb_channels;
    int sample_rate=frame->sample_rate>0 ? frame->sample_rate : in->av_context->sample_rate;
    int samples=frame->nb_samples;
    if (channels<=0 || sample_rate<=0 || samples<=0) {
        av_channel_layout_uninit(&source_layout);
        av_frame_unref(frame);
        return RESULT_CODEC_ERROR|RESULT_FATAL;
    }

    uint64_t required=(uint64_t)samples*(uint64_t)channels*(uint64_t)pcm_size(in->format);
    if (output_room(out)<required) {
        av_channel_layout_uninit(&source_layout);
        av_frame_unref(frame);
        return RESULT_NOT_ENOUGH_ROOM;
    }

    enum AVSampleFormat destination_format=ffmpeg_output_format(in);
    SwrContext *resampler=NULL;
    e=swr_alloc_set_opts2(&resampler,&source_layout,destination_format,sample_rate,
                          &source_layout,(enum AVSampleFormat)frame->format,sample_rate,0,NULL);
    if (e<0 || !resampler || (e=swr_init(resampler))<0) {
        if (resampler) swr_free(&resampler);
        av_channel_layout_uninit(&source_layout);
        av_frame_unref(frame);
        *internal=e;
        return RESULT_CODEC_ERROR|RESULT_FATAL;
    }

    int bytes=av_samples_get_buffer_size(NULL,channels,samples,destination_format,1);
    unsigned char *converted=bytes>0 ? malloc((size_t)bytes) : NULL;
    uint8_t *planes[AV_NUM_DATA_POINTERS]={0};
    int line_size=0;
    if (!converted || av_samples_fill_arrays(planes,&line_size,converted,channels,samples,destination_format,1)<0) {
        free(converted);
        swr_free(&resampler);
        av_channel_layout_uninit(&source_layout);
        av_frame_unref(frame);
        *internal=AVERROR(ENOMEM);
        return RESULT_CODEC_ERROR|RESULT_FATAL;
    }
    int converted_samples=swr_convert(resampler,planes,samples,(const uint8_t **)frame->extended_data,samples);
    if (converted_samples<0) {
        *internal=converted_samples;
        free(converted);
        swr_free(&resampler);
        av_channel_layout_uninit(&source_layout);
        av_frame_unref(frame);
        return RESULT_CODEC_ERROR|RESULT_FATAL;
    }

    uint64_t output_bytes=(uint64_t)converted_samples*(uint64_t)channels*(uint64_t)pcm_size(in->format);
    output_write(out,converted,output_bytes);
    *samples_written=(uint32_t)converted_samples;
    in->info.channels=channels;
    in->info.samplingRate=sample_rate;
    in->info.frameSamples=converted_samples;
    in->info.framesInSuperframe=1;
    in->info.superframeSize=packet_size;
    in->superframe_remain=0;
    ++in->frames;
    ++ffmpeg_frames_decoded;

    free(converted);
    swr_free(&resampler);
    av_channel_layout_uninit(&source_layout);
    av_frame_unref(frame);
    return 0;
}
static void parse_riff(Instance *in, const unsigned char **input, uint64_t *size) {
    const unsigned char *p=*input, *end=p+*size;
    p+=12;
    while (p+8<=end) {
        uint32_t tag, length; memcpy(&tag,p,4); memcpy(&length,p+4,4); p+=8;
        if (tag==0x61746164) break;                                   /* "data" */
        if (tag==0x20746d66 && length>=52 && p+52<=end) {             /* "fmt " */
            memcpy(in->config,p+44,4); at9_reset(in);
        } else if (tag==0x74636166 && length>=12 && p+12<=end) {      /* "fact" */
            uint32_t total, delay; memcpy(&total,p,4); memcpy(&delay,p+4,4);
            in->gapless_init.total_samples=total; in->gapless_init.skip_samples=(uint16_t)delay;
            gapless_reset(in);
        }
        p+=length;
    }
    *size-=(uint64_t)(p-*input); *input=p;
}

/* ---- job execution ---- */
typedef struct {
    uint64_t flags; int have_flags;
    ChunkBuffer inputs[16]; int input_count;
    ChunkBuffer outputs[16]; int output_count;
    ChunkBuffer input_control, output_control;
} Job;
static int parse_job(const unsigned char *p, uint32_t size, Job *job) {
    memset(job,0,sizeof(*job));
    for (const unsigned char *end=p+size; p<end;) {
        uint32_t word; memcpy(&word,p,4);
        switch (ident(word)) {
        case IDENT_CONTROL_FLAGS: case IDENT_RUN_FLAGS: {
            Chunk c; memcpy(&c,p,8);
            job->flags=((uint64_t)payload(c.word)<<32)|c.size; job->have_flags=1; p+=8; break;
        }
        case IDENT_INPUT_RUN: if (job->input_count==16) return -1; memcpy(&job->inputs[job->input_count++],p,16); p+=16; break;
        case IDENT_OUTPUT_RUN: if (job->output_count==16) return -1; memcpy(&job->outputs[job->output_count++],p,16); p+=16; break;
        case IDENT_INPUT_CONTROL: memcpy(&job->input_control,p,16); p+=16; break;
        case IDENT_OUTPUT_CONTROL: memcpy(&job->output_control,p,16); p+=16; break;
        case IDENT_RETURN_ADDRESS: p+=16; break;
        default: return -1;
        }
    }
    return job->have_flags ? 0 : -1;
}
static void run_job(Context *ctx, uint32_t id, Job *job) {
    ++jobs_run;
    unsigned char *side=job->output_control.size ? job->output_control.address : NULL;
    unsigned char *side_end=side ? side+job->output_control.size : NULL;
    SidebandResult *result=NULL;
    if (side && side+sizeof(SidebandResult)<=side_end) { result=(void *)side; side+=sizeof(*result); memset(result,0,sizeof(*result)); }
    if (id==STATISTICS_INSTANCE) { if (side) memset(side,0,(size_t)(side_end-side)); return; }
    Instance *in=&ctx->instances[id & MAX_INSTANCES];
    if ((id & MAX_INSTANCES)==0 || !in->used) { if (result) result->result=(int32_t)(RESULT_FATAL|RESULT_INVALID_PARAMETER); return; }
    uint64_t f=job->flags;
    uint32_t flags_result=0;
    const unsigned char *control=job->input_control.size ? job->input_control.address : NULL;
    const unsigned char *control_end=control ? control+job->input_control.size : NULL;
    if (CONTROL_RESET(f)) {
        in->total_samples=0;
        gapless_reset(in);
        if (in->initialized) {
            if (in->codec==1) at9_reset(in);
            else ffmpeg_reset(in);
        }
    }
    if (control && SIDEBAND_FORMAT(f) && control+24<=control_end) control+=24; /* output format fixed per instance */
    if (control && SIDEBAND_GAPLESS(f) && control+8<=control_end) {
        SidebandGapless g; memcpy(&g,control,8); control+=8;
        uint32_t processed=in->gapless_init.total_samples-in->gapless.total_samples;
        if (g.total_samples || !g.skip_samples) {
            if (g.total_samples>=processed) {
                int64_t diff=(int64_t)in->gapless_init.total_samples-g.total_samples;
                in->gapless_init.total_samples=g.total_samples; in->gapless.total_samples=(uint32_t)(in->gapless.total_samples-diff);
            } else flags_result|=RESULT_INVALID_PARAMETER;
        }
        uint32_t skipped=(uint32_t)in->gapless_init.skip_samples-in->gapless.skip_samples;
        if (g.skip_samples || !g.total_samples) {
            if (g.skip_samples>=skipped) {
                int32_t diff=(int32_t)in->gapless_init.skip_samples-g.skip_samples;
                in->gapless_init.skip_samples=g.skip_samples; in->gapless.skip_samples=(uint16_t)(in->gapless.skip_samples-diff);
            } else flags_result|=RESULT_INVALID_PARAMETER;
        }
    }
    if (control && CONTROL_RESAMPLE(f) && control+8<=control_end) control+=8;
    if (control && CONTROL_INITIALIZE(f) && control+8<=control_end) {
        memcpy(in->config,control,4);
        if (in->codec==1) at9_reset(in);
        else ffmpeg_reset(in);
    }
    uint64_t in_size=0;
    for (int i=0;i<job->input_count;++i) in_size+=job->inputs[i].size;
    unsigned char scratch[4096];
    unsigned char *joined=NULL;
    const unsigned char *input=NULL;
    if (job->input_count==1) input=job->inputs[0].address;
    else if (job->input_count>1) {
        if (in_size <= sizeof(scratch)) joined=scratch;
        else joined=malloc(in_size ? in_size : 1);
        uint64_t at=0;
        for (int i=0;i<job->input_count;++i) { memcpy(joined+at,job->inputs[i].address,job->inputs[i].size); at+=job->inputs[i].size; }
        input=joined;
    }
    Output out={job->outputs,job->output_count,0,0};
    uint64_t in_start=in_size, out_start=output_room(&out);
    uint32_t frames=0;
    int32_t internal=0;
    if (in_size) for (;;) {
        if (in->gapless_loop && gapless_end(in)) { gapless_reset(in); in->total_samples=0; }
        if (in->codec==1 && (in->codec_flags & 1) && in_size>=4 && !memcmp(input,"RIFF",4)) {
            parse_riff(in,&input,&in_size);
            in->total_samples=0;
        }
        if (!in->initialized) { flags_result|=RESULT_NOT_INITIALIZED; break; }
        if (!gapless_end(in) && output_room(&out)<next_frame_bytes(in)) flags_result|=RESULT_NOT_ENOUGH_ROOM;
        if (in->codec==1 && in_size<in->superframe_remain) flags_result|=RESULT_PARTIAL_INPUT;
        if (flags_result) break;
        uint32_t written=0;
        uint32_t r=in->codec==1
            ? at9_frame(in,&input,&in_size,&out,&written,&internal)
            : ffmpeg_frame(in,&input,&in_size,&out,&written,&internal);
        in->total_samples+=written; ++frames;
        if (r) { flags_result|=r; break; }
        if (!RUN_MULTIPLE_FRAMES(f)) break;
    }
    if (joined && joined != scratch) free(joined);
    if (flags_result && ajm_trace())
        printf("Audio trace: Ajm job instance %u flags %#llx -> result %#x internal %d; input %llu/%llu used, output %llu/%llu written, frames %u\n",
               id,(unsigned long long)f,flags_result,internal,(unsigned long long)(in_start-in_size),(unsigned long long)in_start,
               (unsigned long long)(out_start-output_room(&out)),(unsigned long long)out_start,frames);
    if (result) { result->result=(int32_t)flags_result; result->internal_result=internal; }
    /* Output sideband order: stream, format, gapless, multi-frame, codec info. */
    if (side && SIDEBAND_STREAM(f) && side+16<=side_end) {
        SidebandStream s={(int32_t)(in_start-in_size),(int32_t)(out_start-output_room(&out)),in->total_samples};
        memcpy(side,&s,16); side+=16;
    }
    if (side && SIDEBAND_FORMAT(f) && side+24<=side_end) {
        SidebandFormat s={(uint32_t)in->info.channels,channel_mask(in->info.channels),(uint32_t)in->info.samplingRate,
                          (uint32_t)in->format,0,0};
        if (in->info.framesInSuperframe && in->info.frameSamples)
            s.bitrate=(uint32_t)((uint64_t)in->info.samplingRate*(uint64_t)in->info.superframeSize*8/
                                 ((uint64_t)in->info.framesInSuperframe*(uint64_t)in->info.frameSamples));
        memcpy(side,&s,24); side+=24;
    }
    if (side && SIDEBAND_GAPLESS(f) && side+8<=side_end) { memcpy(side,&in->gapless,8); side+=8; }
    if (side && RUN_MULTIPLE_FRAMES(f) && side+8<=side_end) { uint32_t m[2]={frames,0}; memcpy(side,m,8); side+=8; }
    if (side && RUN_CODEC_INFO(f) && side+16<=side_end) {
        At9Info info={(uint32_t)in->info.superframeSize,(uint32_t)in->info.framesInSuperframe,in->superframe_remain,(uint32_t)in->info.frameSamples};
        memcpy(side,&info,16);
    }
}

/* ---- API ---- */
static Context *context(uint32_t id) { return id && id<=MAX_CONTEXTS ? contexts[id] : NULL; }
static ABI int32_t ajm_initialize(int64_t reserved, uint32_t *out) {
    if (!out || reserved) return ERR_INVALID_PARAMETER;
    host_recursive_lock(&lock);
    for (uint32_t i=1;i<=MAX_CONTEXTS;++i) if (!contexts[i]) {
        contexts[i]=calloc(1,sizeof(Context));
        host_recursive_unlock(&lock);
        if (!contexts[i]) return ERR_OUT_OF_RESOURCES;
        *out=i; puts("Runtime: Ajm context initialized (ATRAC9 via LibAtrac9)");
        return 0;
    }
    host_recursive_unlock(&lock);
    return ERR_OUT_OF_RESOURCES;
}
static ABI int32_t ajm_finalize(uint32_t id) {
    host_recursive_lock(&lock);
    Context *c=context(id);
    if (!c) { host_recursive_unlock(&lock); return ERR_INVALID_CONTEXT; }
    for (int i=0;i<=MAX_INSTANCES;++i) decoder_release(&c->instances[i]);
    free(c); contexts[id]=NULL;
    host_recursive_unlock(&lock);
    return 0;
}
static ABI int32_t ajm_module_register(uint32_t id, uint32_t codec, int64_t reserved) {
    if (reserved || codec>=24) return ERR_INVALID_PARAMETER;
    host_recursive_lock(&lock);
    Context *c=context(id);
    int32_t r=!c ? ERR_INVALID_CONTEXT : c->registered[codec] ? ERR_ALREADY_REGISTERED : 0;
    if (!r) c->registered[codec]=1;
    host_recursive_unlock(&lock);
    return r;
}
static ABI int32_t ajm_module_unregister(uint32_t id, uint32_t codec) {
    host_recursive_lock(&lock);
    Context *c=context(id);
    int32_t r=!c ? ERR_INVALID_CONTEXT : codec>=24 ? ERR_INVALID_PARAMETER : !c->registered[codec] ? ERR_NOT_REGISTERED : 0;
    if (!r) c->registered[codec]=0;
    host_recursive_unlock(&lock);
    return r;
}
static ABI int32_t ajm_instance_create(uint32_t id, uint32_t codec, uint64_t flags, uint32_t *out) {
    if (!out || codec>=24) return ERR_INVALID_PARAMETER;
    if (!(flags & 7)) return ERR_WRONG_REVISION;
    if (codec>2) return ERR_INVALID_PARAMETER;
    if (ajm_trace()) printf("Audio trace: Ajm instance create codec %u flags %#llx\n",codec,(unsigned long long)flags);
    host_recursive_lock(&lock);
    Context *c=context(id);
    int32_t r=!c ? ERR_INVALID_CONTEXT : !c->registered[codec] ? ERR_NOT_REGISTERED : ERR_OUT_OF_RESOURCES;
    if (c && c->registered[codec]) for (uint32_t i=1;i<=MAX_INSTANCES;++i) if (!c->instances[i].used) {
        Instance *in=&c->instances[i];
        memset(in,0,sizeof(*in));
        in->used=1; in->codec=(int)codec; in->channels_hint=(int)((flags>>3)&15); in->format=(int)((flags>>7)&7);
        in->gapless_loop=(int)((flags>>10)&1); in->codec_flags=(uint32_t)(flags>>32);
        if (in->format>FORMAT_FLOAT) { in->used=0; r=ERR_INVALID_PARAMETER; break; }
        if (in->codec!=1) {
            ffmpeg_reset(in);
            if (!in->initialized) { in->used=0; r=ERR_OUT_OF_RESOURCES; break; }
        }
        *out=i|(codec<<14); r=0; break;
    }
    host_recursive_unlock(&lock);
    return r;
}
static ABI int32_t ajm_instance_destroy(uint32_t id, uint32_t instance) {
    host_recursive_lock(&lock);
    Context *c=context(id);
    Instance *in=c ? &c->instances[instance & MAX_INSTANCES] : NULL;
    int32_t r=!c ? ERR_INVALID_CONTEXT : !(instance & MAX_INSTANCES) || !in->used ? ERR_INVALID_INSTANCE : 0;
    if (!r) { decoder_release(in); memset(in,0,sizeof(*in)); }
    host_recursive_unlock(&lock);
    return r;
}
static ABI uint32_t ajm_instance_codec(uint32_t instance) { return (instance>>14)&0x1f; }
static ABI int32_t ajm_batch_start(uint32_t id, unsigned char *buffer, uint32_t size, int priority,
                                   BatchError *error, uint32_t *out) {
    (void)priority;
    if (size & 7) return ERR_MALFORMED_BATCH;
    host_recursive_lock(&lock);
    Context *c=context(id);
    if (!c) { host_recursive_unlock(&lock); return ERR_INVALID_CONTEXT; }
    int slot=-1;
    for (int i=0;i<MAX_BATCHES;++i) if (!batches[i].used) { slot=i; break; }
    if (slot<0) { host_recursive_unlock(&lock); return ERR_OUT_OF_RESOURCES; }
    for (unsigned char *p=buffer, *end=buffer+size; p<end;) {
        Chunk chunk; memcpy(&chunk,p,8);
        unsigned char *body=p+8;
        p=body+chunk.size;
        if (ident(chunk.word)==IDENT_INLINE) continue;
        Job job;
        if (ident(chunk.word)!=IDENT_JOB || p>end || parse_job(body,chunk.size,&job)) {
            if (error) { error->error_code=ERR_MALFORMED_BATCH; error->job_address=body-8; error->command_offset=(uint32_t)(body-8-buffer); error->job_return_address=NULL; }
            host_recursive_unlock(&lock);
            return ERR_MALFORMED_BATCH;
        }
        run_job(c,payload(chunk.word),&job);
    }
    batches[slot]=(Batch){1,(int)id,0};
    ++batches_run;
    *out=(uint32_t)slot+1;
    host_recursive_unlock(&lock);
    return 0;
}
static ABI int32_t ajm_batch_wait(uint32_t id, uint32_t batch, uint32_t timeout, BatchError *error) {
    (void)timeout; (void)error;
    host_recursive_lock(&lock);
    int32_t r=!context(id) ? ERR_INVALID_CONTEXT : !batch || batch>MAX_BATCHES || !batches[batch-1].used ? ERR_INVALID_BATCH : 0;
    if (!r) { if (batches[batch-1].canceled) r=ERR_CANCELLED; batches[batch-1].used=0; }
    host_recursive_unlock(&lock);
    return r;
}
static ABI int32_t ajm_batch_cancel(uint32_t id, uint32_t batch) {
    host_recursive_lock(&lock);
    int32_t r=!context(id) ? ERR_INVALID_CONTEXT : !batch || batch>MAX_BATCHES || !batches[batch-1].used ? ERR_INVALID_BATCH : 0;
    host_recursive_unlock(&lock);
    return r; /* already processed at submission: nothing to cancel */
}
static ABI int32_t ajm_error_dump(void) { return 0; }
static ABI int32_t ajm_memory_register(uint32_t id, void *p, uint64_t pages) { (void)p; (void)pages; return context(id) ? 0 : ERR_INVALID_CONTEXT; }
typedef struct {
    uint64_t frame_size;
    uint32_t num_channels,samples_per_channel,bitrate,sample_rate;
    uint32_t encoder_delay,num_frames,total_samples,ofl_type;
} Mp3FrameInfo;
static ABI int32_t ajm_mp3_parse(const unsigned char *data,uint32_t size,int parse_ofl,Mp3FrameInfo *out) {
    (void)parse_ofl;
    if (!data || size<4 || !out) return ERR_INVALID_PARAMETER;
    uint32_t h=((uint32_t)data[0]<<24)|((uint32_t)data[1]<<16)|((uint32_t)data[2]<<8)|data[3];
    if ((h&0xffe00000u)!=0xffe00000u) return ERR_INVALID_PARAMETER;
    unsigned version=(h>>19)&3, layer=(h>>17)&3, bitrate_index=(h>>12)&15;
    unsigned rate_index=(h>>10)&3, padding=(h>>9)&1, channel_mode=(h>>6)&3;
    if (version==1 || layer!=1 || bitrate_index==0 || bitrate_index==15 || rate_index==3)
        return ERR_INVALID_PARAMETER;
    static const uint16_t rates[3][3]={{11025,12000,8000},{0,0,0},{22050,24000,16000}};
    static const uint16_t rates_v1[3]={44100,48000,32000};
    static const uint16_t br_v1[16]={0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const uint16_t br_v2[16]={0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    uint32_t rate=version==3 ? rates_v1[rate_index] : rates[version][rate_index];
    uint32_t bitrate=(version==3 ? br_v1[bitrate_index] : br_v2[bitrate_index])*1000u;
    memset(out,0,sizeof(*out));
    out->sample_rate=rate; out->bitrate=bitrate; out->num_channels=channel_mode==3 ? 1 : 2;
    out->samples_per_channel=version==3 ? 1152 : 576;
    out->frame_size=((version==3 ? 144u : 72u)*bitrate)/rate+padding;
    return 0;
}

static const RuntimeExport exports[]={
    {"sceAjmInitialize",ajm_initialize}, {"sceAjmFinalize",ajm_finalize},
    {"sceAjmModuleRegister",ajm_module_register}, {"sceAjmModuleUnregister",ajm_module_unregister},
    {"sceAjmInstanceCreate",ajm_instance_create}, {"sceAjmInstanceDestroy",ajm_instance_destroy},
    {"sceAjmInstanceCodecType",ajm_instance_codec},
    {"sceAjmBatchJobControlBufferRa",job_control}, {"sceAjmBatchJobRunBufferRa",job_run},
    {"sceAjmBatchJobRunSplitBufferRa",job_run_split}, {"sceAjmBatchJobInlineBuffer",job_inline},
    {"sceAjmBatchStartBuffer",ajm_batch_start}, {"sceAjmBatchWait",ajm_batch_wait},
    {"sceAjmBatchCancel",ajm_batch_cancel}, {"sceAjmBatchErrorDump",ajm_error_dump},
    {"sceAjmMemoryRegister",ajm_memory_register},
    {"sceAjmDecMp3ParseFrame",ajm_mp3_parse},
};
uintptr_t runtime_ajm_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_ajm_report(void) {
    printf("Runtime: Ajm batches=%zu, jobs=%zu, ATRAC9 frames=%zu, MP3/AAC frames=%zu\n",
           batches_run,jobs_run,at9_frames_decoded,ffmpeg_frames_decoded);
}
