/*
 * pds-nx: minimal overlay drawing (filled boxes and 8x8 bitmap text).
 *
 * Everything is batched into one vertex buffer and drawn with one call. The
 * emulator's renderer shares this GL context, so all state touched here is
 * saved first and put back afterwards.
 *
 * This file is part of pds-nx and is licensed under the GNU GPL v2 or later.
 */
#include <GLES3/gl3.h>
#include <stddef.h>
#include <string.h>

#include <vector>

#include "nx_log.h"
#include "nx_ui.h"
#include "font8x8_basic.h"

struct Vtx {
    float x, y, u, v;
    uint8_t r, g, b, a;
};

static GLuint s_prog, s_vao, s_vbo, s_tex;
static GLint s_u_scale;
static std::vector<Vtx> s_verts;

/* Font texture: 16 x 8 cells of 8x8 pixels = 128 x 64. Cell 127 is solid,
 * used for filled boxes. */
#define TEX_W 128
#define TEX_H 64

static const char *k_vs =
    "#version 300 es\n"
    "layout(location = 0) in vec2 a_pos;\n"
    "layout(location = 1) in vec2 a_uv;\n"
    "layout(location = 2) in vec4 a_col;\n"
    "uniform vec2 u_scale;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_col;\n"
    "void main() {\n"
    "  v_uv = a_uv;\n"
    "  v_col = a_col;\n"
    "  gl_Position = vec4(a_pos.x * u_scale.x - 1.0, 1.0 - a_pos.y * u_scale.y, 0.0, 1.0);\n"
    "}\n";

static const char *k_fs =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "in vec2 v_uv;\n"
    "in vec4 v_col;\n"
    "out vec4 o_col;\n"
    "void main() {\n"
    "  o_col = vec4(v_col.rgb, v_col.a * texture(u_tex, v_uv).r);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        nxlog("ui: shader error: %s", log);
    }
    return sh;
}

bool ui_init(void)
{
    GLint prev_prog, prev_vao, prev_buf, prev_tex, prev_unit, prev_align;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_buf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_unit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);

    GLuint vs = compile(GL_VERTEX_SHADER, k_vs), fs = compile(GL_FRAGMENT_SHADER, k_fs);
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glLinkProgram(s_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        nxlog("ui: program link failed");
        return false;
    }
    s_u_scale = glGetUniformLocation(s_prog, "u_scale");
    glUseProgram(s_prog);
    glUniform1i(glGetUniformLocation(s_prog, "u_tex"), 0);

    static uint8_t pix[TEX_W * TEX_H];
    memset(pix, 0, sizeof(pix));
    for (int c = 0; c < 128; c++) {
        int cx = (c % 16) * 8, cy = (c / 16) * 8;
        for (int y = 0; y < 8; y++) {
            uint8_t row = (c == 127) ? 0xFF : (uint8_t)font8x8_basic[c][y];
            for (int x = 0; x < 8; x++)
                pix[(cy + y) * TEX_W + cx + x] = (row >> x) & 1 ? 255 : 0;
        }
    }
    glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, TEX_W, TEX_H, 0, GL_RED, GL_UNSIGNED_BYTE, pix);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)offsetof(Vtx, x));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)offsetof(Vtx, u));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), (void *)offsetof(Vtx, r));

    glBindVertexArray(prev_vao);
    glBindBuffer(GL_ARRAY_BUFFER, prev_buf);
    glBindTexture(GL_TEXTURE_2D, prev_tex);
    glActiveTexture(prev_unit);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
    glUseProgram(prev_prog);
    nxlog("ui: ready");
    return true;
}

static void quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t c)
{
    uint8_t r = c >> 24, g = c >> 16, b = c >> 8, a = c;
    Vtx q[6] = {
        { x,     y,     u0, v0, r, g, b, a }, { x + w, y,     u1, v0, r, g, b, a },
        { x,     y + h, u0, v1, r, g, b, a }, { x + w, y,     u1, v0, r, g, b, a },
        { x + w, y + h, u1, v1, r, g, b, a }, { x,     y + h, u0, v1, r, g, b, a },
    };
    s_verts.insert(s_verts.end(), q, q + 6);
}

void ui_rect(float x, float y, float w, float h, uint32_t rgba)
{
    /* sample the middle of the solid cell 127 */
    float u = (15 * 8 + 4) / (float)TEX_W, v = (7 * 8 + 4) / (float)TEX_H;
    quad(x, y, w, h, u, v, u, v, rgba);
}

float ui_text_width(float scale, const char *s)
{
    return strlen(s) * 8.0f * scale;
}

float ui_text(float x, float y, float scale, uint32_t rgba, const char *s)
{
    float cx = x, sz = 8.0f * scale;
    for (; *s; s++) {
        unsigned c = (unsigned char)*s;
        if (c >= 127) c = '?';
        if (c != ' ') {
            float u0 = (c % 16) * 8 / (float)TEX_W, v0 = (c / 16) * 8 / (float)TEX_H;
            quad(cx, y, sz, sz, u0, v0, u0 + 8.0f / TEX_W, v0 + 8.0f / TEX_H, rgba);
        }
        cx += sz;
    }
    return cx - x;
}

void ui_flush(int sw, int sh)
{
    if (s_verts.empty() || !s_prog)
        return;

    /* Save what the emulator's renderer may depend on. */
    GLint prog, vao, buf, unit, tex, sampler, vp[4], bsrc_rgb, bdst_rgb, bsrc_a, bdst_a, beq_rgb, beq_a;
    GLboolean blend, depth, cull, stencil, scissor, cmask[4], dmask;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &bdst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsrc_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &bdst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &beq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &beq_a);
    blend = glIsEnabled(GL_BLEND);
    depth = glIsEnabled(GL_DEPTH_TEST);
    cull = glIsEnabled(GL_CULL_FACE);
    stencil = glIsEnabled(GL_STENCIL_TEST);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);

    glViewport(0, 0, sw, sh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(s_prog);
    glUniform2f(s_u_scale, 2.0f / UI_W, 2.0f / UI_H);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glBindSampler(0, 0);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, s_verts.size() * sizeof(Vtx), s_verts.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)s_verts.size());
    s_verts.clear();

    /* Put it all back. */
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glBindSampler(0, sampler);
    glBindTexture(GL_TEXTURE_2D, tex);
    glActiveTexture(unit);
    glUseProgram(prog);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glBlendEquationSeparate(beq_rgb, beq_a);
    glBlendFuncSeparate(bsrc_rgb, bdst_rgb, bsrc_a, bdst_a);
    if (!blend) glDisable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE);
    if (stencil) glEnable(GL_STENCIL_TEST);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
    glDepthMask(dmask);
}
