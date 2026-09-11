#include "d3d8_internal.hpp"
#include "Gui.hpp"

#include <SDL.h>
#ifdef TH08_MODERN_WEB
#include <GLES3/gl3.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif
#ifdef TH08_MODERN_WEB
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#endif

#include <math.h>
#include <new>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace
{
#ifdef TH08_MODERN_WEB
#define TH08_WEB_RENDER_STAGE(stage) fprintf(stderr, "th08-web: renderer: %s\n", stage)
#else
#define TH08_WEB_RENDER_STAGE(stage) ((void)0)
#endif

class LinuxTexture;

typedef void (APIENTRY *GenFramebuffersFunction)(GLsizei, GLuint *);
typedef void (APIENTRY *BindFramebufferFunction)(GLenum, GLuint);
typedef void (APIENTRY *FramebufferTexture2DFunction)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *CheckFramebufferStatusFunction)(GLenum);
typedef void (APIENTRY *DeleteFramebuffersFunction)(GLsizei, const GLuint *);
typedef void (APIENTRY *GenRenderbuffersFunction)(GLsizei, GLuint *);
typedef void (APIENTRY *BindRenderbufferFunction)(GLenum, GLuint);
typedef void (APIENTRY *RenderbufferStorageFunction)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY *FramebufferRenderbufferFunction)(GLenum, GLenum, GLenum, GLuint);
typedef void (APIENTRY *DeleteRenderbuffersFunction)(GLsizei, const GLuint *);
typedef void (APIENTRY *FogCoordfFunction)(GLfloat);

struct FramebufferApi
{
    FramebufferApi()
        : genFramebuffers(NULL), bindFramebuffer(NULL), framebufferTexture2D(NULL),
          checkFramebufferStatus(NULL), deleteFramebuffers(NULL), genRenderbuffers(NULL),
          bindRenderbuffer(NULL), renderbufferStorage(NULL), framebufferRenderbuffer(NULL),
          deleteRenderbuffers(NULL)
    {
    }

    void *Load(const char *coreName, const char *extensionName)
    {
        void *procedure = SDL_GL_GetProcAddress(coreName);
        return procedure != NULL ? procedure : SDL_GL_GetProcAddress(extensionName);
    }

    bool Initialize()
    {
        genFramebuffers = reinterpret_cast<GenFramebuffersFunction>(
            Load("glGenFramebuffers", "glGenFramebuffersEXT"));
        bindFramebuffer = reinterpret_cast<BindFramebufferFunction>(
            Load("glBindFramebuffer", "glBindFramebufferEXT"));
        framebufferTexture2D = reinterpret_cast<FramebufferTexture2DFunction>(
            Load("glFramebufferTexture2D", "glFramebufferTexture2DEXT"));
        checkFramebufferStatus = reinterpret_cast<CheckFramebufferStatusFunction>(
            Load("glCheckFramebufferStatus", "glCheckFramebufferStatusEXT"));
        deleteFramebuffers = reinterpret_cast<DeleteFramebuffersFunction>(
            Load("glDeleteFramebuffers", "glDeleteFramebuffersEXT"));
        genRenderbuffers = reinterpret_cast<GenRenderbuffersFunction>(
            Load("glGenRenderbuffers", "glGenRenderbuffersEXT"));
        bindRenderbuffer = reinterpret_cast<BindRenderbufferFunction>(
            Load("glBindRenderbuffer", "glBindRenderbufferEXT"));
        renderbufferStorage = reinterpret_cast<RenderbufferStorageFunction>(
            Load("glRenderbufferStorage", "glRenderbufferStorageEXT"));
        framebufferRenderbuffer = reinterpret_cast<FramebufferRenderbufferFunction>(
            Load("glFramebufferRenderbuffer", "glFramebufferRenderbufferEXT"));
        deleteRenderbuffers = reinterpret_cast<DeleteRenderbuffersFunction>(
            Load("glDeleteRenderbuffers", "glDeleteRenderbuffersEXT"));
        return genFramebuffers != NULL && bindFramebuffer != NULL && framebufferTexture2D != NULL &&
               checkFramebufferStatus != NULL && deleteFramebuffers != NULL &&
               genRenderbuffers != NULL && bindRenderbuffer != NULL &&
               renderbufferStorage != NULL && framebufferRenderbuffer != NULL &&
               deleteRenderbuffers != NULL;
    }

    GenFramebuffersFunction genFramebuffers;
    BindFramebufferFunction bindFramebuffer;
    FramebufferTexture2DFunction framebufferTexture2D;
    CheckFramebufferStatusFunction checkFramebufferStatus;
    DeleteFramebuffersFunction deleteFramebuffers;
    GenRenderbuffersFunction genRenderbuffers;
    BindRenderbufferFunction bindRenderbuffer;
    RenderbufferStorageFunction renderbufferStorage;
    FramebufferRenderbufferFunction framebufferRenderbuffer;
    DeleteRenderbuffersFunction deleteRenderbuffers;
};

FramebufferApi g_framebufferApi;
#ifndef TH08_MODERN_WEB
FogCoordfFunction g_fogCoordf;
#endif

#ifdef TH08_MODERN_WEB
struct WebVertex
{
    GLfloat x, y, z;
    GLfloat u, v;
    GLfloat fogCoordinate;
    GLubyte red, green, blue, alpha;
};

struct WebDrawState
{
    GLuint texture;
    bool textureEnabled;
    bool blendEnabled;
    DWORD sourceBlend, destinationBlend;
    bool depthTestEnabled, depthWriteEnabled;
    DWORD depthFunction;
    bool scissorEnabled;
    GLint scissorX, scissorY;
    GLsizei scissorWidth, scissorHeight;
    DWORD minFilter, magFilter, addressU, addressV;
    bool rgbUsesTexture, alphaUsesTexture;
    GLfloat alphaThreshold;
    DWORD colorOperation, colorArgument1, colorArgument2;
    DWORD alphaOperation, alphaArgument1, alphaArgument2;
    DWORD textureFactor;
    bool alphaTestEnabled;
    DWORD alphaFunction, alphaReference;
    bool fogEnabled;
    DWORD fogColor;
    GLfloat fogStart, fogEnd;
    GLfloat viewportWidth, viewportHeight;
};

bool InitializeWebPipeline();
void DestroyWebPipeline();
bool DrawWebVertices(GLenum mode, const WebVertex *vertices, UINT count,
                     const WebDrawState &state);
bool QueueWebVertexRange(GLenum mode, size_t first, UINT count,
                         const WebDrawState &state);
void FlushWebDraws();
bool DrawWebBlit(GLuint texture, UINT width, UINT height, bool flipVertical, bool linearFilter);
#endif

void PushRendererState()
{
#ifndef TH08_MODERN_WEB
    glPushAttrib(GL_ALL_ATTRIB_BITS);
#endif
}

void PopRendererState()
{
#ifndef TH08_MODERN_WEB
    glPopAttrib();
#endif
}

void SelectDrawBuffer(GLenum buffer)
{
#ifdef TH08_MODERN_WEB
    // WebGL selects the draw target through the currently bound framebuffer
    // and does not expose desktop glDrawBuffer.
    (void)buffer;
#else
    glDrawBuffer(buffer);
#endif
}

void SelectReadBuffer(GLenum buffer)
{
#ifdef TH08_MODERN_WEB
    // WebGL likewise reads from the currently bound framebuffer.
    (void)buffer;
#else
    glReadBuffer(buffer);
#endif
}

UINT BytesPerPixel(D3DFORMAT format)
{
    switch (format)
    {
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4: return 2;
    default: return 4;
    }
}

void DecodePixel(const BYTE *source, D3DFORMAT format, BYTE *rgba)
{
    WORD pixel;
    switch (format)
    {
    case D3DFMT_R8G8B8:
        rgba[0] = source[2]; rgba[1] = source[1]; rgba[2] = source[0]; rgba[3] = 255; break;
    case D3DFMT_R5G6B5:
        memcpy(&pixel, source, sizeof(pixel));
        rgba[0] = static_cast<BYTE>(((pixel >> 11) & 31) * 255 / 31);
        rgba[1] = static_cast<BYTE>(((pixel >> 5) & 63) * 255 / 63);
        rgba[2] = static_cast<BYTE>((pixel & 31) * 255 / 31); rgba[3] = 255; break;
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        memcpy(&pixel, source, sizeof(pixel));
        rgba[0] = static_cast<BYTE>(((pixel >> 10) & 31) * 255 / 31);
        rgba[1] = static_cast<BYTE>(((pixel >> 5) & 31) * 255 / 31);
        rgba[2] = static_cast<BYTE>((pixel & 31) * 255 / 31);
        rgba[3] = format == D3DFMT_A1R5G5B5 && !(pixel & 0x8000) ? 0 : 255; break;
    case D3DFMT_A4R4G4B4:
        memcpy(&pixel, source, sizeof(pixel));
        rgba[0] = static_cast<BYTE>(((pixel >> 8) & 15) * 17);
        rgba[1] = static_cast<BYTE>(((pixel >> 4) & 15) * 17);
        rgba[2] = static_cast<BYTE>((pixel & 15) * 17);
        rgba[3] = static_cast<BYTE>(((pixel >> 12) & 15) * 17); break;
    default:
        rgba[0] = source[2]; rgba[1] = source[1]; rgba[2] = source[0];
        rgba[3] = format == D3DFMT_X8R8G8B8 ? 255 : source[3]; break;
    }
}

void EncodePixel(BYTE *destination, D3DFORMAT format, const BYTE *rgba)
{
    WORD pixel;
    switch (format)
    {
    case D3DFMT_R8G8B8:
        destination[0] = rgba[2]; destination[1] = rgba[1]; destination[2] = rgba[0]; break;
    case D3DFMT_R5G6B5:
        pixel = static_cast<WORD>(((rgba[0] * 31 / 255) << 11) |
                                  ((rgba[1] * 63 / 255) << 5) | (rgba[2] * 31 / 255));
        memcpy(destination, &pixel, sizeof(pixel)); break;
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        pixel = static_cast<WORD>(((format == D3DFMT_X1R5G5B5 || rgba[3] >= 128) ? 0x8000 : 0) |
                                  ((rgba[0] * 31 / 255) << 10) |
                                  ((rgba[1] * 31 / 255) << 5) | (rgba[2] * 31 / 255));
        memcpy(destination, &pixel, sizeof(pixel)); break;
    case D3DFMT_A4R4G4B4:
        pixel = static_cast<WORD>(((rgba[3] >> 4) << 12) | ((rgba[0] >> 4) << 8) |
                                  ((rgba[1] >> 4) << 4) | (rgba[2] >> 4));
        memcpy(destination, &pixel, sizeof(pixel)); break;
    default:
        destination[0] = rgba[2]; destination[1] = rgba[1]; destination[2] = rgba[0];
        destination[3] = format == D3DFMT_X8R8G8B8 ? 255 : rgba[3]; break;
    }
}

void Identity(D3DMATRIX *matrix)
{
    memset(matrix, 0, sizeof(*matrix));
    matrix->_11 = matrix->_22 = matrix->_33 = matrix->_44 = 1.0f;
}

class LinuxSurface : public IDirect3DSurface8
{
  public:
    LinuxSurface(UINT width_, UINT height_, D3DFORMAT format_, bool backbuffer_, LinuxTexture *owner_)
        : refs(1), width(width_), height(height_), format(format_), backbuffer(backbuffer_), owner(owner_),
          dirty(!backbuffer_)
    {
        pitch = width * BytesPerPixel(format);
        pixels.resize(pitch * height);
    }
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG value = --refs; if (value == 0) delete this; return value; }
    HRESULT GetDesc(D3DSURFACE_DESC *description)
    {
        if (description == NULL) return E_INVALIDARG;
        memset(description, 0, sizeof(*description));
        description->Format = format; description->Type = D3DRTYPE_SURFACE;
        description->Pool = backbuffer ? D3DPOOL_DEFAULT : D3DPOOL_SYSTEMMEM;
        description->Size = static_cast<UINT>(pixels.size());
        description->Width = width; description->Height = height; return S_OK;
    }
    HRESULT LockRect(D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags)
    {
        if (locked == NULL) return E_INVALIDARG;
        if (backbuffer && (flags & D3DLOCK_READONLY)) ReadBackbuffer();
        UINT left = rect != NULL && rect->left > 0 ? static_cast<UINT>(rect->left) : 0;
        UINT top = rect != NULL && rect->top > 0 ? static_cast<UINT>(rect->top) : 0;
        if (left >= width || top >= height) return E_INVALIDARG;
        locked->Pitch = pitch;
        locked->pBits = &pixels[top * pitch + left * BytesPerPixel(format)]; return S_OK;
    }
    HRESULT UnlockRect() { dirty = true; return S_OK; }
    HRESULT GetDC(HDC *dc)
    { if (dc == NULL) return E_INVALIDARG; *dc = CreateCompatibleDC(NULL); return *dc ? S_OK : E_FAIL; }
    HRESULT ReleaseDC(HDC dc) { return DeleteDC(dc) ? S_OK : E_FAIL; }
    void ReadBackbuffer()
    {
        if (!backbuffer || width == 0 || height == 0) return;
#ifdef TH08_MODERN_WEB
        FlushWebDraws();
#endif
        std::vector<BYTE> rgba(width * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, &rgba[0]);
        const UINT bytes = BytesPerPixel(format);
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x)
                EncodePixel(&pixels[y * pitch + x * bytes], format,
                            &rgba[((height - 1 - y) * width + x) * 4]);
        dirty = false;
    }
    void FlushBackbuffer()
    {
        if (!backbuffer || !dirty || width == 0 || height == 0) return;
        std::vector<BYTE> rgba(width * height * 4);
        const UINT bytes = BytesPerPixel(format);
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x)
                DecodePixel(&pixels[y * pitch + x * bytes], format, &rgba[(y * width + x) * 4]);

        GLuint name = 0;
        PushRendererState();
#ifndef TH08_MODERN_WEB
        glDisable(GL_ALPHA_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_TEXTURE_2D);
#endif
        glGenTextures(1, &name); glBindTexture(GL_TEXTURE_2D, name);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#ifdef TH08_MODERN_WEB
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, &rgba[0]);
        DrawWebBlit(name, width, height, false, false);
#else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, &rgba[0]);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0.0, width, height, 0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glColor4ub(255, 255, 255, 255);
        glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f, 0.0f);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(static_cast<float>(width), 0.0f);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(0.0f, static_cast<float>(height));
        glTexCoord2f(1.0f, 1.0f); glVertex2f(static_cast<float>(width), static_cast<float>(height));
        glEnd();
        glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW);
#endif
        glDeleteTextures(1, &name);
        PopRendererState();
        dirty = false;
    }
    ULONG refs;
    UINT width, height, pitch;
    D3DFORMAT format;
    bool backbuffer;
    LinuxTexture *owner;
    bool dirty;
    std::vector<BYTE> pixels;
};

class LinuxTexture : public IDirect3DTexture8
{
  public:
    LinuxTexture(UINT width, UINT height, D3DFORMAT format)
        : refs(1), priority(0), glName(0), uploaded(false)
    { surface = new LinuxSurface(width, height, format, false, this); }
    ~LinuxTexture()
    {
        surface->owner = NULL; surface->Release();
        if (glName != 0)
        {
#ifdef TH08_MODERN_WEB
            FlushWebDraws();
#endif
            glDeleteTextures(1, &glName);
        }
    }
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG value = --refs; if (value == 0) delete this; return value; }
    DWORD SetPriority(DWORD value) { DWORD old = priority; priority = value; return old; }
    void PreLoad() { Upload(); }
    HRESULT GetLevelDesc(UINT level, D3DSURFACE_DESC *description)
    { return level == 0 ? surface->GetDesc(description) : E_INVALIDARG; }
    HRESULT GetSurfaceLevel(UINT level, IDirect3DSurface8 **result)
    {
        if (level != 0 || result == NULL) return E_INVALIDARG;
        surface->AddRef(); *result = surface; return S_OK;
    }
    HRESULT LockRect(UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags)
    { return level == 0 ? surface->LockRect(locked, rect, flags) : E_INVALIDARG; }
    HRESULT UnlockRect(UINT level)
    { if (level != 0) return E_INVALIDARG; uploaded = false; return surface->UnlockRect(); }
    void Upload()
    {
        if (uploaded && !surface->dirty) return;
#ifdef TH08_MODERN_WEB
        FlushWebDraws();
#endif
        if (glName == 0) glGenTextures(1, &glName);
        glBindTexture(GL_TEXTURE_2D, glName);
        std::vector<BYTE> rgba(surface->width * surface->height * 4);
        UINT bytes = BytesPerPixel(surface->format);
        for (UINT y = 0; y < surface->height; ++y)
            for (UINT x = 0; x < surface->width; ++x)
                DecodePixel(&surface->pixels[y * surface->pitch + x * bytes], surface->format,
                            &rgba[(y * surface->width + x) * 4]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0,
#ifdef TH08_MODERN_WEB
                     GL_RGBA8,
#else
                     GL_RGBA,
#endif
                     surface->width, surface->height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba.empty() ? NULL : &rgba[0]);
        uploaded = true; surface->dirty = false;
    }
    ULONG refs;
    DWORD priority;
    GLuint glName;
    bool uploaded;
    LinuxSurface *surface;
};

class LinuxVertexBuffer : public IDirect3DVertexBuffer8
{
  public:
    explicit LinuxVertexBuffer(UINT size) : refs(1), bytes(size) {}
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG value = --refs; if (value == 0) delete this; return value; }
    HRESULT Lock(UINT offset, UINT size, BYTE **data, DWORD)
    {
        if (data == NULL || offset > bytes.size()) return E_INVALIDARG;
        if (size == 0) size = static_cast<UINT>(bytes.size() - offset);
        if (offset + size > bytes.size()) return E_INVALIDARG;
        *data = bytes.empty() ? NULL : &bytes[offset]; return S_OK;
    }
    HRESULT Unlock() { return S_OK; }
    ULONG refs;
    std::vector<BYTE> bytes;
};

GLenum PrimitiveMode(D3DPRIMITIVETYPE type)
{
    switch (type)
    {
    case D3DPT_POINTLIST: return GL_POINTS;
    case D3DPT_LINELIST: return GL_LINES;
    case D3DPT_LINESTRIP: return GL_LINE_STRIP;
    case D3DPT_TRIANGLESTRIP: return GL_TRIANGLE_STRIP;
    case D3DPT_TRIANGLEFAN: return GL_TRIANGLE_FAN;
    default: return GL_TRIANGLES;
    }
}

UINT VertexCount(D3DPRIMITIVETYPE type, UINT primitiveCount)
{
    if (type == D3DPT_POINTLIST) return primitiveCount;
    if (type == D3DPT_LINELIST) return primitiveCount * 2;
    if (type == D3DPT_LINESTRIP) return primitiveCount + 1;
    if (type == D3DPT_TRIANGLELIST) return primitiveCount * 3;
    return primitiveCount + 2;
}

GLenum CompareFunction(DWORD function)
{
    switch (function)
    {
    case D3DCMP_NEVER: return GL_NEVER;
    case D3DCMP_LESS: return GL_LESS;
    case D3DCMP_EQUAL: return GL_EQUAL;
    case D3DCMP_LESSEQUAL: return GL_LEQUAL;
    case D3DCMP_GREATER: return GL_GREATER;
    case D3DCMP_NOTEQUAL: return GL_NOTEQUAL;
    case D3DCMP_GREATEREQUAL: return GL_GEQUAL;
    default: return GL_ALWAYS;
    }
}

GLenum BlendFunction(DWORD function)
{
    switch (function)
    {
    case D3DBLEND_ZERO: return GL_ZERO;
    case D3DBLEND_ONE: return GL_ONE;
    case D3DBLEND_SRCCOLOR: return GL_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR: return GL_ONE_MINUS_SRC_COLOR;
    case D3DBLEND_SRCALPHA: return GL_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    case D3DBLEND_DESTALPHA: return GL_DST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case D3DBLEND_DESTCOLOR: return GL_DST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
    case D3DBLEND_SRCALPHASAT: return GL_SRC_ALPHA_SATURATE;
    default: return GL_ONE;
    }
}

bool TextureOperationUsesTexture(DWORD operation, DWORD argument1, DWORD argument2)
{
    if (operation == D3DTOP_DISABLE) return false;
    if (operation == D3DTOP_SELECTARG1)
        return (argument1 & D3DTA_SELECTMASK) == D3DTA_TEXTURE;
    return (argument1 & D3DTA_SELECTMASK) == D3DTA_TEXTURE ||
           (argument2 & D3DTA_SELECTMASK) == D3DTA_TEXTURE;
}

#ifndef TH08_MODERN_WEB
GLenum TextureArgumentSource(DWORD argument)
{
    switch (argument & D3DTA_SELECTMASK)
    {
    case D3DTA_TEXTURE: return GL_TEXTURE;
    case D3DTA_TFACTOR: return GL_CONSTANT;
    default: return GL_PRIMARY_COLOR;
    }
}

void ConfigureTextureComponent(GLenum combineParameter, GLenum source0Parameter,
                               GLenum source1Parameter, GLenum operand0Parameter,
                               GLenum operand1Parameter, DWORD operation,
                               DWORD argument1, DWORD argument2, GLenum operand)
{
    glTexEnvi(GL_TEXTURE_ENV, combineParameter,
              operation == D3DTOP_SELECTARG1 ? GL_REPLACE : GL_MODULATE);
    glTexEnvi(GL_TEXTURE_ENV, source0Parameter, TextureArgumentSource(argument1));
    glTexEnvi(GL_TEXTURE_ENV, operand0Parameter, operand);
    glTexEnvi(GL_TEXTURE_ENV, source1Parameter, TextureArgumentSource(argument2));
    glTexEnvi(GL_TEXTURE_ENV, operand1Parameter, operand);
}
#else
struct WebPipeline
{
    WebPipeline()
        : program(0), bufferIndex(0), activeBufferIndex(0), bufferOffset(0), frameActive(false),
          viewportLocation(-1), textureLocation(-1), textureMaskLocation(-1),
          alphaThresholdLocation(-1), fogColorLocation(-1)
    {
        for (int index = 0; index < 3; ++index)
        {
            vertexBuffers[index] = 0;
            vertexArrays[index] = 0;
            bufferCapacities[index] = 0;
        }
    }

    GLuint program, vertexBuffers[3], vertexArrays[3];
    GLsizeiptr bufferCapacities[3];
    UINT bufferIndex, activeBufferIndex;
    GLsizeiptr bufferOffset;
    bool frameActive;
    GLint viewportLocation, textureLocation, textureMaskLocation;
    GLint alphaThresholdLocation, fogColorLocation;
};

WebPipeline g_webPipeline;

struct WebDrawCommand
{
    GLenum mode;
    GLint first;
    GLsizei count;
    WebDrawState state;
};

std::vector<WebVertex> g_webQueuedVertices;
std::vector<WebDrawCommand> g_webQueuedCommands;
double g_webGameFlushMilliseconds = 0.0;
double g_webMaximumGameFlushMilliseconds = 0.0;
double g_webBlitMilliseconds = 0.0;
double g_webMaximumBlitMilliseconds = 0.0;
double g_webUploadMilliseconds = 0.0;
double g_webMaximumUploadMilliseconds = 0.0;
unsigned long g_webMeasuredFrames = 0;
unsigned long g_webMeasuredFlushes = 0;
unsigned long g_webMeasuredCommands = 0;
unsigned long g_webMeasuredVertices = 0;
bool g_webMeasurementComplete = false;
enum WebPresentationMode
{
    WEB_PRESENTATION_AUTO = 0,
    WEB_PRESENTATION_DIRECT = 1,
    WEB_PRESENTATION_BITMAP = 2,
    WEB_PRESENTATION_PROXY = 3
};
int g_webPresentationMode = WEB_PRESENTATION_AUTO;
bool g_webPresentationDiagnostics = false;

bool WebMeasurementActive()
{
    return g_webPresentationDiagnostics || !g_webMeasurementComplete;
}

void RecordWebUpload(double milliseconds)
{
    if (!WebMeasurementActive())
        return;
    g_webUploadMilliseconds += milliseconds;
    if (milliseconds > g_webMaximumUploadMilliseconds)
        g_webMaximumUploadMilliseconds = milliseconds;
}

void ResetWebMeasurementWindow()
{
    g_webGameFlushMilliseconds = 0.0;
    g_webMaximumGameFlushMilliseconds = 0.0;
    g_webBlitMilliseconds = 0.0;
    g_webMaximumBlitMilliseconds = 0.0;
    g_webUploadMilliseconds = 0.0;
    g_webMaximumUploadMilliseconds = 0.0;
    g_webMeasuredFrames = 0;
    g_webMeasuredFlushes = 0;
    g_webMeasuredCommands = 0;
    g_webMeasuredVertices = 0;
}

GLuint CompileWebShader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
        return shader;

    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::vector<char> log(length > 1 ? length : 2, 0);
    glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), NULL, &log[0]);
    fprintf(stderr, "th08-web: WebGL2 shader compilation failed: %s\n", &log[0]);
    glDeleteShader(shader);
    return 0;
}

bool InitializeWebPipeline()
{
    if (g_webPipeline.program != 0)
        return true;

    static const char *vertexSource =
        "#version 300 es\n"
        "precision highp float;\n"
        "layout(location=0) in vec3 aPosition;\n"
        "layout(location=1) in vec2 aUv;\n"
        "layout(location=2) in vec4 aColor;\n"
        "layout(location=3) in float aFogFactor;\n"
        "uniform vec2 uViewport;\n"
        "out vec2 vUv;\n"
        "out vec4 vColor;\n"
        "out float vFogFactor;\n"
        "void main() {\n"
        "  vec2 clip = vec2(aPosition.x * 2.0 / uViewport.x - 1.0,"
        "                   1.0 - aPosition.y * 2.0 / uViewport.y);\n"
        "  gl_Position = vec4(clip, aPosition.z, 1.0);\n"
        "  gl_PointSize = 1.0;\n"
        "  vUv = aUv; vColor = aColor; vFogFactor = aFogFactor;\n"
        "}\n";
    static const char *fragmentSource =
        "#version 300 es\n"
        "precision mediump float;\n"
        "uniform sampler2D uTexture;\n"
        "uniform vec4 uTextureMask;\n"
        "uniform float uAlphaThreshold;\n"
        "uniform vec4 uFogColor;\n"
        "in vec2 vUv;\n"
        "in vec4 vColor;\n"
        "in float vFogFactor;\n"
        "out vec4 outColor;\n"
        "void main() {\n"
        "  vec4 textureColor = texture(uTexture, vUv);\n"
        "  vec4 result = mix(vColor, textureColor * vColor, uTextureMask);\n"
        "  if (result.a < uAlphaThreshold) discard;\n"
        "  result.rgb = mix(uFogColor.rgb, result.rgb, vFogFactor);\n"
        "  outColor = result;\n"
        "}\n";

    GLuint vertexShader = CompileWebShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fragmentShader = CompileWebShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (vertexShader == 0 || fragmentShader == 0)
    {
        if (vertexShader != 0) glDeleteShader(vertexShader);
        if (fragmentShader != 0) glDeleteShader(fragmentShader);
        return false;
    }

    g_webPipeline.program = glCreateProgram();
    glAttachShader(g_webPipeline.program, vertexShader);
    glAttachShader(g_webPipeline.program, fragmentShader);
    glLinkProgram(g_webPipeline.program);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    GLint linked = GL_FALSE;
    glGetProgramiv(g_webPipeline.program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE)
    {
        GLint length = 0;
        glGetProgramiv(g_webPipeline.program, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(length > 1 ? length : 2, 0);
        glGetProgramInfoLog(g_webPipeline.program, static_cast<GLsizei>(log.size()), NULL, &log[0]);
        fprintf(stderr, "th08-web: WebGL2 program link failed: %s\n", &log[0]);
        DestroyWebPipeline();
        return false;
    }

    g_webPipeline.viewportLocation = glGetUniformLocation(g_webPipeline.program, "uViewport");
    g_webPipeline.textureLocation = glGetUniformLocation(g_webPipeline.program, "uTexture");
    g_webPipeline.textureMaskLocation = glGetUniformLocation(g_webPipeline.program, "uTextureMask");
    g_webPipeline.alphaThresholdLocation = glGetUniformLocation(g_webPipeline.program, "uAlphaThreshold");
    g_webPipeline.fogColorLocation = glGetUniformLocation(g_webPipeline.program, "uFogColor");

    glGenVertexArrays(3, g_webPipeline.vertexArrays);
    glGenBuffers(3, g_webPipeline.vertexBuffers);
    for (int index = 0; index < 3; ++index)
    {
        glBindVertexArray(g_webPipeline.vertexArrays[index]);
        glBindBuffer(GL_ARRAY_BUFFER, g_webPipeline.vertexBuffers[index]);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(WebVertex),
                              reinterpret_cast<const void *>(offsetof(WebVertex, x)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(WebVertex),
                              reinterpret_cast<const void *>(offsetof(WebVertex, u)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(WebVertex),
                              reinterpret_cast<const void *>(offsetof(WebVertex, red)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(WebVertex),
                              reinterpret_cast<const void *>(offsetof(WebVertex, fogCoordinate)));
    }
    glUseProgram(g_webPipeline.program);
    glUniform1i(g_webPipeline.textureLocation, 0);
    g_webQueuedVertices.reserve(32768);
    g_webQueuedCommands.reserve(2048);
    fprintf(stderr, "th08-web: renderer: direct WebGL2 shader/VBO pipeline ready\n");
    return true;
}

void DestroyWebPipeline()
{
    FlushWebDraws();
    glDeleteBuffers(3, g_webPipeline.vertexBuffers);
    glDeleteVertexArrays(3, g_webPipeline.vertexArrays);
    if (g_webPipeline.program != 0)
        glDeleteProgram(g_webPipeline.program);
    g_webPipeline = WebPipeline();
}

void SetWebColorUniform(GLint location, DWORD color)
{
    glUniform4f(location, ((color >> 16) & 255) / 255.0f,
                ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f,
                ((color >> 24) & 255) / 255.0f);
}

bool BeginWebFrame()
{
    if (g_webPipeline.frameActive)
        return true;
    if (!InitializeWebPipeline())
        return false;

    const double started = WebMeasurementActive() ? emscripten_get_now() : 0.0;
    const UINT index = g_webPipeline.bufferIndex++ % 3;
    g_webPipeline.activeBufferIndex = index;
    if (g_webPipeline.bufferCapacities[index] == 0)
        g_webPipeline.bufferCapacities[index] = 1024 * 1024;
    glBindVertexArray(g_webPipeline.vertexArrays[index]);
    glBindBuffer(GL_ARRAY_BUFFER, g_webPipeline.vertexBuffers[index]);
    // Allocate fresh streaming storage once per presented frame. This keeps
    // WebGL/ANGLE from waiting for an earlier frame that still owns the old
    // store, while subsequent uploads append without overwriting each other.
    glBufferData(GL_ARRAY_BUFFER, g_webPipeline.bufferCapacities[index], NULL, GL_STREAM_DRAW);
    g_webPipeline.bufferOffset = 0;
    g_webPipeline.frameActive = true;
    if (WebMeasurementActive())
        RecordWebUpload(emscripten_get_now() - started);
    return true;
}

void EndWebFrame()
{
    g_webPipeline.frameActive = false;
    g_webPipeline.bufferOffset = 0;
}

bool BindWebVertexData(const WebVertex *vertices, UINT count, GLint *firstVertex)
{
    if (!BeginWebFrame() || vertices == NULL || count == 0 || firstVertex == NULL)
        return false;
    const double started = WebMeasurementActive() ? emscripten_get_now() : 0.0;
    const UINT index = g_webPipeline.activeBufferIndex;
    const GLsizeiptr byteCount = static_cast<GLsizeiptr>(count * sizeof(WebVertex));
    glBindVertexArray(g_webPipeline.vertexArrays[index]);
    glBindBuffer(GL_ARRAY_BUFFER, g_webPipeline.vertexBuffers[index]);
    if (g_webPipeline.bufferOffset + byteCount > g_webPipeline.bufferCapacities[index])
    {
        GLsizeiptr capacity = g_webPipeline.bufferCapacities[index];
        while (capacity < byteCount)
            capacity *= 2;
        // A frame larger than the current store gets another fresh store;
        // already submitted draws retain the orphaned storage.
        glBufferData(GL_ARRAY_BUFFER, capacity, NULL, GL_STREAM_DRAW);
        g_webPipeline.bufferCapacities[index] = capacity;
        g_webPipeline.bufferOffset = 0;
    }
    *firstVertex = static_cast<GLint>(g_webPipeline.bufferOffset / sizeof(WebVertex));
    glBufferSubData(GL_ARRAY_BUFFER, g_webPipeline.bufferOffset, byteCount, vertices);
    g_webPipeline.bufferOffset += byteCount;
    if (WebMeasurementActive())
        RecordWebUpload(emscripten_get_now() - started);
    return true;
}

void ApplyWebDrawState(const WebDrawState &state, WebDrawState *applied, bool *valid)
{
    if (!*valid || state.blendEnabled != applied->blendEnabled)
    {
        if (state.blendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    }
    if (state.blendEnabled &&
        (!*valid || !applied->blendEnabled || state.sourceBlend != applied->sourceBlend ||
         state.destinationBlend != applied->destinationBlend))
        glBlendFunc(BlendFunction(state.sourceBlend), BlendFunction(state.destinationBlend));

    if (!*valid || state.depthTestEnabled != applied->depthTestEnabled)
    {
        if (state.depthTestEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    }
    if (state.depthTestEnabled &&
        (!*valid || !applied->depthTestEnabled || state.depthFunction != applied->depthFunction))
        glDepthFunc(CompareFunction(state.depthFunction));
    if (!*valid || state.depthWriteEnabled != applied->depthWriteEnabled)
        glDepthMask(state.depthWriteEnabled ? GL_TRUE : GL_FALSE);

    if (!*valid || state.scissorEnabled != applied->scissorEnabled)
    {
        if (state.scissorEnabled) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    }
    if (state.scissorEnabled &&
        (!*valid || !applied->scissorEnabled || state.scissorX != applied->scissorX ||
         state.scissorY != applied->scissorY || state.scissorWidth != applied->scissorWidth ||
         state.scissorHeight != applied->scissorHeight))
        glScissor(state.scissorX, state.scissorY, state.scissorWidth, state.scissorHeight);

    if (!*valid || state.viewportWidth != applied->viewportWidth ||
        state.viewportHeight != applied->viewportHeight)
        glUniform2f(g_webPipeline.viewportLocation, state.viewportWidth, state.viewportHeight);
    if (!*valid || state.rgbUsesTexture != applied->rgbUsesTexture ||
        state.alphaUsesTexture != applied->alphaUsesTexture)
        glUniform4f(g_webPipeline.textureMaskLocation,
                    state.rgbUsesTexture ? 1.0f : 0.0f,
                    state.rgbUsesTexture ? 1.0f : 0.0f,
                    state.rgbUsesTexture ? 1.0f : 0.0f,
                    state.alphaUsesTexture ? 1.0f : 0.0f);
    if (!*valid || state.alphaThreshold != applied->alphaThreshold)
        glUniform1f(g_webPipeline.alphaThresholdLocation, state.alphaThreshold);
    if (!*valid || state.fogColor != applied->fogColor)
        SetWebColorUniform(g_webPipeline.fogColorLocation, state.fogColor);

    if (!*valid || state.textureEnabled != applied->textureEnabled || state.texture != applied->texture)
        glBindTexture(GL_TEXTURE_2D, state.textureEnabled ? state.texture : 0);
    if (state.textureEnabled &&
        (!*valid || !applied->textureEnabled || state.texture != applied->texture ||
         state.minFilter != applied->minFilter || state.magFilter != applied->magFilter ||
         state.addressU != applied->addressU || state.addressV != applied->addressV))
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        state.minFilter == D3DTEXF_LINEAR ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        state.magFilter == D3DTEXF_LINEAR ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        state.addressU == D3DTADDRESS_CLAMP ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                        state.addressV == D3DTADDRESS_CLAMP ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    }
    *applied = state;
    *valid = true;
}

void FlushWebDraws()
{
    if (g_webQueuedCommands.empty())
        return;
    const double started = emscripten_get_now();
    const size_t commandCount = g_webQueuedCommands.size();
    const size_t vertexCount = g_webQueuedVertices.size();
    GLint firstVertex = 0;
    if (!BindWebVertexData(&g_webQueuedVertices[0],
                           static_cast<UINT>(g_webQueuedVertices.size()), &firstVertex))
    {
        g_webQueuedVertices.clear();
        g_webQueuedCommands.clear();
        return;
    }
    glUseProgram(g_webPipeline.program);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_CULL_FACE);
    WebDrawState applied;
    memset(&applied, 0, sizeof(applied));
    bool valid = false;
    for (size_t index = 0; index < g_webQueuedCommands.size(); ++index)
    {
        const WebDrawCommand &command = g_webQueuedCommands[index];
        ApplyWebDrawState(command.state, &applied, &valid);
        glDrawArrays(command.mode, firstVertex + command.first, command.count);
    }
    g_webQueuedVertices.clear();
    g_webQueuedCommands.clear();
    if (WebMeasurementActive())
    {
        const double elapsed = emscripten_get_now() - started;
        g_webGameFlushMilliseconds += elapsed;
        if (elapsed > g_webMaximumGameFlushMilliseconds)
            g_webMaximumGameFlushMilliseconds = elapsed;
        g_webMeasuredFlushes++;
        g_webMeasuredCommands += static_cast<unsigned long>(commandCount);
        g_webMeasuredVertices += static_cast<unsigned long>(vertexCount);
    }
}

bool QueueWebVertexRange(GLenum mode, size_t first, UINT count,
                         const WebDrawState &state)
{
    if (count == 0)
        return false;
    if (mode == GL_TRIANGLES && !g_webQueuedCommands.empty())
    {
        WebDrawCommand &previous = g_webQueuedCommands.back();
        if (previous.mode == mode &&
            previous.first + previous.count == static_cast<GLint>(first) &&
            memcmp(&previous.state, &state, sizeof(state)) == 0)
        {
            previous.count += static_cast<GLsizei>(count);
            return true;
        }
    }
    WebDrawCommand command;
    command.mode = mode;
    command.first = static_cast<GLint>(first);
    command.count = static_cast<GLsizei>(count);
    command.state = state;
    g_webQueuedCommands.push_back(command);
    return true;
}

bool DrawWebVertices(GLenum mode, const WebVertex *vertices, UINT count,
                     const WebDrawState &state)
{
    FlushWebDraws();
    const double started = emscripten_get_now();
    GLint firstVertex = 0;
    if (!BindWebVertexData(vertices, count, &firstVertex))
        return false;
    glUseProgram(g_webPipeline.program);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_CULL_FACE);
    WebDrawState applied;
    memset(&applied, 0, sizeof(applied));
    bool valid = false;
    ApplyWebDrawState(state, &applied, &valid);
    glDrawArrays(mode, firstVertex, count);
    if (WebMeasurementActive())
    {
        const double elapsed = emscripten_get_now() - started;
        g_webBlitMilliseconds += elapsed;
        if (elapsed > g_webMaximumBlitMilliseconds)
            g_webMaximumBlitMilliseconds = elapsed;
    }
    return true;
}

bool DrawWebBlit(GLuint texture, UINT width, UINT height, bool flipVertical, bool linearFilter)
{
    const GLfloat top = flipVertical ? 1.0f : 0.0f;
    const GLfloat bottom = flipVertical ? 0.0f : 1.0f;
    WebVertex vertices[4] = {
        {0.0f, 0.0f, 0.0f, 0.0f, top, 1.0f, 255, 255, 255, 255},
        {static_cast<GLfloat>(width), 0.0f, 0.0f, 1.0f, top, 1.0f, 255, 255, 255, 255},
        {0.0f, static_cast<GLfloat>(height), 0.0f, 0.0f, bottom, 1.0f, 255, 255, 255, 255},
        {static_cast<GLfloat>(width), static_cast<GLfloat>(height), 0.0f, 1.0f, bottom, 1.0f,
         255, 255, 255, 255}
    };
    WebDrawState state;
    memset(&state, 0, sizeof(state));
    state.texture = texture;
    state.textureEnabled = true;
    state.rgbUsesTexture = true;
    state.alphaUsesTexture = true;
    state.alphaThreshold = -1.0f;
    state.depthWriteEnabled = false;
    state.scissorEnabled = false;
    state.minFilter = linearFilter ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    state.magFilter = linearFilter ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    state.addressU = D3DTADDRESS_CLAMP;
    state.addressV = D3DTADDRESS_CLAMP;
    state.colorOperation = D3DTOP_SELECTARG1;
    state.colorArgument1 = D3DTA_TEXTURE;
    state.alphaOperation = D3DTOP_SELECTARG1;
    state.alphaArgument1 = D3DTA_TEXTURE;
    state.textureFactor = 0xffffffffu;
    state.alphaFunction = D3DCMP_ALWAYS;
    state.viewportWidth = static_cast<GLfloat>(width);
    state.viewportHeight = static_cast<GLfloat>(height);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_FALSE);
    return DrawWebVertices(GL_TRIANGLE_STRIP, vertices, 4, state);
}
#endif

class LinuxDevice : public IDirect3DDevice8
{
  public:
    LinuxDevice(SDL_Window *window_, const D3DPRESENT_PARAMETERS &parameters)
        : refs(1), window(window_), context(NULL), backbuffer(NULL), texture(NULL), vertexBuffer(NULL),
#ifdef TH08_MODERN_WEB
          webContext(0), bitmapPresentation(false),
#endif
          fvf(0), streamStride(0), renderFramebuffer(0), renderColorTexture(0), renderDepthBuffer(0),
          dialogueSnapshotTexture(0), framebufferReady(false), dialogueSnapshotReady(false),
          wasDialogPresent(false), presentCount(0)
    {
        memset(renderStates, 0, sizeof(renderStates)); memset(textureStates, 0, sizeof(textureStates));
        Identity(&world); Identity(&view); Identity(&projection); Identity(&textureTransform);
        TH08_WEB_RENDER_STAGE("creating GL context");
#ifdef TH08_MODERN_WEB
        EmscriptenWebGLContextAttributes attributes;
        emscripten_webgl_init_context_attributes(&attributes);
        attributes.alpha = EM_FALSE;
        attributes.depth = EM_TRUE;
        attributes.stencil = EM_FALSE;
        attributes.antialias = EM_FALSE;
        attributes.majorVersion = 2;
        attributes.minorVersion = 0;
        attributes.enableExtensionsByDefault = EM_TRUE;
        attributes.explicitSwapControl = EM_FALSE;
        if (g_webPresentationMode == WEB_PRESENTATION_AUTO)
        {
            bitmapPresentation = EM_ASM_INT({
                return typeof navigator != "undefined" && navigator.userAgent.includes("Firefox/");
            }) != 0;
        }
        else
        {
            bitmapPresentation = g_webPresentationMode == WEB_PRESENTATION_BITMAP;
        }
        attributes.proxyContextToMainThread = g_webPresentationMode == WEB_PRESENTATION_PROXY
                                                  ? EMSCRIPTEN_WEBGL_CONTEXT_PROXY_ALWAYS
                                                  : EMSCRIPTEN_WEBGL_CONTEXT_PROXY_DISALLOW;
        webContext = emscripten_webgl_create_context("#canvas", &attributes);
        if (webContext > 0 && emscripten_webgl_make_context_current(webContext) == EMSCRIPTEN_RESULT_SUCCESS)
        {
            context = reinterpret_cast<SDL_GLContext>(static_cast<intptr_t>(webContext));
            EM_ASM({
                Browser.useWebGL = true;
                Browser.moduleContextCreatedCallbacks.forEach((callback) => callback());
            });
        }
#else
        context = SDL_GL_CreateContext(window);
#endif
        TH08_WEB_RENDER_STAGE(context != NULL ? "GL context created" : "GL context creation failed");
        if (context == NULL) return;
#ifndef TH08_MODERN_WEB
        SDL_GL_MakeCurrent(window, context);
#endif
        TH08_WEB_RENDER_STAGE("GL context current");
#ifndef TH08_MODERN_WEB
        g_fogCoordf = reinterpret_cast<FogCoordfFunction>(SDL_GL_GetProcAddress("glFogCoordf"));
        if (g_fogCoordf == NULL)
            g_fogCoordf = reinterpret_cast<FogCoordfFunction>(SDL_GL_GetProcAddress("glFogCoordfEXT"));
        SDL_GL_SetSwapInterval(parameters.FullScreen_PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE ? 0 : 1);
#else
        if (!InitializeWebPipeline())
        {
            TH08_WEB_RENDER_STAGE("direct WebGL2 pipeline failed");
            return;
        }
#endif
        TH08_WEB_RENDER_STAGE("resetting render target");
        framebufferReady = ResetInternal(parameters);
        TH08_WEB_RENDER_STAGE(framebufferReady ? "render target ready" : "render target failed");
        renderStates[D3DRS_TEXTUREFACTOR] = 0xffffffffu;
        renderStates[D3DRS_SRCBLEND] = D3DBLEND_SRCALPHA;
        renderStates[D3DRS_DESTBLEND] = D3DBLEND_INVSRCALPHA;
        renderStates[D3DRS_ZWRITEENABLE] = TRUE;
        renderStates[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL; renderStates[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
        textureStates[D3DTSS_COLOROP] = D3DTOP_MODULATE;
        textureStates[D3DTSS_COLORARG1] = D3DTA_TEXTURE; textureStates[D3DTSS_COLORARG2] = D3DTA_DIFFUSE;
        textureStates[D3DTSS_ALPHAOP] = D3DTOP_MODULATE;
        textureStates[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE; textureStates[D3DTSS_ALPHAARG2] = D3DTA_DIFFUSE;
        textureStates[D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP; textureStates[D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
        textureStates[D3DTSS_MINFILTER] = D3DTEXF_POINT; textureStates[D3DTSS_MAGFILTER] = D3DTEXF_POINT;
        glDisable(GL_CULL_FACE);
#ifndef TH08_MODERN_WEB
        glDisable(GL_LIGHTING);
#endif
    }
    ~LinuxDevice()
    {
#ifdef TH08_MODERN_WEB
        if (webContext > 0)
            emscripten_webgl_make_context_current(webContext);
#else
        if (context != NULL) SDL_GL_MakeCurrent(window, context);
#endif
        if (texture != NULL) texture->Release();
        if (vertexBuffer != NULL) vertexBuffer->Release();
        if (backbuffer != NULL) backbuffer->Release();
        DestroyRenderTarget();
#ifdef TH08_MODERN_WEB
        DestroyWebPipeline();
        if (webContext > 0)
            emscripten_webgl_destroy_context(webContext);
#else
        if (context != NULL) SDL_GL_DeleteContext(context);
#endif
    }
    bool Ready() const { return context != NULL && backbuffer != NULL && framebufferReady; }
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG value = --refs; if (value == 0) delete this; return value; }
    HRESULT TestCooperativeLevel() { return S_OK; }
    HRESULT Reset(D3DPRESENT_PARAMETERS *parameters)
    {
        if (parameters == NULL) return E_INVALIDARG;
        framebufferReady = ResetInternal(*parameters);
        return framebufferReady ? S_OK : E_FAIL;
    }
    HRESULT Present(const RECT *, const RECT *, HWND, const RGNDATA *)
    {
#ifdef TH08_MODERN_WEB
        BeginWebFrame();
#endif
        backbuffer->FlushBackbuffer();
#ifdef TH08_MODERN_WEB
        FlushWebDraws();
#endif
        presentCount++;

        int drawableWidth, drawableHeight;
        SDL_GL_GetDrawableSize(window, &drawableWidth, &drawableHeight);
        g_framebufferApi.bindFramebuffer(GL_FRAMEBUFFER, 0);
        SelectDrawBuffer(GL_BACK);
        glViewport(0, 0, drawableWidth, drawableHeight);

        PushRendererState();
#ifdef TH08_MODERN_WEB
        // DrawWebBlit owns the texture binding and sampler state. Avoid
        // emitting three redundant commands per frame, especially on the
        // Firefox build where each GL call crosses the pthread proxy.
        DrawWebBlit(renderColorTexture, drawableWidth, drawableHeight, true, true);
        if (WebMeasurementActive() && ++g_webMeasuredFrames == 600)
        {
            fprintf(stderr,
                    "th08-web: renderer: 600-frame CPU window: %.3f ms avg / %.3f ms max game, %.3f / %.3f ms blit, %.3f / %.3f ms streaming uploads; %.1f flushes, %.1f draws, %.1f vertices per frame\n",
                    g_webGameFlushMilliseconds / g_webMeasuredFrames,
                    g_webMaximumGameFlushMilliseconds,
                    g_webBlitMilliseconds / g_webMeasuredFrames,
                    g_webMaximumBlitMilliseconds,
                    g_webUploadMilliseconds / g_webMeasuredFrames,
                    g_webMaximumUploadMilliseconds,
                    static_cast<double>(g_webMeasuredFlushes) / g_webMeasuredFrames,
                    static_cast<double>(g_webMeasuredCommands) / g_webMeasuredFrames,
                    static_cast<double>(g_webMeasuredVertices) / g_webMeasuredFrames);
            if (g_webPresentationDiagnostics)
                ResetWebMeasurementWindow();
            else
                g_webMeasurementComplete = true;
        }
#else
        glDisable(GL_ALPHA_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST); glDisable(GL_LIGHTING); glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, renderColorTexture);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        glOrtho(0.0, drawableWidth, drawableHeight, 0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glColor4ub(255, 255, 255, 255);
        glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(0.0f, 0.0f);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(static_cast<float>(drawableWidth), 0.0f);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f, static_cast<float>(drawableHeight));
        glTexCoord2f(1.0f, 0.0f); glVertex2f(static_cast<float>(drawableWidth),
                                            static_cast<float>(drawableHeight));
        glEnd();
        glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW);
#endif
        PopRendererState();

#ifdef TH08_MODERN_WEB
        glFlush();
        if (g_webPresentationMode == WEB_PRESENTATION_PROXY)
            emscripten_webgl_commit_frame();
        if (bitmapPresentation)
        {
            EM_ASM({
                try {
                    const diagnostics = !!$0;
                    const started = diagnostics ? performance.now() : 0;
                    const source = GL.currentContext && GL.currentContext.GLctx.canvas;
                    const bitmap = source.transferToImageBitmap();
                    const message = { th08WebFrame: bitmap };
                    if (diagnostics) {
                        message.transferMilliseconds = performance.now() - started;
                        message.producedAt = performance.timeOrigin + performance.now();
                    }
                    postMessage(message, [bitmap]);
                } catch (error) {
                    if (!Module.th08BitmapPresentationError) {
                        Module.th08BitmapPresentationError = true;
                        console.error("th08-web: Firefox bitmap presentation failed", error);
                    }
                }
            }, g_webPresentationDiagnostics ? 1 : 0);
        }
        EndWebFrame();
#endif
#ifndef TH08_MODERN_WEB
        glFlush();
        SDL_GL_SwapWindow(window);
#endif

        g_framebufferApi.bindFramebuffer(GL_FRAMEBUFFER, renderFramebuffer);
        SelectDrawBuffer(GL_COLOR_ATTACHMENT0);
        SelectReadBuffer(GL_COLOR_ATTACHMENT0);
        glViewport(0, 0, backbuffer->width, backbuffer->height);
        return S_OK;
    }
    HRESULT GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE, IDirect3DSurface8 **result)
    {
        if (index != 0 || result == NULL || backbuffer == NULL) return E_INVALIDARG;
        backbuffer->AddRef(); *result = backbuffer; return S_OK;
    }
    HRESULT CreateTexture(UINT width, UINT height, UINT, DWORD, D3DFORMAT format, D3DPOOL,
                          IDirect3DTexture8 **result)
    {
        if (result == NULL || width == 0 || height == 0) return E_INVALIDARG;
        if (format == D3DFMT_UNKNOWN) format = D3DFMT_A8R8G8B8;
        *result = new(std::nothrow) LinuxTexture(width, height, format);
        return *result != NULL ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT CreateVertexBuffer(UINT size, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer8 **result)
    {
        if (result == NULL) return E_INVALIDARG;
        *result = new(std::nothrow) LinuxVertexBuffer(size); return *result != NULL ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE, BOOL,
                               IDirect3DSurface8 **result)
    { return CreateSurface(width, height, format, result); }
    HRESULT CreateImageSurface(UINT width, UINT height, D3DFORMAT format, IDirect3DSurface8 **result)
    { return CreateSurface(width, height, format, result); }
    HRESULT CopyRects(IDirect3DSurface8 *sourceRaw, const RECT *sourceRects, UINT count,
                      IDirect3DSurface8 *destinationRaw, const POINT *destinationPoints)
    {
        LinuxSurfaceAccess source, destination;
        if (!th08_linux_surface_access(sourceRaw, &source, true) ||
            !th08_linux_surface_access(destinationRaw, &destination, false)) return E_INVALIDARG;
        if (source.format != destination.format) return E_NOTIMPL;
        if (count == 0) count = 1;
        UINT bytes = BytesPerPixel(source.format);
        for (UINT index = 0; index < count; ++index)
        {
            RECT rect;
            if (sourceRects != NULL) rect = sourceRects[index];
            else { rect.left = 0; rect.top = 0; rect.right = source.width; rect.bottom = source.height; }
            POINT point; point.x = destinationPoints != NULL ? destinationPoints[index].x : 0;
            point.y = destinationPoints != NULL ? destinationPoints[index].y : 0;
            UINT copyWidth = rect.right > rect.left ? rect.right - rect.left : 0;
            UINT copyHeight = rect.bottom > rect.top ? rect.bottom - rect.top : 0;
            if (point.x < 0 || point.y < 0 || rect.left < 0 || rect.top < 0) continue;
            if (static_cast<UINT>(point.x) + copyWidth > destination.width) copyWidth = destination.width - point.x;
            if (static_cast<UINT>(point.y) + copyHeight > destination.height) copyHeight = destination.height - point.y;
            for (UINT y = 0; y < copyHeight; ++y)
                memcpy(destination.pixels + (point.y + y) * destination.pitch + point.x * bytes,
                       source.pixels + (rect.top + y) * source.pitch + rect.left * bytes, copyWidth * bytes);
        }
        th08_linux_surface_changed(destinationRaw); return S_OK;
    }
    HRESULT BeginScene()
    {
#ifdef TH08_MODERN_WEB
        BeginWebFrame();
#endif
        const bool dialogPresent = th08::g_Gui.IsDialogPresent() != 0;
        if (dialogPresent && !wasDialogPresent)
            CaptureDialogueSnapshot();
        if (dialogPresent && dialogueSnapshotReady)
            RestoreDialogueSnapshot();
        wasDialogPresent = dialogPresent;
        return S_OK;
    }
    HRESULT EndScene() { return S_OK; }
    HRESULT Clear(DWORD, const D3DRECT *, DWORD flags, D3DCOLOR color, float depth, DWORD)
    {
#ifdef TH08_MODERN_WEB
        FlushWebDraws();
#endif
        if ((flags & D3DCLEAR_TARGET) && getenv("TH08_LINUX_RENDER_TRACE") != NULL)
        {
            FILE *trace = fopen("modern-render.txt", "ab");
            if (trace != NULL)
            {
                fprintf(trace,
                        "frame=%lu flags=%08lx color=%08lx viewport=%lu,%lu,%lu,%lu caller=%p\n",
                        presentCount, static_cast<unsigned long>(flags), static_cast<unsigned long>(color),
                        static_cast<unsigned long>(viewport.X), static_cast<unsigned long>(viewport.Y),
                        static_cast<unsigned long>(viewport.Width), static_cast<unsigned long>(viewport.Height),
                        __builtin_return_address(0));
                fclose(trace);
            }
        }

        GLbitfield mask = 0;
        if (flags & D3DCLEAR_TARGET)
        {
            glClearColor(((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f,
                         (color & 255) / 255.0f, ((color >> 24) & 255) / 255.0f);
            mask |= GL_COLOR_BUFFER_BIT;
        }
        if (flags & D3DCLEAR_ZBUFFER)
        {
#ifdef TH08_MODERN_WEB
            glClearDepthf(depth);
#else
            glClearDepth(depth);
#endif
            mask |= GL_DEPTH_BUFFER_BIT;
        }
        if (flags & D3DCLEAR_STENCIL) mask |= GL_STENCIL_BUFFER_BIT;
        const int targetWidth = backbuffer != NULL ? backbuffer->width : viewport.Width;
        const int targetHeight = backbuffer != NULL ? backbuffer->height : viewport.Height;
        const int left = static_cast<int>(viewport.X);
        const int bottom = targetHeight - static_cast<int>(viewport.Y + viewport.Height);
        const int width = static_cast<int>(viewport.Width);
        const int height = static_cast<int>(viewport.Height);
        glViewport(0, 0, targetWidth, targetHeight);
        glEnable(GL_SCISSOR_TEST);
        glScissor(left, bottom, width, height);
        if (flags & D3DCLEAR_ZBUFFER) glDepthMask(GL_TRUE);
        glClear(mask);
        if (flags & D3DCLEAR_ZBUFFER)
            glDepthMask(renderStates[D3DRS_ZWRITEENABLE] ? GL_TRUE : GL_FALSE);
        glDisable(GL_SCISSOR_TEST);
        return S_OK;
    }
    HRESULT SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix)
    {
        if (matrix == NULL) return E_INVALIDARG;
        if (state == D3DTS_WORLD) world = *matrix;
        else if (state == D3DTS_VIEW) view = *matrix;
        else if (state == D3DTS_PROJECTION) projection = *matrix;
        else if (state == D3DTS_TEXTURE0) textureTransform = *matrix;
        return S_OK;
    }
    HRESULT SetViewport(const D3DVIEWPORT8 *value)
    { if (value == NULL) return E_INVALIDARG; viewport = *value; return S_OK; }
    HRESULT GetViewport(D3DVIEWPORT8 *value)
    { if (value == NULL) return E_INVALIDARG; *value = viewport; return S_OK; }
    HRESULT SetRenderState(D3DRENDERSTATETYPE state, DWORD value)
    { if (static_cast<UINT>(state) < 256) renderStates[state] = value; return S_OK; }
    HRESULT SetTexture(DWORD stage, IDirect3DTexture8 *value)
    {
        if (stage != 0) return S_OK;
        LinuxTexture *next = static_cast<LinuxTexture *>(value);
        if (next != NULL) next->AddRef();
        if (texture != NULL) texture->Release(); texture = next; return S_OK;
    }
    HRESULT SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE state, DWORD value)
    { if (stage == 0 && static_cast<UINT>(state) < 32) textureStates[state] = value; return S_OK; }
    HRESULT SetVertexShader(DWORD value) { fvf = value; return S_OK; }
    HRESULT SetStreamSource(UINT stream, IDirect3DVertexBuffer8 *value, UINT stride)
    {
        if (stream != 0) return E_INVALIDARG;
        LinuxVertexBuffer *next = static_cast<LinuxVertexBuffer *>(value);
        if (next != NULL) next->AddRef();
        if (vertexBuffer != NULL) vertexBuffer->Release();
        vertexBuffer = next; streamStride = stride; return S_OK;
    }
    HRESULT DrawPrimitive(D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount)
    {
        if (vertexBuffer == NULL || streamStride == 0) return E_FAIL;
        UINT offset = startVertex * streamStride, count = VertexCount(type, primitiveCount);
        if (offset + count * streamStride > vertexBuffer->bytes.size()) return E_INVALIDARG;
        return Draw(type, primitiveCount, &vertexBuffer->bytes[offset], streamStride);
    }
    HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primitiveCount, const void *vertices, UINT stride)
    { return vertices != NULL ? Draw(type, primitiveCount, static_cast<const BYTE *>(vertices), stride) : E_INVALIDARG; }
    HRESULT GetDeviceCaps(D3DCAPS8 *caps)
    {
        if (caps == NULL) return E_INVALIDARG;
        memset(caps, 0, sizeof(*caps)); caps->DeviceType = D3DDEVTYPE_HAL;
        caps->Caps2 = D3DCAPS2_CANRENDERWINDOWED;
        caps->PresentationIntervals = D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_IMMEDIATE;
        caps->DevCaps = D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_HWRASTERIZATION |
                        D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                        D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TLVERTEXVIDEOMEMORY;
        caps->MaxTextureWidth = caps->MaxTextureHeight = 4096;
        caps->MaxTextureBlendStages = 1; caps->MaxSimultaneousTextures = 1;
        caps->MaxPrimitiveCount = 0x100000; caps->MaxStreams = 1; caps->MaxStreamStride = 256;
        caps->TextureOpCaps = D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_SELECTARG1;
        return S_OK;
    }
    HRESULT ResourceManagerDiscardBytes(DWORD) { return S_OK; }

  private:
    HRESULT CreateSurface(UINT width, UINT height, D3DFORMAT format, IDirect3DSurface8 **result)
    {
        if (result == NULL || width == 0 || height == 0) return E_INVALIDARG;
        if (format == D3DFMT_UNKNOWN) format = D3DFMT_A8R8G8B8;
        *result = new(std::nothrow) LinuxSurface(width, height, format, false, NULL);
        return *result != NULL ? S_OK : E_OUTOFMEMORY;
    }
    void DestroyRenderTarget()
    {
#ifdef TH08_MODERN_WEB
        FlushWebDraws();
#endif
        if (dialogueSnapshotTexture != 0)
            glDeleteTextures(1, &dialogueSnapshotTexture);
        if (renderDepthBuffer != 0 && g_framebufferApi.deleteRenderbuffers != NULL)
            g_framebufferApi.deleteRenderbuffers(1, &renderDepthBuffer);
        if (renderFramebuffer != 0 && g_framebufferApi.deleteFramebuffers != NULL)
            g_framebufferApi.deleteFramebuffers(1, &renderFramebuffer);
        if (renderColorTexture != 0)
            glDeleteTextures(1, &renderColorTexture);
        renderDepthBuffer = renderFramebuffer = renderColorTexture = dialogueSnapshotTexture = 0;
        dialogueSnapshotReady = false;
        wasDialogPresent = false;
    }
    bool CreateRenderTarget(UINT width, UINT height)
    {
        if (!g_framebufferApi.Initialize())
        {
            fprintf(stderr, "th08-modern: OpenGL framebuffer objects are unavailable\n");
            return false;
        }

        glGenTextures(1, &renderColorTexture);
        glBindTexture(GL_TEXTURE_2D, renderColorTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0,
#ifdef TH08_MODERN_WEB
                     GL_RGBA8,
#else
                     GL_RGBA,
#endif
                     width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

        glGenTextures(1, &dialogueSnapshotTexture);
        glBindTexture(GL_TEXTURE_2D, dialogueSnapshotTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0,
#ifdef TH08_MODERN_WEB
                     GL_RGBA8,
#else
                     GL_RGBA,
#endif
                     width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

        g_framebufferApi.genRenderbuffers(1, &renderDepthBuffer);
        g_framebufferApi.bindRenderbuffer(GL_RENDERBUFFER, renderDepthBuffer);
        g_framebufferApi.renderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);

        g_framebufferApi.genFramebuffers(1, &renderFramebuffer);
        g_framebufferApi.bindFramebuffer(GL_FRAMEBUFFER, renderFramebuffer);
        g_framebufferApi.framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                              GL_TEXTURE_2D, renderColorTexture, 0);
        g_framebufferApi.framebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                                 GL_RENDERBUFFER, renderDepthBuffer);
        SelectDrawBuffer(GL_COLOR_ATTACHMENT0);
        SelectReadBuffer(GL_COLOR_ATTACHMENT0);
        GLenum status = g_framebufferApi.checkFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            fprintf(stderr, "th08-modern: unable to create OpenGL framebuffer (status 0x%04x)\n",
                    static_cast<unsigned int>(status));
            DestroyRenderTarget();
            return false;
        }
        return true;
    }
    void CaptureDialogueSnapshot()
    {
        if (dialogueSnapshotTexture == 0 || backbuffer == NULL)
            return;
#ifdef TH08_MODERN_WEB
        FlushWebDraws();
#endif
        glBindTexture(GL_TEXTURE_2D, dialogueSnapshotTexture);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, backbuffer->width, backbuffer->height);
        dialogueSnapshotReady = true;
    }
    void RestoreDialogueSnapshot()
    {
        const UINT width = backbuffer->width;
        const UINT height = backbuffer->height;
        PushRendererState();
#ifdef TH08_MODERN_WEB
        DrawWebBlit(dialogueSnapshotTexture, width, height, true, true);
#else
        glDisable(GL_ALPHA_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST); glDisable(GL_LIGHTING); glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, dialogueSnapshotTexture);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        glOrtho(0.0, width, height, 0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        glColor4ub(255, 255, 255, 255);
        glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(0.0f, 0.0f);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(static_cast<float>(width), 0.0f);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f, static_cast<float>(height));
        glTexCoord2f(1.0f, 0.0f); glVertex2f(static_cast<float>(width), static_cast<float>(height));
        glEnd();
        glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW);
#endif
        PopRendererState();
    }
    bool ResetInternal(const D3DPRESENT_PARAMETERS &parameters)
    {
        UINT width = parameters.BackBufferWidth != 0 ? parameters.BackBufferWidth : 640;
        UINT height = parameters.BackBufferHeight != 0 ? parameters.BackBufferHeight : 480;
        D3DFORMAT format = parameters.BackBufferFormat;
        if (format == D3DFMT_UNKNOWN) format = D3DFMT_X8R8G8B8;
        DestroyRenderTarget();
        if (backbuffer != NULL) backbuffer->Release();
        backbuffer = new LinuxSurface(width, height, format, true, NULL);
        if (backbuffer == NULL || !CreateRenderTarget(width, height)) return false;
        viewport.X = viewport.Y = 0; viewport.Width = width; viewport.Height = height;
        viewport.MinZ = 0.0f; viewport.MaxZ = 1.0f;
        glViewport(0, 0, width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
#ifdef TH08_MODERN_WEB
        glClearDepthf(1.0f);
#else
        glClearDepth(1.0);
#endif
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        return true;
    }
    void TransformPosition(const float *position, bool transformed, float *xOut, float *yOut,
                           float *zOut, float *fogCoordinateOut)
    {
        if (transformed)
        {
            // D3D8 pre-transformed vertices use integer pixel centers, while
            // OpenGL samples at half-integer centers.
            *xOut = position[0] + 0.5f; *yOut = position[1] + 0.5f; *zOut = position[2];
            *fogCoordinateOut = 0.0f;
            return;
        }
        float vector[4] = {position[0], position[1], position[2], 1.0f};
        const D3DMATRIX *matrices[3] = {&world, &view, &projection};
        for (int index = 0; index < 3; ++index)
        {
            const D3DMATRIX &m = *matrices[index]; float next[4];
            next[0] = vector[0] * m._11 + vector[1] * m._21 + vector[2] * m._31 + vector[3] * m._41;
            next[1] = vector[0] * m._12 + vector[1] * m._22 + vector[2] * m._32 + vector[3] * m._42;
            next[2] = vector[0] * m._13 + vector[1] * m._23 + vector[2] * m._33 + vector[3] * m._43;
            next[3] = vector[0] * m._14 + vector[1] * m._24 + vector[2] * m._34 + vector[3] * m._44;
            memcpy(vector, next, sizeof(vector));
            if (index == 1)
                *fogCoordinateOut = fabsf(vector[2]);
        }
        float reciprocal = fabsf(vector[3]) > 1.0e-8f ? 1.0f / vector[3] : 1.0f;
        *xOut = viewport.X + (vector[0] * reciprocal + 1.0f) * viewport.Width * 0.5f;
        *yOut = viewport.Y + (1.0f - vector[1] * reciprocal) * viewport.Height * 0.5f;
        *zOut = viewport.MinZ + vector[2] * reciprocal * (viewport.MaxZ - viewport.MinZ);
    }
#ifndef TH08_MODERN_WEB
    void PrepareState()
    {
        const UINT width = backbuffer != NULL ? backbuffer->width : viewport.Width;
        const UINT height = backbuffer != NULL ? backbuffer->height : viewport.Height;
        const int drawableWidth = width;
        const int drawableHeight = height;
        glEnable(GL_SCISSOR_TEST);
        glScissor(static_cast<int>(viewport.X * drawableWidth / width),
                  drawableHeight - static_cast<int>((viewport.Y + viewport.Height) * drawableHeight / height),
                  static_cast<int>(viewport.Width * drawableWidth / width),
                  static_cast<int>(viewport.Height * drawableHeight / height));
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0.0, width, height, 0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        if (renderStates[D3DRS_ALPHABLENDENABLE])
        { glEnable(GL_BLEND); glBlendFunc(BlendFunction(renderStates[D3DRS_SRCBLEND]), BlendFunction(renderStates[D3DRS_DESTBLEND])); }
        else glDisable(GL_BLEND);
        if (renderStates[D3DRS_ALPHATESTENABLE])
        { glEnable(GL_ALPHA_TEST); glAlphaFunc(CompareFunction(renderStates[D3DRS_ALPHAFUNC]), (renderStates[D3DRS_ALPHAREF] & 255) / 255.0f); }
        else glDisable(GL_ALPHA_TEST);
        if (renderStates[D3DRS_ZENABLE]) { glEnable(GL_DEPTH_TEST); glDepthFunc(CompareFunction(renderStates[D3DRS_ZFUNC])); }
        else glDisable(GL_DEPTH_TEST);
        glDepthMask(renderStates[D3DRS_ZWRITEENABLE] ? GL_TRUE : GL_FALSE);
        if (renderStates[D3DRS_FOGENABLE] &&
            renderStates[D3DRS_FOGVERTEXMODE] == D3DFOG_LINEAR && g_fogCoordf != NULL)
        {
            const DWORD color = renderStates[D3DRS_FOGCOLOR];
            const GLfloat fogColor[4] = {
                ((color >> 16) & 255) / 255.0f,
                ((color >> 8) & 255) / 255.0f,
                (color & 255) / 255.0f,
                1.0f
            };
            GLfloat fogStart, fogEnd;
            memcpy(&fogStart, &renderStates[D3DRS_FOGSTART], sizeof(fogStart));
            memcpy(&fogEnd, &renderStates[D3DRS_FOGEND], sizeof(fogEnd));
            glEnable(GL_FOG);
            glFogi(GL_FOG_MODE, GL_LINEAR);
            glFogi(GL_FOG_COORDINATE_SOURCE, GL_FOG_COORDINATE);
            glFogfv(GL_FOG_COLOR, fogColor);
            glFogf(GL_FOG_START, fogStart);
            glFogf(GL_FOG_END, fogEnd);
        }
        else
        {
            glDisable(GL_FOG);
        }
        const bool colorUsesTexture = TextureOperationUsesTexture(
            textureStates[D3DTSS_COLOROP], textureStates[D3DTSS_COLORARG1],
            textureStates[D3DTSS_COLORARG2]);
        const bool alphaUsesTexture = TextureOperationUsesTexture(
            textureStates[D3DTSS_ALPHAOP], textureStates[D3DTSS_ALPHAARG1],
            textureStates[D3DTSS_ALPHAARG2]);
        if (texture != NULL && (colorUsesTexture || alphaUsesTexture))
        {
            texture->Upload(); glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, texture->glName);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, textureStates[D3DTSS_MINFILTER] == D3DTEXF_LINEAR ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, textureStates[D3DTSS_MAGFILTER] == D3DTEXF_LINEAR ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, textureStates[D3DTSS_ADDRESSU] == D3DTADDRESS_CLAMP ? GL_CLAMP : GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, textureStates[D3DTSS_ADDRESSV] == D3DTADDRESS_CLAMP ? GL_CLAMP : GL_REPEAT);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
            ConfigureTextureComponent(GL_COMBINE_RGB, GL_SOURCE0_RGB, GL_SOURCE1_RGB,
                                      GL_OPERAND0_RGB, GL_OPERAND1_RGB,
                                      textureStates[D3DTSS_COLOROP],
                                      textureStates[D3DTSS_COLORARG1],
                                      textureStates[D3DTSS_COLORARG2], GL_SRC_COLOR);
            ConfigureTextureComponent(GL_COMBINE_ALPHA, GL_SOURCE0_ALPHA, GL_SOURCE1_ALPHA,
                                      GL_OPERAND0_ALPHA, GL_OPERAND1_ALPHA,
                                      textureStates[D3DTSS_ALPHAOP],
                                      textureStates[D3DTSS_ALPHAARG1],
                                      textureStates[D3DTSS_ALPHAARG2], GL_SRC_ALPHA);
            const DWORD factor = renderStates[D3DRS_TEXTUREFACTOR];
            const GLfloat constantColor[4] = {
                ((factor >> 16) & 255) / 255.0f,
                ((factor >> 8) & 255) / 255.0f,
                (factor & 255) / 255.0f,
                ((factor >> 24) & 255) / 255.0f
            };
            glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, constantColor);
        }
        else glDisable(GL_TEXTURE_2D);
    }
    D3DCOLOR EffectiveColor(D3DCOLOR diffuse)
    {
        if (texture != NULL &&
            (TextureOperationUsesTexture(textureStates[D3DTSS_COLOROP],
                                         textureStates[D3DTSS_COLORARG1],
                                         textureStates[D3DTSS_COLORARG2]) ||
             TextureOperationUsesTexture(textureStates[D3DTSS_ALPHAOP],
                                         textureStates[D3DTSS_ALPHAARG1],
                                         textureStates[D3DTSS_ALPHAARG2])))
            return diffuse;
        DWORD factor = renderStates[D3DRS_TEXTUREFACTOR];
        DWORD colorOp = textureStates[D3DTSS_COLOROP], alphaOp = textureStates[D3DTSS_ALPHAOP];
        DWORD colorArg1 = textureStates[D3DTSS_COLORARG1] & D3DTA_SELECTMASK;
        DWORD colorArg2 = textureStates[D3DTSS_COLORARG2] & D3DTA_SELECTMASK;
        DWORD alphaArg1 = textureStates[D3DTSS_ALPHAARG1] & D3DTA_SELECTMASK;
        DWORD alphaArg2 = textureStates[D3DTSS_ALPHAARG2] & D3DTA_SELECTMASK;
        DWORD modifier = colorArg2 == D3DTA_TFACTOR ? factor : diffuse;
        DWORD alphaModifier = alphaArg2 == D3DTA_TFACTOR ? factor : diffuse;
        DWORD rgb = modifier & 0x00ffffffu, alpha = alphaModifier & 0xff000000u;
        if (colorOp == D3DTOP_SELECTARG1 && colorArg1 == D3DTA_TEXTURE) rgb = 0x00ffffffu;
        else if (colorOp == D3DTOP_SELECTARG1 && colorArg1 == D3DTA_DIFFUSE) rgb = diffuse & 0x00ffffffu;
        if (alphaOp == D3DTOP_SELECTARG1 && alphaArg1 == D3DTA_TEXTURE) alpha = 0xff000000u;
        else if (alphaOp == D3DTOP_SELECTARG1 && alphaArg1 == D3DTA_DIFFUSE) alpha = diffuse & 0xff000000u;
        return alpha | rgb;
    }
#else
    D3DCOLOR WebArgumentColor(DWORD argument, D3DCOLOR diffuse) const
    {
        switch (argument & D3DTA_SELECTMASK)
        {
        case D3DTA_TEXTURE: return 0xffffffffu;
        case D3DTA_TFACTOR: return renderStates[D3DRS_TEXTUREFACTOR];
        default: return diffuse;
        }
    }

    D3DCOLOR ModulateWebColors(D3DCOLOR left, D3DCOLOR right) const
    {
        const DWORD alpha = (((left >> 24) & 255) * ((right >> 24) & 255) / 255) << 24;
        const DWORD red = (((left >> 16) & 255) * ((right >> 16) & 255) / 255) << 16;
        const DWORD green = (((left >> 8) & 255) * ((right >> 8) & 255) / 255) << 8;
        const DWORD blue = (left & 255) * (right & 255) / 255;
        return alpha | red | green | blue;
    }

    D3DCOLOR WebVertexCoefficient(D3DCOLOR diffuse) const
    {
        const DWORD colorOperation = textureStates[D3DTSS_COLOROP];
        const DWORD colorArgument1 = textureStates[D3DTSS_COLORARG1];
        const DWORD colorArgument2 = textureStates[D3DTSS_COLORARG2];
        const DWORD alphaOperation = textureStates[D3DTSS_ALPHAOP];
        const DWORD alphaArgument1 = textureStates[D3DTSS_ALPHAARG1];
        const DWORD alphaArgument2 = textureStates[D3DTSS_ALPHAARG2];
        D3DCOLOR color;
        if (colorOperation == D3DTOP_SELECTARG1)
            color = WebArgumentColor(colorArgument1, diffuse);
        else
            color = ModulateWebColors(WebArgumentColor(colorArgument1, diffuse),
                                      WebArgumentColor(colorArgument2, diffuse));
        D3DCOLOR alpha;
        if (alphaOperation == D3DTOP_SELECTARG1)
            alpha = WebArgumentColor(alphaArgument1, diffuse);
        else
            alpha = ModulateWebColors(WebArgumentColor(alphaArgument1, diffuse),
                                      WebArgumentColor(alphaArgument2, diffuse));
        return (color & 0x00ffffffu) | (alpha & 0xff000000u);
    }

    GLfloat WebFogFactor(GLfloat coordinate) const
    {
        if (!renderStates[D3DRS_FOGENABLE] ||
            renderStates[D3DRS_FOGVERTEXMODE] != D3DFOG_LINEAR)
            return 1.0f;
        GLfloat start, end;
        memcpy(&start, &renderStates[D3DRS_FOGSTART], sizeof(start));
        memcpy(&end, &renderStates[D3DRS_FOGEND], sizeof(end));
        const GLfloat span = end - start;
        if (fabsf(span) <= 1.0e-8f)
            return coordinate <= start ? 1.0f : 0.0f;
        const GLfloat factor = (end - coordinate) / span;
        if (factor < 0.0f) return 0.0f;
        if (factor > 1.0f) return 1.0f;
        return factor;
    }

    void PrepareWebState(WebDrawState *state)
    {
        const UINT width = backbuffer != NULL ? backbuffer->width : viewport.Width;
        const UINT height = backbuffer != NULL ? backbuffer->height : viewport.Height;
        const bool colorUsesTexture = TextureOperationUsesTexture(
            textureStates[D3DTSS_COLOROP], textureStates[D3DTSS_COLORARG1],
            textureStates[D3DTSS_COLORARG2]);
        const bool alphaUsesTexture = TextureOperationUsesTexture(
            textureStates[D3DTSS_ALPHAOP], textureStates[D3DTSS_ALPHAARG1],
            textureStates[D3DTSS_ALPHAARG2]);
        const bool useTexture = texture != NULL && (colorUsesTexture || alphaUsesTexture);
        if (useTexture)
            texture->Upload();

        memset(state, 0, sizeof(*state));
        state->texture = useTexture ? texture->glName : 0;
        state->textureEnabled = useTexture;
        state->blendEnabled = renderStates[D3DRS_ALPHABLENDENABLE] != 0;
        state->sourceBlend = renderStates[D3DRS_SRCBLEND];
        state->destinationBlend = renderStates[D3DRS_DESTBLEND];
        state->depthTestEnabled = renderStates[D3DRS_ZENABLE] != 0;
        state->depthWriteEnabled = renderStates[D3DRS_ZWRITEENABLE] != 0;
        state->depthFunction = renderStates[D3DRS_ZFUNC];
        state->scissorEnabled = true;
        state->scissorX = static_cast<GLint>(viewport.X);
        state->scissorY = static_cast<GLint>(height - viewport.Y - viewport.Height);
        state->scissorWidth = static_cast<GLsizei>(viewport.Width);
        state->scissorHeight = static_cast<GLsizei>(viewport.Height);
        state->minFilter = textureStates[D3DTSS_MINFILTER];
        state->magFilter = textureStates[D3DTSS_MAGFILTER];
        state->addressU = textureStates[D3DTSS_ADDRESSU];
        state->addressV = textureStates[D3DTSS_ADDRESSV];
        state->rgbUsesTexture = useTexture && colorUsesTexture;
        state->alphaUsesTexture = useTexture && alphaUsesTexture;
        state->colorOperation = textureStates[D3DTSS_COLOROP];
        state->colorArgument1 = textureStates[D3DTSS_COLORARG1];
        state->colorArgument2 = textureStates[D3DTSS_COLORARG2];
        state->alphaOperation = textureStates[D3DTSS_ALPHAOP];
        state->alphaArgument1 = textureStates[D3DTSS_ALPHAARG1];
        state->alphaArgument2 = textureStates[D3DTSS_ALPHAARG2];
        state->textureFactor = renderStates[D3DRS_TEXTUREFACTOR];
        state->alphaTestEnabled = renderStates[D3DRS_ALPHATESTENABLE] != 0;
        state->alphaFunction = renderStates[D3DRS_ALPHAFUNC];
        state->alphaReference = renderStates[D3DRS_ALPHAREF];
        state->alphaThreshold = -1.0f;
        if (state->alphaTestEnabled)
        {
            if (state->alphaFunction == D3DCMP_GREATEREQUAL)
                state->alphaThreshold = (state->alphaReference & 255) / 255.0f;
            else if (state->alphaFunction == D3DCMP_NEVER)
                state->alphaThreshold = 2.0f;
        }
        state->fogEnabled = renderStates[D3DRS_FOGENABLE] != 0 &&
                            renderStates[D3DRS_FOGVERTEXMODE] == D3DFOG_LINEAR;
        state->fogColor = renderStates[D3DRS_FOGCOLOR];
        memcpy(&state->fogStart, &renderStates[D3DRS_FOGSTART], sizeof(state->fogStart));
        memcpy(&state->fogEnd, &renderStates[D3DRS_FOGEND], sizeof(state->fogEnd));
        state->viewportWidth = static_cast<GLfloat>(width);
        state->viewportHeight = static_cast<GLfloat>(height);
    }
#endif
    HRESULT Draw(D3DPRIMITIVETYPE type, UINT primitiveCount, const BYTE *data, UINT stride)
    {
        UINT count = VertexCount(type, primitiveCount);
        if (count == 0)
            return S_OK;
        bool transformed = (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
        UINT offset = transformed ? 16 : 12;
        if (fvf & D3DFVF_NORMAL) offset += 12;
        if (fvf & D3DFVF_PSIZE) offset += 4;
        bool hasDiffuse = (fvf & D3DFVF_DIFFUSE) != 0; UINT colorOffset = offset;
        if (hasDiffuse) offset += 4;
        if (fvf & D3DFVF_SPECULAR) offset += 4;
        bool hasTexture = (fvf & D3DFVF_TEXCOUNT_MASK) != 0; UINT textureOffset = offset;
#ifdef TH08_MODERN_WEB
        WebDrawState state;
        PrepareWebState(&state);
        const size_t firstVertex = g_webQueuedVertices.size();
        g_webQueuedVertices.resize(firstVertex + count);
#else
        PrepareState(); glBegin(PrimitiveMode(type));
#endif
        for (UINT index = 0; index < count; ++index)
        {
            const BYTE *vertex = data + index * stride; float x, y, z, fogCoordinate;
            TransformPosition(reinterpret_cast<const float *>(vertex), transformed, &x, &y, &z,
                              &fogCoordinate);
            D3DCOLOR color = hasDiffuse ? *reinterpret_cast<const D3DCOLOR *>(vertex + colorOffset) : 0xffffffffu;
#ifndef TH08_MODERN_WEB
            color = EffectiveColor(color);
#endif
            float u = 0.0f, v = 0.0f;
            if (hasTexture)
            {
                const float *uv = reinterpret_cast<const float *>(vertex + textureOffset);
                u = uv[0]; v = uv[1];
                if (!transformed)
                {
                    u = uv[0] * textureTransform._11 + uv[1] * textureTransform._21 + textureTransform._31;
                    v = uv[0] * textureTransform._12 + uv[1] * textureTransform._22 + textureTransform._32;
                }
#ifndef TH08_MODERN_WEB
                glTexCoord2f(u, v);
#endif
            }
#ifdef TH08_MODERN_WEB
            color = WebVertexCoefficient(color);
            WebVertex &output = g_webQueuedVertices[firstVertex + index];
            // TransformPosition returns D3D's [0, 1] post-transform depth.
            // The desktop path applies an OpenGL projection that negates the
            // submitted 1-2z value; the direct shader has no such matrix, so
            // map near/far to WebGL clip space here instead.
            output.x = x; output.y = y; output.z = 2.0f * z - 1.0f;
            output.u = u; output.v = v; output.fogCoordinate = WebFogFactor(fogCoordinate);
            output.red = static_cast<GLubyte>((color >> 16) & 255);
            output.green = static_cast<GLubyte>((color >> 8) & 255);
            output.blue = static_cast<GLubyte>(color & 255);
            output.alpha = static_cast<GLubyte>((color >> 24) & 255);
#else
            glColor4ub((color >> 16) & 255, (color >> 8) & 255, color & 255, (color >> 24) & 255);
            if (g_fogCoordf != NULL)
                g_fogCoordf(fogCoordinate);
            glVertex3f(x, y, 1.0f - 2.0f * z);
#endif
        }
#ifdef TH08_MODERN_WEB
        return QueueWebVertexRange(PrimitiveMode(type), firstVertex, count, state) ? S_OK : E_FAIL;
#else
        glEnd(); return S_OK;
#endif
    }
    ULONG refs;
    SDL_Window *window;
    SDL_GLContext context;
    LinuxSurface *backbuffer;
    LinuxTexture *texture;
    LinuxVertexBuffer *vertexBuffer;
#ifdef TH08_MODERN_WEB
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE webContext;
    bool bitmapPresentation;
#endif
    DWORD fvf;
    UINT streamStride;
    GLuint renderFramebuffer, renderColorTexture, renderDepthBuffer, dialogueSnapshotTexture;
    bool framebufferReady, dialogueSnapshotReady, wasDialogPresent;
    unsigned long presentCount;
    DWORD renderStates[256], textureStates[32];
    D3DMATRIX world, view, projection, textureTransform;
    D3DVIEWPORT8 viewport;
};

class LinuxDirect3D : public IDirect3D8
{
  public:
    LinuxDirect3D() : refs(1) {}
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG value = --refs; if (value == 0) delete this; return value; }
    HRESULT GetAdapterDisplayMode(UINT, D3DDISPLAYMODE *mode)
    {
        if (mode == NULL) return E_INVALIDARG;
        mode->Width = 640; mode->Height = 480; mode->RefreshRate = 60; mode->Format = D3DFMT_X8R8G8B8; return S_OK;
    }
    HRESULT CheckDeviceFormat(UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT) { return S_OK; }
    HRESULT CreateDevice(UINT, D3DDEVTYPE, HWND window, DWORD, D3DPRESENT_PARAMETERS *parameters,
                         IDirect3DDevice8 **result)
    {
        if (window == NULL || parameters == NULL || result == NULL) return E_INVALIDARG;
        LinuxDevice *device = new(std::nothrow) LinuxDevice(reinterpret_cast<SDL_Window *>(window), *parameters);
        if (device == NULL) return E_OUTOFMEMORY;
        if (!device->Ready()) { delete device; *result = NULL; return E_FAIL; }
        *result = device; return S_OK;
    }
  private: ULONG refs;
};
} // namespace

#ifdef TH08_MODERN_WEB
extern "C" EMSCRIPTEN_KEEPALIVE void th08_web_configure_presentation(int mode, int diagnostics)
{
    if (mode >= WEB_PRESENTATION_DIRECT && mode <= WEB_PRESENTATION_PROXY)
        g_webPresentationMode = mode;
    else
        g_webPresentationMode = WEB_PRESENTATION_AUTO;
    g_webPresentationDiagnostics = diagnostics != 0;
}
#endif

bool th08_linux_surface_access(IDirect3DSurface8 *surfaceRaw, LinuxSurfaceAccess *access, bool readBackbuffer)
{
    if (surfaceRaw == NULL || access == NULL) return false;
    LinuxSurface *surface = static_cast<LinuxSurface *>(surfaceRaw);
    if (surface->backbuffer || readBackbuffer) surface->ReadBackbuffer();
    access->pixels = surface->pixels.empty() ? NULL : &surface->pixels[0];
    access->width = surface->width; access->height = surface->height;
    access->pitch = surface->pitch; access->format = surface->format; return true;
}

void th08_linux_surface_changed(IDirect3DSurface8 *surfaceRaw)
{
    if (surfaceRaw == NULL) return;
    LinuxSurface *surface = static_cast<LinuxSurface *>(surfaceRaw);
    surface->dirty = true;
    if (surface->owner != NULL) surface->owner->uploaded = false;
    surface->FlushBackbuffer();
}

extern "C" IDirect3D8 *Direct3DCreate8(UINT sdkVersion)
{ return sdkVersion == D3D_SDK_VERSION ? new(std::nothrow) LinuxDirect3D() : NULL; }
