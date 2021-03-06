// Copyright 2014 Wouter van Oortmerssen. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "stdafx.h"
#include "glinterface.h"
#include "glincludes.h"

unordered_map<string, Shader *> shadermap;

Shader *LookupShader(const char *name)
{
    auto shi = shadermap.find(name);
    if (shi != shadermap.end()) return shi->second;
    return nullptr;
}

void ShaderShutDown()
{
    for (auto &it : shadermap)
        delete it.second;
}

string GLSLError(uint obj, bool isprogram, const char *source)
{
    GLint length = 0;
    if (isprogram) glGetProgramiv(obj, GL_INFO_LOG_LENGTH, &length);
    else           glGetShaderiv (obj, GL_INFO_LOG_LENGTH, &length);
    if (length > 1)
    {
        GLchar *log = new GLchar[length];
        if (isprogram) glGetProgramInfoLog(obj, length, &length, log);
        else           glGetShaderInfoLog (obj, length, &length, log);
        string err = "GLSL ERROR: ";
        err += log;
        int i = 0;
        if (source) for (;;)
        {
            err += to_string(++i);
            err += ": ";
            const char *next = strchr(source, '\n');
            if (next) { err += string(source, next - source + 1); source = next + 1; }
            else { err += string(source) + "\n"; break; }
        } 
        delete[] log;
        return err;
    }
    return "";
}

uint CompileGLSLShader(GLenum type, uint program, const GLchar *source, string &err) 
{
    uint obj = glCreateShader(type);
    glShaderSource(obj, 1, &source, nullptr);
    glCompileShader(obj);
    GLint success;
    glGetShaderiv(obj, GL_COMPILE_STATUS, &success);
    if (success)
    {
        glAttachShader(program, obj);
        return obj; 
    }
    err = GLSLError(obj, false, source);
    glDeleteShader(obj);
    return 0;
}

string ParseMaterialFile(char *mbuf)
{
    auto p = mbuf;

    string err;
    string last;
    string vfunctions, pfunctions, cfunctions, vertex, pixel, compute, vdecl, pdecl, csdecl, shader;
    string *accum = nullptr;

    auto word = [&]()
    {
        p += strspn(p, " \t\r");
        size_t len = strcspn(p, " \t\r\0");
        last = string(p, len);
        p += len;
    };

    auto finish = [&]() -> bool
    {
        if (!shader.empty())
        {

            auto sh = new Shader();
            if (compute.length())
            {
                auto header = "#version 430\n";
                err = sh->Compile(shader.c_str(),
                                  (header + csdecl + cfunctions + "void main()\n{\n" + compute + "}\n").c_str());
            }
            else
            {
                string header;
                #ifdef PLATFORM_ES2
                    header += "#ifdef GL_ES\nprecision highp float;\n#endif\n";
                #else
                    #ifdef __APPLE__
                    auto supported = glGetString(GL_SHADING_LANGUAGE_VERSION);
                    // Apple randomly changes what it supports, so just ask for that.
                    header += string("#version ") + char(supported[0]) + char(supported[2]) + char(supported[3]) + "\n";
                    #else
                    header += "#version 130\n";
                    #endif
                #endif
                err = sh->Compile(shader.c_str(),
                                  (header + vdecl + vfunctions + "void main()\n{\n" + vertex + "}\n").c_str(),
                                  (header + pdecl + pfunctions + "void main()\n{\n" + pixel + "}\n").c_str());
            }
            if (!err.empty())
                return true;
            shadermap[shader] = sh;
            shader.clear();
        }
        return false;
    };

    for (;;)
    {
        auto start = p;
        auto end = p + strcspn(p, "\n\0");
        bool eof = !*end;
        *end = 0;
        
        word();

        if (!last.empty())
        {
            if      (last == "VERTEXFUNCTIONS")  { if (finish()) return err; vfunctions.clear(); accum = &vfunctions; }
            else if (last == "PIXELFUNCTIONS")   { if (finish()) return err; pfunctions.clear(); accum = &pfunctions; }
            else if (last == "COMPUTEFUNCTIONS") { if (finish()) return err; cfunctions.clear(); accum = &cfunctions; }
            else if (last == "VERTEX")           {                           vertex.clear();     accum = &vertex;     }
            else if (last == "PIXEL")            {                           pixel.clear();      accum = &pixel;      }
            else if (last == "COMPUTE")          {                           compute.clear();    accum = &compute;    }
            else if (last == "SHADER")
            {
                if (finish()) return err;
                word();
                shader = last;
                vdecl.clear();
                pdecl.clear();
                csdecl.clear();
                vertex.clear();
                pixel.clear();
                compute.clear();
                accum = nullptr;
            }
            else if (last == "UNIFORMS")
            {
                string &decl = accum == &compute ? csdecl : (accum == &vertex ? vdecl : pdecl);
                for (;;)
                {
                    word();
                    if (last.empty()) break;
                    else if (last == "mvp")          decl += "uniform mat4 mvp;\n";
                    else if (last == "col")          decl += "uniform vec4 col;\n";
                    else if (last == "camera")       decl += "uniform vec3 camera;\n";
                    else if (last == "light1")       decl += "uniform vec3 light1;\n";
                    else if (last == "lightparams1") decl += "uniform vec2 lightparams1;\n";
                    else if (last == "bones")        decl += "uniform vec4 bones[230];\n";   // FIXME: configurable
                    else if (last == "pointscale")   decl += "uniform float pointscale;\n";
                    else if (!strncmp(last.c_str(), "tex", 3))
                    {
                        auto p = last.c_str() + 3;
                        bool cubemap = false;
                        bool floatingp = false;
                        if (!strncmp(p, "cube", 4))
                        {
                            p += 4;
                            cubemap = true;
                        }
                        if (*p == 'f')
                        {
                            p++;
                            floatingp = true;
                        }
                        auto unit = atoi(p);
                        if (accum == &compute)
                        {
                            decl += "layout(binding = " + to_string(unit) + ", " +
                                    (floatingp ? "rgba32f" : "rgba8") + ") ";
                        }
                        decl += "uniform ";
                        decl += accum == &compute ? (cubemap ? "imageCube" : "image2D")
                                                  : (cubemap ? "samplerCube" : "sampler2D");
                        decl += " " + last + ";\n";
                    }
                    else return "unknown uniform: " + last; 
                }
            }
            else if (last == "UNIFORM")
            {
                string &decl = accum == &compute ? csdecl : (accum == &vertex ? vdecl : pdecl);
                word();
                auto type = last;
                word();
                auto name = last;
                if (type.empty() || name.empty()) return "uniform decl must specify type and name";
                decl += "uniform " + type + " " + name + ";\n";
            }
            else if (last == "INPUTS")
            {
                string decl;
                for (;;)
                {
                    word();
                    if (last.empty()) break;
                    auto pos = strstr(last.c_str(), ":");
                    if (!pos)
                    {
                        return "input " + last + " doesn't specify number of components, e.g. anormal:3";
                    }
                    int comp = atoi(pos + 1);
                    if (comp <= 0 || comp > 4)
                    {
                        return "input " + last + " can only use 1..4 components";
                    }
                    last = last.substr(0, pos - last.c_str());
                    string d = " vec" + to_string(comp) + " " + last + ";\n";
                    if (accum == &vertex) vdecl += "attribute" + d;
                    else { d = "varying" + d; vdecl += d; pdecl += d; }
                }
            }
            else if (last == "LAYOUT")
            {
                word();
                auto xs = last;
                word();
                auto ys = last;
                csdecl += "layout(local_size_x = " + xs + ", local_size_y = " + ys + ") in;\n";
            }
            else
            {
                if (!accum) return "GLSL code outside of FUNCTIONS/VERTEX/PIXEL block: " + string(start);
                *accum += start;
                *accum += "\n";
            }
        }

        if (eof) break;
        *end = '\n';

        p = end + 1;
    }

    finish();
    return err;
}

string LoadMaterialFile(const char *mfile)
{
    auto mbuf = (char *)LoadFile(mfile);
    if (!mbuf) return string("cannot load material file: ") + mfile;
    auto err = ParseMaterialFile(mbuf);
    free(mbuf);
    return err;
}

string Shader::Compile(const char *name, const char *vscode, const char *pscode)
{
    program = glCreateProgram();

    string err;
    vs = CompileGLSLShader(GL_VERTEX_SHADER,   program, vscode, err);
    if (!vs) return string("couldn't compile vertex shader: ") + name + "\n" + err;
    ps = CompileGLSLShader(GL_FRAGMENT_SHADER, program, pscode, err);
    if (!ps) return string("couldn't compile pixel shader: ") + name + "\n" + err;

    glBindAttribLocation(program, 0, "apos");
    glBindAttribLocation(program, 1, "anormal");
    glBindAttribLocation(program, 2, "atc");
    glBindAttribLocation(program, 3, "acolor");
    glBindAttribLocation(program, 4, "aweights");
    glBindAttribLocation(program, 5, "aindices");

    Link(name);

    return "";
}

string Shader::Compile(const char *name, const char *cscode)
{
    #if !defined(PLATFORM_ES2) && !defined(__APPLE__)
        program = glCreateProgram();

        string err;
        cs = CompileGLSLShader(GL_COMPUTE_SHADER, program, cscode, err);
        if (!cs) return string("couldn't compile compute shader: ") + name + "\n" + err;

        Link(name);

        return "";
    #else
        return "compute shaders not supported";
    #endif
}

void Shader::Link(const char *name)
{
    glLinkProgram(program);
    GLint status;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE)
    {
        GLSLError(program, true, nullptr);
        throw string("linking failed for shader: ") + name;
    }

    mvp_i          = glGetUniformLocation(program, "mvp");
    col_i          = glGetUniformLocation(program, "col");
    camera_i       = glGetUniformLocation(program, "camera");
    light1_i       = glGetUniformLocation(program, "light1");
    lightparams1_i = glGetUniformLocation(program, "lightparams1");
    bones_i        = glGetUniformLocation(program, "bones");
    pointscale_i   = glGetUniformLocation(program, "pointscale");

    glUseProgram(program);

    for (int i = 0; i < MAX_SAMPLERS; i++)
    {
        tex_i[i] = glGetUniformLocation(program, ("tex" + to_string(i)).c_str());
        if (tex_i[i] >= 0) glUniform1i(tex_i[i], i);
    }
}

Shader::~Shader()
{
    if (program) glDeleteProgram(program);
    if (ps) glDeleteShader(ps);
    if (vs) glDeleteShader(vs);
    if (cs) glDeleteShader(cs);
}

void Shader::Activate()
{
    glUseProgram(program);
}

void Shader::Set()
{
    Activate();

    if (mvp_i >= 0) glUniformMatrix4fv(mvp_i, 1, false, view2clip * otransforms.object2view);
    if (col_i >= 0) glUniform4fv(col_i, 1, curcolor.begin());
    if (camera_i >= 0) glUniform3fv(camera_i, 1, otransforms.view2object[3].begin());
    if (pointscale_i >= 0) glUniform1f(pointscale_i, pointscale);

    if (lights.size() > 0)
    {
        if (light1_i >= 0) glUniform3fv(light1_i, 1, (otransforms.view2object * lights[0].pos).begin());
        if (lightparams1_i >= 0) glUniform2fv(lightparams1_i, 1, lights[0].params.begin());
    }
}

void Shader::SetAnim(float3x4 *bones, int num)
{
    if (bones_i >= 0) glUniform4fv(bones_i, num * 3, (float *)bones);    // FIXME: check if num fits with shader def
}

void Shader::SetTextures(uint *textures)
{
    for (int i = 0; i < MAX_SAMPLERS; i++)
        if (tex_i[i] >= 0)
            SetTexture(i, textures[i]);
}

bool Shader::SetUniform(const char *name, const float *val, int components, int elements)
{
    auto loc = glGetUniformLocation(program, name);
    if (loc < 0) return false;
    switch (components)
    {
        case 1: glUniform1fv(loc, elements, val); return true;
        case 2: glUniform2fv(loc, elements, val); return true;
        case 3: glUniform3fv(loc, elements, val); return true;
        case 4: glUniform4fv(loc, elements, val); return true;
        default: return false;
    }
}

void DispatchCompute(const int3 &groups)
{
    #if !defined(PLATFORM_ES2) && !defined(__APPLE__)
        if (glDispatchCompute) glDispatchCompute(groups.x(), groups.y(), groups.z());
        // Make sure any imageStore/VBOasSSBO operations have completed.
        // Would be better to decouple this from DispatchCompute.
        if (glMemoryBarrier) glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
    #else
        assert(false);
    #endif
}

// Simple function for getting some uniform / shader storage attached to a shader. Should ideally be split up for more
// flexibility.
uint UniformBufferObject(Shader *sh, const float *data, size_t len, const char *uniformblockname, bool ssbo)
{
    GLuint bo = 0;
    #if !defined(PLATFORM_ES2) && !defined(__APPLE__)
        if (sh && glGetProgramResourceIndex && glShaderStorageBlockBinding && glBindBufferBase &&
                  glUniformBlockBinding && glGetUniformBlockIndex)
        {
            sh->Activate();
            auto idx = ssbo ? glGetProgramResourceIndex(sh->program, GL_SHADER_STORAGE_BLOCK, uniformblockname)
                            : glGetUniformBlockIndex(sh->program, uniformblockname);
            if (idx != GL_INVALID_INDEX)
            {
                auto type = ssbo ? GL_SHADER_STORAGE_BUFFER : GL_UNIFORM_BUFFER;
                glGenBuffers(1, &bo);
                glBindBuffer(type, bo);
                glBufferData(type, sizeof(float) * len, data, GL_STATIC_DRAW);
                glBindBuffer(type, 0);
                static GLuint bo_binding_point_index = 0;
                bo_binding_point_index++;  // FIXME: how do we allocate these properly?
                glBindBufferBase(type, bo_binding_point_index, bo);
                if (ssbo) glShaderStorageBlockBinding(sh->program, idx, bo_binding_point_index);
                else      glUniformBlockBinding      (sh->program, idx, bo_binding_point_index);
            }
        }
    #else
        // UBO's are in ES 3.0, not sure why OS X doesn't have them
    #endif
    return bo;
}

void BindVBOAsSSBO(uint bind_point_index, uint vbo)
{
    #if !defined(PLATFORM_ES2) && !defined(__APPLE__)
        if (glBindBufferBase)
        {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, bind_point_index, vbo);
        }
    #endif
}
