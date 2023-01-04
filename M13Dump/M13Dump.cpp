// license:BSD-3-Clause
// copyright-holders:Aaron Giles,Paul Priest
//============================================================
//
//  ledutil.c - Win32 example code that tracks changing
//  outputs and updates the keyboard LEDs in response
//
//============================================================
//
//  This is sample code. To use it as a starting point, you
//  should do the following:
//
//  1. Change the CLIENT_ID define to something unique.
//
//  2. Change the WINDOW_CLASS and WINDOW_NAME defines to
//  something unique.
//
//  3. Delete all the code from the >8 snip 8< comment and
//  downward.
//
//  4. Implement the following functions:
//
//      output_startup - called at app init time
//      output_shutdown - called before the app exits
//      output_mame_start - called when MAME starts
//      output_mame_stop - called when MAME exits
//      output_set_state - called whenever state changes
//
//============================================================
#define _CRT_SECURE_NO_WARNINGS

// standard windows headers
#include <windows.h>
#include <winioctl.h>

// standard C headers
#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MAME output header file
typedef int running_machine;
#include "osdcomm.h"
#include "modules/output/win32_output.h"


//============================================================
//  DEBUGGING
//============================================================

// note you need to compile as a console app to have any of
// these printfs show up
#define DEBUG_VERSION       1

#if DEBUG_VERSION
#define DEBUG_PRINTF(x)     printf x
#else
#define DEBUG_PRINTF(x)
#endif


//============================================================
//  CONSTANTS
//============================================================

// unique client ID
#define CLIENT_ID                           (('M' << 24) | ('L' << 16) | ('E' << 8) | ('D' << 0))

// LED methods
//#define LED_METHOD_PS2                      0
//#define LED_METHOD_USB                      1

// window parameters
#define WINDOW_CLASS                        TEXT("LEDSample")
#define WINDOW_NAME                         TEXT("LEDSample")

// window styles
#define WINDOW_STYLE                        WS_OVERLAPPEDWINDOW
#define WINDOW_STYLE_EX                     0

// Define the keyboard indicators.
// (Definitions borrowed from ntddkbd.h)

//#define IOCTL_KEYBOARD_SET_INDICATORS       CTL_CODE(FILE_DEVICE_KEYBOARD, 0x0002, METHOD_BUFFERED, FILE_ANY_ACCESS)
//#define IOCTL_KEYBOARD_QUERY_TYPEMATIC      CTL_CODE(FILE_DEVICE_KEYBOARD, 0x0008, METHOD_BUFFERED, FILE_ANY_ACCESS)
//#define IOCTL_KEYBOARD_QUERY_INDICATORS     CTL_CODE(FILE_DEVICE_KEYBOARD, 0x0010, METHOD_BUFFERED, FILE_ANY_ACCESS)

//#define KEYBOARD_SCROLL_LOCK_ON             1
//#define KEYBOARD_NUM_LOCK_ON                2
//#define KEYBOARD_CAPS_LOCK_ON               4



//============================================================
//  TYPE DEFINITIONS
//============================================================

struct KEYBOARD_INDICATOR_PARAMETERS
{
	USHORT UnitId;             // Unit identifier.
	USHORT LedFlags;           // LED indicator state.
};


struct id_map_entry
{
	id_map_entry *          next;
	const char *            name;
	WPARAM                  id;
};



//============================================================
//  GLOBAL VARIABLES
//============================================================

//static int                  ledmethod;
unsigned char				valueVR;
unsigned char				value;
static int                  original_state;
static int                  current_state;
static int                  pause_state;
static HANDLE               hKbdDev;

static HWND                 mame_target;
static HWND                 listener_hwnd;

static id_map_entry *       idmaplist;

// message IDs
static UINT                 om_mame_start;
static UINT                 om_mame_stop;
static UINT                 om_mame_update_state;
static UINT                 om_mame_register_client;
static UINT                 om_mame_unregister_client;
static UINT                 om_mame_get_id_string;



//============================================================
//  FUNCTION PROTOTYPES
//============================================================

static int create_window_class(void);
static LRESULT CALLBACK listener_window_proc(HWND wnd, UINT message, WPARAM wparam, LPARAM lparam);
static LRESULT handle_mame_start(WPARAM wparam, LPARAM lparam);
static LRESULT handle_mame_stop(WPARAM wparam, LPARAM lparam);
static LRESULT handle_copydata(WPARAM wparam, LPARAM lparam);
static void reset_id_to_outname_cache(void);
static const char *map_id_to_outname(WPARAM id);
static LRESULT handle_update_state(WPARAM wparam, LPARAM lparam);

// these functions provide the meat
static void output_startup(const char *commandline);
static void output_mame_start(void);
static void output_set_state(const char *name, int32_t state);
static void output_mame_stop(void);
static void output_shutdown(void);

//static int led_get_state(void);
//static void led_set_state(int state);


//============================================================
//  main
//============================================================

int main(int argc, char *argv[])
{
	const char *arg = (argc > 1) ? argv[1] : "";
	int exitcode = 1;
	HWND otherwnd;
	MSG message;
	int result;

	// see if there is another instance of us running
	otherwnd = FindWindow(WINDOW_CLASS, WINDOW_NAME);

	// if the argument is "-kill", post a close message
	if (strcmp(arg, "-kill") == 0)
	{
		if (otherwnd != nullptr)
			PostMessage(otherwnd, WM_QUIT, 0, 0);
		return (otherwnd != nullptr) ? 1 : 0;
	}

	// if we had another instance, defer to it
	if (otherwnd != nullptr)
		return 0;

	// call the startup code
	output_startup(arg);

	// create our window class
	result = create_window_class();
	if (result != 0)
		goto error;

	// create a window
	listener_hwnd = CreateWindowEx(
						WINDOW_STYLE_EX,
						WINDOW_CLASS,
						WINDOW_NAME,
						WINDOW_STYLE,
						0, 0,
						1, 1,
						nullptr,
						nullptr,
						GetModuleHandle(nullptr),
						nullptr);
	if (listener_hwnd == nullptr)
		goto error;

	// allocate message ids
	om_mame_start = RegisterWindowMessage(OM_MAME_START);
	if (om_mame_start == 0)
		goto error;
	om_mame_stop = RegisterWindowMessage(OM_MAME_STOP);
	if (om_mame_stop == 0)
		goto error;
	om_mame_update_state = RegisterWindowMessage(OM_MAME_UPDATE_STATE);
	if (om_mame_update_state == 0)
		goto error;

	om_mame_register_client = RegisterWindowMessage(OM_MAME_REGISTER_CLIENT);
	if (om_mame_register_client == 0)
		goto error;
	om_mame_unregister_client = RegisterWindowMessage(OM_MAME_UNREGISTER_CLIENT);
	if (om_mame_unregister_client == 0)
		goto error;
	om_mame_get_id_string = RegisterWindowMessage(OM_MAME_GET_ID_STRING);
	if (om_mame_get_id_string == 0)
		goto error;

	// see if MAME is already running
	otherwnd = FindWindow(OUTPUT_WINDOW_CLASS, OUTPUT_WINDOW_NAME);
	if (otherwnd != nullptr)
		handle_mame_start((WPARAM)otherwnd, 0);
	printf("M13Dump.EXE V1.5\nPlug Drive board on COM5...\n");
	
	// process messages
	while (GetMessage(&message, nullptr, 0, 0))
	{
		TranslateMessage(&message);
		DispatchMessage(&message);
	}

	// reset on the way out if still live
	if (mame_target != nullptr)
		handle_mame_stop((WPARAM)mame_target, 0);
	exitcode = 0;

error:
	// call the shutdown code
	output_shutdown();

	return exitcode;
}


//============================================================
//  create_window_class
//============================================================

static int create_window_class(void)
{
	static int classes_created = FALSE;

	/* only do this once */
	if (!classes_created)
	{
		WNDCLASS wc = { 0 };

		// initialize the description of the window class
		wc.lpszClassName    = WINDOW_CLASS;
		wc.hInstance        = GetModuleHandle(nullptr);
		wc.lpfnWndProc      = listener_window_proc;

		// register the class; fail if we can't
		if (!RegisterClass(&wc))
			return 1;
		classes_created = TRUE;
	}

	return 0;
}


//============================================================
//  window_proc
//============================================================

static LRESULT CALLBACK listener_window_proc(HWND wnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	// OM_MAME_START: register ourselves with the new MAME (first instance only)
	if (message == om_mame_start)
		return handle_mame_start(wparam, lparam);

	// OM_MAME_STOP: no need to unregister, just note that we've stopped caring and reset the LEDs
	else if (message == om_mame_stop)
		return handle_mame_stop(wparam, lparam);

	// OM_MAME_UPDATE_STATE: update the state of this item if we care
	else if (message == om_mame_update_state)
		return handle_update_state(wparam, lparam);

	// WM_COPYDATA: extract the string and create an ID map entry
	else if (message == WM_COPYDATA)
		return handle_copydata(wparam, lparam);

	// everything else is default
	else
		return DefWindowProc(wnd, message, wparam, lparam);
}


//============================================================
//  handle_mame_start
//============================================================

static LRESULT handle_mame_start(WPARAM wparam, LPARAM lparam)
{
	DEBUG_PRINTF(("Process ID %08X detected\n", (uint32_t)wparam));

	// make this the targeted version of MAME
	mame_target = (HWND)wparam;

	// initialize the LED states
	output_mame_start();
	reset_id_to_outname_cache();

	// register ourselves as a client
	PostMessage(mame_target, om_mame_register_client, (WPARAM)listener_hwnd, CLIENT_ID);

	// get the game name
	map_id_to_outname(0);
	return 0;
}


//============================================================
//  handle_mame_stop
//============================================================

static LRESULT handle_mame_stop(WPARAM wparam, LPARAM lparam)
{
	DEBUG_PRINTF(("\nProcess ID %08X: Game Over\n\nWaiting new game...\n", (uint32_t)wparam));

	// ignore if this is not the instance we care about
	if (mame_target != (HWND)wparam)
		return 1;

	// clear our target out
	mame_target = nullptr;
	reset_id_to_outname_cache();

	// reset the LED states
	output_mame_stop();
	return 0;
}


//============================================================
//  handle_copydata
//============================================================

static LRESULT handle_copydata(WPARAM wparam, LPARAM lparam)
{
	COPYDATASTRUCT *copydata = (COPYDATASTRUCT *)lparam;
	copydata_id_string *data = (copydata_id_string *)copydata->lpData;
	id_map_entry *entry;
	char *string;

	//DEBUG_PRINTF(("copydata (%08X)\n", (uint32_t)wparam));

	// ignore requests we don't care about
	if (mame_target != (HWND)wparam)
		return 1;

	// allocate memory
	entry = (id_map_entry *)malloc(sizeof(*entry));
	if (entry == nullptr)
		return 0;

	string = (char *)malloc(strlen(data->string) + 1);
	if (string == nullptr)
	{
		free(entry);
		return 0;
	}

	// if all allocations worked, make a new entry
	entry->next = idmaplist;
	entry->name = string;
	entry->id = data->id;

	// copy the string and hook us into the list
	strcpy(string, data->string);
	idmaplist = entry;

	// ****** ALEX HERE *******************
	//DEBUG_PRINTF(("  id %d = '%s'\n", (int)entry->id, entry->name));
	if ((int)entry->id == 0)
	{
		DEBUG_PRINTF(("Game detected: %s\n", entry->name));

		printf("\n Drive Board    Lamps    Coin1 Coin2 Start Red  Blue Yellow Green Leader\n -----------    -----    -----------------------------------------------\n");

}

	return 0;
}


//============================================================
//  reset_id_to_outname_cache
//============================================================

static void reset_id_to_outname_cache(void)
{
	// free our ID list
	while (idmaplist != nullptr)
	{
		id_map_entry *temp = idmaplist;
		idmaplist = temp->next;
		free((void*)temp->name);
		free(temp);
	}
}


//============================================================
//  map_id_to_outname
//============================================================

static const char *map_id_to_outname(WPARAM id)
{
	id_map_entry *entry;

	// see if we have an entry in our map
	for (entry = idmaplist; entry != nullptr; entry = entry->next)
		if (entry->id == id)
			return entry->name;

	// no entry yet; we have to ask
	SendMessage(mame_target, om_mame_get_id_string, (WPARAM)listener_hwnd, id);

	// now see if we have the entry in our map
	for (entry = idmaplist; entry != nullptr; entry = entry->next)
		if (entry->id == id)
			return entry->name;

	// if not, use an empty string
	return "";
}


//============================================================
//  handle_update_state
//============================================================

static LRESULT handle_update_state(WPARAM wparam, LPARAM lparam)
{
	//DEBUG_PRINTF(("update_state: id=%d state=%d\n", (uint32_t)wparam, (uint32_t)lparam));
	output_set_state(map_id_to_outname(wparam), lparam);
	return 0;
}


//
// END BOILERPLATE CODE
//
// ------------------------>8 snip 8<-------------------------
//
// BEGIN LED-SPECIFIC CODE
//


//============================================================
//  output_startup
//============================================================

static void output_startup(const char *commandline)
{
	// default to PS/2, override if USB is specified as a parameter
	//ledmethod = LED_METHOD_PS2;
	//if (commandline != nullptr && strcmp(commandline, "-usb") == 0)
		//ledmethod = LED_METHOD_USB;

}


//============================================================
//  output_shutdown
//============================================================

static void output_shutdown(void)
{
	// nothing to do here
}


//============================================================
//  output_mame_start
//============================================================

static void output_mame_start(void)
{
//	HRESULT error_number;

	// initialize the system based on the method

}


//============================================================
//  output_mame_stop
//============================================================

static void output_mame_stop(void)
{
	int error_number = 0;

	// restore the initial LED states

}


//============================================================
//  output_set_state
//============================================================

static void output_set_state(const char *outname, int32_t state)
{
	// *********************** ALEX HERE ***********************************
	//DEBUG_PRINTF(("%s\n", outname));


	// look for LED0/LED1/LED2 states and update accordingly
	if (strcmp(outname, "RawDrive") == 0)
	{
		//DEBUG_PRINTF(("RawDrive: %02X\n", state));
		value = state;
	}
	else if (strcmp(outname, "RawLamps") == 0)
	{
		//DEBUG_PRINTF(("RawLamps: %02X\n", state));
		valueVR = state;
	}
	printf("\r     %02X          %02X        %1s     %1s     %1s    %1s     %1s     %1s     %1s   %s", value, valueVR, (valueVR & 1) ? "X" : " ", (valueVR & 2) ? "X" : " ", (valueVR & 4) ? "X" : " ", (valueVR & 8) ? "X" : " ", (valueVR & 16) ? "X" : " ", (valueVR & 32) ? "X" : " ", (valueVR & 64) ? "X" : " ", (valueVR & 128) ? "Leader" : "      ");

}


