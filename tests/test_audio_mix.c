/* Synthetic AudioOut tests: no ROM, audio device, or Vulkan device required. */
#include <assert.h>
#include <math.h>
#include "../src/runtime_audio.c"

uint64_t host_monotonic_ns(void) { return 1000000000; }
void host_sleep_until_ns(uint64_t deadline) { (void)deadline; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static void assert_near(float actual, float expected) {
    assert(fabsf(actual-expected)<0.000001f);
}

static Port make_port(int channels, int is_float, int std_layout, int downmix, int frames) {
    Port p={0};
    p.channels=channels; p.is_float=is_float; p.std_layout=std_layout;
    p.sample_bytes=is_float ? 4 : 2; p.frames=frames; p.stereo_downmix=downmix;
    p.output_channels=downmix ? 2 : channels;
    p.output_sample_bytes=downmix ? 4 : p.sample_bytes;
    for (int c=0;c<8;++c) p.volume[c]=VOLUME_0DB;
    return p;
}

static void test_matrix_impulses(void) {
    const float left[8]={1,0,0.7071f,0,0.7071f,0,0.7071f,0};
    const float right[8]={0,1,0.7071f,0,0,0.7071f,0,0.7071f};
    /* Cover S16_8CH, Float_8CH and both _Std variants. */
    for (int is_float=0;is_float<=1;++is_float) for (int layout=0;layout<=1;++layout) {
        Port p=make_port(8,is_float,layout,1,1);
        for (int c=0;c<8;++c) {
            float input_f32[8]={0}, output[3]={0,0,1234};
            int16_t input_s16[8]={0};
            input_f32[c]=0.5f; input_s16[c]=16384;
            const void *input=is_float ? (const void *)input_f32 : (const void *)input_s16;
            assert(convert_port_audio(&p,input,output)==2*sizeof(float));
            assert_near(output[0],0.5f*left[c]); assert_near(output[1],0.5f*right[c]);
            assert(output[2]==1234);
        }
    }
}

static void test_sum_and_guest_volume(void) {
    Port p=make_port(8,1,0,1,2);
    float input[16]={0.5f,0.25f,0.5f,100.0f,0.5f,0.25f,0.5f,0.25f,
                     -0.5f,-0.25f,-0.5f,-100.0f,-0.5f,-0.25f,-0.5f,-0.25f};
    float output[4];
    assert(convert_port_audio(&p,input,output)==sizeof(output));
    assert_near(output[0],0.5f+0.7071f*1.5f); assert_near(output[1],0.25f+0.7071f);
    assert_near(output[2],-output[0]); assert_near(output[3],-output[1]);
    assert(output[0]>1.0f); /* match fork: no 0.60 attenuation or pre-SDL limiter */
    p.volume[0]=VOLUME_0DB/2; p.volume[2]=0; p.volume[4]=0; p.volume[6]=0;
    convert_port_audio(&p,input,output);
    assert_near(output[0],0.25f); assert_near(output[1],0.25f+0.7071f*0.5f);
    assert_near(output[2],-output[0]); assert_near(output[3],-output[1]);
    for (int c=0;c<8;++c) p.volume[c]=0;
    convert_port_audio(&p,input,output);
    for (int c=0;c<4;++c) assert_near(output[c],0);
}

static void test_native_paths(void) {
    float input[8]={0.125f,0.25f,0.375f,0.5f,0.625f,0.75f,0.875f,-1}, output[8];
    Port p=make_port(8,1,0,0,1);
    const int remap[8]={0,1,2,3,6,7,4,5};
    assert(convert_port_audio(&p,input,output)==sizeof(output));
    for (int c=0;c<8;++c) assert_near(output[remap[c]],input[c]);
    p.std_layout=1;
    convert_port_audio(&p,input,output);
    for (int c=0;c<8;++c) assert_near(output[c],input[c]);
    for (int channels=1;channels<=2;++channels) {
        p=make_port(channels,1,0,0,1);
        assert(convert_port_audio(&p,input,output)==(size_t)channels*sizeof(float));
        for (int c=0;c<channels;++c) assert_near(output[c],input[c]);
        int16_t s16[2]={16384,-16384}, converted[2];
        p=make_port(channels,0,0,0,1);
        assert(convert_port_audio(&p,s16,converted)==(size_t)channels*sizeof(int16_t));
        assert(converted[0]==16383);
        if (channels==2) assert(converted[1]==-16383);
    }
}

static void test_maximum_buffer(void) {
    Port p=make_port(8,1,0,1,2048);
    float input[2048*8]={0}, output[2048*2+1];
    output[2048*2]=1234;
    input[(2048-1)*8]=0.5f;
    assert(convert_port_audio(&p,input,output)==2048*2*sizeof(float));
    assert_near(output[(2048-1)*2],0.5f);
    assert(output[2048*2]==1234);
}

static void test_output_uses_stereo_bytes_but_guest_sample_count(void) {
    /* A disconnected SDL stream exercises queueing without opening a real device. */
    Port *p=&ports[0];
    *p=make_port(8,1,0,1,256); p->used=1;
    host_mutex_init(&p->lock);
    SDL_AudioSpec spec={SDL_AUDIO_F32,2,48000};
    p->stream=SDL_CreateAudioStream(&spec,&spec);
    assert(p->stream);
    float input[256*8]={0}; input[0]=0.5f;
    p->dump=tmpfile(); assert(p->dump);
    const int output_bytes=256*2*(int)sizeof(float);
    assert(output_port(0x20000000,input,0)==256*8);
    assert(SDL_GetAudioStreamQueued(p->stream)==output_bytes*2);
    assert(ftell(p->dump)==output_bytes);
    rewind(p->dump);
    float first[2]; assert(fread(first,sizeof(float),2,p->dump)==2);
    assert_near(first[0],0.5f); assert_near(first[1],0);
    assert(output_port(0x20000000,NULL,0)==0);
    assert(SDL_GetAudioStreamQueued(p->stream)==output_bytes*2);
    assert(audio_close(0x20000000)==0);
}

static void test_device_open(void) {
    /* Optional integration test: opens the default device and sends silence only. */
    assert(audio_init()==0);
    for (int format=5;format<=6;++format) {
        int32_t handle=audio_open(1,0,0,256,48000,(uint32_t)format);
        assert(handle>0);
        int32_t error=0;
        Port *p=port_of(handle,&error);
        assert(p && p->stream);
        SDL_AudioSpec input_spec, device_spec;
        assert(SDL_GetAudioStreamFormat(p->stream,&input_spec,NULL));
        assert(SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(p->stream),&device_spec,NULL));
        assert(input_spec.channels==p->output_channels);
        assert(input_spec.format==(p->output_sample_bytes==4 ? SDL_AUDIO_F32 : SDL_AUDIO_S16));
        assert(p->stereo_downmix==needs_stereo_downmix(8,device_spec.channels));
        unsigned char silence[256*8*4]={0};
        assert(output_port(handle,silence,0)==256*8);
        printf("Audio device smoke: format=%d, host=%dch, stream=%dch/%d bytes, stereo mix=%d\n",
               format,device_spec.channels,p->output_channels,p->output_sample_bytes,p->stereo_downmix);
        assert(audio_close(handle)==0);
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}

int main(int argc, char **argv) {
    assert(needs_stereo_downmix(8,1) && needs_stereo_downmix(8,2));
    assert(!needs_stereo_downmix(8,0) && !needs_stereo_downmix(8,6)
           && !needs_stereo_downmix(8,8) && !needs_stereo_downmix(2,2));
    test_matrix_impulses();
    test_sum_and_guest_volume();
    test_native_paths();
    test_maximum_buffer();
    test_output_uses_stereo_bytes_but_guest_sample_count();
    if (argc>1 && strcmp(argv[1],"--device-smoke")==0) test_device_open();
    puts("Audio mix tests: PASS (cuesta4 matrix, F32/S16, layouts, volumes, queue and ABI)");
    return 0;
}
