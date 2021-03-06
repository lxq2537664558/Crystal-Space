//
//  OSXDriver2D.cpp
//
//
//  Created by mreda on Wed Oct 31 2001.
//  Copyright (c) 2001 Matt Reda. All rights reserved.
//

#include "cssysdef.h"
#include "csutil/scf.h"
#include "iutil/cmdline.h"
#include "iutil/event.h"
#include "iutil/eventq.h"
#include "iutil/objreg.h"
#include "ivaria/reporter.h"
#include "csutil/cmdhelp.h"
#include "csutil/event.h"
#include "csutil/eventnames.h"

#include "csplugincommon/macosx/OSXDriver2D.h"

#include <sys/time.h>



// Constructor
// Initialize graphics driver
OSXDriver2D::OSXDriver2D(csGraphics2D *inCanvas)
{
    scfiEventHandler = 0;

    canvas = inCanvas;
    originalMode = 0;

    origWidth = 0;
    origHeight = 0;
    display = kCGDirectMainDisplay;

    delegate = OSXDelegate2D_new(this);
}


// Destructor
// Destroy driver
OSXDriver2D::~OSXDriver2D()
{
    if (scfiEventHandler != 0)
    {
        csRef<iEventQueue> queue = csQueryRegistry<iEventQueue> (objectReg);
        if (queue.IsValid())
            queue->RemoveListener(scfiEventHandler);
        scfiEventHandler->DecRef();
    }

    Close();	// Just in case it hasn't been called

#ifdef CS_OSX_10_6
    CGDisplayModeRelease(originalMode);
#endif
    OSXDelegate2D_delete(delegate);
}


// Initialize
// Initialize 2D canvas plugin
bool OSXDriver2D::Initialize(iObjectRegistry *reg)
{
    objectReg = reg;

    // Get assistant
    assistant = csQueryRegistry<iOSXAssistant> (reg);

    // Create event handler
    if (scfiEventHandler == 0)
        scfiEventHandler = new EventHandler(this);

    // Listen for key down events
    csRef<iEventQueue> queue = csQueryRegistry<iEventQueue> (reg);
    focusChangedEvt = csevFocusChanged(reg);
     commandLineHelpEvt = csevCommandLineHelp(reg);
     keyboardDownEvt = csevKeyboardDown(reg);
     csEventID filter[] = {focusChangedEvt, commandLineHelpEvt, keyboardDownEvt,CS_EVENTLIST_END};
    if (queue.IsValid())
      queue->RegisterListener(scfiEventHandler, filter);
	  

    // Figure out what screen we will be using
    ChooseDisplay();

    // Get and save original mode and gamma - released in Close()
#ifdef CS_OSX_10_6
    originalMode = CGDisplayCopyDisplayMode(display);
#else
    originalMode = CGDisplayCurrentMode(display);
#endif
    SaveGamma(display, originalGamma);

    return true;
}


// Open
// Open graphics system (set mode, open window, etc)
bool OSXDriver2D::Open()
{
    // Copy original values
    origWidth = canvas->fbWidth;
    origHeight = canvas->fbHeight;

    // Switch to fullscreen mode if necessary
    if (canvas->FullScreen == true)
        if ((inFullscreenMode = EnterFullscreenMode()) == false)
            return false;

    // Create window
    if (OSXDelegate2D_openWindow(delegate, canvas->win_title.GetData(),
	canvas->fbWidth, canvas->fbHeight, canvas->Depth,
	canvas->FullScreen, display, screen) == false)
        return false;

    return true;
}


// Close
// Close graphics system
void OSXDriver2D::Close()
{
    // Close window
    OSXDelegate2D_closeWindow(delegate);

    // If we're in fullscreen mode, get out of it
    if (inFullscreenMode == true)
    {
        ExitFullscreenMode();
        inFullscreenMode = false;
    }
}


// HandleEvent
// Handle an event
// Look for Alt-Enter to toggle fullscreen mode
bool OSXDriver2D::HandleEvent(iEvent &ev)
{
    bool handled = false;

    if (csEventNameRegistry::IsKindOf(objectReg, ev.Name, focusChangedEvt))
    {
      bool shouldPause = !assistant->always_runs();
      OSXDelegate2D_focusChanged(delegate, 
				 csCommandEventHelper::GetInfo (&ev), shouldPause);
      handled = true;
    }
    else if (ev.Name == commandLineHelpEvt)
    {
      csCommandLineHelper::PrintTitle ("MacOS X 2D graphics drivers", 1);
      csCommandLineHelper::PrintOption ("screen", "Screen number to display on", csVariant (0));
      handled = true;
    }
    else if (ev.Name == keyboardDownEvt)
    {
      if ((csKeyEventHelper::GetRawCode(&ev) == '\r') && 
	  (csKeyEventHelper::GetModifiersBits(&ev) & CSMASK_ALT))
	handled = ToggleFullscreen();
    }
    
    return handled;
}


// DispatchEvent
// Dispatch an event to the assistant
void OSXDriver2D::DispatchEvent(OSXEvent ev, OSXView view)
{
    assistant->dispatch_event(ev, view);
}


// HideMouse
// Hide the mouse
void OSXDriver2D::HideMouse()
{
    assistant->hide_mouse_pointer();
}


// ShowMouse
// Show the mouse cursor
void OSXDriver2D::ShowMouse()
{
    assistant->show_mouse_pointer();
}


#ifdef CS_OSX_10_6

static inline bool ieql(CFStringRef s, CFStringRef t)
{ return CFStringCompare(s,t,kCFCompareCaseInsensitive) == kCFCompareEqualTo; }


static size_t extract_bpp(CGDisplayModeRef mode)
{
    size_t depth = 0;
    CFStringRef enc = CGDisplayModeCopyPixelEncoding(mode);
    if (ieql(enc, CFSTR(IO32BitDirectPixels)))
        depth = 32;
    else if (ieql(enc, CFSTR(IO16BitDirectPixels)))
        depth = 16;
    else if (ieql(enc, CFSTR(IO8BitIndexedPixels)))
        depth = 8;
    CFRelease(enc);
    return depth;
}


// Caller is responsible for releasing returned value.
static CGDisplayModeRef find_best_mode(
    CGDirectDisplayID display, size_t bpp, size_t width, size_t height)
{
    CGDisplayModeRef exact = 0;
    CGDisplayModeRef fallback = 0;
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(display, 0);
    for (CFIndex i = 0, iN = CFArrayGetCount(modes); i < iN; ++i)
    {
	CGDisplayModeRef mode = (CGDisplayModeRef)
	    CFArrayGetValueAtIndex(modes, i);
	if (CGDisplayModeGetWidth(mode) >= width &&
	    CGDisplayModeGetHeight(mode) >= height) {
	    size_t mode_bpp = extract_bpp(mode);
	    if (mode_bpp == bpp)
		exact = mode;
	    else if (mode_bpp > bpp)
		fallback = mode;
	}
    }
    CGDisplayModeRef best;
    if (exact != 0)
	best = (CGDisplayModeRef)CFRetain(exact);
    else if (fallback != 0)
	best = (CGDisplayModeRef)CFRetain(fallback);
    else
	best = CGDisplayCopyDisplayMode(display);
    CFRelease(modes);
    return best;
}


// EnterFullscreenMode
// Switch to fullscreen mode - returns true on success
bool OSXDriver2D::EnterFullscreenMode()
{
    CGDisplayModeRef mode = find_best_mode(
        display, canvas->Depth, canvas->fbWidth, canvas->fbHeight);
    FadeToRGB(display, 0.0, 0.0, 0.0);
    if (CGDisplayCapture(display) == CGDisplayNoErr)
    {
	if (CGDisplaySetDisplayMode(display, mode, 0) == CGDisplayNoErr)
	{
	    canvas->vpWidth = CGDisplayModeGetWidth(mode);
	    canvas->vpHeight = CGDisplayModeGetHeight(mode);
	    canvas->Depth = extract_bpp(mode);
	    CFRelease(mode);
	    FadeToGammaTable(display, originalGamma);
	    return true;
	}
	CGDisplayRelease(display);
    }
    CFRelease(mode);
    FadeToGammaTable(display, originalGamma);
    return false;
}


#else /* CS_OSX_10_6 */

// EnterFullscreenMode
// Switch to fullscreen mode - returns true on success
bool OSXDriver2D::EnterFullscreenMode()
{
    // Find mode and copy parameters
    CFDictionaryRef mode = CGDisplayBestModeForParameters(display, 
                        canvas->Depth, canvas->fbWidth, canvas->fbHeight, 0);
    if (mode == 0)
        return false;

    // Fade to black
    FadeToRGB(display, 0.0, 0.0, 0.0);

    // Lock displays
    if (CGDisplayCapture(display) != CGDisplayNoErr)
        return false;

    // Switch to new mode
    if (CGDisplaySwitchToMode(display, mode) == CGDisplayNoErr)
    {
        // Extract actual Width/Height/Depth
        CFNumberGetValue(
	    (CFNumberRef) CFDictionaryGetValue(mode, kCGDisplayWidth),
	    kCFNumberLongType, &canvas->fbWidth);
	canvas->vpWidth = canvas->fbWidth;
        CFNumberGetValue(
	    (CFNumberRef) CFDictionaryGetValue(mode, kCGDisplayHeight),
	    kCFNumberLongType, &canvas->fbHeight);
	canvas->vpHeight = canvas->fbHeight;
        CFNumberGetValue(
	    (CFNumberRef) CFDictionaryGetValue(mode, kCGDisplayBitsPerPixel), 
	    kCFNumberLongType, &canvas->Depth);
        
        // Fade back to original gamma
        FadeToGammaTable(display, originalGamma);
    }
    else
    {
        CGDisplayRelease(display);
        return false;
    }

    return true;
}

#endif


// ExitFullscreenMode
// Switch out of fullscreen mode, to mode stored in originalMode
void OSXDriver2D::ExitFullscreenMode()
{
    FadeToRGB(display, 0.0, 0.0, 0.0);
#ifdef CS_OSX_10_6
    CGDisplaySetDisplayMode(display, originalMode, 0);
#else
    CGDisplaySwitchToMode(display, originalMode);
#endif
    CGDisplayRelease(display);
    FadeToGammaTable(display, originalGamma);
}


// ToggleFullscreen
// Toggle current state of fullscreen
bool OSXDriver2D::ToggleFullscreen()
{
    bool oldAllowResizing = canvas->AllowResizing;
    bool success = true;

    OSXDelegate2D_closeWindow(delegate);

    if (canvas->FullScreen == true)
    {
        ExitFullscreenMode();
        inFullscreenMode = false;
    }

    // Restore original dimensions; force resize by forcing AllowResize to true
    canvas->FullScreen = !canvas->FullScreen;
    canvas->AllowResizing = true;
    canvas->Resize(origWidth, origHeight);
    canvas->AllowResizing = oldAllowResizing;

    if ((success == true) && (canvas->FullScreen == true))
    {
        inFullscreenMode = EnterFullscreenMode();
        success = inFullscreenMode;
    }

    if (success == true)
        OSXDelegate2D_openWindow(delegate, canvas->win_title.GetData(),
                                canvas->fbWidth, canvas->fbHeight, canvas->Depth, 
                                canvas->FullScreen, display, screen);

    return success;
}


// FadeToRGB
// Uses CoreGraphics to fade to a given color 
void OSXDriver2D::FadeToRGB(CGDirectDisplayID disp, float r, float g, float b)
{
    int i;
    GammaTable gamma;
    
    for (i = 0; i < 256; i++)
    {
        gamma.r[i] = r;
        gamma.g[i] = g;
        gamma.b[i] = b;
    }
    
    FadeToGammaTable(disp, gamma);
}


// FadeToGamma
// Fade to a given gamma table
void OSXDriver2D::FadeToGammaTable(CGDirectDisplayID disp, GammaTable table)
{
    static const float TOTAL_USEC = 1000000.0;	// 1 second total
    
    int i;
    float x, start_usec, end_usec, current_usec;
    timeval start, current;
    GammaTable temp;
    
    gettimeofday(&start, 0);
    start_usec = (start.tv_sec * 1000000) + start.tv_usec;
    end_usec = start_usec + TOTAL_USEC;
    
    do {
        gettimeofday(&current, 0);
        current_usec = (current.tv_sec * 1000000) + current.tv_usec;
        
        // Calculate fraction of elapsed time
        x = (current_usec - start_usec) / TOTAL_USEC;        
        
        for (i = 0; i < 256; i++)
        {
            temp.r[i] = originalGamma.r[i] + 
                                (x * (table.r[i] - originalGamma.r[i])); 
            temp.g[i] = originalGamma.g[i] + 
                                (x * (table.g[i] - originalGamma.g[i])); 
            temp.b[i] = originalGamma.b[i] + 
                                (x * (table.b[i] - originalGamma.b[i]));
        }
        
        CGSetDisplayTransferByTable(disp, 256, temp.r, temp.g, temp.b);
    } while (current_usec < end_usec);

    CGSetDisplayTransferByTable(disp, 256, table.r, table.g, table.b);
}


// SaveGamma
// Save the current gamma values to the given table
void OSXDriver2D::SaveGamma(CGDirectDisplayID disp, GammaTable &table)
{
    CGDisplayErr err;
    uint32_t sampleCount;
    
    err = CGGetDisplayTransferByTable(disp, 256, table.r, table.g, 
                                        table.b, &sampleCount);
    if (err != kCGErrorSuccess)
        csFPrintf(stderr, "Error %d reading gamma values\n", err);
}


// ChooseDisplay
// Choose which display to use
// Updates the screen and display members
void OSXDriver2D::ChooseDisplay()
{
    csRef<iCommandLineParser> parser = 
	csQueryRegistry<iCommandLineParser> (objectReg);
    const char *s = parser->GetOption("screen");
    if (s != 0)
        screen = (unsigned int)atoi(s);
    else
    {
        csConfigAccess cfg(objectReg, "/config/video.cfg");
        screen = (unsigned int)cfg->GetInt("Video.ScreenNumber", 0);
    }

    // Get list of displays and get id of display to use if not default
    display = kCGDirectMainDisplay;
    if (screen != 0)
    {
        CGDisplayErr err;
        CGDisplayCount numDisplays;
	// Who is going to have more than 32 displays??
        CGDirectDisplayID displayList[32];
        
        err = CGGetActiveDisplayList(32, displayList, &numDisplays);
        if (err == CGDisplayNoErr)
        {
            if (screen < numDisplays)
                display = displayList[screen];
            else
                csReport(objectReg, CS_REPORTER_SEVERITY_WARNING, 
                        "crystalspace.canvas.osxdriver2d",
                        "WARNING: Requested screen %u but only %d are "
			"available - using main display\n", 
                        screen, numDisplays);
        }
        else
            csReport(objectReg, CS_REPORTER_SEVERITY_WARNING, 
                    "crystalspace.canvas.osxdriver2d",
                    "WARNING: Requested screen %u but unable to get screen "
		    "list - using main display\n", screen);
                    
        if (display == kCGDirectMainDisplay)
            screen = 0;
    }
}


/// C API to driver class
#define DRV2D_FUNC(ret, func) __private_extern__ "C" ret OSXDriver2D_##func

typedef void *OSXDriver2DHandle;
typedef void *OSXEventHandle;
typedef void *OSXViewHandle;


// C API to driver class
DRV2D_FUNC(void, DispatchEvent)(OSXDriver2DHandle driver, OSXEventHandle ev,
    OSXViewHandle view)
{
    ((OSXDriver2D *) driver)->DispatchEvent(ev, view);
}

DRV2D_FUNC(bool, Resize)(OSXDriver2DHandle driver, int w, int h)
{
    return ((OSXDriver2D *) driver)->Resize(w, h);
}

DRV2D_FUNC(void, HideMouse)(OSXDriver2DHandle driver)
{
    ((OSXDriver2D *) driver)->HideMouse();
}

DRV2D_FUNC(void, ShowMouse)(OSXDriver2DHandle driver)
{
    ((OSXDriver2D *) driver)->ShowMouse();
}

#undef DRV2D_FUNC
