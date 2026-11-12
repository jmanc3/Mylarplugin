#include "drawing/opengl_context.h"
#include "drawing_internal.h"
#include "gl_shaders.h"

#include <GLES3/gl3.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <unordered_map>

// Ported from tunes' OpenGL ES 3 renderer. Cairo/Pango only rasterize text on
// cache misses; every shape, color, gradient, surface and group is drawn by GL.

namespace drawing {
namespace {
constexpr double pi = std::numbers::pi;
struct Point {
    double x, y;
};
struct Transform {
    double sx = 1, sy = 1, tx = 0, ty = 0;
    Point apply(Point p) const { return {p.x * sx + tx, p.y * sy + ty}; }
    Point inverse(Point p) const { return {(p.x - tx) / sx, (p.y - ty) / sy}; }
};
Rect intersect(Rect a, Rect b) {
    double x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0.0, std::min(a.x + a.width, b.x + b.width) - x),
            std::max(0.0, std::min(a.y + a.height, b.y + b.height) - y)};
}
bool empty(Rect r) { return r.width <= 0 || r.height <= 0; }
Rect integral(Rect r) {
    if (empty(r))
        return {r.x, r.y, 0, 0};
    return {std::floor(r.x), std::floor(r.y), std::ceil(r.x + r.width) - std::floor(r.x),
            std::ceil(r.y + r.height) - std::floor(r.y)};
}
struct Texture {
    GLuint id = 0;
    int width = 0, height = 0;
    Texture() { glGenTextures(1, &id); }
    ~Texture() { glDeleteTextures(1, &id); }
    Texture(const Texture &) = delete;
    Texture &operator=(const Texture &) = delete;
    void allocate(int w, int h, bool mask, const void *data = nullptr) {
        width = w;
        height = h;
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, mask ? GL_R8 : GL_RGBA8, w, h, 0, mask ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE,
                     data);
    }
};
struct Target {
    Rect bounds;
    Texture color;
    GLuint draw = 0, resolve = 0, multisample = 0, stencil = 0;
    int samples;
    bool sampleable;
    Target(Rect b, int count, bool sampled) : bounds(b), samples(count), sampleable(sampled) {
        glGenFramebuffers(1, &draw);
        glGenFramebuffers(1, &resolve);
        glGenRenderbuffers(1, &multisample);
        glGenRenderbuffers(1, &stencil);
    }
    void allocate() {
        int w = bounds.width, h = bounds.height;
        if (sampleable) {
            color.allocate(w, h, false);
            glBindFramebuffer(GL_FRAMEBUFFER, resolve);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color.id, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                throw std::runtime_error("OpenGL texture framebuffer is incomplete");
        }
        glBindFramebuffer(GL_FRAMEBUFFER, draw);
        glBindRenderbuffer(GL_RENDERBUFFER, multisample);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, multisample);
        glBindRenderbuffer(GL_RENDERBUFFER, stencil);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencil);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("OpenGL multisample framebuffer is incomplete");
    }
    ~Target() {
        glDeleteFramebuffers(1, &draw);
        glDeleteFramebuffers(1, &resolve);
        glDeleteRenderbuffers(1, &multisample);
        glDeleteRenderbuffers(1, &stencil);
    }
    size_t bytes() const {
        return size_t(bounds.width) * size_t(bounds.height) * ((sampleable ? 4 : 0) + 8 * samples);
    }
    void resolved() {
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, draw);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve);
        glBlitFramebuffer(0, 0, bounds.width, bounds.height, 0, 0, bounds.width, bounds.height, GL_COLOR_BUFFER_BIT,
                          GL_NEAREST);
    }
};
// A textured paint source: a popped group (flipped FBO texture) or a surface.
struct Source {
    std::shared_ptr<Target> group;
    std::shared_ptr<Texture> texture;
    Rect bounds{}; // device space
    bool flipped = false;
    GLuint id() const { return group ? group->color.id : texture->id; }
};
struct Gradient {
    Point from{}, to{}; // device space
    float a[4]{}, b[4]{}; // premultiplied
};
struct State {
    Transform transform;
    double color[4]{0, 0, 0, 1};
    cairo_operator_t composite = CAIRO_OPERATOR_OVER;
    cairo_line_cap_t cap = CAIRO_LINE_CAP_BUTT;
    cairo_line_join_t join = CAIRO_LINE_JOIN_MITER;
    cairo_fill_rule_t rule = CAIRO_FILL_RULE_WINDING;
    double line_width = 2;
    std::vector<double> dashes;
    double dash_offset = 0;
    Rect clip;
    std::shared_ptr<Target> clip_mask;
    std::shared_ptr<Source> source;
    std::shared_ptr<Gradient> gradient;
};
struct Path {
    std::vector<Point> points;
    bool closed = false;
};
struct Vertex {
    float x, y, u, v, r, g, b, a, opacity;
};
struct BatchKey {
    GLuint texture = 0;
    int kind = 0;
    bool mipmap = false;
    cairo_operator_t composite = CAIRO_OPERATOR_OVER;
    Rect clip{};
    Target *mask = nullptr;
    const Gradient *gradient = nullptr;
    bool operator==(const BatchKey &b) const {
        return texture == b.texture && kind == b.kind && mipmap == b.mipmap && composite == b.composite &&
               mask == b.mask && gradient == b.gradient && clip.x == b.clip.x && clip.y == b.clip.y &&
               clip.width == b.clip.width && clip.height == b.clip.height;
    }
};
cairo_operator_t supported(cairo_operator_t op) {
    switch (op) {
    case CAIRO_OPERATOR_CLEAR:
    case CAIRO_OPERATOR_SOURCE:
    case CAIRO_OPERATOR_ADD:
        return op;
    default:
        return CAIRO_OPERATOR_OVER;
    }
}
GLuint shader(GLenum kind, const char *code) {
    GLuint id = glCreateShader(kind);
    glShaderSource(id, 1, &code, nullptr);
    glCompileShader(id);
    GLint ok;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(id, sizeof log, nullptr, log);
        glDeleteShader(id);
        throw std::runtime_error(std::string("OpenGL shader compilation failed: ") + log);
    }
    return id;
}
struct Pipeline {
    GLuint program = 0, vao = 0, vbo = 0;
    GLuint samplers[2]{};
    GLint target, source_kind, has_clip, clip_rect, clip_target, erase, gradient_line, gradient_from, gradient_to;
    Pipeline() {
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenSamplers(2, samplers);
        for (int i = 0; i < 2; ++i) {
            glSamplerParameteri(samplers[i], GL_TEXTURE_MIN_FILTER, i ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
            glSamplerParameteri(samplers[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }
    ~Pipeline() {
        glDeleteSamplers(2, samplers);
        glDeleteProgram(program);
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
    }
    void initialize() {
        auto vs = shader(GL_VERTEX_SHADER, gl_shaders::vertex);
        GLuint fs;
        try {
            fs = shader(GL_FRAGMENT_SHADER, gl_shaders::fragment);
        } catch (...) {
            glDeleteShader(vs);
            throw;
        }
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[4096];
            glGetProgramInfoLog(program, sizeof log, nullptr, log);
            throw std::runtime_error(std::string("OpenGL shader linking failed: ") + log);
        }
        glUseProgram(program);
        target = glGetUniformLocation(program, "target");
        source_kind = glGetUniformLocation(program, "sourceKind");
        has_clip = glGetUniformLocation(program, "hasClip");
        clip_rect = glGetUniformLocation(program, "clipRect");
        clip_target = glGetUniformLocation(program, "clipTarget");
        erase = glGetUniformLocation(program, "erasePass");
        gradient_line = glGetUniformLocation(program, "gradientLine");
        gradient_from = glGetUniformLocation(program, "gradientFrom");
        gradient_to = glGetUniformLocation(program, "gradientTo");
        glUniform1i(glGetUniformLocation(program, "sourceTexture"), 0);
        glUniform1i(glGetUniformLocation(program, "clipTexture"), 1);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        const int sizes[] = {2, 2, 4, 1};
        size_t offset = 0;
        for (int i = 0; i < 4; ++i) {
            glEnableVertexAttribArray(i);
            glVertexAttribPointer(i, sizes[i], GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(offset));
            offset += sizes[i] * sizeof(float);
        }
    }
};
struct TextEntry {
    TextRaster raster;
    std::shared_ptr<Texture> texture;
    uint64_t used = 0;
};
struct SurfaceEntry {
    std::weak_ptr<const char> lifetime;
    std::shared_ptr<Texture> texture;
    uint64_t used = 0;
};

class GLContext final : public OpenGLContext {
    Pipeline pipeline_;
    std::shared_ptr<Target> root_, target_;
    std::vector<std::shared_ptr<Target>> pool_;
    State state_;
    std::vector<State> saved_;
    struct Group {
        State state;
        std::shared_ptr<Target> parent;
        size_t depth;
    };
    std::vector<Group> groups_;
    std::vector<Path> paths_;
    bool new_path_ = true;
    Point current_{0, 0}; // device space
    std::vector<Vertex> vertices_;
    BatchKey batch_;
    std::shared_ptr<Target> batch_mask_;
    std::shared_ptr<Source> batch_source_;
    std::shared_ptr<Texture> batch_texture_;
    std::shared_ptr<Gradient> batch_gradient_;
    std::unordered_map<cairo_surface_t *, SurfaceEntry> surfaces_;
    std::unordered_map<std::string, TextEntry> texts_;
    int samples_ = 4, max_texture_ = 0;
    uint64_t frame_ = 0;

    void bind_target() {
        glBindFramebuffer(GL_FRAMEBUFFER, target_->draw);
        glViewport(0, 0, target_->bounds.width, target_->bounds.height);
        const auto b = target_->bounds;
        glUniform4f(pipeline_.target, b.x, b.y, b.width, b.height);
    }
    std::shared_ptr<Target> target(Rect bounds, bool sampled = true) {
        bounds = integral(bounds);
        // Empty clips still need a legal framebuffer; scissoring suppresses draws.
        bounds.width = std::max(1.0, bounds.width);
        bounds.height = std::max(1.0, bounds.height);
        if (sampled) {
            // Stable size buckets allow small animation/clip changes to reuse FBOs.
            bounds.width = std::min(double(max_texture_), std::ceil(bounds.width / 32) * 32);
            bounds.height = std::min(double(max_texture_), std::ceil(bounds.height / 32) * 32);
        }
        if (bounds.width > max_texture_ || bounds.height > max_texture_)
            throw std::runtime_error("Drawing target exceeds GL_MAX_TEXTURE_SIZE");
        for (auto &entry : pool_) {
            if (entry.use_count() == 1 && entry->sampleable == sampled && entry->bounds.width == bounds.width &&
                entry->bounds.height == bounds.height) {
                entry->bounds = bounds;
                return entry;
            }
        }
        auto result = std::make_shared<Target>(bounds, samples_, sampled);
        result->allocate();
        pool_.push_back(result);
        return result;
    }
    void clear_target() {
        bind_target();
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glStencilMask(255);
        glClearColor(0, 0, 0, 0);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    void uniforms(const BatchKey &key) {
        bind_target();
        auto clip = intersect(key.clip, target_->bounds);
        auto scissor = integral(clip);
        glEnable(GL_SCISSOR_TEST);
        glScissor(scissor.x - target_->bounds.x,
                  target_->bounds.height - (scissor.y - target_->bounds.y + scissor.height), scissor.width,
                  scissor.height);
        glUniform4f(pipeline_.clip_rect, clip.x, clip.y, clip.width, clip.height);
        glUniform1i(pipeline_.source_kind, key.kind);
        glUniform1i(pipeline_.has_clip, key.mask != nullptr);
        if (key.gradient) {
            const auto g = key.gradient;
            glUniform4f(pipeline_.gradient_line, g->from.x, g->from.y, g->to.x, g->to.y);
            glUniform4fv(pipeline_.gradient_from, 1, g->a);
            glUniform4fv(pipeline_.gradient_to, 1, g->b);
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, key.texture);
        glBindSampler(0, pipeline_.samplers[key.mipmap ? 1 : 0]);
        glBindSampler(1, pipeline_.samplers[0]);
        if (key.mask) {
            const auto b = key.mask->bounds;
            glUniform4f(pipeline_.clip_target, b.x, b.y, b.width, b.height);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, key.mask->color.id);
            glActiveTexture(GL_TEXTURE0);
        }
    }
    void upload(const std::vector<Vertex> &vertices) {
        glBindVertexArray(pipeline_.vao);
        glBindBuffer(GL_ARRAY_BUFFER, pipeline_.vbo);
        // Orphan the streaming buffer; never wait on an in-flight batch.
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STREAM_DRAW);
    }
    void draw_batch() {
        if (vertices_.empty())
            return;
        uniforms(batch_);
        upload(vertices_);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        const bool erase = batch_.composite == CAIRO_OPERATOR_CLEAR || batch_.composite == CAIRO_OPERATOR_SOURCE;
        glUniform1i(pipeline_.erase, erase);
        if (erase) {
            glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
            glDrawArrays(GL_TRIANGLES, 0, vertices_.size());
        }
        if (batch_.composite != CAIRO_OPERATOR_CLEAR) {
            glUniform1i(pipeline_.erase, 0);
            glBlendFunc(GL_ONE, batch_.composite == CAIRO_OPERATOR_OVER ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE);
            glDrawArrays(GL_TRIANGLES, 0, vertices_.size());
        }
        vertices_.clear();
        batch_mask_.reset();
        batch_source_.reset();
        batch_texture_.reset();
        batch_gradient_.reset();
    }
    void start_batch(GLuint texture, int kind, bool mipmap = false, const std::shared_ptr<Gradient> &gradient = {}) {
        BatchKey key{texture, kind, mipmap, state_.composite, state_.clip, state_.clip_mask.get(), gradient.get()};
        if (!vertices_.empty() &&
            (!(key == batch_) || key.composite == CAIRO_OPERATOR_SOURCE || vertices_.size() >= 4092))
            draw_batch();
        batch_ = key;
        batch_mask_ = state_.clip_mask;
        batch_gradient_ = gradient;
    }
    static Vertex vertex(Point p, double u, double v, const double c[4], double alpha) {
        double a = std::clamp(c[3], 0.0, 1.0) * alpha;
        return {float(p.x),     float(p.y),     float(u), float(v),    float(c[0] * a),
                float(c[1] * a), float(c[2] * a), float(a), float(alpha)};
    }
    static constexpr double white[4] = {1, 1, 1, 1};
    // Emits device-space triangles filled with the current source.
    void source_geometry(const std::vector<Point> &points, double alpha = 1) {
        if (empty(state_.clip) || points.empty())
            return;
        const auto source = state_.source;
        if (source) {
            start_batch(source->id(), 3, !source->flipped);
            batch_source_ = source;
        } else if (state_.gradient) {
            start_batch(0, 6, false, state_.gradient);
        } else {
            start_batch(0, 0);
        }
        for (auto p : points) {
            double u = 0, v = 0;
            if (source) {
                u = (p.x - source->bounds.x) / source->bounds.width;
                v = (p.y - source->bounds.y) / source->bounds.height;
                if (source->flipped)
                    v = 1 - v;
            }
            vertices_.push_back(vertex(p, u, v, source || state_.gradient ? white : state_.color, alpha));
        }
    }
    static std::vector<Point> quad(Rect r) {
        return {{r.x, r.y},
                {r.x + r.width, r.y},
                {r.x + r.width, r.y + r.height},
                {r.x, r.y},
                {r.x + r.width, r.y + r.height},
                {r.x, r.y + r.height}};
    }
    // r is in device space.
    void textured_quad(const std::shared_ptr<Texture> &texture, int kind, Rect r, const double color[4],
                       double alpha) {
        if (empty(state_.clip) || empty(r) || empty(intersect(r, state_.clip)))
            return;
        start_batch(texture->id, kind);
        batch_texture_ = texture;
        const auto points = quad(r);
        const double uv[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
        for (int i = 0; i < 6; ++i)
            vertices_.push_back(vertex(points[i], uv[i][0], uv[i][1], color, alpha));
    }
    Rect path_bounds() const {
        double x = std::numeric_limits<double>::infinity(), y = x, right = -x, bottom = -x;
        for (const auto &path : paths_)
            for (auto p : path.points) {
                x = std::min(x, p.x);
                y = std::min(y, p.y);
                right = std::max(right, p.x);
                bottom = std::max(bottom, p.y);
            }
        return std::isfinite(x) ? Rect{x, y, right - x, bottom - y} : Rect{0, 0, 0, 0};
    }
    void consume_path() {
        paths_.clear();
        new_path_ = true;
    }
    std::vector<Point> fans() const {
        std::vector<Point> result;
        for (const auto &path : paths_)
            for (size_t i = 2; i < path.points.size(); ++i)
                result.insert(result.end(), {path.points[0], path.points[i - 1], path.points[i]});
        return result;
    }
    bool convex() const {
        const Path *shape = nullptr;
        for (const auto &path : paths_)
            if (path.points.size() >= 3) {
                if (shape)
                    return false;
                shape = &path;
            }
        if (!shape)
            return false;
        const auto &p = shape->points;
        double sign = 0, turn = 0;
        for (size_t i = 0; i < p.size(); ++i) {
            auto a = p[i], b = p[(i + 1) % p.size()], c = p[(i + 2) % p.size()];
            double cross = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
            if (std::abs(cross) < 1e-8)
                continue;
            if (sign * cross < 0)
                return false;
            sign = cross;
            turn += std::atan2(cross, (b.x - a.x) * (c.x - b.x) + (b.y - a.y) * (c.y - b.y));
        }
        return std::abs(turn) <= 2 * pi + 1e-5;
    }
    void stencil_geometry(const std::vector<Point> &geometry, Rect bounds, bool stroke) {
        if (geometry.empty() || empty(intersect(bounds, state_.clip)))
            return;
        draw_batch();
        auto region = intersect(integral(bounds), state_.clip);
        BatchKey key{0, 0, false, CAIRO_OPERATOR_OVER, region, nullptr, nullptr};
        uniforms(key);
        glEnable(GL_STENCIL_TEST);
        glStencilMask(255);
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDisable(GL_BLEND);
        glStencilFunc(GL_ALWAYS, 1, 255);
        const bool even_odd = state_.rule == CAIRO_FILL_RULE_EVEN_ODD;
        if (stroke)
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        else if (even_odd)
            glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
        else {
            glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
            glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
        }
        std::vector<Vertex> vertices;
        vertices.reserve(geometry.size());
        for (auto p : geometry)
            vertices.push_back(vertex(p, 0, 0, white, 1));
        upload(vertices);
        glDrawArrays(GL_TRIANGLES, 0, vertices.size());
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glStencilFunc(GL_NOTEQUAL, 0, even_odd && !stroke ? 1 : 255);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilMask(0);
        source_geometry(quad(integral(bounds)));
        draw_batch();
        glDisable(GL_STENCIL_TEST);
        glStencilMask(255);
    }
    std::shared_ptr<Texture> surface_texture(cairo_surface_t *surface) {
        auto token = surface_token(surface);
        auto it = surfaces_.find(surface);
        if (it != surfaces_.end() && it->second.lifetime.lock() == token) {
            it->second.used = frame_;
            return it->second.texture;
        }
        cairo_surface_flush(surface);
        const int w = cairo_image_surface_get_width(surface), h = cairo_image_surface_get_height(surface);
        const int stride = cairo_image_surface_get_stride(surface);
        const auto format = cairo_image_surface_get_format(surface);
        const auto data = cairo_image_surface_get_data(surface);
        if (!data || w <= 0 || h <= 0 || w > max_texture_ || h > max_texture_)
            return nullptr;
        if (format != CAIRO_FORMAT_ARGB32 && format != CAIRO_FORMAT_RGB24 && format != CAIRO_FORMAT_A8)
            return nullptr;
        draw_batch();
        std::vector<unsigned char> rgba(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y) {
            const auto row = data + size_t(y) * stride;
            auto out = rgba.data() + size_t(y) * w * 4;
            for (int x = 0; x < w; ++x, out += 4) {
                if (format == CAIRO_FORMAT_A8) {
                    out[0] = out[1] = out[2] = out[3] = row[x];
                    continue;
                }
                auto pixel = reinterpret_cast<const uint32_t *>(row)[x];
                out[0] = pixel >> 16;
                out[1] = pixel >> 8;
                out[2] = pixel;
                out[3] = format == CAIRO_FORMAT_RGB24 ? 255 : pixel >> 24;
            }
        }
        auto texture = std::make_shared<Texture>();
        texture->allocate(w, h, false, rgba.data());
        // Trilinear minification keeps downscaled icons from shimmering.
        glGenerateMipmap(GL_TEXTURE_2D);
        surfaces_[surface] = {token, texture, frame_};
        return texture;
    }
    void trim_caches() {
        size_t bytes = 0;
        for (auto it = surfaces_.begin(); it != surfaces_.end();) {
            if (it->second.lifetime.expired())
                it = surfaces_.erase(it);
            else {
                bytes += size_t(it->second.texture->width) * it->second.texture->height * 16 / 3;
                ++it;
            }
        }
        while (bytes > 128 * 1024 * 1024 && !surfaces_.empty()) {
            auto old = std::min_element(surfaces_.begin(), surfaces_.end(),
                                        [](auto &a, auto &b) { return a.second.used < b.second.used; });
            bytes -= size_t(old->second.texture->width) * old->second.texture->height * 16 / 3;
            surfaces_.erase(old);
        }
        bytes = 0;
        for (auto &[key, entry] : texts_)
            if (entry.texture)
                bytes += size_t(entry.texture->width) * entry.texture->height * (entry.raster.rgba ? 4 : 1);
        while ((bytes > 16 * 1024 * 1024 || texts_.size() > 2048) && !texts_.empty()) {
            auto old = std::min_element(texts_.begin(), texts_.end(),
                                        [](auto &a, auto &b) { return a.second.used < b.second.used; });
            if (old->second.used == frame_)
                break; // everything left is in use this frame
            if (old->second.texture)
                bytes -= size_t(old->second.texture->width) * old->second.texture->height *
                         (old->second.raster.rgba ? 4 : 1);
            texts_.erase(old);
        }
        size_t idle = 0;
        std::erase_if(pool_, [&](const auto &t) {
            if (t.use_count() != 1)
                return false;
            idle += t->bytes();
            return idle > std::max(size_t(64 * 1024 * 1024), root_->bytes() * 2);
        });
    }
    void stroke_mesh(std::vector<Point> &mesh) {
        const double half = state_.line_width / 2;
        const auto &tf = state_.transform;
        auto triangle = [&](Point a, Point b, Point c) {
            mesh.insert(mesh.end(), {tf.apply(a), tf.apply(b), tf.apply(c)});
        };
        auto disk = [&](Point center) {
            double radius = half * std::max(std::abs(tf.sx), std::abs(tf.sy));
            int n = std::clamp(int(std::ceil(pi / std::acos(std::clamp(1 - .15 / std::max(.15, radius), -1.0, 1.0)))),
                               8, 256);
            for (int i = 0; i < n; ++i)
                triangle(center, {center.x + half * std::cos(2 * pi * i / n), center.y + half * std::sin(2 * pi * i / n)},
                         {center.x + half * std::cos(2 * pi * (i + 1) / n),
                          center.y + half * std::sin(2 * pi * (i + 1) / n)});
        };
        auto polyline = [&](std::vector<Point> points, bool closed, bool degenerate_dot) {
            if (points.size() == 1 && degenerate_dot && state_.cap == CAIRO_LINE_CAP_ROUND)
                disk(points.front());
            if (points.size() < 2)
                return;
            if (closed && std::hypot(points.front().x - points.back().x, points.front().y - points.back().y) < 1e-8)
                points.pop_back();
            if (points.size() < 2)
                return;
            size_t segments = points.size() - (closed ? 0 : 1);
            std::vector<Point> normals;
            for (size_t i = 0; i < segments; ++i) {
                auto a = points[i], b = points[(i + 1) % points.size()];
                double length = std::hypot(b.x - a.x, b.y - a.y);
                Point direction{(b.x - a.x) / length, (b.y - a.y) / length};
                Point n{-direction.y * half, direction.x * half};
                normals.push_back(n);
                if (!closed && state_.cap == CAIRO_LINE_CAP_SQUARE) {
                    if (i == 0) {
                        a.x -= direction.x * half;
                        a.y -= direction.y * half;
                    }
                    if (i + 1 == segments) {
                        b.x += direction.x * half;
                        b.y += direction.y * half;
                    }
                }
                Point p{a.x + n.x, a.y + n.y}, q{b.x + n.x, b.y + n.y}, r{b.x - n.x, b.y - n.y},
                    s{a.x - n.x, a.y - n.y};
                triangle(p, q, r);
                triangle(p, r, s);
            }
            size_t first = closed ? 0 : 1, last = closed ? points.size() : points.size() - 1;
            for (size_t i = first; i < last; ++i) {
                auto center = points[i];
                if (state_.join == CAIRO_LINE_JOIN_ROUND) {
                    disk(center);
                    continue;
                }
                auto a = normals[(i + segments - 1) % segments], b = normals[i % segments];
                double denom = half * half + a.x * b.x + a.y * b.y;
                for (double side : {-1.0, 1.0}) {
                    Point p{center.x + side * a.x, center.y + side * a.y},
                        q{center.x + side * b.x, center.y + side * b.y};
                    triangle(p, center, q);
                    if (state_.join == CAIRO_LINE_JOIN_BEVEL)
                        continue;
                    // Cairo's default miter limit (10), with bevel fallback.
                    Point m = center;
                    if (std::abs(denom) > 1e-10) {
                        m.x += side * (a.x + b.x) * half * half / denom;
                        m.y += side * (a.y + b.y) * half * half / denom;
                    }
                    if (std::hypot(m.x - center.x, m.y - center.y) > half * 10)
                        m = center;
                    triangle(p, m, q);
                }
            }
            if (!closed && state_.cap == CAIRO_LINE_CAP_ROUND) {
                disk(points.front());
                disk(points.back());
            }
        };
        // Splits a user-space polyline into dashes.
        auto dashed = [&](const std::vector<Point> &points, bool closed) {
            double total = 0;
            for (double d : state_.dashes)
                total += std::max(0.0, d);
            if (total <= 0) {
                polyline(points, closed, false);
                return;
            }
            std::vector<Point> all = points;
            if (closed && !points.empty())
                all.push_back(points.front());
            size_t index = 0;
            double remaining = std::fmod(state_.dash_offset, total);
            if (remaining < 0)
                remaining += total;
            while (remaining >= state_.dashes[index]) {
                remaining -= state_.dashes[index];
                index = (index + 1) % state_.dashes.size();
            }
            remaining = state_.dashes[index] - remaining;
            bool on = index % 2 == 0;
            std::vector<Point> current;
            if (on && !all.empty())
                current.push_back(all.front());
            for (size_t i = 1; i < all.size(); ++i) {
                Point a = all[i - 1], b = all[i];
                double length = std::hypot(b.x - a.x, b.y - a.y), t = 0;
                while (length - t > remaining) {
                    t += remaining;
                    Point p{a.x + (b.x - a.x) * t / length, a.y + (b.y - a.y) * t / length};
                    if (on) {
                        current.push_back(p);
                        polyline(current, false, false);
                        current.clear();
                    } else {
                        current = {p};
                    }
                    on = !on;
                    index = (index + 1) % state_.dashes.size();
                    remaining = state_.dashes[index];
                }
                remaining -= length - t;
                if (on)
                    current.push_back(b);
            }
            if (on && current.size() >= 2)
                polyline(current, false, false);
        };
        for (const auto &path : paths_) {
            std::vector<Point> points;
            for (auto p : path.points) {
                p = tf.inverse(p);
                if (points.empty() || std::hypot(p.x - points.back().x, p.y - points.back().y) > 1e-8)
                    points.push_back(p);
            }
            if (state_.dashes.empty())
                polyline(points, path.closed, path.points.size() > 1);
            else if (points.size() >= 2)
                dashed(points, path.closed);
        }
    }
    void stroke_impl() {
        if (state_.line_width <= 0 || state_.transform.sx == 0 || state_.transform.sy == 0)
            return;
        std::vector<Point> mesh;
        stroke_mesh(mesh);
        Rect bounds = path_bounds();
        double pad = state_.line_width * 5 * std::max(std::abs(state_.transform.sx), std::abs(state_.transform.sy));
        bounds = {bounds.x - pad, bounds.y - pad, bounds.width + 2 * pad, bounds.height + 2 * pad};
        stencil_geometry(mesh, bounds, true);
    }
    void paint_impl(double alpha) {
        auto bounds = intersect(state_.clip, target_->bounds);
        if (state_.source && (state_.composite == CAIRO_OPERATOR_OVER || state_.composite == CAIRO_OPERATOR_ADD))
            bounds = intersect(bounds, state_.source->bounds);
        source_geometry(quad(bounds), std::clamp(alpha, 0.0, 1.0));
        consume_path();
    }
    Point user_current() const { return state_.transform.inverse(current_); }

  public:
    GLContext(int width, int height) {
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_);
        GLint maximum;
        glGetIntegerv(GL_MAX_SAMPLES, &maximum);
        samples_ = std::max(1, std::min(4, maximum));
        pipeline_.initialize();
        vertices_.reserve(4096);
        resize(width, height);
        begin_frame();
    }
    ~GLContext() override = default;
    void resize(int width, int height) override {
        if (width <= 0 || height <= 0)
            throw std::invalid_argument("Invalid OpenGL target size");
        draw_batch();
        saved_.clear();
        groups_.clear();
        state_ = {};
        root_.reset();
        target_.reset();
        pool_.clear();
        root_ = target({0, 0, double(width), double(height)}, false);
        target_ = root_;
        state_.clip = root_->bounds;
        consume_path();
    }
    void begin_frame() override {
        draw_batch();
        // A paint callback that threw or forgot a restore must not poison
        // every following frame.
        saved_.clear();
        groups_.clear();
        ++frame_;
        trim_caches();
        state_ = {};
        state_.clip = root_->bounds;
        target_ = root_;
        consume_path();
        glUseProgram(pipeline_.program);
        glBindVertexArray(pipeline_.vao);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_DITHER);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        clear_target();
    }
    void save() override { saved_.push_back(state_); }
    void restore() override {
        if (saved_.empty() || (!groups_.empty() && saved_.size() <= groups_.back().depth))
            return; // Cairo ignores (and flags) unbalanced restores; never crash a dock over it.
        // A queued batch retains references to any textures the restored state drops.
        state_ = std::move(saved_.back());
        saved_.pop_back();
    }
    void translate(double x, double y) override {
        state_.transform.tx += x * state_.transform.sx;
        state_.transform.ty += y * state_.transform.sy;
    }
    void scale(double x, double y) override {
        state_.transform.sx *= x;
        state_.transform.sy *= y;
    }
    void new_path() override { consume_path(); }
    void new_sub_path() override { new_path_ = true; }
    void move_to(double x, double y) override {
        current_ = state_.transform.apply({x, y});
        paths_.push_back({{current_}, false});
        new_path_ = false;
    }
    void line_to(double x, double y) override {
        if (new_path_ || paths_.empty())
            move_to(x, y);
        else {
            current_ = state_.transform.apply({x, y});
            paths_.back().points.push_back(current_);
        }
    }
    void arc(double x, double y, double radius, double start, double end) override {
        if (radius < 0)
            return;
        while (end < start)
            end += 2 * pi;
        end = std::min(end, start + 2 * pi);
        const double device_radius = radius * std::max(std::abs(state_.transform.sx), std::abs(state_.transform.sy));
        const double step = device_radius > .2 ? 2 * std::acos(std::clamp(1 - .2 / device_radius, -1.0, 1.0)) : pi / 2;
        const int count = std::clamp(int(std::ceil((end - start) / std::max(.001, step))), 1, 8192);
        for (int i = 0; i <= count; ++i) {
            double a = start + (end - start) * i / count;
            line_to(x + radius * std::cos(a), y + radius * std::sin(a));
        }
    }
    void close_path() override {
        if (!paths_.empty() && !paths_.back().points.empty()) {
            paths_.back().closed = true;
            // Cairo retains the starting point for a following line/arc.
            current_ = paths_.back().points.front();
            paths_.push_back({{current_}, false});
            new_path_ = false;
        }
    }
    void rectangle(double x, double y, double width, double height) override {
        move_to(x, y);
        line_to(x + width, y);
        line_to(x + width, y + height);
        line_to(x, y + height);
        close_path();
    }
    void fill_preserve() override {
        auto geometry = fans();
        if (convex())
            source_geometry(geometry);
        else
            stencil_geometry(geometry, path_bounds(), false);
    }
    void fill() override {
        fill_preserve();
        consume_path();
    }
    void stroke_preserve() override { stroke_impl(); }
    void stroke() override {
        stroke_impl();
        consume_path();
    }
    void paint_source(double alpha) override { paint_impl(alpha); }
    void clip_path() override {
        auto bounds = intersect(state_.clip, path_bounds());
        // Axis-aligned rectangles are the common UI clip and need no mask allocation.
        const Path *rectangle_path = nullptr;
        for (const auto &p : paths_)
            if (p.points.size() >= 3) {
                if (rectangle_path) {
                    rectangle_path = nullptr;
                    break;
                }
                rectangle_path = &p;
            }
        bool rect = rectangle_path && rectangle_path->points.size() == 4;
        if (rect)
            for (size_t i = 0; i < 4; ++i) {
                auto a = rectangle_path->points[i], b = rectangle_path->points[(i + 1) % 4];
                if (a.x != b.x && a.y != b.y)
                    rect = false;
            }
        if (rect || empty(bounds)) {
            if (rect) {
                // close_path leaves move-only subpaths. They have no fill area
                // and must not enlarge the rectangular clip.
                const auto &points = rectangle_path->points;
                double left = points[0].x, right = left, top = points[0].y, bottom = top;
                for (const auto &p : points) {
                    left = std::min(left, p.x);
                    right = std::max(right, p.x);
                    top = std::min(top, p.y);
                    bottom = std::max(bottom, p.y);
                }
                bounds = intersect(state_.clip, {left, top, right - left, bottom - top});
            }
            state_.clip = bounds;
            consume_path();
            return;
        }
        draw_batch();
        auto parent = target_;
        auto saved = state_;
        target_ = target(bounds);
        clear_target();
        state_.clip = bounds;
        state_.source.reset();
        state_.gradient.reset();
        state_.color[0] = state_.color[1] = state_.color[2] = state_.color[3] = 1;
        state_.composite = CAIRO_OPERATOR_OVER;
        fill();
        draw_batch();
        target_->resolved();
        auto mask = target_;
        target_ = parent;
        state_ = saved;
        state_.clip = bounds;
        state_.clip_mask = mask;
        bind_target();
    }
    void reset_clip() override {
        state_.clip = target_->bounds;
        state_.clip_mask.reset();
    }
    void set_source_rgba(double r, double g, double b, double a) override {
        state_.color[0] = r;
        state_.color[1] = g;
        state_.color[2] = b;
        state_.color[3] = a;
        state_.source.reset();
        state_.gradient.reset();
    }
    void set_linear_gradient(double x0, double y0, double x1, double y1, const double from[4],
                             const double to[4]) override {
        auto g = std::make_shared<Gradient>();
        g->from = state_.transform.apply({x0, y0});
        g->to = state_.transform.apply({x1, y1});
        for (int i = 0; i < 3; ++i) {
            g->a[i] = from[i] * from[3];
            g->b[i] = to[i] * to[3];
        }
        g->a[3] = from[3];
        g->b[3] = to[3];
        state_.gradient = std::move(g);
        state_.source.reset();
    }
    void set_source_surface(cairo_surface_t *surface, double x, double y) override {
        state_.gradient.reset();
        state_.source.reset();
        if (!surface || cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE)
            return;
        auto texture = surface_texture(surface);
        if (!texture) {
            state_.color[3] = 0;
            return;
        }
        auto a = state_.transform.apply({x, y});
        auto b = state_.transform.apply({x + texture->width, y + texture->height});
        auto source = std::make_shared<Source>();
        source->texture = texture;
        source->bounds = {std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y)};
        state_.source = std::move(source);
    }
    void set_line_width(double width) override { state_.line_width = width; }
    double get_line_width() override { return state_.line_width; }
    void set_line_cap(cairo_line_cap_t cap) override { state_.cap = cap; }
    void set_line_join(cairo_line_join_t join) override { state_.join = join; }
    void set_dash(const double *dashes, int count, double offset) override {
        state_.dashes.clear();
        state_.dash_offset = offset;
        bool any = false;
        for (int i = 0; dashes && i < count; ++i) {
            state_.dashes.push_back(std::max(0.0, dashes[i]));
            any |= dashes[i] > 0;
        }
        if (!any)
            state_.dashes.clear();
        else if (state_.dashes.size() % 2)
            // Cairo repeats an odd dash array to alternate on/off.
            state_.dashes.insert(state_.dashes.end(), state_.dashes.begin(), state_.dashes.end());
    }
    void set_fill_rule(cairo_fill_rule_t rule) override { state_.rule = rule; }
    void set_operator(cairo_operator_t op) override { state_.composite = supported(op); }
    void push_group() override {
        draw_batch();
        groups_.push_back({state_, target_, saved_.size()});
        target_ = target(intersect(state_.clip, target_->bounds));
        clear_target();
    }
    void pop_group_to_source() override {
        if (groups_.empty() || saved_.size() != groups_.back().depth)
            return;
        draw_batch();
        target_->resolved();
        auto source = std::make_shared<Source>();
        source->group = target_;
        source->bounds = target_->bounds;
        source->flipped = true;
        auto group = std::move(groups_.back());
        groups_.pop_back();
        state_ = std::move(group.state);
        target_ = std::move(group.parent);
        state_.gradient.reset();
        state_.source = std::move(source);
        bind_target();
    }
    void show_layout(PangoLayout *layout) override {
        if (!layout || empty(state_.clip))
            return;
        const bool own_colors = layout_has_own_colors(layout);
        std::string key = layout_key(layout);
        if (own_colors) {
            // Color glyphs/markup render with the source color baked in.
            char color[96];
            std::snprintf(color, sizeof color, "\x1e%.4f,%.4f,%.4f,%.4f", state_.color[0], state_.color[1],
                          state_.color[2], state_.color[3]);
            key.append(color);
        }
        auto [it, inserted] = texts_.try_emplace(std::move(key));
        auto &entry = it->second;
        entry.used = frame_;
        if (inserted) {
            const double opaque[4] = {state_.color[0], state_.color[1], state_.color[2], 1};
            entry.raster = rasterize_layout(layout, own_colors, opaque);
            if (entry.raster.width && entry.raster.height && entry.raster.width <= max_texture_ &&
                entry.raster.height <= max_texture_) {
                draw_batch();
                entry.texture = std::make_shared<Texture>();
                entry.texture->allocate(entry.raster.width, entry.raster.height, !entry.raster.rgba,
                                        entry.raster.pixels.data());
            }
            entry.raster.pixels.clear();
            entry.raster.pixels.shrink_to_fit();
        }
        if (!entry.texture)
            return;
        const auto &tf = state_.transform;
        // Snap the layout origin to the pixel grid so glyphs stay crisp.
        Point origin{std::round(current_.x), std::round(current_.y)};
        Rect r{origin.x + entry.raster.x * tf.sx, origin.y + entry.raster.y * tf.sy, entry.raster.width * tf.sx,
               entry.raster.height * tf.sy};
        if (r.width < 0) {
            r.x += r.width;
            r.width = -r.width;
        }
        if (r.height < 0) {
            r.y += r.height;
            r.height = -r.height;
        }
        if (entry.raster.rgba) {
            textured_quad(entry.texture, 1, r, white, std::clamp(state_.color[3], 0.0, 1.0));
        } else {
            textured_quad(entry.texture, 2, r, state_.color, 1);
        }
    }
    void layout_path(PangoLayout *layout) override {
        if (!layout)
            return;
        auto cr = scratch_cairo();
        cairo_save(cr);
        cairo_identity_matrix(cr);
        cairo_new_path(cr);
        cairo_move_to(cr, 0, 0);
        pango_cairo_layout_path(cr, layout);
        auto path = cairo_copy_path_flat(cr);
        cairo_new_path(cr);
        cairo_restore(cr);
        const auto origin = user_current();
        for (int i = 0; i < path->num_data; i += path->data[i].header.length) {
            const auto &header = path->data[i].header;
            switch (header.type) {
            case CAIRO_PATH_MOVE_TO:
                move_to(origin.x + path->data[i + 1].point.x, origin.y + path->data[i + 1].point.y);
                break;
            case CAIRO_PATH_LINE_TO:
                line_to(origin.x + path->data[i + 1].point.x, origin.y + path->data[i + 1].point.y);
                break;
            case CAIRO_PATH_CLOSE_PATH:
                close_path();
                break;
            default:
                break;
            }
        }
        cairo_path_destroy(path);
    }
    void flush() override {
        draw_batch();
        saved_.clear();
        groups_.clear();
        target_ = root_;
        glDisable(GL_SCISSOR_TEST);
        // Resolve straight into EGL's single-sample back buffer: one GPU blit.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, root_->draw);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, root_->bounds.width, root_->bounds.height, 0, 0, root_->bounds.width,
                          root_->bounds.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glFlush();
    }
};
} // namespace

std::unique_ptr<OpenGLContext> create_opengl_context(int width, int height) {
    GLint major = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    if (major < 3)
        throw std::runtime_error("OpenGL ES 3 context is required");
    return std::make_unique<GLContext>(width, height);
}
} // namespace drawing
