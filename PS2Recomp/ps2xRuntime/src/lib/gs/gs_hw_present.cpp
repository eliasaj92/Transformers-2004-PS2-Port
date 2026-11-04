// SPDX-License-Identifier: GPL-3.0-or-later
// Keep the existing raylib presenter/overlays, importing D3D12 frames on the GPU.
#include "gs_hw_present.h"
#include "ps2_host_backend.h"
#include "runtime/gs/gs_hw_backend.h"
#include "runtime/ps2_settings.h"
#include "../Kernel/TestScript.h"
#include "../debug_counters.h"
#include "../ps2_playtest.h"
#include "rlgl.h"
#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#ifdef PS2X_WITH_HWGS
#include "tf_hwgs.h"
#include "external/glad.h"
extern "C" { typedef void (*TfGLProc)(void); TfGLProc glfwGetProcAddress(const char* name); }
namespace {
constexpr GLenum MEMORY_DEDICATED=0x9581, HANDLE_D3D12_RESOURCE=0x958A;
constexpr GLenum HANDLE_D3D12_FENCE=0x9594,FENCE_VALUE=0x9595,LAYOUT_GENERAL=0x958D,DEVICE_LUID=0x9599;
#define TF_GL(name,result,args) using Fn##name=result (GLAD_API_PTR*) args; Fn##name p##name=nullptr
TF_GL(CreateMemoryObjectsEXT,void,(GLsizei,GLuint*));
TF_GL(DeleteMemoryObjectsEXT,void,(GLsizei,const GLuint*));
TF_GL(MemoryObjectParameterivEXT,void,(GLuint,GLenum,const GLint*));
TF_GL(ImportMemoryWin32HandleEXT,void,(GLuint,GLuint64,GLenum,void*));
TF_GL(TexStorageMem2DEXT,void,(GLenum,GLsizei,GLenum,GLsizei,GLsizei,GLuint,GLuint64));
TF_GL(GenSemaphoresEXT,void,(GLsizei,GLuint*));
TF_GL(DeleteSemaphoresEXT,void,(GLsizei,const GLuint*));
TF_GL(ImportSemaphoreWin32HandleEXT,void,(GLuint,GLenum,void*));
TF_GL(SemaphoreParameterui64vEXT,void,(GLuint,GLenum,const GLuint64*));
TF_GL(WaitSemaphoreEXT,void,(GLuint,GLuint,const GLuint*,GLuint,const GLuint*,const GLenum*));
TF_GL(SignalSemaphoreEXT,void,(GLuint,GLuint,const GLuint*,GLuint,const GLuint*,const GLenum*));
TF_GL(GetUnsignedBytevEXT,void,(GLenum,GLubyte*));
#undef TF_GL
struct Imported { GLuint texture=0,memory=0,semaphore=0; uint32_t generation=0; };
std::array<Imported,3> imported;
std::array<uint8_t,8> luid{};
GLuint readFbo=0,drawFbo=0;
bool initialized=false,haveFrame=false;
uint32_t lastDisplayFbp=UINT32_MAX;
uint64_t lastSequence=0;
void check(const char* action) {
    const auto error=glGetError();
    if(error!=GL_NO_ERROR) {
        char message[192];std::snprintf(message,sizeof(message),"HWGS direct: %s (GL error 0x%x)",action,error);
        throw std::runtime_error(message);
    }
}
bool extension(const char* name) {
    GLint count=0;glGetIntegerv(GL_NUM_EXTENSIONS,&count);
    for(GLint i=0;i<count;++i) {
        const auto* value=reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS,static_cast<GLuint>(i)));
        if(value && std::strcmp(value,name)==0)return true;
    }
    return false;
}
void destroy(Imported& i) {
    if(i.texture)glDeleteTextures(1,&i.texture);
    if(i.memory)pDeleteMemoryObjectsEXT(1,&i.memory);
    if(i.semaphore)pDeleteSemaphoresEXT(1,&i.semaphore);
    i={};
}
void importFrame(const TfGpuFrame& frame,Imported& i) {
    if(i.generation==frame.generation && i.texture)return;
    if(std::memcmp(luid.data(),frame.adapter_luid,luid.size())!=0)
        throw std::runtime_error("HWGS direct requires the renderer and window on the same GPU");
    destroy(i);
    pCreateMemoryObjectsEXT(1,&i.memory);
    const GLint dedicated=GL_TRUE;
    pMemoryObjectParameterivEXT(i.memory,MEMORY_DEDICATED,&dedicated);
    // Khronos specifies size is ignored for D3D resources; use 0 for compatibility.
    pImportMemoryWin32HandleEXT(i.memory,0,HANDLE_D3D12_RESOURCE,frame.memory_handle);
    check("import shared texture memory");
    glGenTextures(1,&i.texture);glBindTexture(GL_TEXTURE_2D,i.texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    pTexStorageMem2DEXT(GL_TEXTURE_2D,1,GL_RGBA8,frame.texture_width,frame.texture_height,i.memory,0);
    check("bind shared texture storage");
    pGenSemaphoresEXT(1,&i.semaphore);
    pImportSemaphoreWin32HandleEXT(i.semaphore,HANDLE_D3D12_FENCE,frame.fence_handle);
    check("import GPU fence");
    i.generation=frame.generation;
}
void captureRequested(const Texture2D& texture) {
    if(!haveFrame)return;
    char path[512];
    if(!ps2_test::takeDumpRequest(path,sizeof(path)))return;
    // Explicit diagnostic captures may read pixels, but never re-upload them.
    // Normal presentation, including scripted frames without a capture, stays GPU-only.
    Image image=LoadImageFromTexture(texture);
    ExportImage(image,path);UnloadImage(image);
    std::fprintf(stderr,"[hwgs:direct] requested capture=%s size=%dx%d\n",path,texture.width,texture.height);
}
}
#endif

bool ps2HwgsDirectEnabled() {
#ifdef PS2X_WITH_HWGS
    // Backend changes are restart-only. Pending menu edits must not switch the
    // active presenter away from the already-open hardware device.
    static const bool enabled=ps2x::settings::get("renderer.backend")=="hwgs" && tf_gs_direct_enabled();
    return enabled;
#else
    return false;
#endif
}
void ps2HwgsDirectInitialize() {
#ifdef PS2X_WITH_HWGS
    if(!ps2HwgsDirectEnabled() || initialized)return;
    for(const char* name:{"GL_EXT_memory_object_win32","GL_EXT_semaphore_win32"})
        if(!extension(name))throw std::runtime_error(std::string("HWGS direct requires ")+name);
#define LOAD(name) p##name=reinterpret_cast<Fn##name>(glfwGetProcAddress("gl" #name));if(!p##name)throw std::runtime_error("HWGS direct missing gl" #name)
    LOAD(CreateMemoryObjectsEXT);LOAD(DeleteMemoryObjectsEXT);LOAD(MemoryObjectParameterivEXT);
    LOAD(ImportMemoryWin32HandleEXT);LOAD(TexStorageMem2DEXT);LOAD(GenSemaphoresEXT);
    LOAD(DeleteSemaphoresEXT);LOAD(ImportSemaphoreWin32HandleEXT);LOAD(SemaphoreParameterui64vEXT);
    LOAD(WaitSemaphoreEXT);LOAD(SignalSemaphoreEXT);LOAD(GetUnsignedBytevEXT);
#undef LOAD
    pGetUnsignedBytevEXT(DEVICE_LUID,luid.data());check("query OpenGL adapter");
    glGenFramebuffers(1,&readFbo);glGenFramebuffers(1,&drawFbo);
    initialized=true;
    std::fprintf(stderr,"[hwgs:direct] OpenGL shared memory/fence support ready; full-frame CPU upload disabled\n");
#endif
}
void ps2HwgsDirectUpdate(Texture2D& texture,uint32_t& width,uint32_t& height) {
#ifdef PS2X_WITH_HWGS
    TfGpuFrame frame{};
    if(tf_gs_direct_acquire(&frame)) {
        if(!initialized || frame.slot>=imported.size())throw std::runtime_error("HWGS direct presenter is not initialized");
        if(frame.sequence<=lastSequence)throw std::runtime_error("HWGS direct frame sequence moved backwards");
        rlDrawRenderBatchActive();
        GLint oldRead=0,oldDraw=0,oldTexture=0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&oldRead);glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&oldDraw);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,&oldTexture);
        auto& item=imported[frame.slot];importFrame(frame,item);
        if(texture.width!=int(frame.width) || texture.height!=int(frame.height)) {
            const GLuint id=rlLoadTexture(nullptr,frame.width,frame.height,PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,1);
            if(!id)throw std::runtime_error("HWGS direct output texture allocation failed");
            if(oldTexture==static_cast<GLint>(texture.id))oldTexture=0;
            UnloadTexture(texture);texture={id,int(frame.width),int(frame.height),1,PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        }
        const GLenum layout=LAYOUT_GENERAL;
        const GLuint64 ready=frame.ready_value,release=frame.release_value;
        pSemaphoreParameterui64vEXT(item.semaphore,FENCE_VALUE,&ready);
        pWaitSemaphoreEXT(item.semaphore,0,nullptr,1,&item.texture,&layout);
        glBindFramebuffer(GL_READ_FRAMEBUFFER,readFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,item.texture,0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,drawFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture.id,0);
        if(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE ||
           glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("HWGS direct copy framebuffer is incomplete");
        const GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0,0,frame.width,frame.height,0,0,frame.width,frame.height,GL_COLOR_BUFFER_BIT,GL_NEAREST);
        // The PS2 output alpha is not host transparency. Set opacity on the GPU.
        GLfloat clear[4];GLboolean mask[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE,clear);glGetBooleanv(GL_COLOR_WRITEMASK,mask);
        glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_TRUE);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
        glColorMask(mask[0],mask[1],mask[2],mask[3]);glClearColor(clear[0],clear[1],clear[2],clear[3]);
        if(scissor)glEnable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER,oldRead);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,oldDraw);
        glBindTexture(GL_TEXTURE_2D,oldTexture);
        pSemaphoreParameterui64vEXT(item.semaphore,FENCE_VALUE,&release);
        pSignalSemaphoreEXT(item.semaphore,0,nullptr,1,&item.texture,&layout);
        glFlush(); // Submit the release before allowing the producer to reuse this slot.
        check("GPU frame copy/release");
        tf_gs_direct_release(frame.slot,frame.generation);
        ps2HwgsNoteDirectFrame(frame.width,frame.scale,frame.scene!=0);
        if(frame.display_fbp!=lastDisplayFbp)ps2_playtest::noteFlip();
        lastDisplayFbp=frame.display_fbp;lastSequence=frame.sequence;
        ps2dbg::presentedFrames.fetch_add(1,std::memory_order_relaxed);
        ps2dbg::vsyncTick.store(frame.tick,std::memory_order_relaxed);
        haveFrame=true;
    }
    width=texture.width;height=texture.height;
    captureRequested(texture);
#endif
}
void ps2HwgsDirectShutdown() {
#ifdef PS2X_WITH_HWGS
    if(!initialized)return;
    glFinish();
    for(auto& item:imported)destroy(item);
    glDeleteFramebuffers(1,&readFbo);glDeleteFramebuffers(1,&drawFbo);
    readFbo=drawFbo=0;initialized=haveFrame=false;
    lastDisplayFbp=UINT32_MAX;lastSequence=0;
#endif
}
