"""Exercise the production PCM writer with an I2S sink and failure injection."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'components/furi_hal/furi_hal_speaker.c').read_text()
start = source.index('bool furi_hal_speaker_write_pcm(')
end = source.index('\n#else', start)
writer = source[start:end]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define furi_check assert
#define ESP_OK 0
enum {SpeakerModeIdle, SpeakerModeTone};
static int speaker_mode = SpeakerModeIdle;
static bool i2s_channel_enabled = true;
static void* i2s_tx_handle;
static const int16_t* expected;
static size_t offset, calls;
static int failure;
static bool furi_hal_speaker_is_mine(void) { return true; }
static int i2s_channel_write(void* h, const void* data, size_t bytes,
                             size_t* written, unsigned timeout) {
    (void)h;
    assert(timeout == 1000 && bytes <= 512 && bytes % 4 == 0);
    const int16_t* stereo = data;
    for(size_t i=0; i<bytes/4; ++i) {
        assert(stereo[i*2] == expected[offset+i]);
        assert(stereo[i*2+1] == expected[offset+i]);
    }
    offset += bytes/4; ++calls;
    *written = failure == 2 ? bytes-4 : bytes;
    return failure == 1 ? -1 : ESP_OK;
}
'''
main = r'''
int main(void) {
    int16_t samples[257];
    for(int i=0;i<257;++i) samples[i]=(int16_t)(i*200-25000);
    expected=samples;
    assert(furi_hal_speaker_write_pcm(samples,257));
    assert(calls==3 && offset==257);
    assert(furi_hal_speaker_write_pcm(NULL,0));
    assert(calls==3);
    speaker_mode=SpeakerModeTone;
    assert(!furi_hal_speaker_write_pcm(samples,1));
    speaker_mode=SpeakerModeIdle; i2s_channel_enabled=false;
    assert(!furi_hal_speaker_write_pcm(samples,1));
    i2s_channel_enabled=true;
    for(failure=1;failure<=2;++failure) {
        calls=offset=0;
        assert(!furi_hal_speaker_write_pcm(samples,257));
        assert(calls==1 && offset==128);
    }
}
'''
with tempfile.TemporaryDirectory() as temp:
    path = Path(temp)
    (path/'test.c').write_text(harness + writer + main)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
print('PASS: PCM chunk bounds, stereo conversion, inactive/tone rejection, I2S errors and short writes')
