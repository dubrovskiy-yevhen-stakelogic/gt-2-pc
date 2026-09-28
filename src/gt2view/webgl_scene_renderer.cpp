#include "gt2view/webgl_scene_renderer.h"
#include "gt2export/png_writer.h"
#include "web_shaders.h"
#include <emscripten/html5.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace gt2view {
namespace {
GLuint Shader(GLenum kind, const char* source) {
    const auto shader = glCreateShader(kind);
    glShaderSource(shader, 1, &source, nullptr); glCompileShader(shader);
    GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[8192]{}; glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        glDeleteShader(shader); throw std::runtime_error(std::string("WebGL shader: ") + log);
    }
    return shader;
}
GLuint Texture(GLenum internal, int width, int height, GLenum format, GLenum type) {
    GLuint id; glGenTextures(1, &id); glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, width, height, 0, format, type, nullptr);
    return id;
}
void CheckGl(const char* operation) {
    const auto error = glGetError();
    if (error != GL_NO_ERROR) throw std::runtime_error(std::string(operation) + ": WebGL error " + std::to_string(error));
}
}
WebGlSceneRenderer::WebGlSceneRenderer() {
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture_);
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples_);
    GLint layers = 0; glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &layers);
    if (maxTexture_ < int((kExternalTexels + 4095) / 4096) || layers < int(DecodedTextureCache::kLayers))
        throw std::runtime_error("This GPU cannot hold GT2's texture atlas (8192 texture size / 512 array layers required).");
    const auto vs = Shader(GL_VERTEX_SHADER, web::vert), fs = Shader(GL_FRAGMENT_SHADER, web::frag);
    program_ = glCreateProgram(); glAttachShader(program_, vs); glAttachShader(program_, fs); glLinkProgram(program_);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint linked = 0; glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (!linked) { char log[8192]{}; glGetProgramInfoLog(program_, sizeof(log), nullptr, log); throw std::runtime_error(log); }
    glUseProgram(program_);
    const char* textures[] = {"vramImage", "externalImage", "materialTable", "decodedPages", "handAlbedo", "rearView"};
    for (int i = 0; i < 6; ++i) glUniform1i(glGetUniformLocation(program_, textures[i]), i);
    mvp_ = glGetUniformLocation(program_, "pc.mvp"); paint_ = glGetUniformLocation(program_, "pc.paint");
    brake_ = glGetUniformLocation(program_, "pc.brakeLit"); stp_ = glGetUniformLocation(program_, "pc.stpPass");
    optionsUniform_ = glGetUniformLocation(program_, "pc.options");
    glUniform4f(glGetUniformLocation(program_, "pc.hudClip"), 0, 0, 0, 0);
    glGenVertexArrays(1, &vao_); glBindVertexArray(vao_);
    glGenBuffers(1, &vertices_); glBindBuffer(GL_ARRAY_BUFFER, vertices_);
    glBufferData(GL_ARRAY_BUFFER, size_t(kMaxVertices) * sizeof(SceneVertex), nullptr, GL_DYNAMIC_DRAW);
    const size_t offsets[] = {offsetof(SceneVertex,pos), offsetof(SceneVertex,texel), offsetof(SceneVertex,color),
        offsetof(SceneVertex,page), offsetof(SceneVertex,clut), offsetof(SceneVertex,flags)};
    for (int i = 0; i < 6; ++i) {
        glEnableVertexAttribArray(i);
        if (i < 3) glVertexAttribPointer(i, i == 1 ? 2 : 3, GL_FLOAT, GL_FALSE, sizeof(SceneVertex), reinterpret_cast<void*>(offsets[i]));
        else glVertexAttribIPointer(i, 1, GL_UNSIGNED_INT, sizeof(SceneVertex), reinterpret_cast<void*>(offsets[i]));
    }
    glGenBuffers(1, &rects_); glBindBuffer(GL_ARRAY_BUFFER, rects_);
    glBufferData(GL_ARRAY_BUFFER, size_t(kMaxVertices) * 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(6); glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    vram_ = Texture(GL_R16UI, kVramWidth, kVramRows, GL_RED_INTEGER, GL_UNSIGNED_SHORT);
    external_ = Texture(GL_R32UI, 4096, (kExternalTexels + 4095) / 4096, GL_RED_INTEGER, GL_UNSIGNED_INT);
    materials_ = Texture(GL_RGBA32UI, 256, 16, GL_RGBA_INTEGER, GL_UNSIGNED_INT);
    black_ = Texture(GL_RGBA8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
    const uint32_t black = 0xff000000; glTexSubImage2D(GL_TEXTURE_2D,0,0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,&black);
    glGenTextures(1, &pages_); glBindTexture(GL_TEXTURE_2D_ARRAY, pages_);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 9, GL_RGBA8, 256, 256, DecodedTextureCache::kLayers);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    vramShadow_.resize(size_t(kVramWidth) * kVramRows);
    shadow_.resize(kMaxVertices);
    int w, h; emscripten_get_canvas_element_size("#canvas", &w, &h); extent_ = {uint32_t(w), uint32_t(h)};
    glDisable(GL_DITHER); glDisable(GL_CULL_FACE); glFrontFace(GL_CCW);
    CheckGl("renderer initialization");
}
WebGlSceneRenderer::~WebGlSceneRenderer() {
    DestroyTarget(scene_); DestroyTarget(resolve_); DestroyTarget(mirror_);
    GLuint textures[]{vram_, external_, materials_, pages_, black_}; glDeleteTextures(5, textures);
    glDeleteBuffers(1, &vertices_); glDeleteBuffers(1, &rects_); glDeleteVertexArrays(1, &vao_); glDeleteProgram(program_);
}
void WebGlSceneRenderer::SetFoveation(int level) {
    if (level) throw std::runtime_error("Foveation requires a VR renderer; WebGL 2 is flat mode.");
}
void WebGlSceneRenderer::SetVertices(uint32_t first, const std::vector<SceneVertex>& vertices) {
    if (uint64_t(first) + vertices.size() > kMaxVertices) throw std::runtime_error("WebGL vertex buffer is full");
    if (vertices.empty()) return;
    std::copy(vertices.begin(), vertices.end(), shadow_.begin() + first);
    for (auto it = ranges_.begin(); it != ranges_.end();) {
        const uint32_t begin = uint32_t(it->first >> 32), count = uint32_t(it->first);
        if (uint64_t(begin) < uint64_t(first) + vertices.size() && uint64_t(first) < uint64_t(begin) + count) it = ranges_.erase(it);
        else ++it;
    }
    glBindBuffer(GL_ARRAY_BUFFER, vertices_);
    glBufferSubData(GL_ARRAY_BUFFER, size_t(first)*sizeof(SceneVertex), vertices.size()*sizeof(SceneVertex), vertices.data());
    std::vector<float> rect(vertices.size()*4);
    for (size_t t = 0; t < vertices.size(); t += 3) {
        const size_t n = std::min<size_t>(3, vertices.size()-t);
        float r[4]{vertices[t].texel[0], vertices[t].texel[1], vertices[t].texel[0], vertices[t].texel[1]};
        for (size_t k = 1; k < n; ++k) for (size_t c = 0; c < 2; ++c) {
            r[c] = std::min(r[c], vertices[t+k].texel[c]); r[c+2] = std::max(r[c+2], vertices[t+k].texel[c]);
        }
        for (size_t k = 0; k < n; ++k) std::copy_n(r, 4, rect.data()+(t+k)*4);
    }
    glBindBuffer(GL_ARRAY_BUFFER, rects_); glBufferSubData(GL_ARRAY_BUFFER, size_t(first)*16, rect.size()*4, rect.data());
}
void WebGlSceneRenderer::UploadVram(uint32_t row, uint32_t count, const uint16_t* words) {
    if (uint64_t(row)+count > kVramRows) throw std::runtime_error("WebGL VRAM range");
    std::copy_n(words, size_t(count)*1024, vramShadow_.data()+size_t(row)*1024);
    decoded_.Invalidate(row, count);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, vram_);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,row,1024,count,GL_RED_INTEGER,GL_UNSIGNED_SHORT,words);
}
void WebGlSceneRenderer::UploadExternalTexture(uint32_t first, uint32_t count, const uint32_t* words) {
    if (uint64_t(first)+count > kExternalTexels) throw std::runtime_error("WebGL external texture range");
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, external_);
    while (count) {
        const uint32_t x=first%4096, width=std::min(count,4096-x);
        const uint32_t rows=x==0 && count>=4096 ? count/4096 : 1;
        const uint32_t n=width*rows;
        glTexSubImage2D(GL_TEXTURE_2D,0,x,first/4096,width,rows,GL_RED_INTEGER,GL_UNSIGNED_INT,words);
        first+=n; count-=n; words+=n;
    }
}
void WebGlSceneRenderer::PrepareMaterials(const std::vector<DrawItem>& items) {
    decoded_.Begin();
    for (const auto& item : items) {
        if (uint64_t(item.firstVertex)+item.vertexCount > kMaxVertices) throw std::runtime_error("WebGL draw range");
        auto [it, added] = ranges_.try_emplace((uint64_t(item.firstVertex)<<32)|item.vertexCount);
        if (added) {
            std::unordered_set<uint64_t> seen;
            for (uint32_t i=0;i<item.vertexCount;i+=3) {
                const auto& v=shadow_[item.firstVertex+i];
                if (!(v.flags&kTextured) || (v.flags&(kOverlay|kExternalTexture|kHandTexture|kHdUi|kSmoothUi))) continue;
                const auto key=uint64_t(v.page)|(uint64_t(v.clut|(((v.flags>>8)&3u)<<28)|((v.flags&kCarPaint)?1u<<30:0u))<<32);
                if (seen.insert(key).second) it->second.push_back(v);
            }
        }
        for (const auto& v : it->second) {
            auto clut=v.clut;
            if (v.flags&kCarPaint) { clut+=item.paint<<16; if ((clut&65535)==224 && item.brakeLit) clut+=16; }
            decoded_.Prepare(v.page,clut|(((v.flags>>8)&3)<<28),vramShadow_.data(),kVramRows);
        }
    }
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D,materials_);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,256,16,GL_RGBA_INTEGER,GL_UNSIGNED_INT,decoded_.table.data());
    if (!decoded_.uploads.empty()) {
        glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D_ARRAY,pages_);
        for (uint32_t layer : decoded_.uploads) glTexSubImage3D(GL_TEXTURE_2D_ARRAY,0,0,0,layer,256,256,1,GL_RGBA,GL_UNSIGNED_BYTE,
            decoded_.pixels.data()+size_t(layer)*DecodedTextureCache::kTexels);
        glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    }
}
void WebGlSceneRenderer::DestroyTarget(Target& t) {
    glDeleteFramebuffers(1,&t.framebuffer); glDeleteRenderbuffers(1,&t.depth);
    if (t.samples>1) glDeleteRenderbuffers(1,&t.color); else glDeleteTextures(1,&t.color);
    t={};
}
void WebGlSceneRenderer::EnsureTarget(Target& t, Size size, uint32_t samples, bool texture) {
    (void)texture;
    if (t.framebuffer && t.size.width==size.width && t.size.height==size.height && t.samples==samples) return;
    DestroyTarget(t); t.size=size; t.samples=samples;
    glGenFramebuffers(1,&t.framebuffer); glBindFramebuffer(GL_FRAMEBUFFER,t.framebuffer);
    if (samples>1) {
        glGenRenderbuffers(1,&t.color); glBindRenderbuffer(GL_RENDERBUFFER,t.color);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER,samples,GL_RGBA8,size.width,size.height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,t.color);
    } else {
        t.color=Texture(GL_RGBA8,size.width,size.height,GL_RGBA,GL_UNSIGNED_BYTE);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t.color,0);
    }
    glGenRenderbuffers(1,&t.depth); glBindRenderbuffer(GL_RENDERBUFFER,t.depth);
    if (samples>1) glRenderbufferStorageMultisample(GL_RENDERBUFFER,samples,GL_DEPTH_COMPONENT24,size.width,size.height);
    else glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,size.width,size.height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,t.depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) throw std::runtime_error("WebGL framebuffer incomplete");
}
void WebGlSceneRenderer::BindTextures(bool mirror) {
    const GLuint textures[]{vram_,external_,materials_,pages_,black_,mirror ? black_ : (mirror_.color ? mirror_.color : black_)};
    for (int i=0;i<6;++i) { glActiveTexture(GL_TEXTURE0+i); glBindTexture(i==3 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D,textures[i]); }
}
void WebGlSceneRenderer::DrawItems(const std::vector<DrawItem>& items,size_t first,size_t last,size_t sceneItems,Size size,bool mirror) {
    glViewport(0,0,size.width,size.height); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_GEQUAL); glEnable(GL_SCISSOR_TEST);
    for (size_t i=first;i<last;++i) {
        const auto& d=items[i]; if (!d.vertexCount || ((d.space==kSpaceMirrorSource)!=mirror)) continue;
        int x0=0,y0=0,x1=int(size.width),y1=int(size.height);
        if(d.scissor[2]>d.scissor[0]) {
            x0=std::clamp(int(d.scissor[0]*size.width),0,x1); x1=std::clamp(int(d.scissor[2]*size.width),x0,x1);
            y0=std::clamp(int(d.scissor[1]*size.height),0,y1); y1=std::clamp(int(d.scissor[3]*size.height),y0,y1);
        }
        glScissor(x0,int(size.height)-y1,x1-x0,y1-y0);
        if(d.clearDepth) { glDepthMask(GL_TRUE); glClear(GL_DEPTH_BUFFER_BIT); }
        glUniformMatrix4fv(mvp_,1,GL_FALSE,d.mvp); glUniform1ui(paint_,d.paint); glUniform1ui(brake_,d.brakeLit); glUniform1ui(stp_,d.stpPass);
        glUniform1ui(optionsUniform_,i<sceneItems ? (options_.smoothTextures?1u:0u)|(options_.affine?2u:0u)|(options_.mipmaps?8u:0u) : 0u);
        glEnable(GL_BLEND); glBlendEquationSeparate(d.blend==2 ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD,GL_FUNC_ADD);
        if(d.blend<4) {
            const float factor=d.blend==3 ? 0.25f : 0.5f; glBlendColor(factor,factor,factor,1);
            glBlendFuncSeparate(d.blend==0 || d.blend==3 ? GL_CONSTANT_COLOR : GL_ONE,d.blend==0 ? GL_CONSTANT_COLOR : GL_ONE,GL_ONE,GL_ZERO);
            glDepthMask(GL_FALSE); glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(0,1);
        } else {
            glBlendFuncSeparate(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_TRUE); glDisable(GL_POLYGON_OFFSET_FILL);
        }
        glDrawArrays(GL_TRIANGLES,d.firstVertex,d.vertexCount);
    }
    glDepthMask(GL_TRUE); glDisable(GL_SCISSOR_TEST); glDisable(GL_POLYGON_OFFSET_FILL);
}
void WebGlSceneRenderer::Draw(const std::vector<DrawItem>& items,const std::string& shot,size_t sceneItems) {
    int w,h; emscripten_get_canvas_element_size("#canvas",&w,&h);
    if(w<=0 || h<=0) return;
    extent_={uint32_t(w),uint32_t(h)}; sceneItems=std::min(sceneItems,items.size());
    glUseProgram(program_); glBindVertexArray(vao_); PrepareMaterials(items);
    glClearDepthf(0); glDepthMask(GL_TRUE); glDisable(GL_SCISSOR_TEST); glClearColor(clearColor[0],clearColor[1],clearColor[2],1);
    if(std::any_of(items.begin(),items.end(),[](const auto& d){return d.space==kSpaceMirrorSource;})) {
        EnsureTarget(mirror_,{512,256},1,true); glBindFramebuffer(GL_FRAMEBUFFER,mirror_.framebuffer); BindTextures(true);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); DrawItems(items,0,sceneItems,sceneItems,mirror_.size,true);
    }
    samples_=1;
    if(sceneItems) {
        // Use sample counts supported by both RGBA8 and depth24 (WebGL's mandatory formats).
        const uint32_t cap=uint32_t(std::min(maxSamples_,4));
        if(options_.msaa>=4 && cap>=4) samples_=4;
        else if(options_.msaa>=2 && cap>=2) samples_=2;
        const float scale=std::clamp(options_.sceneScale,0.25f,4.0f);
        Size size{uint32_t(std::clamp(int(options_.sceneWidth?options_.sceneWidth:uint32_t(w*scale)),1,maxTexture_)),
                  uint32_t(std::clamp(int(options_.sceneHeight?options_.sceneHeight:uint32_t(h*scale)),1,maxTexture_))};
        EnsureTarget(scene_,size,samples_,false); glBindFramebuffer(GL_FRAMEBUFFER,scene_.framebuffer); BindTextures(false);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); DrawItems(items,0,sceneItems,sceneItems,size,false);
        GLuint source=scene_.framebuffer;
        if(samples_>1) {
            EnsureTarget(resolve_,size,1,true); glBindFramebuffer(GL_READ_FRAMEBUFFER,scene_.framebuffer); glBindFramebuffer(GL_DRAW_FRAMEBUFFER,resolve_.framebuffer);
            glBlitFramebuffer(0,0,size.width,size.height,0,0,size.width,size.height,GL_COLOR_BUFFER_BIT,GL_NEAREST); source=resolve_.framebuffer;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER,source); glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);
        glBlitFramebuffer(0,0,size.width,size.height,0,0,w,h,GL_COLOR_BUFFER_BIT,GL_LINEAR);
    }
    glBindFramebuffer(GL_FRAMEBUFFER,0); BindTextures(false);
    glClear(sceneItems ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    DrawItems(items,sceneItems,items.size(),sceneItems,extent_,false);
    if(!shot.empty()) {
        std::vector<uint8_t> rgba(size_t(w)*h*4), flipped(rgba.size());
        glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
        for(int y=0;y<h;++y) std::copy_n(rgba.data()+size_t(h-1-y)*w*4,size_t(w)*4,flipped.data()+size_t(y)*w*4);
        gt2::WritePngRgba(shot,w,h,flipped);
    }
    CheckGl("draw");
}
}
