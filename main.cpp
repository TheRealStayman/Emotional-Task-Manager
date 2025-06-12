#define UNICODE
#define _UNICODE
#include <windows.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <string>
#include <thread>
#include <chrono>
#include <shellapi.h>
#include <pdh.h>
#include <map>
#include <vector>
#include <iostream>
#include <Dbt.h>
#include <winevt.h>
#include <mutex>
#include <condition_variable>
#include "resource.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wevtapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib") // Add this line for CreateStreamOnHGlobal

using namespace Gdiplus;

// Window defines
#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAYICON 1
#define ID_EXIT 1001
#define ID_ALWAYS_ON_TOP 1002

// Window class name for message-only window
#define WINDOW_CLASS_NAME TEXT("EmotionalTaskManager")

// Enum for emotional states
enum EmotionalState {
    HAPPY,               // Default
    PLEASED,             // Below thresholds after being above
    NEUTRAL,             // Memory > 90%
    GRIMACE,             // App error
    GRIMACE_TWO_SWEAT,   // Memory > 95%
    SURPRISED,           // Device connect/disconnect
    ANGUISH,             // CPU > 50%
    ANGUISH_VERY,        // CPU > 70%
    ANGUISH_EXTREMELY,   // CPU > 90%
    TIRED,               // Battery < 30%
    TIRED_VERY,          // Battery < 20%
    TIRED_EXTREMELY      // Battery < 10% 
};

// Global variables for window management
HWND g_hwnd = NULL;
int g_windowWidth = 200;
int g_windowHeight = 200;
bool g_alwaysOnTop = true;
NOTIFYICONDATAW nid = {0};
HICON g_customTrayIcon = NULL; // For managing the lifecycle of the custom tray icon

// Global variables for monitoring
PDH_HQUERY cpuQuery;
PDH_HCOUNTER cpuTotal;
EVT_HANDLE g_hSubscription = NULL;
double g_cpuUsage = 0.0;
double g_memoryUsage = 0.0;
int g_batteryPercent = 100;
bool g_hasBattery = false;

// Global variables for emotional state
EmotionalState g_currentState = HAPPY;
bool g_isBlinking = false;
std::map<EmotionalState, Image*> g_images;
std::map<EmotionalState, Image*> g_blinkImages;
std::map<EmotionalState, int> g_blinkIntervals;  // In seconds
std::map<EmotionalState, double> g_blinkDurations;  // In seconds
bool g_wasAboveThreshold = false;
bool g_temporaryState = false;
std::chrono::steady_clock::time_point g_temporaryStateStartTime;
std::chrono::seconds g_temporaryStateDuration(2); // Show temporary states for 2 seconds
HMODULE hInst = GetModuleHandle(NULL);

// Synchronization for state changes
std::mutex g_stateMutex;
std::condition_variable g_stateCV;
bool g_stateChanged = false;

// Function prototypes
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void StartBlinkTimer();
void MonitorSystem();
void UpdateEmotionalState();
void PlaceWindowOnSecondaryMonitor(HWND hwnd);
void AddToSystemTray(HWND hwnd);
void RemoveFromSystemTray();
HMENU CreateContextMenu();
bool InitializeDeviceNotifications();
bool InitializeEventLogMonitoring();
double GetCPUUsage();
double GetMemoryUsage();
void CheckBatteryStatus();
void ProcessWindowMessages();
void LoadImages(ULONG_PTR gdiplusToken); // Modified prototype
void DrawCurrentState();
void HandleDisplayChange();

// Callback for event log notifications
DWORD WINAPI SubscriptionCallback(EVT_SUBSCRIBE_NOTIFY_ACTION action, PVOID context, EVT_HANDLE hEvent) {
    if (action == EvtSubscribeActionDeliver) {
        // Application error detected
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_temporaryState = true;
            g_temporaryStateStartTime = std::chrono::steady_clock::now();
            g_currentState = GRIMACE;
            g_stateChanged = true;
        }
        g_stateCV.notify_one();
        
        // We could extract more detailed information from the event if needed
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
    
    return ERROR_SUCCESS;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Initialize GDI+
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    // Initialize PDH for CPU monitoring
    PdhOpenQuery(NULL, NULL, &cpuQuery);
    PdhAddCounter(cpuQuery, TEXT("\\Processor(_Total)\\% Processor Time"), NULL, &cpuTotal);
    PdhCollectQueryData(cpuQuery);

    // Load images
    LoadImages(gdiplusToken); // Pass the token
    
    // Load the application icon from resources.
    // Assumes IDI_APP_ICON is an integer resource ID defined in resource.h,
    // and the icon (e.g., "img/icon.ico") is compiled into the executable via an .rc file.
    HICON hAppIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON, 
                                      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 
                                      LR_DEFAULTCOLOR); // Load standard size icon from resources
    HICON hAppIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON, 
                                       GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 
                                       LR_DEFAULTCOLOR); // Load small size icon from resources

    if (!hAppIcon) {
        // Fallback to default application icon if custom one fails to load
        hAppIcon = LoadIconW(NULL, IDI_APPLICATION); 
    }
    if (!hAppIconSm) {
        // Fallback for small icon, can use the large one or default
        hAppIconSm = LoadIconW(NULL, IDI_APPLICATION); // Or use hAppIcon if it loaded
    }

    // Register window class
    WNDCLASSEXW wcex = {0};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = WINDOW_CLASS_NAME;
    wcex.hIcon = hAppIcon; // Set the large icon
    wcex.hIconSm = hAppIconSm; // Set the small icon, was hAppIcon before
    RegisterClassExW(&wcex);

    // Create the window
    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        WINDOW_CLASS_NAME,
        L"Emotional Task Manager",
        WS_POPUP,
        0, 0,
        g_windowWidth, g_windowHeight,
        NULL, NULL, hInstance, NULL);

    if (!g_hwnd) {
        MessageBoxW(NULL, L"Window creation failed!", L"Error", MB_ICONERROR);
        return -1;
    }

    // Set the layered window attributes for transparency
    SetLayeredWindowAttributes(g_hwnd, RGB(255, 0, 255), 0, LWA_COLORKEY);

    // Place window on secondary monitor
    PlaceWindowOnSecondaryMonitor(g_hwnd);

    // Add to system tray
    AddToSystemTray(g_hwnd);

    // Initialize device notifications
    if (!InitializeDeviceNotifications()) {
        OutputDebugStringW(L"Warning: Device notifications could not be initialized\n");
    }
    
    // Initialize event log monitoring
    if (!InitializeEventLogMonitoring()) {
        OutputDebugStringW(L"Warning: Event log monitoring could not be initialized\n");
    }

    // Make the window visible
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    // Start the blink timer in a separate thread
    std::thread blinkThread(StartBlinkTimer);
    blinkThread.detach();

    // Start the monitoring thread
    std::thread monitorThread(MonitorSystem);
    monitorThread.detach();

    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Cleanup
    RemoveFromSystemTray(); // This will call Shell_NotifyIconW(NIM_DELETE, &nid)
    
    // Destroy the custom tray icon if it was loaded and not already cleaned up by WM_DESTROY
    if (g_customTrayIcon) {
        DestroyIcon(g_customTrayIcon);
        g_customTrayIcon = NULL;
    }

    PdhCloseQuery(cpuQuery);
    if (g_hSubscription) {
        EvtClose(g_hSubscription);
    }
    
    // Clean up images
    for (auto& pair : g_images) {
        delete pair.second;
    }
    for (auto& pair : g_blinkImages) {
        delete pair.second;
    }
    
    // Destroy the application icon if it was loaded (and not the default one from LoadIcon)
    // However, class icons are typically managed by the system, so explicit destruction here might not be necessary
    // if (hAppIcon && hAppIcon != LoadIcon(NULL, IDI_APPLICATION)) {
    //     DestroyIcon(hAppIcon); 
    // }
    // For simplicity and common practice with WNDCLASSEX, we'll let the system manage it.

    GdiplusShutdown(gdiplusToken);
    
    return (int)msg.wParam;
}

Gdiplus::Image* LoadGdiplusImageFromResource(int resourceId, const wchar_t* resourceType) {
    HMODULE hModule = GetModuleHandle(NULL);
    HRSRC hRes = FindResource(hModule, MAKEINTRESOURCE(resourceId), resourceType);
    if (hRes == NULL) {
        DWORD dwError = GetLastError(); // Get error code
        OutputDebugStringW((L"Failed to find resource ID: " + std::to_wstring(resourceId) + 
                            L" Type: " + (IS_INTRESOURCE(resourceType) ? std::to_wstring(reinterpret_cast<UINT_PTR>(resourceType)) : resourceType) +
                            L" Error: " + std::to_wstring(dwError) + L"\n").c_str());
        return nullptr;
    }

    DWORD dwSize = SizeofResource(hModule, hRes);
    HGLOBAL hResLoad = LoadResource(hModule, hRes);
    if (hResLoad == NULL) {
        OutputDebugStringW((L"Failed to load resource ID: " + std::to_wstring(resourceId) + L"\n").c_str());
        return nullptr;
    }

    void* pData = LockResource(hResLoad);
    if (pData == NULL) {
        OutputDebugStringW((L"Failed to lock resource ID: " + std::to_wstring(resourceId) + L"\n").c_str());
        return nullptr;
    }

    // Allocate global memory and copy the resource data into it
    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, dwSize);
    if (hGlobal)
    {
        void* pGlobalData = GlobalLock(hGlobal);
        if (pGlobalData)
        {
            memcpy(pGlobalData, pData, dwSize);
            GlobalUnlock(hGlobal);

            // Create an IStream from the global memory
            IStream* pStream = nullptr;
            if (CreateStreamOnHGlobal(hGlobal, TRUE, &pStream) == S_OK)
            {
                // Gdiplus::Image takes ownership of the IStream
                Gdiplus::Image* image = new Gdiplus::Image(pStream);
                pStream->Release(); // Release our hold on it
                
                // Check if the image was created successfully from the stream
                if (image->GetLastStatus() == Gdiplus::Ok)
                {
                    OutputDebugStringW((L"Successfully loaded image for resource ID: " + std::to_wstring(resourceId) + L"\n").c_str());
                    return image;
                }
                else
                {
                    OutputDebugStringW((L"GDI+ failed to create image from stream for resource ID: " + std::to_wstring(resourceId) + L". Status: " + std::to_wstring(image->GetLastStatus()) + L"\n").c_str());
                    delete image;
                }
            }
            else
            {
                OutputDebugStringW((L"CreateStreamOnHGlobal failed for resource ID: " + std::to_wstring(resourceId) + L"\n").c_str());
                GlobalFree(hGlobal); // Manually free if CreateStreamOnHGlobal failed
            }
        }
        else
        {
            OutputDebugStringW((L"GlobalLock failed for resource ID: " + std::to_wstring(resourceId) + L"\n").c_str());
            GlobalFree(hGlobal); // Manually free if GlobalLock failed
        }
        // If CreateStreamOnHGlobal succeeds, hGlobal is freed by the IStream's Release.
    }
    else {
        OutputDebugStringW((L"GlobalAlloc failed for resource ID: " + std::to_wstring(resourceId) + L"\n").c_str());
    }
    
    return nullptr;
}

void LoadImages(ULONG_PTR gdiplusToken) { // Modified signature
    // Load all the emotion images
    g_images[HAPPY] = LoadGdiplusImageFromResource(ID_IMAGE_HAPPY, RT_RCDATA);
    g_images[PLEASED] = LoadGdiplusImageFromResource(ID_IMAGE_PLEASED, RT_RCDATA);
    g_images[NEUTRAL] = LoadGdiplusImageFromResource(ID_IMAGE_NEUTRAL, RT_RCDATA);
    g_images[GRIMACE] = LoadGdiplusImageFromResource(ID_IMAGE_GRIMACE, RT_RCDATA);
    g_images[GRIMACE_TWO_SWEAT] = LoadGdiplusImageFromResource(ID_IMAGE_GRIMACE_TWO_SWEAT, RT_RCDATA);
    g_images[SURPRISED] = LoadGdiplusImageFromResource(ID_IMAGE_SURPRISED, RT_RCDATA);
    g_images[ANGUISH] = LoadGdiplusImageFromResource(ID_IMAGE_ANGUISH, RT_RCDATA);
    g_images[ANGUISH_VERY] = LoadGdiplusImageFromResource(ID_IMAGE_ANGUISH_VERY, RT_RCDATA);
    g_images[ANGUISH_EXTREMELY] = LoadGdiplusImageFromResource(ID_IMAGE_ANGUISH_EXTREMELY, RT_RCDATA);
    g_images[TIRED] = LoadGdiplusImageFromResource(ID_IMAGE_TIRED, RT_RCDATA);
    g_images[TIRED_VERY] = LoadGdiplusImageFromResource(ID_IMAGE_TIRED_VERY, RT_RCDATA);
    g_images[TIRED_EXTREMELY] = LoadGdiplusImageFromResource(ID_IMAGE_TIRED_EXTREMELY, RT_RCDATA);

    // Load all the blink images
    g_blinkImages[HAPPY] = LoadGdiplusImageFromResource(ID_IMAGE_HAPPY_BLINK, RT_RCDATA);
    g_blinkImages[PLEASED] = nullptr; // No blink for pleased
    g_blinkImages[NEUTRAL] = LoadGdiplusImageFromResource(ID_IMAGE_NEUTRAL_BLINK, RT_RCDATA);
    g_blinkImages[GRIMACE] = nullptr; // No blink for grimace
    g_blinkImages[GRIMACE_TWO_SWEAT] = LoadGdiplusImageFromResource(ID_IMAGE_GRIMACE_TWO_SWEAT_BLINK, RT_RCDATA);
    g_blinkImages[SURPRISED] = nullptr; // No blink for surprised
    g_blinkImages[ANGUISH] = LoadGdiplusImageFromResource(ID_IMAGE_ANGUISH_BLINK, RT_RCDATA);
    g_blinkImages[ANGUISH_VERY] = LoadGdiplusImageFromResource(ID_IMAGE_ANGUISH_VERY_BLINK, RT_RCDATA);
    g_blinkImages[ANGUISH_EXTREMELY] = nullptr; // No blink for anguish_extremely
    g_blinkImages[TIRED] = LoadGdiplusImageFromResource(ID_IMAGE_NEUTRAL_BLINK, RT_RCDATA); // Tired uses neutral_blink
    g_blinkImages[TIRED_VERY] = LoadGdiplusImageFromResource(ID_IMAGE_TIRED_VERY_BLINK, RT_RCDATA);
    g_blinkImages[TIRED_EXTREMELY] = nullptr; // No blink for tired_extremely

    // ULONG_PTR token_; // This local uninitialized token was incorrect
    // Critical check for default image
    if (!g_images[HAPPY] || g_images[HAPPY]->GetLastStatus() != Ok) {
        MessageBoxW(NULL, L"Failed to load critical image resources (e.g., HAPPY state). The application cannot continue.", L"Resource Load Error", MB_ICONERROR | MB_OK);
        // Post a quit message or exit directly if this is catastrophic
        // For example, if called from WinMain before message loop, direct exit might be simpler.
        // If in a thread or after message loop started, PostQuitMessage is better.
        // Since this is in WinMain before the loop, we can consider exiting.
        // For now, this message box will at least indicate the problem.
        // To ensure termination if this happens before the message loop:
        GdiplusShutdown(gdiplusToken); // Use the passed token
        exit(1); // Or handle more gracefully
    }


    // Set blink intervals (in seconds)
    for (int i = HAPPY; i <= TIRED_EXTREMELY; i++) {
        g_blinkIntervals[(EmotionalState)i] = 4;
        g_blinkDurations[(EmotionalState)i] = 0.1;
    }
    
    // Special case for tired_very
    g_blinkIntervals[TIRED_VERY] = 6;
    g_blinkDurations[TIRED_VERY] = 0.2;

    // Get first valid image dimensions
    for (auto& pair : g_images) {
        if (pair.second && pair.second->GetLastStatus() == Ok) {
            g_windowWidth = pair.second->GetWidth();
            g_windowHeight = pair.second->GetHeight();
            break;
        }
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        
        // Create an in-memory DC for double buffering
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBitmap = CreateCompatibleBitmap(hdc, g_windowWidth, g_windowHeight);
        HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);
        
        // Fill background with magenta (which will be transparent)
        HBRUSH magentaBrush = CreateSolidBrush(RGB(255, 0, 255));
        RECT rect = { 0, 0, g_windowWidth, g_windowHeight };
        FillRect(memDC, &rect, magentaBrush);
        DeleteObject(magentaBrush);
        
        // Create Graphics object
        Graphics graphics(memDC);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        
        DrawCurrentState();
        
        // Draw the appropriate image
        if (g_isBlinking && g_blinkImages[g_currentState] && g_blinkImages[g_currentState]->GetLastStatus() == Ok) {
            graphics.DrawImage(g_blinkImages[g_currentState], 0, 0);
        } else if (g_images[g_currentState] && g_images[g_currentState]->GetLastStatus() == Ok) {
            graphics.DrawImage(g_images[g_currentState], 0, 0);
        }
        
        // Copy from memory DC to window DC
        BitBlt(hdc, 0, 0, g_windowWidth, g_windowHeight, memDC, 0, 0, SRCCOPY);
        
        // Cleanup
        SelectObject(memDC, oldBitmap);
        DeleteObject(memBitmap);
        DeleteDC(memDC);
        
        EndPaint(hWnd, &ps);
    }
    break;
    
    case WM_DISPLAYCHANGE:
        {
            // Monitor configuration has changed - delay handling to ensure Windows has updated
            std::thread([=]() {
                Sleep(1000); // Increased delay to ensure monitor info is fully updated
                HandleDisplayChange();
            }).detach();
        }
        return 0;
        
    case WM_DEVICECHANGE:
    {
        switch (wParam) {
            case DBT_DEVICEARRIVAL:
            case DBT_DEVICEREMOVECOMPLETE:
            {
                // Device connected or disconnected - show surprised face
                {
                    std::lock_guard<std::mutex> lock(g_stateMutex);
                    g_temporaryState = true;
                    g_temporaryStateStartTime = std::chrono::steady_clock::now();
                    g_currentState = SURPRISED;
                    g_stateChanged = true;
                }
                g_stateCV.notify_one();
                InvalidateRect(g_hwnd, NULL, FALSE);
                
                // For display devices, check if we need to reposition the window
                DEV_BROADCAST_HDR* pHdr = (DEV_BROADCAST_HDR*)lParam;
                
                // Check if this is possibly a display device change
                if (pHdr && (pHdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE || 
                             wParam == DBT_DEVICEARRIVAL)) {
                    // Use a separate thread with longer delay for device connections
                    // to ensure the system has fully recognized the new display
                    std::thread([=]() {
                        // Longer delay for device arrival to ensure display is fully initialized
                        int delay = (wParam == DBT_DEVICEARRIVAL) ? 2000 : 800;
                        Sleep(delay); 
                        HandleDisplayChange();
                    }).detach();
                }
                
                return TRUE;
            }
        }
        break;
    }
    
    case WM_TRAYICON:
        // Handle tray icon messages
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
            // Show context menu on right-click or left-click
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hWnd);
            
            // Create and display popup menu
            HMENU hMenu = CreateContextMenu();
            TrackPopupMenu(hMenu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hWnd, NULL);
            DestroyMenu(hMenu);
        }
        return 0;
    
    case WM_RBUTTONUP:
        // Show the same context menu when right-clicking on the window
        {
            POINT pt;
            GetCursorPos(&pt);
            
            // Create and display popup menu
            HMENU hMenu = CreateContextMenu();
            TrackPopupMenu(hMenu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hWnd, NULL);
            DestroyMenu(hMenu);
        }
        return 0;
    
    case WM_COMMAND:
        // Handle menu commands
        switch (LOWORD(wParam)) {
        case ID_EXIT:
            DestroyWindow(hWnd);
            return 0;
            
        case ID_ALWAYS_ON_TOP:
            // Toggle always on top state
            g_alwaysOnTop = !g_alwaysOnTop;
            
            if (g_alwaysOnTop) {
                SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            } else {
                SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            }
            return 0;
        }
        break;
    
    case WM_DESTROY:
        RemoveFromSystemTray();
        // Destroy the custom tray icon if it was loaded
        if (g_customTrayIcon) {
            DestroyIcon(g_customTrayIcon);
            g_customTrayIcon = NULL; 
        }
        PostQuitMessage(0);
        break;
    
    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    
    return 0;
}

void DrawCurrentState() {
    // This function is called from WM_PAINT
    // The current state is determined in UpdateEmotionalState
    // No need to do anything here since painting happens in WndProc
}

void StartBlinkTimer() {
    using namespace std::chrono;
    
    std::map<EmotionalState, steady_clock::time_point> lastBlinkTimes;
    
    // Initialize all blink times to now
    steady_clock::time_point now = steady_clock::now();
    for (int i = HAPPY; i <= TIRED_EXTREMELY; i++) {
        lastBlinkTimes[(EmotionalState)i] = now;
    }
    
    while (g_hwnd) {
        // Get current time
        now = steady_clock::now();
        
        // Get current state safely
        EmotionalState currentState;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            currentState = g_currentState;
        }
        
        // Check if we have a blink image for this state
        if (g_blinkImages[currentState] != nullptr) {
            // Check if it's time to blink
            auto interval = seconds(g_blinkIntervals[currentState]);
            if (now - lastBlinkTimes[currentState] >= interval) {
                // Time to blink
                g_isBlinking = true;
                
                // Update the window
                InvalidateRect(g_hwnd, NULL, FALSE);
                UpdateWindow(g_hwnd);
                
                // Wait for blink duration
                std::this_thread::sleep_for(duration<double>(g_blinkDurations[currentState]));
                
                // Return to normal
                g_isBlinking = false;
                
                // Update the window
                InvalidateRect(g_hwnd, NULL, FALSE);
                UpdateWindow(g_hwnd);
                
                // Update last blink time
                lastBlinkTimes[currentState] = steady_clock::now();
            }
        }
        
        // Sleep for a short time before checking again
        std::this_thread::sleep_for(milliseconds(50));
    }
}

void MonitorSystem() {
    while (g_hwnd) {
        // Update CPU usage
        g_cpuUsage = GetCPUUsage();
        
        // Update memory usage
        g_memoryUsage = GetMemoryUsage();
        
        // Update battery status
        CheckBatteryStatus();
        
        // Update emotional state based on system metrics
        UpdateEmotionalState();
        
        // Sleep for a short time before checking again
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Process any Windows messages (especially device change notifications)
        ProcessWindowMessages();
    }
}

void UpdateEmotionalState() {
    using namespace std::chrono;
    
    // Lock for thread safety
    std::unique_lock<std::mutex> lock(g_stateMutex);
    
    // First, check if we're in a temporary state
    if (g_temporaryState) {
        auto now = steady_clock::now();
        if (now - g_temporaryStateStartTime >= g_temporaryStateDuration) {
            g_temporaryState = false;
            g_stateChanged = true;
            // Will fall through to determine the real state
        } else {
            // We're still in temporary state, no need to update
            return;
        }
    }
    
    // Track if we were over thresholds before
    bool wasOverThresholdBefore = g_wasAboveThreshold;
    bool isOverThresholdNow = false;
    
    // Determine the new state based on priorities
    EmotionalState newState = HAPPY; // Default state
    
    // Check CPU usage (highest priority)
    if (g_cpuUsage > 90.0) {
        newState = ANGUISH_EXTREMELY;
        isOverThresholdNow = true;
    } else if (g_cpuUsage > 70.0) {
        newState = ANGUISH_VERY;
        isOverThresholdNow = true;
    } else if (g_cpuUsage > 50.0) {
        newState = ANGUISH;
        isOverThresholdNow = true;
    }
    // If CPU is not high, check battery (second priority)
    else if (g_hasBattery && g_batteryPercent < 10) {
        newState = TIRED_EXTREMELY;
        isOverThresholdNow = true;
    } else if (g_hasBattery && g_batteryPercent < 20) {
        newState = TIRED_VERY;
        isOverThresholdNow = true;
    } else if (g_hasBattery && g_batteryPercent < 30) {
        newState = TIRED;
        isOverThresholdNow = true;
    }
    // If CPU and battery are fine, check memory (third priority)
    else if (g_memoryUsage > 95.0) {
        newState = GRIMACE_TWO_SWEAT;
        isOverThresholdNow = true;
    } else if (g_memoryUsage > 90.0) {
        newState = NEUTRAL;
        isOverThresholdNow = true;
    }
    // Special case: if we were over threshold and now we're not, show pleased briefly
    else if (wasOverThresholdBefore && !isOverThresholdNow) {
        newState = PLEASED;
        g_temporaryState = true;
        g_temporaryStateStartTime = steady_clock::now();
    }
    
    // Update threshold tracker
    g_wasAboveThreshold = isOverThresholdNow;
    
    // Update state if it changed
    if (newState != g_currentState) {
        g_currentState = newState;
        g_stateChanged = true;
        
        // Redraw the window
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
    
    // Notify waiting threads if state changed
    if (g_stateChanged) {
        g_stateChanged = false;
        lock.unlock();
        g_stateCV.notify_one();
    }
}

double GetCPUUsage() {
    PDH_FMT_COUNTERVALUE counterVal;
    PdhCollectQueryData(cpuQuery);
    PdhGetFormattedCounterValue(cpuTotal, PDH_FMT_DOUBLE, NULL, &counterVal);
    return counterVal.doubleValue;
}

double GetMemoryUsage() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    
    return memInfo.dwMemoryLoad; // Returns memory load as percentage
}

void CheckBatteryStatus() {
    SYSTEM_POWER_STATUS powerStatus;
    
    if (GetSystemPowerStatus(&powerStatus)) {
        // Check if system has a battery
        g_hasBattery = (powerStatus.BatteryFlag != 128); // 128 means no battery
        
        if (g_hasBattery) {
            g_batteryPercent = powerStatus.BatteryLifePercent;
            if (g_batteryPercent > 100) g_batteryPercent = 100; // Sanitize value
        } else {
            g_batteryPercent = 100; // Default for PCs without battery
        }
    }
}

void ProcessWindowMessages() {
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

// Function to create context menu with consistent options
HMENU CreateContextMenu() {
    HMENU hMenu = CreatePopupMenu();
    
    // Add Always On Top option with checkmark if enabled
    UINT flags = MF_STRING;
    if (g_alwaysOnTop) {
        flags |= MF_CHECKED;
    }
    AppendMenuW(hMenu, flags, ID_ALWAYS_ON_TOP, L"Always On Top");
    
    // Add separator and Exit option
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_EXIT, L"Exit");
    
    return hMenu;
}

// Function to add window to system tray
void AddToSystemTray(HWND hwnd) {
    // Set up the notification icon data structure
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = ID_TRAYICON;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    
    // Load an icon for the system tray from resources.
    // Assumes IDI_APP_ICON is an integer resource ID.
    HICON loadedIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON, 
                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 
                                  LR_DEFAULTCOLOR); // Explicitly use default color loading
    if (loadedIcon) {
        // If we previously loaded a custom icon, destroy it before replacing
        if (g_customTrayIcon) {
            DestroyIcon(g_customTrayIcon);
        }
        g_customTrayIcon = loadedIcon; // Store handle for later destruction
        nid.hIcon = g_customTrayIcon;
    } else {
        // Fallback to default application icon if custom one fails to load
        // No need to manage g_customTrayIcon here as LoadIcon provides a shared icon
        nid.hIcon = LoadIcon(NULL, IDI_APPLICATION); 
    }
    
    // Set tooltip text
    wcscpy_s(nid.szTip, _countof(nid.szTip), L"Emotional Task Manager");
    
    // Add the icon to the system tray
    Shell_NotifyIconW(NIM_ADD, &nid);
    
    // DO NOT destroy the icon handle here (nid.hIcon or g_customTrayIcon).
    // It must persist. It will be destroyed on WM_DESTROY or program exit.
    // The previous DestroyIcon(nid.hIcon) call here was incorrect.
}

// Function to remove window from system tray
void RemoveFromSystemTray() {
    Shell_NotifyIconW(NIM_DELETE, &nid);
    // Actual HICON destruction (if g_customTrayIcon is set) happens
    // in WM_DESTROY or WinMain cleanup.
}

// Initialize Windows event log monitoring
bool InitializeEventLogMonitoring() {
    // Subscribe to all error events
    const LPWSTR pwsQuery = L"*[System[(Level=1 or Level=2)]]"; // Level 1=Critical, 2=Error
    
    g_hSubscription = EvtSubscribe(
        NULL,                           // Session
        NULL,                           // SignalEvent
        L"Application",                 // Channel name (Application log)
        pwsQuery,                       // Query
        NULL,                           // Bookmark
        NULL,                           // Context
        (EVT_SUBSCRIBE_CALLBACK)SubscriptionCallback, // Callback
        EvtSubscribeToFutureEvents      // Flags
    );
    
    return (g_hSubscription != NULL);
}

// Initialize the message-only window for receiving device notifications
bool InitializeDeviceNotifications() {
    // This already happens in the main window creation
    
    // Register for device notifications
    DEV_BROADCAST_DEVICEINTERFACE notificationFilter;
    ZeroMemory(&notificationFilter, sizeof(notificationFilter));
    notificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
    notificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    
    HDEVNOTIFY hDevNotify = RegisterDeviceNotification(
        g_hwnd,
        &notificationFilter,
        DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
        
    return (hDevNotify != NULL);
}

void HandleDisplayChange() {
    // Force a full update of monitor information first
    EnumDisplayMonitors(NULL, NULL, [](HMONITOR hMon, HDC hdcMon, LPRECT lprcMon, LPARAM dwData) -> BOOL {
        return TRUE; // Just enumerate to refresh the system's monitor data
    }, 0);

    // Now reposition the window
    PlaceWindowOnSecondaryMonitor(g_hwnd);
}

void PlaceWindowOnSecondaryMonitor(HWND hwnd) {
    // Get the number of monitors
    int monitorCount = GetSystemMetrics(SM_CMONITORS);
    
    // Handle for monitor being examined
    HMONITOR hMonitor = NULL;
    MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
    
    if (monitorCount > 1) {
        // Multiple monitors detected, try to find the second one
        // Enumerate monitors to find the second one
        struct EnumMonitorsData {
            int currentIndex;
            int targetIndex;
            HMONITOR targetMonitor;
        };
        
        EnumMonitorsData data = { 0, 1, NULL }; // Look for second monitor (index 1)
        
        EnumDisplayMonitors(NULL, NULL, [](HMONITOR hMon, HDC hdcMon, LPRECT lprcMon, LPARAM dwData) -> BOOL {
            EnumMonitorsData* pData = reinterpret_cast<EnumMonitorsData*>(dwData);
            if (pData->currentIndex == pData->targetIndex) {
                pData->targetMonitor = hMon;
                return FALSE; // Stop enumeration
            }
            pData->currentIndex++;
            return TRUE; // Continue enumeration
        }, (LPARAM)&data);
        
        // If we found the second monitor, get its info
        if (data.targetMonitor && GetMonitorInfo(data.targetMonitor, &monitorInfo)) {
            hMonitor = data.targetMonitor;
        } else {
            // Second monitor not found, use the primary monitor
            hMonitor = MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
            GetMonitorInfo(hMonitor, &monitorInfo);
        }
    } else {
        // Only one monitor, use primary
        hMonitor = MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
        GetMonitorInfo(hMonitor, &monitorInfo);
    }
    
    // Get the actual monitor rectangle (which might have changed due to resolution changes)
    RECT monitorRect;
    if (hMonitor) {
        HDC hdc = GetDC(NULL);
        MONITORINFOEX monitorInfoEx = {}; // Zero-initialize
        monitorInfoEx.cbSize = sizeof(MONITORINFOEX); // Set the size member
        if (GetMonitorInfo(hMonitor, &monitorInfoEx)) {
            // Use the work area (excludes taskbar)
            monitorRect = monitorInfoEx.rcWork;
        } else {
            // Fallback to the monitor info we already have
            monitorRect = monitorInfo.rcWork;
        }
        ReleaseDC(NULL, hdc);
    } else {
        // Use the monitor info we already have
        monitorRect = monitorInfo.rcWork;
    }
    
    // Calculate position at the bottom right corner of the selected monitor
    int x = monitorRect.right - g_windowWidth;
    int y = monitorRect.bottom - g_windowHeight;
    
    // Move the window (keep appropriate z-order based on g_alwaysOnTop setting)
    HWND insertAfter = g_alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;
    
    // Set window position with proper flags to ensure it's correctly positioned
    SetWindowPos(hwnd, insertAfter, x, y, g_windowWidth, g_windowHeight, 
                 SWP_SHOWWINDOW | SWP_NOCOPYBITS);
}
