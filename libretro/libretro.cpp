#include <stdarg.h>
#include "libretro.h"

#include "MMU.h"
#include "NDSSystem.h"
#include "debug.h"
#include "sndsdl.h"
#include "render3D.h"
#include "rasterize.h"
#include "saves.h"
#include "firmware.h"
#include "GPU_osd.h"
#include "addons.h"

#include "performance.h"

//

static retro_log_printf_t log_cb = NULL;
static retro_video_refresh_t video_cb = NULL;
static retro_input_poll_t poll_cb = NULL;
static retro_input_state_t input_cb = NULL;
static retro_audio_sample_batch_t audio_batch_cb = NULL;
retro_environment_t environ_cb = NULL;

//

volatile bool execute = false;

namespace /* INPUT */
{
    static bool absolutePointer;

    template<int32_t MIN, int32_t MAX>
    static int32_t Saturate(int32_t aValue)
    {
        return std::max(MIN, std::min(MAX, aValue));
    }

    static int32_t TouchX;
    static int32_t TouchY;

    static const uint32_t FramesWithPointerBase = 60 * 10;
    static int32_t FramesWithPointer;

    template<uint16_t COLOR>
    static void DrawPointerLine(uint16_t* aOut, uint32_t aPitchInPix)
    {
        for(int i = 0; i < 5; i ++)
            aOut[aPitchInPix * i] = COLOR;
    }

    template<uint16_t COLOR>
    static void DrawPointer(uint16_t* aOut, uint32_t aPitchInPix)
    {
        if(FramesWithPointer-- < 0)
            return;

        TouchX = Saturate<0, 255>(TouchX);
        TouchY = Saturate<0, 191>(TouchY);

        if(TouchX >   5) DrawPointerLine<COLOR>(&aOut[TouchY * aPitchInPix + TouchX - 5], 1);
        if(TouchX < 251) DrawPointerLine<COLOR>(&aOut[TouchY * aPitchInPix + TouchX + 1], 1);
        if(TouchY >   5) DrawPointerLine<COLOR>(&aOut[(TouchY - 5) * aPitchInPix + TouchX], aPitchInPix);
        if(TouchY < 187) DrawPointerLine<COLOR>(&aOut[(TouchY + 1) * aPitchInPix + TouchX], aPitchInPix);
    }
}


namespace /* VIDEO */
{
    static uint16_t screenSwap[256 * 192 * 2];
    static retro_pixel_format colorMode;
    static uint32_t frameSkip;
    static uint32_t frameIndex;

    struct LayoutData
    {
        const char* name;
        uint16_t* screens[2];
        uint32_t touchScreenX;
        uint32_t touchScreenY;
        uint32_t width;
        uint32_t height;
        uint32_t pitchInPix;
    };

    static const LayoutData layouts[] =
    {
        { "main_top_ext_bottom", { &screenSwap[0], &screenSwap[256 * 192] }, 0, 192, 256, 384, 256 },
        { "main_bottom_ext_top", { &screenSwap[256 * 192], &screenSwap[0] }, 0, 0, 256, 384, 256 },
        { "main_left_ext_right", { &screenSwap[0], &screenSwap[256] }, 256, 0, 512, 192, 512 },
        { "main_right_ext_left", { &screenSwap[256], &screenSwap[0] }, 0, 0, 512, 192, 512 },
        { 0, 0, 0, 0 }
    };

    static const LayoutData* screenLayout = &layouts[0];

    template<typename T, unsigned EXTRA>
    static void SwapScreen(void* aOut, const void* aIn, uint32_t aPitchInPix)
    {
        static const uint32_t pixPerT = sizeof(T) / 2;
        static const uint32_t pixPerLine = 256 / pixPerT;
        static const T colorMask = (pixPerT == 1) ? 0x001F : ((pixPerT == 2) ? 0x001F001F : 0x001F001F001F001FULL);
        
        assert(pixPerT == 1 || pixPerT == 2 || pixPerT == 4);
        
        const uint32_t pitchInT = aPitchInPix / pixPerT;
        const T* inPix = (const T*)aIn;
        
        for(int i = 0; i < 192; i ++)
        {
            T* outPix = (T*)aOut + (i * pitchInT);

            for(int j = 0; j < pixPerLine; j ++)
            {
                const T p = *inPix++;            
                const T r = ((p >> 10) & colorMask);
                const T g = ((p >> 5) & colorMask) << 5 + EXTRA;
                const T b = ((p >> 0) & colorMask) << 10 + EXTRA;
                *outPix++ = r | g | b;
            }
        }
    }

    void SetupScreens(const char* aLayout)
    {
        screenLayout = &layouts[0];

        for(int i = 0; aLayout && layouts[i].name; i ++)
            if(strcmp(aLayout, layouts[i].name) == 0)
                screenLayout = &layouts[i];
    }

    template<unsigned EXTRA>
    void SwapScreensFn()
    {
        static const uint16_t* const screenSource[2] = {(uint16_t*)&GPU_screen[0], (uint16_t*)&GPU_screen[256 * 192 * 2]};
        SwapScreen<uint32_t, EXTRA>(screenLayout->screens[0], screenSource[0], screenLayout->pitchInPix);
        SwapScreen<uint32_t, EXTRA>(screenLayout->screens[1], screenSource[1], screenLayout->pitchInPix);
        DrawPointer<EXTRA ? 0xFFFF : 0x7FFF>(screenLayout->screens[1], screenLayout->pitchInPix);

        video_cb(screenSwap, screenLayout->width, screenLayout->height, screenLayout->pitchInPix * 2);
    }

    template void SwapScreensFn<0>();
    template void SwapScreensFn<1>();

    void (*SwapScreens)();
}

namespace
{
    uint32_t firmwareLanguage;
}

static void CheckSettings()
{
    retro_variable layout = { "desmume_screens_layout", 0 };
    environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &layout);
    SetupScreens(layout.value);

    retro_variable pointer = { "desmume_pointer_type", 0 };
    environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &pointer);
    absolutePointer = pointer.value && (strcmp(pointer.value, "absolute") == 0);

    retro_variable frameskip = { "desmume_frameskip", 0 };
    environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &frameskip);
    frameSkip = frameskip.value ? strtol(frameskip.value, 0, 10) : 0;

    retro_variable language = { "desmume_firmware_language", 0 };
    environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &language);
    language.value = language.value ? language.value : "English";

    static const struct { const char* name; uint32_t id; } languages[6] = 
    {
        { "Japanese", 0 },
        { "English", 1 },
        { "French", 2 },
        { "German", 3 },
        { "Italian", 4 },
        { "Spanish", 5 }
    };

    for (int i = 0; i != 6; i ++)
    {
        if (strcmp(languages[i].name, language.value) == 0)
        {
            firmwareLanguage = languages[i].id;
            break;
        }
    }
}

void frontend_process_samples(u32 frames, const s16* data)
{
    audio_batch_cb(data, frames);
}

SoundInterface_struct* SNDCoreList[] =
{
    NULL
};

GPU3DInterface* core3DList[] =
{
    &gpu3DRasterize,
    NULL
};

//

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)   { }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_cb = cb; }

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   static const retro_variable values[] =
   {
      { "desmume_screens_layout", "Screen Layout; main_top_ext_bottom|main_bottom_ext_top|main_left_ext_right|main_right_ext_left" },
      { "desmume_pointer_type", "Pointer Mode; relative|absolute" },
      { "desmume_firmware_language", "Language; English|Japanese|French|German|Italian|Spanish" },
      { "desmume_frameskip", "Frame Skip; 0|1|2|3|4|5|6|7|8|9" },
      { 0, 0 }
   };

   environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)values);
}

void retro_get_system_info(struct retro_system_info *info)
{
   info->library_name = "DeSmuME";
   info->library_version = "svn";
   info->valid_extensions = "nds";
   info->need_fullpath = true;   
   info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    // TODO
    info->geometry.base_width = screenLayout->width;
    info->geometry.base_height = screenLayout->height;
    info->geometry.max_width = screenLayout->width;
    info->geometry.max_height = screenLayout->height;
    info->geometry.aspect_ratio = 0.0;
    info->timing.fps = 60.0;
    info->timing.sample_rate = 44100.0;
}

//====================== Message box
#define MSG_ARG \
	char msg_buf[1024] = {0}; \
	{ \
		va_list args; \
		va_start (args, fmt); \
		vsprintf (msg_buf, fmt, args); \
		va_end (args); \
	}

void msgWndInfo(const char *fmt, ...)
{
	MSG_ARG;
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "%s.\n", msg_buf);
}

bool msgWndConfirm(const char *fmt, ...)
{
	MSG_ARG;
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "%s.\n", msg_buf);
   return true;
}

void msgWndError(const char *fmt, ...)
{
	MSG_ARG;
   if (log_cb)
      log_cb(RETRO_LOG_ERROR, "%s.\n", msg_buf);
}

void msgWndWarn(const char *fmt, ...)
{
	MSG_ARG;
   if (log_cb)
      log_cb(RETRO_LOG_WARN, "%s.\n", msg_buf);
}

msgBoxInterface msgBoxWnd = {
	msgWndInfo,
	msgWndConfirm,
	msgWndError,
	msgWndWarn,
};
//====================== Dialogs end


void retro_init (void)
{
   struct retro_log_callback log;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

    colorMode = RETRO_PIXEL_FORMAT_RGB565;
    if(!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &colorMode))
        colorMode = RETRO_PIXEL_FORMAT_0RGB1555;

    SwapScreens = (colorMode == RETRO_PIXEL_FORMAT_RGB565) ? SwapScreensFn<1> : SwapScreensFn<0>;

    CheckSettings();

    // Init DeSmuME
    struct NDS_fw_config_data fw_config;
    NDS_FillDefaultFirmwareConfigData(&fw_config);
    fw_config.language = firmwareLanguage;

    CommonSettings.num_cores = 2;
    CommonSettings.use_jit = true;

    addonsChangePak(NDS_ADDON_NONE);
    NDS_Init();
    NDS_CreateDummyFirmware(&fw_config);
    NDS_3D_ChangeCore(0);
    backup_setManualBackupType(MC_TYPE_AUTODETECT);

    msgbox = &msgBoxWnd;
}

void retro_deinit(void)
{
    NDS_DeInit();

#ifdef PERF_TEST
   rarch_perf_log();
#endif
}

void retro_reset (void)
{
    NDS_Reset();
}

void retro_run (void)
{
    // Settings
    bool changed = false;
    environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &changed);
    if(changed)
        CheckSettings();

    poll_cb();

    bool haveTouch = false;

    // TOUCH: Analog
    const int16_t analogX = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X) / 2048;
    const int16_t analogY = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y) / 2048;
    haveTouch = haveTouch || input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2);    

    TouchX = Saturate<0, 255>(TouchX + analogX);
    TouchY = Saturate<0, 191>(TouchY + analogY);
    FramesWithPointer = (analogX || analogY) ? FramesWithPointerBase : FramesWithPointer;

    // TOUCH: Mouse
    if(!absolutePointer)
    {
        const int16_t mouseX = input_cb(1, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
        const int16_t mouseY = input_cb(1, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
        haveTouch = haveTouch || input_cb(1, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
    
        TouchX = Saturate<0, 255>(TouchX + mouseX);
        TouchY = Saturate<0, 191>(TouchY + mouseY);
        FramesWithPointer = (mouseX || mouseY) ? FramesWithPointerBase : FramesWithPointer;
    }
    // TOUCH: Pointer
    else if(input_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED))
    {
        const float X_FACTOR = ((float)screenLayout->width / 65536.0f);
        const float Y_FACTOR = ((float)screenLayout->height / 65536.0f);
    
        float x = (input_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X) + 32768.0f) * X_FACTOR;
        float y = (input_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y) + 32768.0f) * Y_FACTOR;

        if (x >= screenLayout->touchScreenX && x < screenLayout->touchScreenX + 256 &&
            y >= screenLayout->touchScreenY && y < screenLayout->touchScreenY + 192)
        {
            haveTouch = true;

            TouchX = x - screenLayout->touchScreenX;
            TouchY = y - screenLayout->touchScreenY;
        }
    }

    // TOUCH: Final        
    if(haveTouch)
        NDS_setTouchPos(TouchX, TouchY);
    else
        NDS_releaseTouch();


    // BUTTONS
    NDS_beginProcessingInput();
    UserButtons& input = NDS_getProcessingUserInput().buttons;
    input.G = 0; // debug
    input.E = input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R); // right shoulder
    input.W = input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L); // left shoulder
    input.X = input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);
    input.Y = input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y);
    input.A = input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
    input.B = input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
    input.S = input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
    input.T = input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT);
    input.U = input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
    input.D = input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
    input.L = input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
    input.R = input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);
    input.F = input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2); //Lid
    NDS_endProcessingInput();

    // RUN
    frameIndex ++;
    bool skipped = frameIndex <= frameSkip;

    if (skipped)
    {
        NDS_SkipNextFrame();
    }

    NDS_exec<false>();
    
    // VIDEO: Swap screen colors and pass on
    if (!skipped)
        SwapScreens();
    else
        video_cb(0, screenLayout->width, screenLayout->height, screenLayout->pitchInPix * 2);

    frameIndex = skipped ? frameIndex : 0;
}

size_t retro_serialize_size (void)
{
    // HACK: Usually around 10 MB but can vary frame to frame!
    return 1024 * 1024 * 12;
}

bool retro_serialize(void *data, size_t size)
{
    EMUFILE_MEMORY state;
    savestate_save(&state, 0);
    
    if(state.size() <= size)
    {
        memcpy(data, state.buf(), state.size());
        return true;
    }
    
    return false;
}

bool retro_unserialize(const void * data, size_t size)
{
    EMUFILE_MEMORY state(const_cast<void*>(data), size);
    return savestate_load(&state);
}

bool retro_load_game(const struct retro_game_info *game)
{
    execute = NDS_LoadROM(game->path);
    return execute;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
    if(game_type == RETRO_GAME_TYPE_SUPER_GAME_BOY && num_info == 2)
    {
        strncpy(GBAgameName, info[1].path, sizeof(GBAgameName));
        addonsChangePak(NDS_ADDON_GBAGAME);
        
        return retro_load_game(&info[0]);
    }
    return false;
}

void retro_unload_game (void)
{
    NDS_FreeROM();
    execute = false;
}

// Stubs
void retro_set_controller_port_device(unsigned in_port, unsigned device) { }
void *retro_get_memory_data(unsigned type) { return 0; }
size_t retro_get_memory_size(unsigned type) { return 0; }
unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_cheat_reset(void) { }
void retro_cheat_set(unsigned unused, bool unused1, const char* unused2) { }
unsigned retro_get_region (void) { return RETRO_REGION_NTSC; }

#ifdef PSP
int ftruncate(int fd, off_t length)
{
   int ret;
   SceOff oldpos;
   if (!__PSP_IS_FD_VALID(fd)) {
      errno = EBADF;
      return -1;
   }

   switch(__psp_descriptormap[fd]->type)
   {
      case __PSP_DESCRIPTOR_TYPE_FILE:
         if (__psp_descriptormap[fd]->filename != NULL) {
            if (!(__psp_descriptormap[fd]->flags & (O_WRONLY | O_RDWR)))
               break;
            return truncate(__psp_descriptormap[fd]->filename, length);
            /* ANSI sez ftruncate doesn't move the file pointer */
         }
         break;
      case __PSP_DESCRIPTOR_TYPE_TTY:
      case __PSP_DESCRIPTOR_TYPE_PIPE:
      case __PSP_DESCRIPTOR_TYPE_SOCKET:
      default:
         break;
   }

   errno = EINVAL;
   return -1;
}
#endif
