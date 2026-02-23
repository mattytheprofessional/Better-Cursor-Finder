//  cursor_ring.cpp  –  Better Cursor Finder (BCF)  v2.0
//  @mattytheprofessional

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#ifndef PROPID
  typedef ULONG PROPID;
#endif
#include <gdiplus.h>
#include <cmath>
#include <algorithm>
#include <string>
#include <cstdio>

#pragma comment(lib,"user32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"shell32.lib")

using namespace Gdiplus;
#ifndef M_PI
#define M_PI 3.14159265358979f
#endif

//  SETTINGS
struct AppSettings {
    COLORREF ringColor    = RGB(255,255,255);
    COLORREF outlineColor = RGB(0,0,0);
    int      speed        = 1;       // 0=slow 1=normal 2=fast
    bool     moveCancel   = true;
    bool     darkMode     = true;
    bool     startOnBoot  = false;
};
static AppSettings g_cfg;

//  GLOBALS
static HWND  g_hwndOverlay  = nullptr;
static HWND  g_hwndSettings = nullptr;
static bool  g_animating     = false;
static bool  g_ctrlWas       = false;
static bool  g_comboDetected = false;
static DWORD g_startTime     = 0;
static POINT g_cursor        = {};
static POINT g_animStart     = {};
static bool  g_preExisting[256] = {};
static NOTIFYICONDATA g_nid  = {};
static bool  g_settingsOpen  = false;
static HICON g_hBCFIcon      = nullptr;

static const float ANIM_MAX_R = 88.0f;
static const float ANIM_MIN_R = 3.0f;
static const int   OV_SIZE    = 240;
static const float STROKE_W   = 2.5f;
static const int   MOVE_THR   = 4;
static const int   SW_W       = 340;
static const int   SW_H       = 502;
static const UINT  WM_TRAY    = WM_APP + 1;
static const UINT  TRAY_ID    = 1;


static RECT g_rcTheme;
static RECT g_rcRing, g_rcOutline;
static RECT g_rcSlow, g_rcNorm, g_rcFast;
static RECT g_rcMove, g_rcBoot;
static RECT g_rcGithub;

//  COLOR PICKER STATE
static const int CP_W = 300;
static const int CP_H = 388;

struct CPState {
    HWND     hwnd         = nullptr;
    HWND     hwndEdit     = nullptr;
    float    hue          = 0;
    float    sat          = 1;
    float    val          = 1;
    COLORREF orig         = 0;
    COLORREF*target       = nullptr;
    bool     draggingSV   = false;
    bool     draggingHue  = false;
    bool     suppressEdit = false;
    RECT     rcSV, rcHue, rcOld, rcNew, rcOK, rcCancel;
};
static CPState g_cp;

//  HSV ↔ RGB
static inline float Clamp01(float v){return v<0?0:v>1?1:v;}

static COLORREF HSVtoRGB(float h,float s,float v)
{
    if(s<=0){int c=(int)(v*255+.5f);return RGB(c,c,c);}
    h=fmodf(h,360.0f); if(h<0)h+=360.0f; h/=60.0f;
    int i=(int)h; float f=h-i;
    float p=v*(1-s),q=v*(1-s*f),t=v*(1-s*(1-f));
    float r,g,b;
    switch(i){
        case 0:r=v;g=t;b=p;break; case 1:r=q;g=v;b=p;break;
        case 2:r=p;g=v;b=t;break; case 3:r=p;g=q;b=v;break;
        case 4:r=t;g=p;b=v;break; default:r=v;g=p;b=q;break;
    }
    return RGB((int)(r*255+.5f),(int)(g*255+.5f),(int)(b*255+.5f));
}

static void RGBtoHSV(COLORREF c,float&h,float&s,float&v)
{
    float r=GetRValue(c)/255.f,g=GetGValue(c)/255.f,b=GetBValue(c)/255.f;
    float mx=std::max({r,g,b}),mn=std::min({r,g,b});
    v=mx; float d=mx-mn;
    s=(mx<1e-6f)?0:d/mx;
    if(s<1e-6f){h=0;return;}
    if(mx==r)h=60*(g-b)/d; else if(mx==g)h=60*(b-r)/d+120; else h=60*(r-g)/d+240;
    if(h<0)h+=360;
}

//  REGISTRY
static void LoadSettings()
{
    HKEY k; if(RegOpenKeyExA(HKEY_CURRENT_USER,"Software\\CursorFinder",0,KEY_READ,&k)) return;
    DWORD sz=4,v;
#define RD(n,f) sz=4;if(!RegQueryValueExA(k,n,0,0,(BYTE*)&v,&sz))f=(decltype(f))v;
    RD("RingColor",g_cfg.ringColor) RD("OutlineColor",g_cfg.outlineColor)
    RD("Speed",g_cfg.speed) RD("MoveCancel",g_cfg.moveCancel)
    RD("DarkMode",g_cfg.darkMode) RD("StartOnBoot",g_cfg.startOnBoot)
#undef RD
    RegCloseKey(k);
}
static void SaveSettings()
{
    HKEY k; RegCreateKeyExA(HKEY_CURRENT_USER,"Software\\CursorFinder",0,NULL,0,KEY_WRITE,NULL,&k,NULL);
#define WD(n,x){DWORD _v=(DWORD)(x);RegSetValueExA(k,n,0,REG_DWORD,(BYTE*)&_v,4);}
    WD("RingColor",g_cfg.ringColor) WD("OutlineColor",g_cfg.outlineColor)
    WD("Speed",g_cfg.speed) WD("MoveCancel",g_cfg.moveCancel)
    WD("DarkMode",g_cfg.darkMode) WD("StartOnBoot",g_cfg.startOnBoot)
#undef WD
    RegCloseKey(k);
}
static void ApplyStartup(bool on)
{
    HKEY k; RegOpenKeyExA(HKEY_CURRENT_USER,"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,KEY_WRITE,&k);
    if(on){char p[MAX_PATH];GetModuleFileNameA(NULL,p,MAX_PATH);
           std::string v="\""+std::string(p)+"\"";
           RegSetValueExA(k,"CursorFinder",0,REG_SZ,(BYTE*)v.c_str(),(DWORD)v.size()+1);}
    else RegDeleteValueA(k,"CursorFinder");
    RegCloseKey(k);
}

//  HELPERS
static inline float EaseOutQuint(float t){return 1.f-powf(1.f-Clamp01(t),5.f);}
static float GetDuration(){switch(g_cfg.speed){case 0:return 1600;case 2:return 560;default:return 1050;}}
static Color CR(COLORREF c,BYTE a=255){return Color(a,GetRValue(c),GetGValue(c),GetBValue(c));}

static void BuildRR(GraphicsPath&p,float x,float y,float w,float h,float r){
    p.AddArc(x,y,r*2,r*2,180,90); p.AddArc(x+w-r*2,y,r*2,r*2,270,90);
    p.AddArc(x+w-r*2,y+h-r*2,r*2,r*2,0,90); p.AddArc(x,y+h-r*2,r*2,r*2,90,90);
    p.CloseFigure();
}
static void FillRR(Graphics&g,Color c,float x,float y,float w,float h,float r)
    {GraphicsPath p;BuildRR(p,x,y,w,h,r);SolidBrush b(c);g.FillPath(&b,&p);}
static void DrawRR(Graphics&g,Color c,float lw,float x,float y,float w,float h,float r)
    {GraphicsPath p;BuildRR(p,x,y,w,h,r);Pen pen(c,lw);g.DrawPath(&pen,&p);}

static void DrawSun(Graphics&g,float cx,float cy,float sz,Color col){
    float r=sz*.46f; SolidBrush fb(col); g.FillEllipse(&fb,cx-r*.52f,cy-r*.52f,r*1.04f,r*1.04f);
    Pen p(col,r*.13f); p.SetLineCap(LineCapRound,LineCapRound,DashCapRound);
    for(int i=0;i<8;i++){float a=i*(float)M_PI/4;
        g.DrawLine(&p,cx+r*.64f*cosf(a),cy+r*.64f*sinf(a),cx+r*.96f*cosf(a),cy+r*.96f*sinf(a));}
}
static void DrawMoon(Graphics&g,float cx,float cy,float sz,Color mc,Color bc){
    float r=sz*.46f; SolidBrush mb(mc),bb(bc);
    g.FillEllipse(&mb,cx-r,cy-r,r*2,r*2); g.FillEllipse(&bb,cx-r*.18f,cy-r,r*1.9f,r*1.85f);
}
static void DrawToggle(Graphics&g,float x,float y,bool on,Color onC,Color offC){
    const float w=44,h=24; FillRR(g,on?onC:offC,x,y,w,h,h/2);
    SolidBrush wb(Color(255,255,255,255));
    float dx=on?x+w-h+3:x+3; g.FillEllipse(&wb,dx,y+3,h-6,h-6);
}

//  THEME
struct TC{Color bg,hdrBg,text,sub,sep,accent,togOff,border,cardBg;};
static TC GetTC(){
    if(g_cfg.darkMode)return{
        Color(255,20,20,32),Color(255,26,26,42),Color(255,228,228,240),
        Color(255,120,120,148),Color(255,38,38,58),Color(255,72,148,255),
        Color(255,55,55,78),Color(255,45,45,68),Color(255,28,28,44)};
    return{
        Color(255,244,245,252),Color(255,232,234,248),Color(255,24,24,40),
        Color(255,108,108,132),Color(255,212,214,232),Color(255,52,120,247),
        Color(255,176,176,198),Color(255,205,208,228),Color(255,255,255,255)};
}

//  BCF ICON
static HICON CreateBCFIcon()
{
    const int SZ = 32;

    BITMAPV5HEADER bh = {};
    bh.bV5Size        = sizeof(bh);
    bh.bV5Width       = SZ;
    bh.bV5Height      = -SZ;   
    bh.bV5Planes      = 1;
    bh.bV5BitCount    = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask     = 0x00FF0000;
    bh.bV5GreenMask   = 0x0000FF00;
    bh.bV5BlueMask    = 0x000000FF;
    bh.bV5AlphaMask   = 0xFF000000;

    void*   pvBits = nullptr;
    HDC     hdcS   = GetDC(NULL);
    HDC     hdcM   = CreateCompatibleDC(hdcS);
    HBITMAP hBmp   = CreateDIBSection(hdcM,(BITMAPINFO*)&bh,DIB_RGB_COLORS,&pvBits,NULL,0);
    HBITMAP hOld   = (HBITMAP)SelectObject(hdcM,hBmp);

    memset(pvBits, 0, SZ*SZ*4);

    {
        Graphics g(hdcM);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.SetCompositingMode(CompositingModeSourceOver);

        float m = 1.0f;
        float D = SZ - m*2;

        SolidBrush bgB(Color(255,18,18,30));
        g.FillEllipse(&bgB, m, m, D, D);

        Pen ring(Color(255,72,148,255), 1.8f);
        g.DrawEllipse(&ring, m+0.9f, m+0.9f, D-1.8f, D-1.8f);

        FontFamily ff(L"Arial");
        Font font(&ff, 9, FontStyleBold, UnitPoint);
        SolidBrush white(Color(255,240,240,255));
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(L"BCF", -1, &font, RectF(m, m, D, D), &sf, &white);
    }

    HBITMAP hMask = CreateBitmap(SZ, SZ, 1, 1, NULL);
    {
        HDC     hm  = CreateCompatibleDC(NULL);
        HBITMAP ho  = (HBITMAP)SelectObject(hm, hMask);
        PatBlt(hm, 0, 0, SZ, SZ, WHITENESS);
        HBRUSH hbr = CreateSolidBrush(RGB(0,0,0));
        HBRUSH hob = (HBRUSH)SelectObject(hm, hbr);
        Ellipse(hm, 0, 0, SZ, SZ);
        SelectObject(hm, hob); DeleteObject(hbr);
        SelectObject(hm, ho); DeleteDC(hm);
    }

    ICONINFO ii = {}; ii.fIcon = TRUE; ii.hbmColor = hBmp; ii.hbmMask = hMask;
    HICON hIcon = CreateIconIndirect(&ii);
    SelectObject(hdcM, hOld); DeleteObject(hBmp); DeleteObject(hMask);
    DeleteDC(hdcM); ReleaseDC(NULL, hdcS);
    return hIcon;
}

//  COLOR PICKER — helpers
static void CP_UpdateHexEdit()
{
    char buf[8]; COLORREF c=HSVtoRGB(g_cp.hue,g_cp.sat,g_cp.val);
    sprintf(buf,"%02X%02X%02X",GetRValue(c),GetGValue(c),GetBValue(c));
    g_cp.suppressEdit=true; SetWindowTextA(g_cp.hwndEdit,buf); g_cp.suppressEdit=false;
}
static void CP_UpdateSV(POINT pt){
    float w=(float)(g_cp.rcSV.right-g_cp.rcSV.left);
    float h=(float)(g_cp.rcSV.bottom-g_cp.rcSV.top);
    g_cp.sat=Clamp01((pt.x-g_cp.rcSV.left)/(w-1));
    g_cp.val=1.f-Clamp01((pt.y-g_cp.rcSV.top)/(h-1));
    CP_UpdateHexEdit(); InvalidateRect(g_cp.hwnd,NULL,FALSE);
}
static void CP_UpdateHue(POINT pt){
    float w=(float)(g_cp.rcHue.right-g_cp.rcHue.left);
    g_cp.hue=Clamp01((pt.x-g_cp.rcHue.left)/(w-1))*360.f;
    CP_UpdateHexEdit(); InvalidateRect(g_cp.hwnd,NULL,FALSE);
}

//  COLOR PICKER — render
static void RenderCP(HDC hdc)
{
    HDC memDC=CreateCompatibleDC(hdc);
    HBITMAP bmp=CreateCompatibleBitmap(hdc,CP_W,CP_H);
    HBITMAP old=(HBITMAP)SelectObject(memDC,bmp);

    Graphics g(memDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // Background
    SolidBrush bg(Color(255,22,22,36)); g.FillRectangle(&bg,0,0,CP_W,CP_H);

    // SV Square
    {
        int w=g_cp.rcSV.right-g_cp.rcSV.left, h=g_cp.rcSV.bottom-g_cp.rcSV.top;
        Bitmap svBmp(w,h,PixelFormat32bppARGB);
        BitmapData bd; Rect rect(0,0,w,h);
        svBmp.LockBits(&rect,ImageLockModeWrite,PixelFormat32bppARGB,&bd);
        BYTE*px=(BYTE*)bd.Scan0;
        for(int py=0;py<h;py++) for(int px2=0;px2<w;px2++){
            COLORREF c=HSVtoRGB(g_cp.hue,(float)px2/(w-1),1.f-(float)py/(h-1));
            int idx=py*bd.Stride+px2*4;
            px[idx]=GetBValue(c);px[idx+1]=GetGValue(c);px[idx+2]=GetRValue(c);px[idx+3]=255;
        }
        svBmp.UnlockBits(&bd);
        g.DrawImage(&svBmp,(float)g_cp.rcSV.left,(float)g_cp.rcSV.top,(float)w,(float)h);

        Pen border(Color(255,60,60,80),1.f);
        g.DrawRectangle(&border,(float)g_cp.rcSV.left,(float)g_cp.rcSV.top,(float)w,(float)h);
   
        float sx=g_cp.rcSV.left+g_cp.sat*(w-1);
        float sy=g_cp.rcSV.top+(1.f-g_cp.val)*(h-1);
        Pen po(Color(255,0,0,0),2.f);   g.DrawEllipse(&po,sx-8.f,sy-8.f,16.f,16.f);
        Pen pi(Color(255,255,255,255),2.f); g.DrawEllipse(&pi,sx-6.5f,sy-6.5f,13.f,13.f);
    }

    // Hue Bar
    {
        int w=g_cp.rcHue.right-g_cp.rcHue.left, h=g_cp.rcHue.bottom-g_cp.rcHue.top;
        Bitmap hueBmp(w,h,PixelFormat32bppARGB);
        BitmapData bd; Rect rect(0,0,w,h);
        hueBmp.LockBits(&rect,ImageLockModeWrite,PixelFormat32bppARGB,&bd);
        BYTE*px=(BYTE*)bd.Scan0;
        for(int py=0;py<h;py++) for(int px2=0;px2<w;px2++){
            COLORREF c=HSVtoRGB((float)px2/(w-1)*360.f,1,1);
            int idx=py*bd.Stride+px2*4;
            px[idx]=GetBValue(c);px[idx+1]=GetGValue(c);px[idx+2]=GetRValue(c);px[idx+3]=255;
        }
        hueBmp.UnlockBits(&bd);
        g.DrawImage(&hueBmp,(float)g_cp.rcHue.left,(float)g_cp.rcHue.top,(float)w,(float)h);
        Pen border(Color(255,60,60,80),1.f);
        g.DrawRectangle(&border,(float)g_cp.rcHue.left,(float)g_cp.rcHue.top,(float)w,(float)h);
        // Selector
        float sx=g_cp.rcHue.left+(g_cp.hue/360.f)*(w-1);
        Pen sw(Color(255,255,255,255),2.f);
        g.DrawLine(&sw,sx,(float)g_cp.rcHue.top-2,sx,(float)g_cp.rcHue.bottom+2);
        Pen sb(Color(255,0,0,0),1.f);
        g.DrawLine(&sb,sx-1.5f,(float)g_cp.rcHue.top-2,sx-1.5f,(float)g_cp.rcHue.bottom+2);
        g.DrawLine(&sb,sx+1.5f,(float)g_cp.rcHue.top-2,sx+1.5f,(float)g_cp.rcHue.bottom+2);
    }

    // Preview Labels + Swatches
    {
        FontFamily ff(L"Segoe UI");
        Font fSub(&ff,8,FontStyleRegular,UnitPoint);
        Font fHex(&ff,8,FontStyleBold,UnitPoint);
        SolidBrush subB(Color(255,120,120,160));
        SolidBrush hexB(Color(255,150,170,210));

        float ow=(float)(g_cp.rcOld.right-g_cp.rcOld.left);
        float oh=(float)(g_cp.rcOld.bottom-g_cp.rcOld.top);
        float nw=(float)(g_cp.rcNew.right-g_cp.rcNew.left);


        g.DrawString(L"Original",-1,&fSub,PointF((float)g_cp.rcOld.left,(float)g_cp.rcOld.top-14),&subB);
        g.DrawString(L"New",     -1,&fSub,PointF((float)g_cp.rcNew.left,(float)g_cp.rcNew.top-14),&subB);

        
        COLORREF origC=g_cp.orig;
        FillRR(g,CR(origC),(float)g_cp.rcOld.left,(float)g_cp.rcOld.top,ow,oh,6);
        DrawRR(g,Color(255,60,60,80),1.f,(float)g_cp.rcOld.left,(float)g_cp.rcOld.top,ow,oh,6);

        
        COLORREF newC=HSVtoRGB(g_cp.hue,g_cp.sat,g_cp.val);
        FillRR(g,CR(newC),(float)g_cp.rcNew.left,(float)g_cp.rcNew.top,nw,oh,6);
        DrawRR(g,Color(255,60,60,80),1.f,(float)g_cp.rcNew.left,(float)g_cp.rcNew.top,nw,oh,6);

        
        char origHex[8]; sprintf(origHex,"#%02X%02X%02X",GetRValue(origC),GetGValue(origC),GetBValue(origC));
        wchar_t wOrigHex[8]; MultiByteToWideChar(CP_ACP,0,origHex,-1,wOrigHex,8);
        StringFormat sfHex; sfHex.SetAlignment(StringAlignmentCenter);
        float hexY=(float)g_cp.rcOld.bottom+5;
        g.DrawString(wOrigHex,-1,&fHex, RectF((float)g_cp.rcOld.left,hexY,ow,14),&sfHex,&hexB);

        
        char newHex[8]; sprintf(newHex,"#%02X%02X%02X",GetRValue(newC),GetGValue(newC),GetBValue(newC));
        wchar_t wNewHex[8]; MultiByteToWideChar(CP_ACP,0,newHex,-1,wNewHex,8);
        g.DrawString(wNewHex,-1,&fHex, RectF((float)g_cp.rcNew.left,hexY,nw,14),&sfHex,&hexB);
    }

    
    {
        FillRR(g,Color(255,35,35,55), 10.f,303.f,280.f,28.f,8);
        DrawRR(g,Color(255,72,148,255),1.5f,10.f,303.f,280.f,28.f,8);
        FontFamily ff2(L"Segoe UI"); Font fN(&ff2,10,FontStyleBold,UnitPoint);
        SolidBrush tb(Color(255,72,148,255));
        g.DrawString(L"#",-1,&fN,PointF(18.f,307.f),&tb);
    }

    
    {
        FontFamily ff(L"Segoe UI"); Font fB(&ff,10,FontStyleBold,UnitPoint);
        StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
        FillRR(g,Color(255,72,148,255),(float)g_cp.rcOK.left,(float)g_cp.rcOK.top,
               (float)(g_cp.rcOK.right-g_cp.rcOK.left),(float)(g_cp.rcOK.bottom-g_cp.rcOK.top),6);
        SolidBrush wb(Color(255,255,255,255));
        g.DrawString(L"OK",-1,&fB,RectF((float)g_cp.rcOK.left,(float)g_cp.rcOK.top,
               (float)(g_cp.rcOK.right-g_cp.rcOK.left),(float)(g_cp.rcOK.bottom-g_cp.rcOK.top)),&sf,&wb);

        FillRR(g,Color(255,50,50,68),(float)g_cp.rcCancel.left,(float)g_cp.rcCancel.top,
               (float)(g_cp.rcCancel.right-g_cp.rcCancel.left),(float)(g_cp.rcCancel.bottom-g_cp.rcCancel.top),6);
        SolidBrush gb2(Color(255,170,170,195));
        g.DrawString(L"Cancel",-1,&fB,RectF((float)g_cp.rcCancel.left,(float)g_cp.rcCancel.top,
               (float)(g_cp.rcCancel.right-g_cp.rcCancel.left),(float)(g_cp.rcCancel.bottom-g_cp.rcCancel.top)),&sf,&gb2);
    }

    BitBlt(hdc,0,0,CP_W,CP_H,memDC,0,0,SRCCOPY);
    SelectObject(memDC,old); DeleteObject(bmp); DeleteDC(memDC);
}

//  COLOR PICKER — WndProc
LRESULT CALLBACK ColorPickerProc(HWND hwnd,UINT msg,WPARAM wParam,LPARAM lParam)
{
    switch(msg){
    case WM_CREATE:
        SetRect(&g_cp.rcSV,    10,  10, 290, 198);
        SetRect(&g_cp.rcHue,   10, 208, 290, 230);
        SetRect(&g_cp.rcOld,   10, 256, 144, 284);
        SetRect(&g_cp.rcNew,  156, 256, 290, 284);
        SetRect(&g_cp.rcOK,   156, 345, 290, 373);
        SetRect(&g_cp.rcCancel,10, 345, 144, 373);
        g_cp.hwndEdit=CreateWindowExA(0,"EDIT","",
            WS_CHILD|WS_VISIBLE|ES_UPPERCASE|ES_AUTOHSCROLL|ES_CENTER,
            95,308,118,22,hwnd,(HMENU)101,GetModuleHandle(NULL),NULL);
        SendMessageA(g_cp.hwndEdit,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);
        SendMessageA(g_cp.hwndEdit,EM_SETLIMITTEXT,6,0);
        CP_UpdateHexEdit();
        return 0;

    case WM_PAINT:{PAINTSTRUCT ps;HDC hdc=BeginPaint(hwnd,&ps);RenderCP(hdc);EndPaint(hwnd,&ps);return 0;}
    case WM_ERASEBKGND: return 1;

    case WM_CTLCOLOREDIT:{
        HDC hdcEdit=(HDC)wParam;
        SetBkColor(hdcEdit,RGB(38,38,58));
        SetTextColor(hdcEdit,RGB(72,148,255));
        static HBRUSH hEditBg=CreateSolidBrush(RGB(38,38,58));
        return (LRESULT)hEditBg;
    }

    case WM_LBUTTONDOWN:{
        POINT pt={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
        if(PtInRect(&g_cp.rcSV,pt)) {g_cp.draggingSV=true;SetCapture(hwnd);CP_UpdateSV(pt);}
        else if(PtInRect(&g_cp.rcHue,pt)){g_cp.draggingHue=true;SetCapture(hwnd);CP_UpdateHue(pt);}
        else if(PtInRect(&g_cp.rcOK,pt)){
            *g_cp.target=HSVtoRGB(g_cp.hue,g_cp.sat,g_cp.val);
            SaveSettings(); InvalidateRect(g_hwndSettings,NULL,FALSE);
            DestroyWindow(hwnd); EnableWindow(g_hwndSettings,TRUE); SetForegroundWindow(g_hwndSettings);
        }
        else if(PtInRect(&g_cp.rcCancel,pt)){
            DestroyWindow(hwnd); EnableWindow(g_hwndSettings,TRUE); SetForegroundWindow(g_hwndSettings);
        }
        return 0;
    }
    case WM_MOUSEMOVE:{
        POINT pt={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
        if(g_cp.draggingSV) CP_UpdateSV(pt);
        if(g_cp.draggingHue)CP_UpdateHue(pt);
        return 0;
    }
    case WM_LBUTTONUP:
        g_cp.draggingSV=g_cp.draggingHue=false; ReleaseCapture(); return 0;

    case WM_COMMAND:
        if(LOWORD(wParam)==101&&HIWORD(wParam)==EN_CHANGE&&!g_cp.suppressEdit){
            char buf[16]; GetWindowTextA(g_cp.hwndEdit,buf,sizeof(buf));
            if(strlen(buf)==6){
                unsigned long hex=strtoul(buf,NULL,16);
                RGBtoHSV(RGB((hex>>16)&0xFF,(hex>>8)&0xFF,hex&0xFF),g_cp.hue,g_cp.sat,g_cp.val);
                InvalidateRect(hwnd,NULL,FALSE);
            }
        }
        return 0;

    case WM_KEYDOWN:
        if(wParam==VK_ESCAPE||wParam==VK_RETURN){
            if(wParam==VK_RETURN){
                *g_cp.target=HSVtoRGB(g_cp.hue,g_cp.sat,g_cp.val);
                SaveSettings(); InvalidateRect(g_hwndSettings,NULL,FALSE);
            }
            DestroyWindow(hwnd); EnableWindow(g_hwndSettings,TRUE); SetForegroundWindow(g_hwndSettings);
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd); EnableWindow(g_hwndSettings,TRUE); SetForegroundWindow(g_hwndSettings);
        return 0;
    case WM_DESTROY: g_cp.hwnd=nullptr; return 0;
    }
    return DefWindowProc(hwnd,msg,wParam,lParam);
}

//  OPEN COLOR PICKER
static void OpenColorPicker(COLORREF*target)
{
    if(g_cp.hwnd){SetForegroundWindow(g_cp.hwnd);return;}
    g_cp.target=target; g_cp.orig=*target;
    RGBtoHSV(*target,g_cp.hue,g_cp.sat,g_cp.val);
    g_cp.draggingSV=g_cp.draggingHue=false;

    RECT rc={0,0,CP_W,CP_H};
    AdjustWindowRect(&rc,WS_CAPTION|WS_POPUP|WS_SYSMENU,FALSE);
    int ww=rc.right-rc.left,wh=rc.bottom-rc.top;
    RECT swr; GetWindowRect(g_hwndSettings,&swr);
    int x=swr.left+(SW_W-ww)/2, y=swr.top+(SW_H-wh)/2;

    g_cp.hwnd=CreateWindowExA(0,"CF_ColorPicker","Pick a Color",
        WS_CAPTION|WS_POPUP|WS_SYSMENU,x,y,ww,wh,g_hwndSettings,NULL,GetModuleHandle(NULL),NULL);
    EnableWindow(g_hwndSettings,FALSE);
    ShowWindow(g_cp.hwnd,SW_SHOW);
    SetForegroundWindow(g_cp.hwnd);
}

//  SETTINGS WINDOW — DRAW
static void DrawSettings(HDC hdc)
{
    HDC memDC=CreateCompatibleDC(hdc);
    HBITMAP bmp=CreateCompatibleBitmap(hdc,SW_W,SW_H);
    HBITMAP old=(HBITMAP)SelectObject(memDC,bmp);

    Graphics g(memDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);

    TC t=GetTC();

    SolidBrush bgB(t.bg); g.FillRectangle(&bgB,0,0,SW_W,SW_H);

    {SolidBrush hB(t.hdrBg); g.FillRectangle(&hB,0,0,SW_W,66);}
    {Pen sep(t.sep,1); g.DrawLine(&sep,0.f,66.f,(float)SW_W,66.f);}

    FontFamily ff(L"Segoe UI");
    Font fTitle(&ff,13,FontStyleBold,UnitPoint);
    Font fNorm (&ff,10,FontStyleRegular,UnitPoint);
    Font fSub  (&ff, 9,FontStyleRegular,UnitPoint);
    Font fBtnS (&ff,10,FontStyleBold,UnitPoint);
    Font fLink (&ff, 9,FontStyleUnderline,UnitPoint);
    SolidBrush bText(t.text),bSub(t.sub),bAccent(t.accent);

    {
        float lx=14.f,ly=15.f,lsz=36.f;
    
        SolidBrush logoBg(Color(255,30,30,50));
        g.FillEllipse(&logoBg,lx,ly,lsz,lsz);
        Pen logoRing(Color(255,72,148,255),2.f);
        g.DrawEllipse(&logoRing,lx+1.f,ly+1.f,lsz-2.f,lsz-2.f);
       
        FontFamily ffA(L"Arial"); Font fLogo(&ffA,8,FontStyleBold,UnitPoint);
        SolidBrush logoTxt(Color(255,220,230,255));
        StringFormat sfL; sfL.SetAlignment(StringAlignmentCenter); sfL.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(L"BCF",-1,&fLogo,RectF(lx,ly,lsz,lsz),&sfL,&logoTxt);
    }

    g.DrawString(L"Better Cursor Finder",-1,&fTitle,PointF(58,17),&bText);
    g.DrawString(L"v2.0",-1,&fSub,PointF(60,40),&bSub);

    float iconSz=26.f,iconX=(float)(SW_W-46),iconY=20.f;
    FillRR(g,t.border,iconX-2,iconY-2,iconSz+4,iconSz+4,(iconSz+4)/2);
    if(g_cfg.darkMode) DrawSun (g,iconX+iconSz/2,iconY+iconSz/2,iconSz,Color(255,255,210,60));
    else               DrawMoon(g,iconX+iconSz/2,iconY+iconSz/2,iconSz,Color(255,160,160,220),t.hdrBg);
    SetRect(&g_rcTheme,(int)iconX-2,(int)iconY-2,(int)(iconX+iconSz+4),(int)(iconY+iconSz+4));

    float swX=(float)(SW_W-82),swW=58,swH=30;
    auto sepLine=[&](float y){Pen p(t.sep,1);g.DrawLine(&p,20.f,y,(float)(SW_W-20),y);};
    auto swatch=[&](COLORREF col,float y,RECT&rc){
        float sy=y-2;
        FillRR(g,CR(col),swX,sy,swW,swH,8);
        DrawRR(g,t.border,1.5f,swX,sy,swW,swH,8);
        SetRect(&rc,(int)swX,(int)sy,(int)(swX+swW),(int)(sy+swH));
    };

    // Ring Color
    float y=78;
    g.DrawString(L"Ring Color",-1,&fNorm,PointF(20,y),&bText);
    g.DrawString(L"Change the ring's color",-1,&fSub,PointF(20,y+20),&bSub);
    swatch(g_cfg.ringColor,y+3,g_rcRing);
    sepLine(130);

    // Outline Color
    y=143;
    g.DrawString(L"Outline Color",-1,&fNorm,PointF(20,y),&bText);
    g.DrawString(L"Color of the outline around the ring",-1,&fSub,PointF(20,y+20),&bSub);
    swatch(g_cfg.outlineColor,y+3,g_rcOutline);
    sepLine(195);

    // Animation Speed
    y=208;
    g.DrawString(L"Animation Speed",-1,&fNorm,PointF(20,y),&bText);
    float bY=y+30,bH=34,bGap=7,bW=(SW_W-40-bGap*2)/3.f;
    const wchar_t* spL[]={L"Slow",L"Normal",L"Fast"};
    RECT* spR[]={&g_rcSlow,&g_rcNorm,&g_rcFast};
    for(int i=0;i<3;i++){
        float bx=20.f+i*(bW+bGap); bool sel=(g_cfg.speed==i);
        FillRR(g,sel?t.accent:t.cardBg,bx,bY,bW,bH,8);
        DrawRR(g,sel?t.accent:t.border,1.5f,bx,bY,bW,bH,8);
        SolidBrush bt(sel?Color(255,255,255,255):t.text);
        StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(spL[i],-1,sel?&fBtnS:&fNorm,RectF(bx,bY,bW,bH),&sf,&bt);
        SetRect(spR[i],(int)bx,(int)bY,(int)(bx+bW),(int)(bY+bH));
    }
    sepLine(278);

    // Move cancel toggle
    y=291;
    g.DrawString(L"Cancel on mouse move",-1,&fNorm,PointF(20,y),&bText);
    g.DrawString(L"Stop animation if the mouse moves",-1,&fSub,PointF(20,y+20),&bSub);
    DrawToggle(g,(float)(SW_W-64),y+4,g_cfg.moveCancel,t.accent,t.togOff);
    SetRect(&g_rcMove,SW_W-64,(int)(y+4),SW_W-64+44,(int)(y+28));
    sepLine(352);

    // Launch at startup
    y=365;
    g.DrawString(L"Launch at startup",-1,&fNorm,PointF(20,y),&bText);
    g.DrawString(L"Start automatically with Windows",-1,&fSub,PointF(20,y+20),&bSub);
    DrawToggle(g,(float)(SW_W-64),y+4,g_cfg.startOnBoot,t.accent,t.togOff);
    SetRect(&g_rcBoot,SW_W-64,(int)(y+4),SW_W-64+44,(int)(y+28));
    sepLine(425);

    {
        float gbX=20.f, gbY=433.f, gbW=(float)(SW_W-40), gbH=36.f;
        Color ghBg = g_cfg.darkMode ? Color(255,28,28,46) : Color(255,215,218,238);
        FillRR(g,ghBg,gbX,gbY,gbW,gbH,10);
        DrawRR(g,t.border,1.5f,gbX,gbY,gbW,gbH,10);

        float gx=gbX+20.f, gy=gbY+gbH/2.f, gr=10.f;
        SolidBrush ghDark(Color(255,10,10,20));
        g.FillEllipse(&ghDark,gx-gr,gy-gr,gr*2.f,gr*2.f);
        SolidBrush ghW(Color(255,220,225,255));
        g.FillEllipse(&ghW,gx-gr*.62f,gy-gr*.75f,gr*1.24f,gr*1.1f);
        PointF eL[]={PointF(gx-gr*.55f,gy-gr*.7f),PointF(gx-gr*.75f,gy-gr*1.15f),PointF(gx-gr*.2f,gy-gr*.78f)};
        g.FillPolygon(&ghW,eL,3);
        PointF eR[]={PointF(gx+gr*.55f,gy-gr*.7f),PointF(gx+gr*.75f,gy-gr*1.15f),PointF(gx+gr*.2f,gy-gr*.78f)};
        g.FillPolygon(&ghW,eR,3);
        PointF body[]={
            PointF(gx-gr*.62f,gy+gr*.1f),PointF(gx-gr*.75f,gy+gr*.85f),
            PointF(gx-gr*.3f, gy+gr*.5f),PointF(gx,          gy+gr*.85f),
            PointF(gx+gr*.3f, gy+gr*.5f),PointF(gx+gr*.75f,gy+gr*.85f),
            PointF(gx+gr*.62f,gy+gr*.1f)
        };
        g.FillPolygon(&ghW,body,7);

        Font fGH(&ff,9,FontStyleBold,UnitPoint);
        StringFormat sfGH; sfGH.SetAlignment(StringAlignmentCenter); sfGH.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(L"mattytheprofessional",-1,&fGH,
            RectF(gbX+32.f,gbY,gbW-34.f,gbH),&sfGH,&bAccent);

        SetRect(&g_rcGithub,(int)gbX,(int)gbY,(int)(gbX+gbW),(int)(gbY+gbH));
    }

    StringFormat sfC; sfC.SetAlignment(StringAlignmentCenter); sfC.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(L"System tray - right-click for options",-1,&fSub,RectF(0,474,SW_W,18),&sfC,&bSub);

    BitBlt(hdc,0,0,SW_W,SW_H,memDC,0,0,SRCCOPY);
    SelectObject(memDC,old); DeleteObject(bmp); DeleteDC(memDC);
}

//  SETTINGS — WndProc
LRESULT CALLBACK SettingsWndProc(HWND hwnd,UINT msg,WPARAM wParam,LPARAM lParam)
{
    switch(msg){
    case WM_PAINT:{PAINTSTRUCT ps;HDC hdc=BeginPaint(hwnd,&ps);DrawSettings(hdc);EndPaint(hwnd,&ps);return 0;}
    case WM_ERASEBKGND: return 1;

    case WM_SETCURSOR:{
        POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd,&pt);
        if(PtInRect(&g_rcGithub,pt)){SetCursor(LoadCursor(NULL,IDC_HAND));return TRUE;}
        return DefWindowProc(hwnd,msg,wParam,lParam);
    }

    case WM_LBUTTONDOWN:{
        POINT pt={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
        auto repaint=[&]{InvalidateRect(hwnd,NULL,FALSE);};

        if(PtInRect(&g_rcTheme,pt)){g_cfg.darkMode=!g_cfg.darkMode;SaveSettings();repaint();return 0;}
        if(PtInRect(&g_rcRing,   pt)){OpenColorPicker(&g_cfg.ringColor);   return 0;}
        if(PtInRect(&g_rcOutline,pt)){OpenColorPicker(&g_cfg.outlineColor);return 0;}
        if(PtInRect(&g_rcSlow,pt)){g_cfg.speed=0;SaveSettings();repaint();return 0;}
        if(PtInRect(&g_rcNorm,pt)){g_cfg.speed=1;SaveSettings();repaint();return 0;}
        if(PtInRect(&g_rcFast,pt)){g_cfg.speed=2;SaveSettings();repaint();return 0;}
        if(PtInRect(&g_rcMove,pt)){g_cfg.moveCancel=!g_cfg.moveCancel;SaveSettings();repaint();return 0;}
        if(PtInRect(&g_rcBoot,pt)){g_cfg.startOnBoot=!g_cfg.startOnBoot;ApplyStartup(g_cfg.startOnBoot);SaveSettings();repaint();return 0;}
        if(PtInRect(&g_rcGithub,pt)){ShellExecuteA(NULL,"open","https://github.com/mattytheprofessional",NULL,NULL,SW_SHOW);return 0;}
        return 0;
    }

    case WM_CLOSE:
        ShowWindow(hwnd,SW_HIDE); g_settingsOpen=false; return 0;
    case WM_SIZE:
        if(wParam==SIZE_MINIMIZED){ShowWindow(hwnd,SW_HIDE);g_settingsOpen=false;} return 0;
    case WM_DESTROY: return 0;
    }
    return DefWindowProc(hwnd,msg,wParam,lParam);
}

//  ANIMATION
static void ClearAndHide()
{
    HDC hdcS=GetDC(NULL);HDC hdcM=CreateCompatibleDC(hdcS);
    BITMAPINFO bmi={};bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth=OV_SIZE;bmi.bmiHeader.biHeight=-OV_SIZE;
    bmi.bmiHeader.biPlanes=1;bmi.bmiHeader.biBitCount=32;bmi.bmiHeader.biCompression=BI_RGB;
    void*pv=nullptr;HBITMAP hb=CreateDIBSection(hdcM,&bmi,DIB_RGB_COLORS,&pv,NULL,0);
    HBITMAP ho=(HBITMAP)SelectObject(hdcM,hb);
    memset(pv,0,OV_SIZE*OV_SIZE*4);
    POINT ptS={0,0};SIZE szW={OV_SIZE,OV_SIZE};
    POINT ptD={g_cursor.x-OV_SIZE/2,g_cursor.y-OV_SIZE/2};
    BLENDFUNCTION bf={};bf.BlendOp=AC_SRC_OVER;bf.SourceConstantAlpha=255;bf.AlphaFormat=AC_SRC_ALPHA;
    UpdateLayeredWindow(g_hwndOverlay,hdcS,&ptD,&szW,hdcM,&ptS,0,&bf,ULW_ALPHA);
    SelectObject(hdcM,ho);DeleteObject(hb);DeleteDC(hdcM);ReleaseDC(NULL,hdcS);
    ShowWindow(g_hwndOverlay,SW_HIDE);
}
static void CancelAnimation(){if(!g_animating)return;g_animating=false;ClearAndHide();}

static void StartAnimation(){
    GetCursorPos(&g_cursor);g_animStart=g_cursor;
    g_animating=true;g_startTime=GetTickCount();
    SetWindowPos(g_hwndOverlay,HWND_TOPMOST,
                 g_cursor.x-OV_SIZE/2,g_cursor.y-OV_SIZE/2,OV_SIZE,OV_SIZE,
                 SWP_NOACTIVATE|SWP_SHOWWINDOW);
}
static bool AnyNewKeyPressed(){
    for(int vk=1;vk<256;vk++){
        if(vk==VK_CONTROL||vk==VK_LCONTROL||vk==VK_RCONTROL)continue;
        if(g_preExisting[vk])continue;
        if(GetAsyncKeyState(vk)&0x8000)return true;
    }
    return false;
}
static void CheckCancel(){
    if(!g_animating)return;
    if(g_cfg.moveCancel){
        POINT cur;GetCursorPos(&cur);
        int dx=cur.x-g_animStart.x,dy=cur.y-g_animStart.y;
        if(dx*dx+dy*dy>MOVE_THR*MOVE_THR){CancelAnimation();return;}
    }
    if(AnyNewKeyPressed())CancelAnimation();
}
static void RenderFrame(float progress)
{
    float r=ANIM_MAX_R*(1.f-EaseOutQuint(progress));
    if(r<ANIM_MIN_R){g_animating=false;ClearAndHide();return;}
    float alpha;
    if(progress<0.06f)alpha=progress/0.06f;
    else if(progress<0.72f)alpha=1.f;
    else alpha=1.f-(progress-0.72f)/0.28f;
    alpha=Clamp01(alpha);

    BYTE rR=GetRValue(g_cfg.ringColor),rG=GetGValue(g_cfg.ringColor),rB=GetBValue(g_cfg.ringColor);
    BYTE oR=GetRValue(g_cfg.outlineColor),oG=GetGValue(g_cfg.outlineColor),oB=GetBValue(g_cfg.outlineColor);

    HDC hdcS=GetDC(NULL);HDC hdcM=CreateCompatibleDC(hdcS);
    BITMAPINFO bmi={};bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth=OV_SIZE;bmi.bmiHeader.biHeight=-OV_SIZE;
    bmi.bmiHeader.biPlanes=1;bmi.bmiHeader.biBitCount=32;bmi.bmiHeader.biCompression=BI_RGB;
    void*pv=nullptr;HBITMAP hb=CreateDIBSection(hdcM,&bmi,DIB_RGB_COLORS,&pv,NULL,0);
    HBITMAP ho=(HBITMAP)SelectObject(hdcM,hb);
    memset(pv,0,OV_SIZE*OV_SIZE*4);
    {
        Graphics gfx(hdcM);gfx.SetSmoothingMode(SmoothingModeAntiAlias);
        gfx.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        float cx=OV_SIZE/2.f,cy=OV_SIZE/2.f;
        {Pen p(Color((BYTE)(alpha*14),rR,rG,rB),18.f);gfx.DrawEllipse(&p,cx-r,cy-r,r*2,r*2);}
        {Pen p(Color((BYTE)(alpha*36),rR,rG,rB), 9.f);gfx.DrawEllipse(&p,cx-r,cy-r,r*2,r*2);}
        {Pen p(Color((BYTE)(alpha*78),rR,rG,rB),4.5f);gfx.DrawEllipse(&p,cx-r,cy-r,r*2,r*2);}
        {Pen p(Color((BYTE)(alpha*210),oR,oG,oB),STROKE_W+3.f);gfx.DrawEllipse(&p,cx-r,cy-r,r*2,r*2);}
        {Pen p(Color((BYTE)(alpha*228),rR,rG,rB),STROKE_W);gfx.DrawEllipse(&p,cx-r,cy-r,r*2,r*2);}
        float ir=r-(STROKE_W+2.2f);
        if(ir>1.f){Pen p(Color((BYTE)(alpha*210),oR,oG,oB),STROKE_W+2.f);gfx.DrawEllipse(&p,cx-ir,cy-ir,ir*2,ir*2);}
    }
    POINT ptS={0,0};SIZE szW={OV_SIZE,OV_SIZE};
    POINT ptD={g_cursor.x-OV_SIZE/2,g_cursor.y-OV_SIZE/2};
    BLENDFUNCTION bf={};bf.BlendOp=AC_SRC_OVER;bf.SourceConstantAlpha=255;bf.AlphaFormat=AC_SRC_ALPHA;
    UpdateLayeredWindow(g_hwndOverlay,hdcS,&ptD,&szW,hdcM,&ptS,0,&bf,ULW_ALPHA);
    SelectObject(hdcM,ho);DeleteObject(hb);DeleteDC(hdcM);ReleaseDC(NULL,hdcS);
}

//  SETTINGS
static void ShowSettings(){
    if(!g_hwndSettings)return;
    if(g_settingsOpen){
        
        if(IsIconic(g_hwndSettings)) ShowWindow(g_hwndSettings,SW_RESTORE);
        
        SetWindowPos(g_hwndSettings,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
        SetForegroundWindow(g_hwndSettings);
        BringWindowToTop(g_hwndSettings);
        return;
    }
    
    int sw=GetSystemMetrics(SM_CXSCREEN),sh=GetSystemMetrics(SM_CYSCREEN);
    
    RECT wr; GetWindowRect(g_hwndSettings,&wr);
    int ww=wr.right-wr.left, wh=wr.bottom-wr.top;
    int x=(sw-ww)/2, y=(sh-wh)/2;
    SetWindowPos(g_hwndSettings,HWND_TOP,x,y,0,0,SWP_NOSIZE|SWP_SHOWWINDOW);
    ShowWindow(g_hwndSettings,SW_SHOWNORMAL);
   
    DWORD fgTid=GetWindowThreadProcessId(GetForegroundWindow(),NULL);
    DWORD myTid=GetCurrentThreadId();
    AttachThreadInput(fgTid,myTid,TRUE);
    SetForegroundWindow(g_hwndSettings);
    BringWindowToTop(g_hwndSettings);
    AttachThreadInput(fgTid,myTid,FALSE);
    InvalidateRect(g_hwndSettings,NULL,FALSE);
    g_settingsOpen=true;
}

//  OVERLAY — WndProc (tray messages)
LRESULT CALLBACK OverlayWndProc(HWND hwnd,UINT msg,WPARAM wParam,LPARAM lParam)
{
    switch(msg){
    case WM_TRAY:
        if(lParam==WM_LBUTTONUP){ShowSettings();return 0;}
        if(lParam==WM_RBUTTONUP){
            POINT pt;GetCursorPos(&pt);SetForegroundWindow(hwnd);
            HMENU menu=CreatePopupMenu();
            AppendMenuA(menu,MF_STRING,1,"BCF Settings");
            AppendMenuA(menu,MF_SEPARATOR,0,NULL);
            AppendMenuA(menu,MF_STRING,2,"Shutdown BCF");
            int cmd=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,pt.x,pt.y,0,hwnd,NULL);
            DestroyMenu(menu);
            if(cmd==1)ShowSettings();
            if(cmd==2)PostQuitMessage(0);
            return 0;
        }
        return 0;
    case WM_DESTROY:PostQuitMessage(0);return 0;
    }
    return DefWindowProc(hwnd,msg,wParam,lParam);
}

//  ENTRY POINT
int WINAPI WinMain(HINSTANCE hInst,HINSTANCE,LPSTR,int)
{
    HANDLE hMutex=CreateMutexA(NULL,TRUE,"BCF_v2_Mutex");
    if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(hMutex);return 0;}

    LoadSettings();
    GdiplusStartupInput gi;ULONG_PTR token;GdiplusStartup(&token,&gi,NULL);

    g_hBCFIcon=CreateBCFIcon();

    // Overlay window 
    WNDCLASSEXA wc={};wc.cbSize=sizeof(wc);wc.lpfnWndProc=OverlayWndProc;
    wc.hInstance=hInst;wc.lpszClassName="CF_Overlay";RegisterClassExA(&wc);
    g_hwndOverlay=CreateWindowExA(
        WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,
        "CF_Overlay","",WS_POPUP,0,0,OV_SIZE,OV_SIZE,NULL,NULL,hInst,NULL);
    ShowWindow(g_hwndOverlay,SW_HIDE);

    // Tray icon
    g_nid.cbSize=sizeof(NOTIFYICONDATA);g_nid.hWnd=g_hwndOverlay;g_nid.uID=TRAY_ID;
    g_nid.uFlags=NIF_ICON|NIF_TIP|NIF_MESSAGE;g_nid.uCallbackMessage=WM_TRAY;
    g_nid.hIcon=g_hBCFIcon;
    lstrcpyA(g_nid.szTip,"Better Cursor Finder - Left-click: Settings");
    Shell_NotifyIconA(NIM_ADD,&g_nid);

    // Settings window
    WNDCLASSEXA wcs={};wcs.cbSize=sizeof(wcs);wcs.lpfnWndProc=SettingsWndProc;
    wcs.hInstance=hInst;wcs.lpszClassName="CF_Settings";
    wcs.hCursor=LoadCursor(NULL,IDC_ARROW);RegisterClassExA(&wcs);
   
    RECT adjRC={0,0,SW_W,SW_H};
    AdjustWindowRect(&adjRC,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE);
    int winW=adjRC.right-adjRC.left, winH=adjRC.bottom-adjRC.top;

    g_hwndSettings=CreateWindowExA(0,"CF_Settings","Better Cursor Finder",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        0,0,winW,winH,NULL,NULL,hInst,NULL);
    
    SendMessageA(g_hwndSettings,WM_SETICON,ICON_SMALL,(LPARAM)g_hBCFIcon);
    SendMessageA(g_hwndSettings,WM_SETICON,ICON_BIG,  (LPARAM)g_hBCFIcon);
    
    HMENU hSys=GetSystemMenu(g_hwndSettings,FALSE);
    DeleteMenu(hSys,SC_CLOSE,MF_BYCOMMAND);
    ShowWindow(g_hwndSettings,SW_HIDE);

    // Color picker window class
    WNDCLASSEXA wcp={};wcp.cbSize=sizeof(wcp);wcp.lpfnWndProc=ColorPickerProc;
    wcp.hInstance=hInst;wcp.lpszClassName="CF_ColorPicker";
    wcp.hCursor=LoadCursor(NULL,IDC_ARROW);RegisterClassExA(&wcp);

    // Main loop
    MSG msg;
    while(true){
        while(PeekMessageA(&msg,NULL,0,0,PM_REMOVE)){
            if(msg.message==WM_QUIT){
                Shell_NotifyIconA(NIM_DELETE,&g_nid);
                if(g_hBCFIcon)DestroyIcon(g_hBCFIcon);
                GdiplusShutdown(token);CloseHandle(hMutex);return 0;
            }
            TranslateMessage(&msg);DispatchMessageA(&msg);
        }

        bool ctrlDown=(GetAsyncKeyState(VK_CONTROL)&0x8000)!=0;

        if(ctrlDown&&!g_ctrlWas){
            g_comboDetected=false;
            memset(g_preExisting,0,sizeof(g_preExisting));
            for(int vk=1;vk<256;vk++) if(GetAsyncKeyState(vk)&0x8000)g_preExisting[vk]=true;
        }
        if(ctrlDown){
            for(int vk=1;vk<256;vk++){
                if(vk==VK_CONTROL||vk==VK_LCONTROL||vk==VK_RCONTROL)continue;
                if(g_preExisting[vk])continue;
                if(GetAsyncKeyState(vk)&0x8000){g_comboDetected=true;break;}
            }
        }
        if(!ctrlDown&&g_ctrlWas){
            if(!g_comboDetected)StartAnimation();
            g_comboDetected=false;
        }
        g_ctrlWas=ctrlDown;

        CheckCancel();
        if(g_animating){
            DWORD elapsed=GetTickCount()-g_startTime;
            float progress=std::min((float)elapsed/GetDuration(),1.f);
            GetCursorPos(&g_cursor);
            RenderFrame(progress);
        }
        Sleep(6);
    }
}