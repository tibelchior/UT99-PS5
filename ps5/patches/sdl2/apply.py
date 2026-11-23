#!/usr/bin/env python3
"""PS5 patches for SDL2 2.28.x (applied to a copy in the build volume; idempotent).
 - dummy video: every window-surface update goes to ut_ps5_present (the console's display)
 - dummy audio: mixed blocks go to ut_ps5_audio_play (sceAudioOut), 48 kHz S16 stereo
"""
import sys, pathlib
root = pathlib.Path(sys.argv[1])

def patch(path, old, new, marker):
    p = root / path
    s = p.read_text()
    if marker in s:
        return
    if old not in s:
        sys.exit(f"patch anchor not found in {path}: {old[:50]!r}")
    p.write_text(s.replace(old, new, 1))

# --- video
patch("src/video/dummy/SDL_nullframebuffer.c",
      '    /* Send the data to the display */\n',
      '    /* Send the data to the display */\n'
      '    if (ut_ps5_present) { /* PS5 */\n'
      '        ut_ps5_present(surface->pixels, surface->w, surface->h, surface->pitch);\n'
      '    }\n',
      "ut_ps5_present(surface")
patch("src/video/dummy/SDL_nullframebuffer.c",
      'int SDL_DUMMY_UpdateWindowFramebuffer(',
      '/* PS5: provided by the title (ps5_present.cpp); weak so other builds link without it. */\n'
      'extern void ut_ps5_present(const void *pixels, int width, int height, int pitch) __attribute__((weak));\n\n'
      'int SDL_DUMMY_UpdateWindowFramebuffer(',
      "extern void ut_ps5_present")

# --- audio
patch("src/audio/dummy/SDL_dummyaudio.c",
      'static int DUMMYAUDIO_OpenDevice(_THIS, const char *devname)\n{\n',
      '/* PS5: the title provides the console\'s audio output (ps5_io.cpp). */\n'
      'extern void ut_ps5_audio_play(const void *frames, int count) __attribute__((weak));\n\n'
      '/* SDL only plays when the driver hands out a device buffer (the default returns NULL: it just sleeps). */\n'
      'static Uint8 *ps5_buf; static int ps5_buf_size;\n'
      'static Uint8 *DUMMYAUDIO_GetDeviceBuf(_THIS)\n{\n'
      '    if (!ut_ps5_audio_play) return NULL;\n'
      '    if (!ps5_buf || ps5_buf_size != (int)_this->spec.size) {\n'
      '        SDL_free(ps5_buf);\n        ps5_buf_size = (int)_this->spec.size;\n        ps5_buf = (Uint8 *)SDL_calloc(1, ps5_buf_size);\n'
      '    }\n    return ps5_buf;\n}\n\n'
      'static void DUMMYAUDIO_PlayDevice(_THIS)\n{\n'
      '    if (ut_ps5_audio_play && ps5_buf) {\n'
      '        ut_ps5_audio_play(ps5_buf, _this->spec.size / 4); /* S16 stereo frames; blocks at the console\'s pace */\n'
      '    }\n}\n\n'
      'static void DUMMYAUDIO_WaitDevice(_THIS)\n{\n'
      '    if (!ut_ps5_audio_play) {\n        SDL_Delay((_this->spec.samples * 1000) / _this->spec.freq);\n    }\n}\n\n'
      'static int DUMMYAUDIO_OpenDevice(_THIS, const char *devname)\n{\n'
      '    if (ut_ps5_audio_play && !_this->iscapture) {\n'
      '        _this->spec.format = AUDIO_S16SYS;\n        _this->spec.channels = 2;\n'
      '        _this->spec.freq = 48000;\n        _this->spec.samples = 1024;\n    }\n',
      "DUMMYAUDIO_PlayDevice")
patch("src/audio/dummy/SDL_dummyaudio.c",
      '    impl->OpenDevice = DUMMYAUDIO_OpenDevice;\n',
      '    impl->OpenDevice = DUMMYAUDIO_OpenDevice;\n    impl->GetDeviceBuf = DUMMYAUDIO_GetDeviceBuf;\n    impl->PlayDevice = DUMMYAUDIO_PlayDevice;\n    impl->WaitDevice = DUMMYAUDIO_WaitDevice;\n',
      "impl->PlayDevice")

# --- audio diagnostics: one line when the device opens, and the first/500th play call (ut_ps5_log -> ut99.log)
patch("src/audio/dummy/SDL_dummyaudio.c",
      'extern void ut_ps5_audio_play(const void *frames, int count) __attribute__((weak));\n',
      'extern void ut_ps5_audio_play(const void *frames, int count) __attribute__((weak));\n'
      'extern void ut_ps5_log(const char *text) __attribute__((weak));\n'
      'static void ps5_audio_note(const char *what, int a, int b, int c, int d) {\n'
      '    char line[160];\n'
      '    if (ut_ps5_log) { SDL_snprintf(line, sizeof line, "[sdl audio] %s %d %d %d %d", what, a, b, c, d); ut_ps5_log(line); }\n'
      '}\n',
      "ps5_audio_note")
patch("src/audio/dummy/SDL_dummyaudio.c",
      '        _this->spec.freq = 48000;\n        _this->spec.samples = 1024;\n    }\n',
      '        _this->spec.freq = 48000;\n        _this->spec.samples = 1024;\n    }\n'
      '    ps5_audio_note("open device, hook/freq/channels/samples", ut_ps5_audio_play != 0, _this->spec.freq, _this->spec.channels, _this->spec.samples);\n',
      "open device, hook")
# --- joystick: the virtual joystick option is tied to HIDAPI; the title feeds it from scePad
patch("CMakeLists.txt",
      'dep_option(SDL_VIRTUAL_JOYSTICK    "Enable the virtual-joystick driver" ON SDL_HIDAPI OFF)',
      'option(SDL_VIRTUAL_JOYSTICK "Enable the virtual-joystick driver" ON) # PS5: no HIDAPI dependency',
      "PS5: no HIDAPI dependency")
print("sdl2 patches ok")
