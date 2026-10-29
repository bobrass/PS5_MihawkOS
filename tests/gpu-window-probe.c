/* SPDX-License-Identifier: GPL-3.0-or-later
 * Actual broker-displayed pixels and hardware-queue input, not API success
 * alone. Run under mihawk.exe on Default. Independent GDI HWNDs from PE32 and
 * PE64 deliberately interleave. Captures use the existing numbered broker
 * request; no process image masquerades as desktop background. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>
#include <string.h>
void WINAPI glClearColor(float,float,float,float);
void WINAPI glClear(unsigned);
unsigned WINAPI glGetError(void);
int _fltused;
static unsigned checks,failures,captures;
static HWND gpu_window;
static HDC gpu_dc;
static HGLRC gpu_context;
static COLORREF gpu_colour;
static BOOL use_dxvk,use_readback,observed_dxvk;
static void check(BOOL,const char *);
static IDXGISwapChain *swap;
static ID3D11Device *device;
static ID3D11DeviceContext *context;
static ID3D11RenderTargetView *target;
static unsigned swap_width,swap_height;
static BOOL readback_dxvk(COLORREF colour)
{
    ID3D11Texture2D *back=NULL,*staging=NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT status=IDXGISwapChain_GetBuffer(swap,0,&IID_ID3D11Texture2D,(void **)&back);
    if(FAILED(status))return FALSE;
    ID3D11Texture2D_GetDesc(back,&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    status=ID3D11Device_CreateTexture2D(device,&desc,NULL,&staging);
    if(SUCCEEDED(status)) {
        ID3D11DeviceContext_CopyResource(context,(ID3D11Resource *)staging,(ID3D11Resource *)back);
        status=ID3D11DeviceContext_Map(context,(ID3D11Resource *)staging,0,D3D11_MAP_READ,0,&mapped);
        if(SUCCEEDED(status)) {
            const BYTE *pixel=(const BYTE *)mapped.pData+(desc.Height/2)*mapped.RowPitch+(desc.Width/2)*4;
            char line[256];DWORD written;
            BOOL okay=pixel[0]==GetRValue(colour)&&pixel[1]==GetGValue(colour)&&pixel[2]==GetBValue(colour);
            wsprintfA(line,"window-compose dxvk-readback extent=%ux%u format=%u expected=%u,%u,%u actual=%u,%u,%u\n",
                desc.Width,desc.Height,desc.Format,GetRValue(colour),GetGValue(colour),GetBValue(colour),pixel[0],pixel[1],pixel[2]);
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,lstrlenA(line),&written,NULL);OutputDebugStringA(line);
            ID3D11DeviceContext_Unmap(context,(ID3D11Resource *)staging,0);
            if(!okay)status=E_FAIL;
        }
    }
    if(staging)ID3D11Texture2D_Release(staging);
    ID3D11Texture2D_Release(back);return SUCCEEDED(status);
}
static BOOL resize_dxvk(unsigned width,unsigned height)
{
    ID3D11Texture2D *back=NULL;
    if(target) {
        ID3D11DeviceContext_OMSetRenderTargets(context,0,NULL,NULL);
        ID3D11RenderTargetView_Release(target);target=NULL;
        if(FAILED(IDXGISwapChain_ResizeBuffers(swap,0,width,height,DXGI_FORMAT_UNKNOWN,0)))return FALSE;
    }
    if(FAILED(IDXGISwapChain_GetBuffer(swap,0,&IID_ID3D11Texture2D,(void **)&back)))return FALSE;
    HRESULT status=ID3D11Device_CreateRenderTargetView(device,(ID3D11Resource *)back,NULL,&target);
    ID3D11Texture2D_Release(back);
    if(FAILED(status))return FALSE;
    observed_dxvk=FALSE;swap_width=width;swap_height=height;return TRUE;
}
static BOOL graphics_okay=TRUE;
static void render(void)
{
    if(!IsWindowVisible(gpu_window))return;
    if(use_dxvk) {
        RECT client;GetClientRect(gpu_window,&client);
        unsigned width=client.right-client.left,height=client.bottom-client.top;
        if(!width || !height || !swap)return;
        if((width!=swap_width || height!=swap_height) && !resize_dxvk(width,height)) {graphics_okay=FALSE;return;}
        if(!target){graphics_okay=FALSE;return;}
        const float colour[]={GetRValue(gpu_colour)/255.0f,GetGValue(gpu_colour)/255.0f,GetBValue(gpu_colour)/255.0f,1};
        ID3D11DeviceContext_ClearRenderTargetView(context,target,colour);
        /* Explicit diagnostic only: Map may drain GPU work, so the default
         * presentation gate never uses readback as a synchronization aid. */
        if(use_readback && !observed_dxvk) {
            observed_dxvk=TRUE;check(readback_dxvk(gpu_colour),"Direct3D clear has real render-target pixels before Present");
        }
        if(FAILED(IDXGISwapChain_Present(swap,1,0)))graphics_okay=FALSE;
        return;
    }
    if(!gpu_context)return;
    glClearColor(GetRValue(gpu_colour)/255.0f,GetGValue(gpu_colour)/255.0f,GetBValue(gpu_colour)/255.0f,1.0f);
    glClear(0x4000);
    if(glGetError() || !SwapBuffers(gpu_dc))graphics_okay=FALSE;
}
static const COLORREF blue=RGB(30,110,210),green=RGB(30,180,70),red=RGB(220,50,60),yellow=RGB(240,180,30);
static void check(BOOL okay,const char *what)
{
    char line[256];DWORD written;++checks;if(!okay)++failures;
    wsprintfA(line,"window-compose %s: %s error=%lu\n",okay?"PASS":"FAIL",what,GetLastError());
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,lstrlenA(line),&written,NULL);OutputDebugStringA(line);
}
static void pump(DWORD ms)
{
    DWORD start=GetTickCount();do {MSG m;while(PeekMessageW(&m,NULL,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}render();Sleep(5);}while(GetTickCount()-start<ms);
}
static BOOL write_file(const char *path,const void *data,DWORD bytes)
{
    char tmp[MAX_PATH];DWORD written;wsprintfA(tmp,"%s.tmp",path);
    HANDLE f=CreateFileA(tmp,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);if(f==INVALID_HANDLE_VALUE)return FALSE;
    BOOL okay=WriteFile(f,data,bytes,&written,NULL)&&written==bytes;okay=CloseHandle(f)&&okay;
    return okay&&MoveFileExA(tmp,path,MOVEFILE_REPLACE_EXISTING);
}
static BOOL read_file(const char *path,void *data,DWORD bytes)
{
    DWORD read;HANDLE f=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if(f==INVALID_HANDLE_VALUE)return FALSE;BOOL okay=ReadFile(f,data,bytes,&read,NULL)&&read==bytes;CloseHandle(f);return okay;
}
static LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM w,LPARAM l)
{
    if(msg==WM_PAINT) {PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);HBRUSH brush=CreateSolidBrush((COLORREF)GetWindowLongPtrW(hwnd,GWLP_USERDATA));FillRect(dc,&ps.rcPaint,brush);DeleteObject(brush);EndPaint(hwnd,&ps);return 0;}
    if(msg==WM_LBUTTONDOWN) {SetPropW(hwnd,L"Clicks",(HANDLE)((ULONG_PTR)GetPropW(hwnd,L"Clicks")+1));return 0;}
    if(msg==WM_APP+1)return (LRESULT)GetPropW(hwnd,L"Clicks");
    return DefWindowProcW(hwnd,msg,w,l);
}
static HWND window(COLORREF color,HWND owner,int x,int y,int width,int height)
{
    HWND hwnd=CreateWindowExW(0,L"MihawkWindowCompose",L"Window composition probe",WS_POPUP, x,y,width,height,owner,NULL,GetModuleHandleW(NULL),NULL);
    if(hwnd){SetWindowLongPtrW(hwnd,GWLP_USERDATA,color);ShowWindow(hwnd,SW_SHOW);InvalidateRect(hwnd,NULL,TRUE);UpdateWindow(hwnd);}return hwnd;
}
static BOOL click(HWND hwnd,int x,int y)
{
    DWORD before=(DWORD)SendMessageW(hwnd,WM_APP+1,0,0);INPUT input[3]={{0}};
    input[0].type=input[1].type=input[2].type=INPUT_MOUSE;
    input[0].mi.dx=x*65535/1919;input[0].mi.dy=y*65535/1079;input[0].mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE;
    input[1].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;input[2].mi.dwFlags=MOUSEEVENTF_LEFTUP;
    if(SendInput(3,input,sizeof(INPUT))!=3)return FALSE;
    DWORD start=GetTickCount();while(GetTickCount()-start<3000){pump(10);if((DWORD)SendMessageW(hwnd,WM_APP+1,0,0)==before+1)return TRUE;}return FALSE;
}
struct Sample {int x,y;COLORREF color;};
static BOOL capture(const char *phase,const struct Sample *samples,unsigned count)
{
    const char *base="Z:\\data\\ps5proton\\logs\\game-frame";char request[MAX_PATH],path[MAX_PATH];
    DWORD start=GetTickCount();HANDLE file=INVALID_HANDLE_VALUE;char header[32]={0};DWORD read=0;
    wsprintfA(request,"%s.ppm.broker-request",base);wsprintfA(path,"%s-%02u.ppm",base,++captures);
    /* Keep the real cursor outside sampled rectangles after input checks. */
    POINT pointer={0};
    check(SetCursorPos(1800,100) && GetCursorPos(&pointer) && pointer.x==1800 && pointer.y==100,
          "application cursor position before actual capture");
    {
        COLORREF color=GetSysColor(COLOR_DESKTOP);char line[160];DWORD written;
        wsprintfA(line,"window-compose desktop_colour=%u,%u,%u\n",GetRValue(color),GetGValue(color),GetBValue(color));
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,lstrlenA(line),&written,NULL);OutputDebugStringA(line);
    }
    DeleteFileA(path);pump(100);
    if(!write_file(request,"",0))return FALSE;
    while(GetTickCount()-start<10000) {
        file=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
        if(file!=INVALID_HANDLE_VALUE) {
            if(ReadFile(file,header,18,&read,NULL)&&read>=17) {
                const char *expected="P6\n1920 1080\n255\n";DWORD bytes=lstrlenA(expected);
                if(!memcmp(header,expected,bytes)&&GetFileSize(file,NULL)==bytes+1920*1080*3) {
                    BOOL okay=TRUE;
                    const struct Sample cursor_sample={1801,102,RGB(255,255,255)};
                    for(unsigned i=0;i<count+1;++i) {
                        const struct Sample *sample=i<count ? &samples[i] : &cursor_sample;
                        unsigned char pixel[3];DWORD offset=bytes+(sample->y*1920+sample->x)*3;
                        SetFilePointer(file,offset,NULL,FILE_BEGIN);
                        BOOL match=ReadFile(file,pixel,3,&read,NULL)&&read==3&&
                            pixel[0]==GetRValue(sample->color)&&pixel[1]==GetGValue(sample->color)&&pixel[2]==GetBValue(sample->color);
                        char line[256];DWORD written;wsprintfA(line,"window-compose capture=%u phase=%s sample=%d,%d expected=%u,%u,%u result=%s\n",captures,phase,sample->x,sample->y,GetRValue(sample->color),GetGValue(sample->color),GetBValue(sample->color),match?"PASS":"FAIL");
                        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,lstrlenA(line),&written,NULL);OutputDebugStringA(line);okay=match&&okay;
                    }
                    CloseHandle(file);return okay;
                }
            }
            CloseHandle(file);
        }
        pump(20);
    }
    return FALSE;
}

static BOOL graphics(HWND hwnd,COLORREF colour)
{
    gpu_window=hwnd;gpu_colour=colour;
    if(use_dxvk) {
        RECT client;GetClientRect(hwnd,&client);
        DXGI_SWAP_CHAIN_DESC desc={0};
        desc.BufferDesc.Width=client.right-client.left;desc.BufferDesc.Height=client.bottom-client.top;
        desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
        desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;
        desc.OutputWindow=hwnd;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        if(FAILED(D3D11CreateDeviceAndSwapChain(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,NULL,0,
            D3D11_SDK_VERSION,&desc,&swap,&device,NULL,&context)) ||
            !resize_dxvk(desc.BufferDesc.Width,desc.BufferDesc.Height))return FALSE;
        render();return graphics_okay;
    }
    PIXELFORMATDESCRIPTOR pfd={0};
    pfd.nSize=sizeof(pfd);pfd.nVersion=1;
    pfd.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;
    pfd.iPixelType=PFD_TYPE_RGBA;pfd.cColorBits=32;
    gpu_window=hwnd;gpu_colour=colour;gpu_dc=GetDC(hwnd);
    int format=gpu_dc?ChoosePixelFormat(gpu_dc,&pfd):0;
    if(!format || !SetPixelFormat(gpu_dc,format,&pfd) || !(gpu_context=wglCreateContext(gpu_dc)) ||
       !wglMakeCurrent(gpu_dc,gpu_context))return FALSE;
    render();return graphics_okay;
}
static void graphics_close(void)
{
    if(context) {ID3D11DeviceContext_ClearState(context);ID3D11DeviceContext_Flush(context);}
    if(target){ID3D11RenderTargetView_Release(target);target=NULL;}
    if(swap){IDXGISwapChain_Release(swap);swap=NULL;}
    if(context){ID3D11DeviceContext_Release(context);context=NULL;}
    if(device){ID3D11Device_Release(device);device=NULL;}
    if(gpu_context){wglMakeCurrent(NULL,NULL);wglDeleteContext(gpu_context);gpu_context=NULL;}
    if(gpu_dc){ReleaseDC(gpu_window,gpu_dc);gpu_dc=NULL;}
}
static unsigned parent_argument(void)
{
    const WCHAR *p=wcsstr(GetCommandLineW(),L"--child ");unsigned value=0;
    if(!p)return 0;p+=8;
    while(*p) {
        unsigned digit=(*p>=L'0'&&*p<=L'9')?*p-L'0':(*p>=L'a'&&*p<=L'f')?*p-L'a'+10:16;
        if(digit>15)break;value=value*16+digit;++p;
    }
    return value;
}
static struct Sample at(HWND hwnd,int x,int y,COLORREF colour)
{
    POINT point={x,y};ClientToScreen(hwnd,&point);
    return (struct Sample){point.x,point.y,colour};
}
static void observe_control(HWND control,const char *phase)
{
    HDC dc=GetDC(control);COLORREF pixel=dc?GetPixel(dc,30,30):CLR_INVALID;
    RECT clip={0};int region=dc?GetClipBox(dc,&clip):ERROR;
    char line[256];DWORD written;
    wsprintfA(line,"window-compose gdi-backing phase=%s pixel=%#lx clip=%d,%ld,%ld,%ld,%ld previous=%#x visible=%u\n",
        phase,pixel,region,clip.left,clip.top,clip.right,clip.bottom,
        (UINT)(UINT_PTR)GetWindow(control,GW_HWNDPREV),IsWindowVisible(control));
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,lstrlenA(line),&written,NULL);OutputDebugStringA(line);
    if(dc)ReleaseDC(control,dc);
    check(pixel==yellow,"GDI child has real painted backing beside GPU");
}
void mainCRTStartup(void)
{
    use_dxvk=wcsstr(GetCommandLineW(),L"--dxvk")!=NULL;
    use_readback=wcsstr(GetCommandLineW(),L"--readback")!=NULL;
    WNDCLASSW type={0};type.hInstance=GetModuleHandleW(NULL);type.lpfnWndProc=proc;
    type.lpszClassName=L"MihawkWindowCompose";type.style=CS_OWNDC;
    if(!RegisterClassW(&type))ExitProcess(2);
    unsigned parent=parent_argument();
    if(parent) {
        /* --into: present into the parent's window itself, which this process does not own, as Chromium's GPU
         * process presents into the browser's compositor window (2026-10-10; record removed from the tree). */
        const BOOL into=wcsstr(GetCommandLineW(),L"--into")!=NULL;
        HWND h=into ? (HWND)(ULONG_PTR)parent :
            CreateWindowExW(0,type.lpszClassName,L"Nested GPU window",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,
                            40,40,450,350,(HWND)(ULONG_PTR)parent,NULL,type.hInstance,NULL);
        uint32_t identity=(uint32_t)(ULONG_PTR)h;
        if(!h||!graphics(h,green)||!write_file("C:\\.pw-compose-ready",&identity,sizeof(identity)))ExitProcess(3);
        DWORD start=GetTickCount();while(GetFileAttributesA("C:\\.pw-compose-stop")==INVALID_FILE_ATTRIBUTES&&GetTickCount()-start<90000)pump(10);
        graphics_close();if(!into)DestroyWindow(h);ExitProcess(graphics_okay?0:4);
    }
    DeleteFileA("C:\\.pw-compose-ready");DeleteFileA("C:\\.pw-compose-stop");
    DWORD start=GetTickCount();while(!IsWindow(GetShellWindow())&&GetTickCount()-start<5000)pump(10);
    HWND shell=GetShellWindow();check(IsWindow(shell),"persistent shell ready");
    HWND root=CreateWindowExW(0,type.lpszClassName,L"GPU window composition probe",WS_OVERLAPPEDWINDOW|WS_VISIBLE|WS_CLIPCHILDREN,
                             200,150,760,580,NULL,NULL,type.hInstance,NULL);
    check(root&&graphics(root,red),use_dxvk?"framed top-level Direct3D through DXVK":"framed top-level OpenGL through Zink");
    STARTUPINFOW startup={0};PROCESS_INFORMATION child={0};startup.cb=sizeof(startup);WCHAR command[256];
    wsprintfW(command,sizeof(void*)==8?L"C:\\gpu-window32-probe.exe --child %x %s %s":L"C:\\gpu-window64-probe.exe --child %x %s %s",(UINT)(UINT_PTR)root,use_dxvk?L"--dxvk":L"",use_readback?L"--readback":L"");
    BOOL started=CreateProcessW(NULL,command,NULL,NULL,FALSE,0,NULL,NULL,&startup,&child);
    check(started,"opposite architecture GPU process");
    uint32_t identity=0;start=GetTickCount();while(!read_file("C:\\.pw-compose-ready",&identity,sizeof(identity))&&GetTickCount()-start<20000)pump(10);
    HWND other=(HWND)(ULONG_PTR)identity;
    check(other&&IsWindow(other)&&GetParent(other)==root,"nested cross-process graphics HWND");
    HWND control=CreateWindowExW(0,type.lpszClassName,L"GDI control",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,
        400,100,200,100,root,NULL,type.hInstance,NULL);
    SetWindowLongPtrW(control,GWLP_USERDATA,yellow);
    /* New child windows do not promise topmost sibling placement. Establish
     * the intended ordering through the same Win32 API applications use. */
    check(control && SetWindowPos(control,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE) &&
          !GetWindow(control,GW_HWNDPREV),"place GDI control above graphics sibling");
    InvalidateRect(control,NULL,TRUE);UpdateWindow(control);pump(100);
    observe_control(control,"initial");
    if(other&&root&&control) {
        struct Sample first[]={at(root,550,220,red),at(root,100,250,green),at(root,430,130,yellow)};
        check(capture("gpu-controls",first,3),"actual GPU image and GDI sibling clipping");
        check(click(other,first[1].x,first[1].y),"nested GPU child hardware mouse");
        HWND modal=window(blue,root,first[1].x-50,first[1].y-50,180,120);
        EnableWindow(root,FALSE);SetForegroundWindow(modal);pump(100);
        struct Sample dialog[]={first[0],{first[1].x,first[1].y,blue},first[2]};
        check(capture("gpu-modal",dialog,3),"GDI modal over retained GPU child");
        check(click(modal,first[1].x,first[1].y),"modal hardware mouse");
        DestroyWindow(modal);EnableWindow(root,TRUE);
        /* A later colour must replace the first frame without any readback. */
        gpu_colour=blue;
        SetWindowPos(root,HWND_TOP,100,100,900,650,0);ShowWindow(other,SW_HIDE);pump(100);
        observe_control(control,"resize-hidden");
        struct Sample hidden[]={at(root,100,250,blue),at(root,430,130,yellow)};
        check(capture("gpu-resize-hidden",hidden,2),"resize parent and hide nested GPU child");
        ShowWindow(other,SW_SHOW);SetForegroundWindow(root);pump(100);
        struct Sample restored[]={at(root,550,220,blue),at(root,100,250,green),at(root,430,130,yellow)};
        check(capture("gpu-restored",restored,3),"restore GPU child at moved parent coordinates");
        check(click(control,restored[2].x,restored[2].y),"GDI control accepts hardware mouse beside GPU");
    }
    write_file("C:\\.pw-compose-stop","",0);DWORD wait=WAIT_FAILED,code=1;
    /* Closing a cross-process child can send synchronous messages to its
     * parent. Keep this window thread dispatching until the process exits. */
    if(started) {
        start=GetTickCount();
        do {pump(10);wait=WaitForSingleObject(child.hProcess,0);}
        while(wait==WAIT_TIMEOUT && GetTickCount()-start<15000);
    }
    BOOL clean=wait==WAIT_OBJECT_0&&GetExitCodeProcess(child.hProcess,&code)&&!code;
    check(clean,"graphics child exits normally");
    if(clean&&IsWindow(root)) {
        gpu_colour=red;pump(100);struct Sample ended[]={at(root,100,250,red),at(root,430,130,yellow)};
        check(capture("gpu-child-exited",ended,2),"owner GPU and GDI survive child teardown");
    }
    if(clean&&IsWindow(root)) {
        /* Chromium's shape: this process owns a plain child window and another process presents into it. */
        DeleteFileA("C:\\.pw-compose-ready");DeleteFileA("C:\\.pw-compose-stop");
        HWND host=CreateWindowExW(0,type.lpszClassName,L"Compositor window",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,
                                  60,320,300,180,root,NULL,type.hInstance,NULL);
        if(host){SetWindowLongPtrW(host,GWLP_USERDATA,0);InvalidateRect(host,NULL,TRUE);UpdateWindow(host);}
        PROCESS_INFORMATION presenter={0};STARTUPINFOW info={0};info.cb=sizeof(info);
        wsprintfW(command,sizeof(void*)==8?L"C:\\gpu-window32-probe.exe --child %x --into %s %s":L"C:\\gpu-window64-probe.exe --child %x --into %s %s",(UINT)(UINT_PTR)host,use_dxvk?L"--dxvk":L"",use_readback?L"--readback":L"");
        BOOL launched=host&&CreateProcessW(NULL,command,NULL,NULL,FALSE,0,NULL,NULL,&info,&presenter);
        check(launched,"presenter process for a window it does not own");
        identity=0;start=GetTickCount();while(launched&&!read_file("C:\\.pw-compose-ready",&identity,sizeof(identity))&&GetTickCount()-start<20000)pump(10);
        check(identity&&(HWND)(ULONG_PTR)identity==host,"presenter bound the owner's window");
        if(identity) {
            pump(200);
            struct Sample into[]={at(host,150,90,green),at(root,430,130,yellow)};
            check(capture("gpu-into-owner",into,2),"another process's GPU image in the owner's window");
        }
        write_file("C:\\.pw-compose-stop","",0);DWORD done=WAIT_FAILED,result=1;
        if(launched) {
            start=GetTickCount();
            do {pump(10);done=WaitForSingleObject(presenter.hProcess,0);}
            while(done==WAIT_TIMEOUT && GetTickCount()-start<15000);
            check(done==WAIT_OBJECT_0&&GetExitCodeProcess(presenter.hProcess,&result)&&!result,"presenter exits normally");
            CloseHandle(presenter.hThread);CloseHandle(presenter.hProcess);
        }
        check(host&&IsWindow(host),"the owner's window survives its presenter");
        if(host)DestroyWindow(host);
    }
    if(IsWindow(root)) {
        /* Steam's restored window (2026-10-27): frameless, its client area two pixels past the 1920x1080 screen. Windows
         * makes that swapchain and shows the part on screen; the broker refused it and the screen stayed black. */
        gpu_colour=green;
        SetWindowLongW(root,GWL_STYLE,(GetWindowLongW(root,GWL_STYLE)&~WS_OVERLAPPEDWINDOW)|WS_POPUP);
        SetWindowPos(root,HWND_TOP,-1,-1,1922,1081,SWP_FRAMECHANGED);pump(200);
        RECT client={0};GetClientRect(root,&client);
        check(client.right==1922&&client.bottom==1081,"client area larger than the screen");
        struct Sample oversize[]={{4,540,green},{1915,600,green},at(root,430,130,yellow)};
        check(capture("gpu-oversize",oversize,3),"GPU window larger than the screen, shown clipped");
    }
    check(graphics_okay,"every parent presentation completed without graphics error");
    check(IsWindow(shell),"shell survives graphics child exit");
    graphics_close();if(root)DestroyWindow(root);
    if(started){CloseHandle(child.hThread);CloseHandle(child.hProcess);}
    DeleteFileA("C:\\.pw-compose-ready");DeleteFileA("C:\\.pw-compose-stop");
    char line[256];DWORD written;wsprintfA(line,"PS5_PROTON_GPU_WINDOW %s checks=%u failures=%u captures=%u\n",failures?"FAIL":"PASS",checks,failures,captures);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,lstrlenA(line),&written,NULL);OutputDebugStringA(line);
    if(clean&&IsWindow(shell))PostMessageW(shell,WM_CLOSE,0,0);
    ExitProcess(failures?1:0);
}
