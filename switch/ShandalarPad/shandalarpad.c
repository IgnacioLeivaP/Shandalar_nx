/* ShandalarPad: drives the mouse and keyboard from a game controller, with
 * the double clicks and precision that Shandalar needs.
 *
 * It is built as a stand-in for the game's own image.dll: the one function
 * that DLL exports (LoadImage) is forwarded to the original, renamed to
 * image_orig.dll, and loading it starts a thread that reads the controller
 * through XInput and sends mouse and keyboard input with SendInput.
 *
 * Under Autorun (Wine for the Nintendo Switch), a program that reads XInput
 * gets the controller to itself: Autorun stops moving the cursor and sending
 * keys from it while that lasts, and the touchscreen keeps pointing. So while
 * this thread runs, every button does what ShandalarPad.ini says, and when it
 * is suspended (L3 + R3 held for a second) Autorun's own controls come back.
 *
 * Button names in the ini are the Switch's own labels. Autorun reports the
 * Switch pad as an Xbox controller by position (Switch A is Xbox B, and so
 * on), and the table below undoes that. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* XInput, declared here so no import library is needed. */
#define XINPUT_GAMEPAD_DPAD_UP        0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN      0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT      0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT     0x0008
#define XINPUT_GAMEPAD_START          0x0010
#define XINPUT_GAMEPAD_BACK           0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB     0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB    0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER  0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XINPUT_GAMEPAD_A              0x1000
#define XINPUT_GAMEPAD_B              0x2000
#define XINPUT_GAMEPAD_X              0x4000
#define XINPUT_GAMEPAD_Y              0x8000

typedef struct
{
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} XINPUT_GAMEPAD;

typedef struct
{
    DWORD          dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
} XINPUT_STATE;

typedef DWORD (WINAPI *xinput_get_state_fn)( DWORD, XINPUT_STATE * );

/* The controls, by their Switch labels. */
enum
{
    BTN_A, BTN_B, BTN_X, BTN_Y, BTN_L, BTN_R, BTN_ZL, BTN_ZR,
    BTN_PLUS, BTN_MINUS, BTN_LSTICK, BTN_RSTICK,
    BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
    BTN_COUNT
};

static const char *const button_names[BTN_COUNT] =
{
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR",
    "Plus", "Minus", "LStick", "RStick",
    "Up", "Down", "Left", "Right"
};

/* Where each Switch button shows up in XInput under Autorun (by position);
 * ZL and ZR are the triggers and are handled apart. */
static const WORD button_xinput[BTN_COUNT] =
{
    XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_X,
    XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0,
    XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
    XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT
};

static const char *const default_actions[BTN_COUNT] =
{
    "lclick",     /* A: left button, held while A is */
    "rclick",     /* B: right button */
    "rdouble",    /* X: right double click, shows the full card */
    "ldouble",    /* Y: left double click, autotaps mana / arranges cards */
    "0x0D",       /* L: Enter */
    "0x20",       /* R: Space */
    "precision",  /* ZL: slow cursor while held */
    "turbo",      /* ZR: fast cursor while held */
    "0x1B",       /* Plus: Escape */
    "none",       /* Minus: left to Autorun (Minus + right stick opens its keyboard) */
    "none",       /* left stick click */
    "none",       /* right stick click */
    "0x26", "0x28", "0x25", "0x27"  /* d-pad: arrows */
};

enum action_kind
{
    ACT_NONE, ACT_LCLICK, ACT_RCLICK, ACT_MCLICK, ACT_LDOUBLE, ACT_RDOUBLE,
    ACT_WHEELUP, ACT_WHEELDOWN, ACT_PRECISION, ACT_TURBO, ACT_KEYS
};

#define MAX_COMBO_KEYS 4

struct action
{
    enum action_kind kind;
    WORD keys[MAX_COMBO_KEYS];
    int key_count;
};

enum stick_mode { STICK_MOUSE, STICK_ARROWS, STICK_OFF };

struct config
{
    int enabled;
    int cursor_speed;       /* pixels per second with the stick pushed all the way */
    int precision_percent;
    int turbo_percent;
    int deadzone_percent;
    int curve_percent;      /* response exponent, times 100 */
    int trigger_threshold;  /* 0-255 */
    int wheel_repeat_ms;
    int suspend_hold_ms;
    int resume_key1, resume_key2;
    int poll_ms;
    int click_gap_ms;
    int log;
    /* Shandalar.ini Window = 1 or 2: the 1024x768 adventure window */
    int fix_window, center_window, window_offset_y, window_w, window_h;
    int fake_screen;
    char window_class[64];
    enum stick_mode left_stick, right_stick;
    struct action actions[BTN_COUNT];
};

static struct config cfg;
static char base_dir[MAX_PATH];
static volatile LONG stopping;
static HANDLE pad_thread;

static void log_line( const char *fmt, ... )
{
    char path[MAX_PATH];
    FILE *file;
    va_list args;
    SYSTEMTIME now;

    if (!cfg.log) return;
    snprintf( path, sizeof(path), "%sShandalarPad.log", base_dir );
    if (!(file = fopen( path, "a" ))) return;
    GetLocalTime( &now );
    fprintf( file, "%02d:%02d:%02d.%03d ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds );
    va_start( args, fmt );
    vfprintf( file, fmt, args );
    va_end( args );
    fputc( '\n', file );
    fclose( file );
}

/* "lclick", "ldouble", "0x11+0x53" and the like. */
static void parse_action( const char *text, struct action *out )
{
    static const struct { const char *name; enum action_kind kind; } names[] =
    {
        { "none", ACT_NONE }, { "lclick", ACT_LCLICK }, { "rclick", ACT_RCLICK },
        { "mclick", ACT_MCLICK }, { "ldouble", ACT_LDOUBLE }, { "rdouble", ACT_RDOUBLE },
        { "wheelup", ACT_WHEELUP }, { "wheeldown", ACT_WHEELDOWN },
        { "precision", ACT_PRECISION }, { "turbo", ACT_TURBO },
    };
    const char *p = text;
    unsigned int i;

    memset( out, 0, sizeof(*out) );
    while (*p == ' ' || *p == '\t') p++;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!_stricmp( p, names[i].name ))
        {
            out->kind = names[i].kind;
            return;
        }
    while (*p && out->key_count < MAX_COMBO_KEYS)
    {
        char *end;
        unsigned long vk = strtoul( p, &end, 0 );

        if (end == p || !vk || vk > 0xFE) break;
        out->keys[out->key_count++] = (WORD)vk;
        p = end;
        while (*p == ' ' || *p == '+') p++;
    }
    out->kind = out->key_count ? ACT_KEYS : ACT_NONE;
    if (!out->key_count && *p) log_line( "unknown action '%s', ignored", text );
}

static enum stick_mode parse_stick( const char *text )
{
    if (!_stricmp( text, "arrows" )) return STICK_ARROWS;
    if (!_stricmp( text, "off" ) || !_stricmp( text, "none" )) return STICK_OFF;
    return STICK_MOUSE;
}

/* GetPrivateProfileInt reads decimal only; key codes are often written 0x11. */
static int ini_int( const char *section, const char *key, int fallback, const char *ini )
{
    char text[32], *end;
    long value;

    if (!GetPrivateProfileStringA( section, key, "", text, sizeof(text), ini ) || !text[0]) return fallback;
    value = strtol( text, &end, 0 );
    return end == text ? fallback : (int)value;
}

static void load_config( void )
{
    char ini[MAX_PATH], text[64];
    const char *s = "ShandalarPad";
    int i;

    snprintf( ini, sizeof(ini), "%sShandalarPad.ini", base_dir );
    cfg.enabled           = ini_int( s, "Enabled", 1, ini );
    cfg.log               = ini_int( s, "Log", 1, ini );
    cfg.cursor_speed      = ini_int( s, "CursorSpeed", 900, ini );
    cfg.precision_percent = ini_int( s, "PrecisionPercent", 30, ini );
    cfg.turbo_percent     = ini_int( s, "TurboPercent", 220, ini );
    cfg.deadzone_percent  = ini_int( s, "DeadzonePercent", 18, ini );
    cfg.curve_percent     = ini_int( s, "CurvePercent", 200, ini );
    cfg.trigger_threshold = ini_int( s, "TriggerThreshold", 64, ini );
    cfg.wheel_repeat_ms   = ini_int( s, "WheelRepeatMs", 120, ini );
    cfg.suspend_hold_ms   = ini_int( s, "SuspendHoldMs", 1000, ini );
    cfg.resume_key1       = ini_int( s, "ResumeKey1", VK_CONTROL, ini );
    cfg.resume_key2       = ini_int( s, "ResumeKey2", VK_MENU, ini );
    cfg.poll_ms           = ini_int( s, "PollMs", 8, ini );
    cfg.click_gap_ms      = ini_int( s, "ClickGapMs", 50, ini );
    cfg.fix_window        = ini_int( "Window", "FixMainWindow", 1, ini );
    cfg.center_window     = ini_int( "Window", "Center", 0, ini );
    cfg.window_offset_y   = ini_int( "Window", "OffsetY", 0, ini );
    cfg.window_w          = ini_int( "Window", "Width", 1024, ini );
    cfg.window_h          = ini_int( "Window", "Height", 768, ini );
    cfg.fake_screen       = ini_int( "Window", "FakeScreen", 0, ini );
    GetPrivateProfileStringA( "Window", "Class", "ShandalarMainClass", cfg.window_class,
                              sizeof(cfg.window_class), ini );
    if (cfg.click_gap_ms < 0) cfg.click_gap_ms = 0;
    if (cfg.poll_ms < 1) cfg.poll_ms = 1;
    if (cfg.deadzone_percent < 0 || cfg.deadzone_percent > 90) cfg.deadzone_percent = 18;
    if (cfg.curve_percent < 50) cfg.curve_percent = 50;

    GetPrivateProfileStringA( s, "LeftStick", "mouse", text, sizeof(text), ini );
    cfg.left_stick = parse_stick( text );
    GetPrivateProfileStringA( s, "RightStick", "mouse", text, sizeof(text), ini );
    cfg.right_stick = parse_stick( text );

    for (i = 0; i < BTN_COUNT; i++)
    {
        GetPrivateProfileStringA( "Buttons", button_names[i], default_actions[i], text, sizeof(text), ini );
        parse_action( text, &cfg.actions[i] );
    }
}

/* ---- Sending input ---------------------------------------------------- */

static void send_mouse( DWORD flags, LONG dx, LONG dy, DWORD data )
{
    INPUT in;

    memset( &in, 0, sizeof(in) );
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.mouseData = data;
    in.mi.dwFlags = flags;
    SendInput( 1, &in, sizeof(in) );
}

/* Moves the cursor by an exact number of pixels. A relative MOUSEEVENTF_MOVE
 * goes through the system's pointer acceleration, which doubles any step past
 * a few pixels; an absolute move lands where it is told. */
static void move_cursor_by( LONG dx, LONG dy )
{
    POINT pos;
    int vx = GetSystemMetrics( SM_XVIRTUALSCREEN ), vy = GetSystemMetrics( SM_YVIRTUALSCREEN );
    int vw = GetSystemMetrics( SM_CXVIRTUALSCREEN ), vh = GetSystemMetrics( SM_CYVIRTUALSCREEN );
    LONG x, y;

    if (!GetCursorPos( &pos ) || vw <= 1 || vh <= 1)
    {
        send_mouse( MOUSEEVENTF_MOVE, dx, dy, 0 );
        return;
    }
    x = pos.x + dx;
    y = pos.y + dy;
    if (x < vx) x = vx;
    if (y < vy) y = vy;
    if (x > vx + vw - 1) x = vx + vw - 1;
    if (y > vy + vh - 1) y = vy + vh - 1;
    /* Absolute coordinates run from 0 to 65535 across the virtual screen;
     * round to the middle of the target pixel. */
    send_mouse( MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK,
                (LONG)(((x - vx) * 65536LL + 32768) / vw), (LONG)(((y - vy) * 65536LL + 32768) / vh), 0 );
}

static void send_key( WORD vk, BOOL up )
{
    INPUT in;
    UINT scan = MapVirtualKeyA( vk, MAPVK_VK_TO_VSC );

    memset( &in, 0, sizeof(in) );
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)scan;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    /* Arrows, Insert/Delete, Home/End and Page Up/Down are extended keys. */
    if ((vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE)
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    SendInput( 1, &in, sizeof(in) );
}

/* The game reads the buttons' state on its own schedule as well as through
 * messages, so a press and release sent together can go unseen: each half of
 * the double click is held for a moment. */
static void double_click( DWORD down, DWORD up )
{
    int i;

    for (i = 0; i < 4; i++)
    {
        send_mouse( (i & 1) ? up : down, 0, 0, 0 );
        if (i < 3 && cfg.click_gap_ms) Sleep( cfg.click_gap_ms );
    }
}

/* A control went down (pressed) or came back up. */
static void action_edge( const struct action *a, BOOL pressed )
{
    int i;

    switch (a->kind)
    {
    case ACT_LCLICK: send_mouse( pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP, 0, 0, 0 ); break;
    case ACT_RCLICK: send_mouse( pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP, 0, 0, 0 ); break;
    case ACT_MCLICK: send_mouse( pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP, 0, 0, 0 ); break;
    case ACT_LDOUBLE: if (pressed) double_click( MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP ); break;
    case ACT_RDOUBLE: if (pressed) double_click( MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP ); break;
    case ACT_WHEELUP: if (pressed) send_mouse( MOUSEEVENTF_WHEEL, 0, 0, WHEEL_DELTA ); break;
    case ACT_WHEELDOWN: if (pressed) send_mouse( MOUSEEVENTF_WHEEL, 0, 0, (DWORD)-WHEEL_DELTA ); break;
    case ACT_KEYS:
        /* Modifiers first on the way down, last on the way up: 0x11+0x53 is Ctrl+S. */
        if (pressed) for (i = 0; i < a->key_count; i++) send_key( a->keys[i], FALSE );
        else for (i = a->key_count - 1; i >= 0; i--) send_key( a->keys[i], TRUE );
        break;
    default: break;
    }
}

/* ---- The adventure window --------------------------------------------- */

/* With Window = 1 or 2 in Shandalar.ini, Adventure Mode runs in a 1024x768
 * window with a title bar, which on Autorun's 1280x720 screen is cut short:
 * the game sizes the window for its frame (1030x800) and the system holds it
 * to the screen's height. The window is subclassed as it is created, so that
 * it has no frame at all, keeps exactly 1024x768 and stays at the top; only
 * the bottom 48 lines are then off the screen. It is done from the start
 * because the game does not repaint after a later change. */
static HHOOK cbt_hook;
static HWND adventure_hwnd;
static WNDPROC adventure_orig_proc;
static BOOL adventure_unicode;

static BOOL is_adventure_window( HWND hwnd )
{
    char cls[64];

    return GetClassNameA( hwnd, cls, sizeof(cls) ) && !_stricmp( cls, cfg.window_class );
}

static int adventure_x( void )
{
    return cfg.center_window ? (GetSystemMetrics( SM_CXSCREEN ) - cfg.window_w) / 2 : 0;
}

static LRESULT CALLBACK adventure_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    LRESULT ret;

    switch (msg)
    {
    case WM_CREATE:
        ret = adventure_unicode ? CallWindowProcW( adventure_orig_proc, hwnd, msg, wparam, lparam )
                                : CallWindowProcA( adventure_orig_proc, hwnd, msg, wparam, lparam );
        /* The style set at creation is the game's; take the frame away now,
         * before anything is drawn. */
        SetWindowLongA( hwnd, GWL_STYLE, GetWindowLongA( hwnd, GWL_STYLE ) );
        SetWindowPos( hwnd, NULL, adventure_x(), cfg.window_offset_y, cfg.window_w, cfg.window_h,
                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED );
        return ret;
    case WM_STYLECHANGING:
        if (wparam == (WPARAM)GWL_STYLE)
            ((STYLESTRUCT *)lparam)->styleNew &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU);
        break;
    case WM_NCCALCSIZE:
        if (wparam) return 0;  /* the client area is the whole window */
        break;
    case WM_NCPAINT:
        return 0;
    case WM_NCACTIVATE:
        return TRUE;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_WINDOWPOSCHANGING:
    {
        WINDOWPOS *pos = (WINDOWPOS *)lparam;

        if (!(pos->flags & SWP_NOSIZE)) { pos->cx = cfg.window_w; pos->cy = cfg.window_h; }
        if (!(pos->flags & SWP_NOMOVE)) { pos->x = adventure_x(); pos->y = cfg.window_offset_y; }
        break;
    }
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mmi = (MINMAXINFO *)lparam;

        ret = adventure_unicode ? CallWindowProcW( adventure_orig_proc, hwnd, msg, wparam, lparam )
                                : CallWindowProcA( adventure_orig_proc, hwnd, msg, wparam, lparam );
        /* The system's limit is the screen plus a frame: 732 lines here. */
        if (mmi->ptMaxTrackSize.x < cfg.window_w) mmi->ptMaxTrackSize.x = cfg.window_w;
        if (mmi->ptMaxTrackSize.y < cfg.window_h) mmi->ptMaxTrackSize.y = cfg.window_h;
        mmi->ptMaxSize.x = cfg.window_w;
        mmi->ptMaxSize.y = cfg.window_h;
        mmi->ptMaxPosition.x = adventure_x();
        mmi->ptMaxPosition.y = cfg.window_offset_y;
        return ret;
    }
    }
    return adventure_unicode ? CallWindowProcW( adventure_orig_proc, hwnd, msg, wparam, lparam )
                             : CallWindowProcA( adventure_orig_proc, hwnd, msg, wparam, lparam );
}

static void subclass_adventure_window( HWND hwnd )
{
    if (adventure_hwnd == hwnd) return;
    adventure_unicode = IsWindowUnicode( hwnd );
    adventure_orig_proc = adventure_unicode
        ? (WNDPROC)SetWindowLongPtrW( hwnd, GWLP_WNDPROC, (LONG_PTR)adventure_proc )
        : (WNDPROC)SetWindowLongPtrA( hwnd, GWLP_WNDPROC, (LONG_PTR)adventure_proc );
    if (adventure_orig_proc) adventure_hwnd = hwnd;
}

/* The hook watches the thread that loads this DLL, the game's main thread,
 * which is the one that creates the window. */
static LRESULT CALLBACK cbt_proc( int code, WPARAM wparam, LPARAM lparam )
{
    if (code == HCBT_CREATEWND && !adventure_hwnd && is_adventure_window( (HWND)wparam ))
    {
        CREATESTRUCTA *cs = ((CBT_CREATEWNDA *)lparam)->lpcs;

        subclass_adventure_window( (HWND)wparam );
        cs->x = adventure_x();
        cs->y = cfg.window_offset_y;
        cs->cx = cfg.window_w;
        cs->cy = cfg.window_h;
        log_line( "adventure window: %s, no frame, %dx%d at %d,%d",
                  adventure_hwnd ? "subclassed" : "could not subclass",
                  cfg.window_w, cfg.window_h, cs->x, cs->y );
    }
    return CallNextHookEx( cbt_hook, code, wparam, lparam );
}

/* In case the window came before the hook. */
static BOOL CALLBACK fix_window_proc( HWND hwnd, LPARAM lparam )
{
    DWORD pid = 0;

    (void)lparam;
    if (adventure_hwnd) return FALSE;
    GetWindowThreadProcessId( hwnd, &pid );
    if (pid != GetCurrentProcessId() || !is_adventure_window( hwnd )) return TRUE;
    subclass_adventure_window( hwnd );
    SetWindowPos( hwnd, NULL, adventure_x(), cfg.window_offset_y, cfg.window_w, cfg.window_h,
                  SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED );
    RedrawWindow( hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN );
    log_line( "adventure window found late: subclassed and moved" );
    return FALSE;
}

/* ---- A 1024x768 screen ------------------------------------------------- */

/* Autorun has one display mode, 1280x720. Shandalar asks for 1024x768 and,
 * without it, falls back to layouts that crash (Window = 0) or leave the
 * duel's hand, card and dialog windows unpainted (Window = 1 or 2). So the
 * game's own modules are told the screen is 1024x768 and that switching to
 * it worked: their imports of the functions below are pointed here. */
static int real_screen_w, real_screen_h;
static int (WINAPI *real_GetSystemMetrics)( int );
static int (WINAPI *real_GetDeviceCaps)( HDC, int );
static LONG (WINAPI *real_ChangeDisplaySettingsA)( DEVMODEA *, DWORD );
static LONG (WINAPI *real_ChangeDisplaySettingsExA)( LPCSTR, DEVMODEA *, HWND, DWORD, void * );
static BOOL (WINAPI *real_SystemParametersInfoA)( UINT, UINT, void *, UINT );
static HMODULE (WINAPI *real_LoadLibraryA)( LPCSTR );
static HMODULE (WINAPI *real_LoadLibraryExA)( LPCSTR, HANDLE, DWORD );
static CRITICAL_SECTION patch_lock;

static int WINAPI fake_GetSystemMetrics( int index )
{
    switch (index)
    {
    case SM_CXSCREEN: case SM_CXVIRTUALSCREEN: case SM_CXFULLSCREEN: case SM_CXMAXIMIZED:
        return cfg.window_w;
    case SM_CYSCREEN: case SM_CYVIRTUALSCREEN: case SM_CYMAXIMIZED:
        return cfg.window_h;
    case SM_CYFULLSCREEN:
        return cfg.window_h - real_GetSystemMetrics( SM_CYCAPTION );
    }
    return real_GetSystemMetrics( index );
}

static int WINAPI fake_GetDeviceCaps( HDC hdc, int index )
{
    int value = real_GetDeviceCaps( hdc, index );

    if ((index == HORZRES || index == DESKTOPHORZRES) && value == real_screen_w) return cfg.window_w;
    if ((index == VERTRES || index == DESKTOPVERTRES) && value == real_screen_h) return cfg.window_h;
    return value;
}

static BOOL is_fake_mode( const DEVMODEA *mode )
{
    if (!mode) return TRUE;  /* back to the registry's mode: nothing to do */
    if ((mode->dmFields & DM_PELSWIDTH) && (int)mode->dmPelsWidth != cfg.window_w) return FALSE;
    if ((mode->dmFields & DM_PELSHEIGHT) && (int)mode->dmPelsHeight != cfg.window_h) return FALSE;
    return TRUE;
}

static LONG WINAPI fake_ChangeDisplaySettingsA( DEVMODEA *mode, DWORD flags )
{
    if (is_fake_mode( mode )) return DISP_CHANGE_SUCCESSFUL;
    return real_ChangeDisplaySettingsA( mode, flags );
}

static LONG WINAPI fake_ChangeDisplaySettingsExA( LPCSTR device, DEVMODEA *mode, HWND hwnd, DWORD flags, void *param )
{
    if (is_fake_mode( mode )) return DISP_CHANGE_SUCCESSFUL;
    return real_ChangeDisplaySettingsExA( device, mode, hwnd, flags, param );
}

static BOOL WINAPI fake_SystemParametersInfoA( UINT action, UINT uparam, void *pparam, UINT ini )
{
    BOOL ret = real_SystemParametersInfoA( action, uparam, pparam, ini );

    if (ret && action == SPI_GETWORKAREA && pparam)
    {
        RECT *rect = pparam;

        if (rect->right > cfg.window_w) rect->right = cfg.window_w;
        if (rect->bottom > cfg.window_h) rect->bottom = cfg.window_h;
    }
    return ret;
}

static void patch_game_modules( void );

static HMODULE WINAPI fake_LoadLibraryA( LPCSTR name )
{
    HMODULE module = real_LoadLibraryA( name );

    if (module) patch_game_modules();
    return module;
}

static HMODULE WINAPI fake_LoadLibraryExA( LPCSTR name, HANDLE file, DWORD flags )
{
    HMODULE module = real_LoadLibraryExA( name, file, flags );

    if (module && !(flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE))) patch_game_modules();
    return module;
}

/* Points a module's imports of the functions above at their stand-ins. The
 * imports are matched by address, so it does not matter which DLL name the
 * module imported them through. */
static int patch_module_imports( HMODULE module )
{
    const struct { void **real; void *fake; } table[] =
    {
        { (void **)&real_GetSystemMetrics, (void *)fake_GetSystemMetrics },
        { (void **)&real_GetDeviceCaps, (void *)fake_GetDeviceCaps },
        { (void **)&real_ChangeDisplaySettingsA, (void *)fake_ChangeDisplaySettingsA },
        { (void **)&real_ChangeDisplaySettingsExA, (void *)fake_ChangeDisplaySettingsExA },
        { (void **)&real_SystemParametersInfoA, (void *)fake_SystemParametersInfoA },
        { (void **)&real_LoadLibraryA, (void *)fake_LoadLibraryA },
        { (void **)&real_LoadLibraryExA, (void *)fake_LoadLibraryExA },
    };
    BYTE *base = (BYTE *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY *dir;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    int patched = 0;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress || !dir->Size) return 0;

    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); imp->Name; imp++)
    {
        IMAGE_THUNK_DATA *thunk = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);

        for (; thunk->u1.Function; thunk++)
        {
            unsigned int i;

            for (i = 0; i < sizeof(table) / sizeof(table[0]); i++)
            {
                DWORD old;

                if ((void *)thunk->u1.Function != *table[i].real) continue;
                if (!VirtualProtect( &thunk->u1.Function, sizeof(thunk->u1.Function), PAGE_READWRITE, &old ))
                    continue;
                thunk->u1.Function = (ULONG_PTR)table[i].fake;
                VirtualProtect( &thunk->u1.Function, sizeof(thunk->u1.Function), old, &old );
                patched++;
            }
        }
    }
    return patched;
}

/* Only the game's own modules, the ones in its folder (this one aside). */
static void patch_game_modules( void )
{
    HANDLE snap;
    MODULEENTRY32 entry;
    size_t dir_len = strlen( base_dir );
    HMODULE self = NULL;

    GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCSTR)(void *)patch_game_modules, &self );
    EnterCriticalSection( &patch_lock );
    snap = CreateToolhelp32Snapshot( TH32CS_SNAPMODULE, GetCurrentProcessId() );
    if (snap != INVALID_HANDLE_VALUE)
    {
        entry.dwSize = sizeof(entry);
        if (Module32First( snap, &entry ))
        {
            do
            {
                int n;

                if (entry.hModule == self || _strnicmp( entry.szExePath, base_dir, dir_len )) continue;
                if ((n = patch_module_imports( entry.hModule )))
                    log_line( "screen 1024x768: %d imports of %s redirected", n, entry.szModule );
            } while (Module32Next( snap, &entry ));
        }
        CloseHandle( snap );
    }
    LeaveCriticalSection( &patch_lock );
}

static void install_fake_screen( void )
{
    HMODULE user32 = GetModuleHandleA( "user32.dll" ), gdi32 = GetModuleHandleA( "gdi32.dll" );
    HMODULE kernel32 = GetModuleHandleA( "kernel32.dll" );

    real_GetSystemMetrics = (void *)GetProcAddress( user32, "GetSystemMetrics" );
    real_GetDeviceCaps = (void *)GetProcAddress( gdi32, "GetDeviceCaps" );
    real_ChangeDisplaySettingsA = (void *)GetProcAddress( user32, "ChangeDisplaySettingsA" );
    real_ChangeDisplaySettingsExA = (void *)GetProcAddress( user32, "ChangeDisplaySettingsExA" );
    real_SystemParametersInfoA = (void *)GetProcAddress( user32, "SystemParametersInfoA" );
    real_LoadLibraryA = (void *)GetProcAddress( kernel32, "LoadLibraryA" );
    real_LoadLibraryExA = (void *)GetProcAddress( kernel32, "LoadLibraryExA" );
    if (!real_GetSystemMetrics || !real_GetDeviceCaps || !real_ChangeDisplaySettingsA ||
        !real_ChangeDisplaySettingsExA || !real_SystemParametersInfoA || !real_LoadLibraryA || !real_LoadLibraryExA)
    {
        log_line( "screen 1024x768: a function is missing, not installed" );
        return;
    }
    real_screen_w = real_GetSystemMetrics( SM_CXSCREEN );
    real_screen_h = real_GetSystemMetrics( SM_CYSCREEN );
    if (real_screen_w == cfg.window_w && real_screen_h == cfg.window_h)
    {
        log_line( "screen is already %dx%d", real_screen_w, real_screen_h );
        return;
    }
    log_line( "screen is %dx%d; telling the game it is %dx%d", real_screen_w, real_screen_h,
              cfg.window_w, cfg.window_h );
    InitializeCriticalSection( &patch_lock );
    patch_game_modules();
}

/* ---- The controller thread -------------------------------------------- */

static xinput_get_state_fn load_xinput( void )
{
    static const char *const dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    unsigned int i;

    for (i = 0; i < sizeof(dlls) / sizeof(dlls[0]); i++)
    {
        HMODULE module = LoadLibraryA( dlls[i] );
        FARPROC fn;

        if (!module) continue;
        if ((fn = GetProcAddress( module, "XInputGetState" )))
        {
            log_line( "using %s", dlls[i] );
            return (xinput_get_state_fn)(void *)fn;
        }
        FreeLibrary( module );
    }
    return NULL;
}

static BOOL our_window_has_focus( void )
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;

    if (!fg) return FALSE;
    GetWindowThreadProcessId( fg, &pid );
    return pid == GetCurrentProcessId();
}

/* The stick's push past the dead zone, in pixels per second along x and y. */
static void stick_velocity( SHORT sx, SHORT sy, double *vx, double *vy )
{
    double x = sx / 32767.0, y = sy / 32767.0;
    double mag = sqrt( x * x + y * y ), dz = cfg.deadzone_percent / 100.0, norm, speed;

    if (mag <= dz) return;
    norm = (mag - dz) / (1.0 - dz);
    if (norm > 1.0) norm = 1.0;
    speed = cfg.cursor_speed * pow( norm, cfg.curve_percent / 100.0 );
    *vx += x / mag * speed;
    *vy -= y / mag * speed;  /* XInput's y points up, the screen's down */
}

/* A stick set to send arrows: which of up, down, left, right it is pushed to. */
static unsigned int stick_arrows( SHORT sx, SHORT sy )
{
    unsigned int bits = 0;

    if (sy >  16000) bits |= 1u << BTN_UP;
    if (sy < -16000) bits |= 1u << BTN_DOWN;
    if (sx < -16000) bits |= 1u << BTN_LEFT;
    if (sx >  16000) bits |= 1u << BTN_RIGHT;
    return bits;
}

static unsigned int read_buttons( const XINPUT_GAMEPAD *g )
{
    unsigned int bits = 0, i;

    for (i = 0; i < BTN_COUNT; i++)
        if (button_xinput[i] && (g->wButtons & button_xinput[i])) bits |= 1u << i;
    if (g->bLeftTrigger >= cfg.trigger_threshold) bits |= 1u << BTN_ZL;
    if (g->bRightTrigger >= cfg.trigger_threshold) bits |= 1u << BTN_ZR;
    return bits;
}

static void release_all( unsigned int held )
{
    unsigned int i;

    for (i = 0; i < BTN_COUNT; i++)
        if (held & (1u << i)) action_edge( &cfg.actions[i], FALSE );
}

static DWORD WINAPI pad_main( void *arg )
{
    xinput_get_state_fn get_state;
    LARGE_INTEGER freq, last, now;
    double acc_x = 0, acc_y = 0;
    unsigned int held = 0;
    DWORD wheel_next = 0, combo_since = 0;
    DWORD window_next = 0;
    BOOL suspended = FALSE, connected = FALSE;
    int i;

    (void)arg;
    if (!(get_state = load_xinput()))
        log_line( "no XInput DLL found; only the window fix is active" );
    QueryPerformanceFrequency( &freq );
    QueryPerformanceCounter( &last );

    while (!stopping)
    {
        XINPUT_STATE state;
        unsigned int now_held, changed;
        double dt, vx = 0, vy = 0, factor = 1.0;
        DWORD tick;

        Sleep( cfg.poll_ms );
        QueryPerformanceCounter( &now );
        dt = (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart;
        last = now;
        if (dt > 0.1) dt = 0.1;  /* after a stall, don't fling the cursor */
        tick = GetTickCount();
        if (cfg.fix_window && !adventure_hwnd && (int)(tick - window_next) >= 0)
        {
            EnumWindows( fix_window_proc, 0 );
            window_next = tick + 500;
        }
        if (!get_state)
        {
            Sleep( 100 );
            continue;
        }

        /* Suspended: leave the controller to Autorun, and watch for the keys
         * it sends for L3 and R3 (Ctrl and Alt by default) to come back. */
        if (suspended)
        {
            if ((GetAsyncKeyState( cfg.resume_key1 ) & 0x8000) && (GetAsyncKeyState( cfg.resume_key2 ) & 0x8000))
            {
                if (!combo_since) combo_since = tick;
                else if (tick - combo_since >= (DWORD)cfg.suspend_hold_ms)
                {
                    send_key( (WORD)cfg.resume_key1, TRUE );
                    send_key( (WORD)cfg.resume_key2, TRUE );
                    suspended = FALSE;
                    combo_since = 0;
                    log_line( "resumed" );
                }
            }
            else combo_since = 0;
            continue;
        }

        if (!our_window_has_focus())
        {
            if (held) release_all( held );
            held = 0;
            Sleep( 50 );
            continue;
        }

        memset( &state, 0, sizeof(state) );
        if (get_state( 0, &state ) != ERROR_SUCCESS)
        {
            if (connected) log_line( "controller disconnected" );
            connected = FALSE;
            if (held) release_all( held );
            held = 0;
            Sleep( 500 );
            continue;
        }
        if (!connected) log_line( "controller connected" );
        connected = TRUE;

        now_held = read_buttons( &state.Gamepad );
        if (cfg.left_stick == STICK_ARROWS) now_held |= stick_arrows( state.Gamepad.sThumbLX, state.Gamepad.sThumbLY );
        if (cfg.right_stick == STICK_ARROWS) now_held |= stick_arrows( state.Gamepad.sThumbRX, state.Gamepad.sThumbRY );

        /* L3 + R3 held: hand the controller back to Autorun. */
        if ((now_held & (1u << BTN_LSTICK)) && (now_held & (1u << BTN_RSTICK)))
        {
            if (!combo_since) combo_since = tick;
            else if (tick - combo_since >= (DWORD)cfg.suspend_hold_ms)
            {
                release_all( held );
                held = 0;
                suspended = TRUE;
                combo_since = 0;
                log_line( "suspended; hold L3 + R3 again to resume" );
                continue;
            }
        }
        else combo_since = 0;

        changed = now_held ^ held;
        for (i = 0; i < BTN_COUNT; i++)
            if (changed & (1u << i)) action_edge( &cfg.actions[i], (now_held & (1u << i)) != 0 );

        /* Held wheel buttons keep scrolling. */
        if (changed & now_held) wheel_next = tick + 3 * (DWORD)cfg.wheel_repeat_ms;
        else if ((int)(tick - wheel_next) >= 0)
        {
            for (i = 0; i < BTN_COUNT; i++)
                if ((now_held & (1u << i)) &&
                    (cfg.actions[i].kind == ACT_WHEELUP || cfg.actions[i].kind == ACT_WHEELDOWN))
                    action_edge( &cfg.actions[i], TRUE );
            wheel_next = tick + (DWORD)cfg.wheel_repeat_ms;
        }
        held = now_held;

        /* The cursor. */
        if (cfg.left_stick == STICK_MOUSE) stick_velocity( state.Gamepad.sThumbLX, state.Gamepad.sThumbLY, &vx, &vy );
        if (cfg.right_stick == STICK_MOUSE) stick_velocity( state.Gamepad.sThumbRX, state.Gamepad.sThumbRY, &vx, &vy );
        for (i = 0; i < BTN_COUNT; i++)
        {
            if (!(held & (1u << i))) continue;
            if (cfg.actions[i].kind == ACT_PRECISION) factor *= cfg.precision_percent / 100.0;
            if (cfg.actions[i].kind == ACT_TURBO) factor *= cfg.turbo_percent / 100.0;
        }
        if (vx == 0 && vy == 0)
        {
            acc_x = acc_y = 0;
            continue;
        }
        acc_x += vx * factor * dt;
        acc_y += vy * factor * dt;
        {
            LONG dx = (LONG)acc_x, dy = (LONG)acc_y;

            if (dx || dy)
            {
                move_cursor_by( dx, dy );
                acc_x -= dx;
                acc_y -= dy;
            }
        }
    }
    if (held) release_all( held );
    return 0;
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, LPVOID reserved )
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        char *slash;

        DisableThreadLibraryCalls( instance );
        GetModuleFileNameA( instance, base_dir, sizeof(base_dir) );
        if ((slash = strrchr( base_dir, '\\' ))) slash[1] = 0;
        else base_dir[0] = 0;
        load_config();
        if (!cfg.enabled) return TRUE;
        log_line( "ShandalarPad loaded (process %lu)", (unsigned long)GetCurrentProcessId() );
        if (cfg.fake_screen) install_fake_screen();
        if (cfg.fix_window)
            cbt_hook = SetWindowsHookExA( WH_CBT, cbt_proc, NULL, GetCurrentThreadId() );
        pad_thread = CreateThread( NULL, 0, pad_main, NULL, 0, NULL );
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        InterlockedExchange( &stopping, 1 );
        if (cbt_hook) UnhookWindowsHookEx( cbt_hook );
        if (pad_thread) CloseHandle( pad_thread );
    }
    return TRUE;
}
