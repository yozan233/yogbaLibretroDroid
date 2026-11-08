/*
 *     Copyright (C) 2019  Filippo Scognamiglio
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <cstdlib>
#include <string>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <utility>
#include <sstream>
#include <cctype>

#include "log.h"

#include "video.h"
#include "renderers/es3/framebufferrenderer.h"
#include "renderers/es3/imagerendereres3.h"
#include "renderers/es2/imagerendereres2.h"

namespace libretrodroid {

static void printGLString(const char *name, GLenum s) {
    const char *v = (const char *) glGetString(s);
    LOGI("GL %s = %s\n", name, v);
}

static GLint getAttribLocationWithFallback(
    GLuint program,
    const char* primary,
    const char* fallback,
    const char* legacy = nullptr
) {
    GLint handle = glGetAttribLocation(program, primary);
    if (handle == -1 && fallback != nullptr) {
        handle = glGetAttribLocation(program, fallback);
    }
    if (handle == -1 && legacy != nullptr) {
        handle = glGetAttribLocation(program, legacy);
    }
    return handle;
}

static GLint getUniformLocationWithFallback(
    GLuint program,
    const char* primary,
    const char* fallback,
    const char* legacy = nullptr
) {
    GLint handle = glGetUniformLocation(program, primary);
    if (handle == -1 && fallback != nullptr) {
        handle = glGetUniformLocation(program, fallback);
    }
    if (handle == -1 && legacy != nullptr) {
        handle = glGetUniformLocation(program, legacy);
    }
    return handle;
}

GLuint loadShader(GLenum shaderType, const char* pSource) {
    // Some Adreno drivers only recognize #version when it is the first byte,
    // even though GLSL permits leading whitespace. XML CDATA commonly starts
    // with a newline, so normalize the source before compiling.
    while (*pSource != '\0' && std::isspace(static_cast<unsigned char>(*pSource))) {
        ++pSource;
    }

    GLuint shader = glCreateShader(shaderType);
    if (shader) {
        glShaderSource(shader, 1, &pSource, nullptr);
        glCompileShader(shader);
        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            GLint infoLen = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
            if (infoLen) {
                char* buf = (char*) malloc(infoLen);
                if (buf) {
                    glGetShaderInfoLog(shader, infoLen, nullptr, buf);
                    LOGE("Could not compile shader %d:\n%s\n",
                         shaderType, buf);
                    free(buf);
                }
            }
            glDeleteShader(shader);
            shader = 0;
        }
    }
    return shader;
}

GLuint createProgram(const char* pVertexSource, const char* pFragmentSource) {
    GLuint vertexShader = loadShader(GL_VERTEX_SHADER, pVertexSource);
    if (!vertexShader) {
        return 0;
    }

    GLuint pixelShader = loadShader(GL_FRAGMENT_SHADER, pFragmentSource);
    if (!pixelShader) {
        glDeleteShader(vertexShader);
        return 0;
    }

    GLuint program = glCreateProgram();
    if (program) {
        glAttachShader(program, vertexShader);
        glAttachShader(program, pixelShader);
        glLinkProgram(program);
        GLint linkStatus = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
        if (linkStatus != GL_TRUE) {
            GLint bufLength = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &bufLength);
            if (bufLength) {
                char* buf = (char*) malloc(bufLength);
                if (buf) {
                    glGetProgramInfoLog(program, bufLength, nullptr, buf);
                    LOGE("Could not link program:\n%s\n", buf);
                    free(buf);
                }
            }
            glDeleteProgram(program);
            program = 0;
        }
    }
    // Once linked, the program retains the compiled code. Keeping the attached
    // shader objects alive leaks GL resources every time an external filter changes.
    glDeleteShader(vertexShader);
    glDeleteShader(pixelShader);
    return program;
}

void Video::updateProgram() {
    if (loadedShaderType.has_value() && loadedShaderType.value() == requestedShaderConfig) {
        return;
    }

    auto shaders = ShaderManager::getShader(requestedShaderConfig);
    auto compilePasses = [](const ShaderManager::Chain& chain, std::vector<ShaderChainEntry>& output) {
        for (size_t passIndex = 0; passIndex < chain.passes.size(); ++passIndex) {
            const auto& item = chain.passes[passIndex];
            auto shader = ShaderChainEntry { };

            shader.gProgram = createProgram(item.vertex.data(), item.fragment.data());
            if (!shader.gProgram) {
                LOGE("Could not create GL program for shader pass %zu.", passIndex);
                for (const auto& compiledShader : output) {
                    if (compiledShader.gProgram != 0) glDeleteProgram(compiledShader.gProgram);
                }
                output.clear();
                return false;
            }

            shader.gvPositionHandle = getAttribLocationWithFallback(
                shader.gProgram,
                "vPosition",
                "aPosition",
                "a_vertex_coordinate"
            );
            shader.gvCoordinateHandle = getAttribLocationWithFallback(
                shader.gProgram,
                "vCoordinate",
                "aTexCoord",
                "a_texcoord"
            );
            shader.gTextureHandle = getUniformLocationWithFallback(
                shader.gProgram,
                "texture",
                "rubyTexture",
                "sampler0"
            );
            shader.gPreviousPassTextureHandle = glGetUniformLocation(shader.gProgram, "previousPass");
            shader.gTextureSizeHandle = getUniformLocationWithFallback(
                shader.gProgram,
                "textureSize",
                "rubyTextureSize"
            );
            shader.gLegacyTextureSizeHandle = glGetUniformLocation(shader.gProgram, "u_texture_size");
            shader.gInputSizeHandle = glGetUniformLocation(shader.gProgram, "rubyInputSize");
            shader.gOutputSizeHandle = glGetUniformLocation(shader.gProgram, "rubyOutputSize");
            shader.gHistoryTexturesHandle = glGetUniformLocation(shader.gProgram, "historyTexture");
            shader.gAuxiliaryTexturesHandle = glGetUniformLocation(shader.gProgram, "textures");
            shader.gScreenDensityHandle = glGetUniformLocation(shader.gProgram, "screenDensity");
            shader.textureCoordinateBias = item.textureCoordinateBias;
            output.push_back(shader);
        }

        return true;
    };

    std::vector<ShaderChainEntry> newShadersChain;
    if (!compilePasses(shaders, newShadersChain)) {
        // There is no previous valid chain during first launch. Install the
        // default shader so an invalid imported filter cannot leave a black screen.
        if (shadersChain.empty()) {
            auto fallbackShaders = ShaderManager::getShader({ShaderManager::Type::SHADER_DEFAULT});
            std::vector<ShaderChainEntry> fallbackChain;
            if (compilePasses(fallbackShaders, fallbackChain)) {
                shadersChain = std::move(fallbackChain);
                activeShaders = fallbackShaders;
                renderer->setShaders(fallbackShaders);
                LOGE("External shader failed during startup; using the default shader.");
            }
        }
        // Keep the last valid chain active instead of terminating the app or
        // recompiling the same invalid source on every frame.
        loadedShaderType = requestedShaderConfig;
        return;
    }

    for (const auto& shader : shadersChain) {
        if (shader.gProgram != 0) {
            glDeleteProgram(shader.gProgram);
        }
    }
    shadersChain = std::move(newShadersChain);
    releaseCustomTextures();

    GLint maxTextureUnits = 1;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxTextureUnits);
    unsigned int availableTextureUnits = static_cast<unsigned int>(std::max(0, maxTextureUnits - 1));
    activeHistoryCount = std::min(shaders.historyCount, availableTextureUnits);
    unsigned int availableAuxiliaryUnits = availableTextureUnits - activeHistoryCount;
    createAuxiliaryTextures(shaders, availableAuxiliaryUnits);
    // Pixel payloads are only needed while creating GL textures. Do not copy
    // them into renderer state or retain duplicate CPU-side image buffers.
    shaders.auxiliaryTextures.clear();
    activeShaders = shaders;

    renderer->setShaders(shaders);
    loadedShaderType = requestedShaderConfig;
}

void Video::releaseCustomTextures() {
    if (!historyTextures.empty()) {
        glDeleteTextures(static_cast<GLsizei>(historyTextures.size()), historyTextures.data());
        historyTextures.clear();
    }
    if (!auxiliaryTextures.empty()) {
        glDeleteTextures(static_cast<GLsizei>(auxiliaryTextures.size()), auxiliaryTextures.data());
        auxiliaryTextures.clear();
    }
    if (historyReadFramebuffer != 0) {
        glDeleteFramebuffers(1, &historyReadFramebuffer);
        historyReadFramebuffer = 0;
    }
    if (historyCopyProgram != 0) {
        glDeleteProgram(historyCopyProgram);
        historyCopyProgram = 0;
    }
    historyCopyPositionHandle = -1;
    historyCopyCoordinateHandle = -1;
    historyCopyTextureHandle = -1;
    historyWriteIndex = 0;
    historyTextureSize = {0, 0};
    activeHistoryCount = 0;
}

void Video::createAuxiliaryTextures(
    const ShaderManager::Chain& shaders,
    unsigned int maxCount
) {
    unsigned int count = std::min(
        static_cast<unsigned int>(shaders.auxiliaryTextures.size()),
        maxCount
    );
    if (count == 0) return;

    auxiliaryTextures.resize(count);
    glGenTextures(static_cast<GLsizei>(count), auxiliaryTextures.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    for (unsigned int i = 0; i < count; ++i) {
        const auto& source = shaders.auxiliaryTextures[i];
        size_t requiredBytes = static_cast<size_t>(source.width) * source.height * 4;
        if (source.width == 0 || source.height == 0 || source.rgba8888.size() < requiredBytes) {
            continue;
        }

        glBindTexture(GL_TEXTURE_2D, auxiliaryTextures[i]);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA,
            static_cast<GLsizei>(source.width),
            static_cast<GLsizei>(source.height),
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            source.rgba8888.data()
        );
        glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_MIN_FILTER,
            shaders.linearTexture ? GL_LINEAR : GL_NEAREST
        );
        glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_MAG_FILTER,
            shaders.linearTexture ? GL_LINEAR : GL_NEAREST
        );
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

void Video::ensureHistoryTextures(unsigned int count) {
    int width = renderer->textureSize.first;
    int height = renderer->textureSize.second;
    if (count == 0 || width <= 0 || height <= 0) return;
    if (historyTextures.size() == count && historyTextureSize == std::make_pair(width, height)) {
        return;
    }

    if (!historyTextures.empty()) {
        glDeleteTextures(static_cast<GLsizei>(historyTextures.size()), historyTextures.data());
    }
    historyTextures.assign(count, 0);
    glGenTextures(static_cast<GLsizei>(count), historyTextures.data());

    std::vector<unsigned char> emptyFrame(static_cast<size_t>(width) * height * 4, 0);
    for (GLuint texture : historyTextures) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA,
            width,
            height,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            emptyFrame.data()
        );
        glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_MIN_FILTER,
            activeShaders.linearTexture ? GL_LINEAR : GL_NEAREST
        );
        glTexParameteri(
            GL_TEXTURE_2D,
            GL_TEXTURE_MAG_FILTER,
            activeShaders.linearTexture ? GL_LINEAR : GL_NEAREST
        );
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    if (historyReadFramebuffer == 0) {
        glGenFramebuffers(1, &historyReadFramebuffer);
    }
    if (historyCopyProgram == 0) {
        static const char* copyVertex =
            "attribute vec2 vPosition;\n"
            "attribute vec2 vCoordinate;\n"
            "varying vec2 coords;\n"
            "void main() {\n"
            "  gl_Position = vec4(vPosition, 0.0, 1.0);\n"
            "  coords = vCoordinate;\n"
            "}\n";
        static const char* copyFragment =
            "precision mediump float;\n"
            "uniform sampler2D texture;\n"
            "varying vec2 coords;\n"
            "void main() {\n"
            "  gl_FragColor = texture2D(texture, coords);\n"
            "}\n";
        historyCopyProgram = createProgram(copyVertex, copyFragment);
        historyCopyPositionHandle = glGetAttribLocation(historyCopyProgram, "vPosition");
        historyCopyCoordinateHandle = glGetAttribLocation(historyCopyProgram, "vCoordinate");
        historyCopyTextureHandle = glGetUniformLocation(historyCopyProgram, "texture");
    }
    historyTextureSize = {width, height};
    historyWriteIndex = 0;
}

void Video::captureCurrentFrameToHistory() {
    if (historyTextures.empty()) return;

    static const GLfloat vertices[] = {
        -1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };
    static const GLfloat coordinates[] = {
        0.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f,
    };

    glBindFramebuffer(GL_FRAMEBUFFER, historyReadFramebuffer);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D,
        historyTextures[historyWriteIndex],
        0
    );
    glViewport(0, 0, historyTextureSize.first, historyTextureSize.second);
    glUseProgram(historyCopyProgram);

    glVertexAttribPointer(historyCopyPositionHandle, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(historyCopyPositionHandle);
    glVertexAttribPointer(historyCopyCoordinateHandle, 2, GL_FLOAT, GL_FALSE, 0, coordinates);
    glEnableVertexAttribArray(historyCopyCoordinateHandle);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(renderer->getTexture()));
    glUniform1i(historyCopyTextureHandle, 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDisableVertexAttribArray(historyCopyPositionHandle);
    glDisableVertexAttribArray(historyCopyCoordinateHandle);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    historyWriteIndex = (historyWriteIndex + 1) % historyTextures.size();
}

void Video::renderFrame() {
    if (skipDuplicateFrames && !isDirty) return;
    bool hasNewFrame = isDirty;
    isDirty = false;

    glDisable(GL_DEPTH_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (immersiveModeEnabled) {
        immersiveMode.renderBackground(
            videoLayout.getScreenWidth(),
            videoLayout.getScreenHeight(),
            videoLayout.getBackgroundVertices(),
            videoLayout.getRelativeForegroundBounds(),
            videoLayout.getFramebufferVertices().data(),
            renderer->getTexture()
        );
    }

    updateProgram();
    ensureHistoryTextures(activeHistoryCount);
    for (int i = 0; i < shadersChain.size(); ++i) {
        auto shader = shadersChain[i];
        auto passData = renderer->getPassData(i);
        auto isLastPass = i == shadersChain.size() - 1;

        glBindFramebuffer(GL_FRAMEBUFFER, passData.framebuffer.value_or(0));

        glViewport(
            0,
            0,
            passData.width.value_or(videoLayout.getScreenWidth()),
            passData.height.value_or(videoLayout.getScreenHeight())
        );

        glUseProgram(shader.gProgram);

        auto vertices = isLastPass ? videoLayout.getForegroundVertices() : videoLayout.getFramebufferVertices();
        if (shader.gvPositionHandle != -1) {
            glVertexAttribPointer(shader.gvPositionHandle, 2, GL_FLOAT, GL_FALSE, 0, vertices.data());
            glEnableVertexAttribArray(shader.gvPositionHandle);
        }

        auto coordinates = videoLayout.getTextureCoordinates();
        // CMOf stores GLES2 frames in a POT backing texture. Its first pass
        // only addresses the active image rectangle; later passes address
        // their complete intermediate framebuffer.
        if (i == 0 && renderer->textureSize.first > 0 && renderer->textureSize.second > 0) {
            float scaleX = getTextureWidth() / static_cast<float>(renderer->textureSize.first);
            float scaleY = getTextureHeight() / static_cast<float>(renderer->textureSize.second);
            for (size_t coordinate = 0; coordinate < coordinates.size(); coordinate += 2) {
                coordinates[coordinate] *= scaleX;
                coordinates[coordinate + 1] *= scaleY;
            }
        }
        if (i == 0 && shader.textureCoordinateBias) {
            for (size_t coordinate = 0; coordinate < coordinates.size(); coordinate += 2) {
                coordinates[coordinate] += 0.0001F;
                coordinates[coordinate + 1] += 0.0001F;
            }
        }
        if (shader.gvCoordinateHandle != -1) {
            glVertexAttribPointer(shader.gvCoordinateHandle, 2, GL_FLOAT, GL_FALSE, 0, coordinates.data());
            glEnableVertexAttribArray(shader.gvCoordinateHandle);
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, renderer->getTexture());
        if (shader.gTextureHandle != -1) {
            glUniform1i(shader.gTextureHandle, 0);
        }

        unsigned int nextTextureUnit = 1;
        if (shader.gHistoryTexturesHandle != -1 && !historyTextures.empty()) {
            std::vector<GLint> historyUnits(historyTextures.size());
            std::iota(historyUnits.begin(), historyUnits.end(), 1);
            for (size_t history = 0; history < historyTextures.size(); ++history) {
                size_t textureIndex =
                    (historyWriteIndex + historyTextures.size() - history - 1) % historyTextures.size();
                glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(history + 1));
                glBindTexture(GL_TEXTURE_2D, historyTextures[textureIndex]);
            }
            glUniform1iv(
                shader.gHistoryTexturesHandle,
                static_cast<GLsizei>(historyUnits.size()),
                historyUnits.data()
            );
        }
        nextTextureUnit += static_cast<unsigned int>(historyTextures.size());

        if (shader.gAuxiliaryTexturesHandle != -1 && !auxiliaryTextures.empty()) {
            std::vector<GLint> auxiliaryUnits(auxiliaryTextures.size());
            for (size_t auxiliary = 0; auxiliary < auxiliaryTextures.size(); ++auxiliary) {
                unsigned int textureUnit = nextTextureUnit + static_cast<unsigned int>(auxiliary);
                auxiliaryUnits[auxiliary] = static_cast<GLint>(textureUnit);
                glActiveTexture(GL_TEXTURE0 + textureUnit);
                glBindTexture(GL_TEXTURE_2D, auxiliaryTextures[auxiliary]);
            }
            glUniform1iv(
                shader.gAuxiliaryTexturesHandle,
                static_cast<GLsizei>(auxiliaryUnits.size()),
                auxiliaryUnits.data()
            );
        }

        if (shader.gPreviousPassTextureHandle != -1 && passData.texture.has_value()) {
            glActiveTexture(GL_TEXTURE0 + 1);
            glBindTexture(GL_TEXTURE_2D, passData.texture.value());
            glUniform1i(shader.gPreviousPassTextureHandle, 1);
        }

        if (shader.gTextureSizeHandle != -1) {
            float textureWidth = renderer->textureSize.first > 0
                ? static_cast<float>(renderer->textureSize.first)
                : getTextureWidth();
            float textureHeight = renderer->textureSize.second > 0
                ? static_cast<float>(renderer->textureSize.second)
                : getTextureHeight();
            glUniform2f(shader.gTextureSizeHandle, textureWidth, textureHeight);
        }

        if (shader.gLegacyTextureSizeHandle != -1) {
            float textureWidth = renderer->textureSize.first > 0
                ? static_cast<float>(renderer->textureSize.first)
                : getTextureWidth();
            float textureHeight = renderer->textureSize.second > 0
                ? static_cast<float>(renderer->textureSize.second)
                : getTextureHeight();
            glUniform4f(
                shader.gLegacyTextureSizeHandle,
                textureWidth,
                textureHeight,
                1.0f / textureWidth,
                1.0f / textureHeight
            );
        }

        if (shader.gInputSizeHandle != -1) {
            glUniform2f(shader.gInputSizeHandle, getTextureWidth(), getTextureHeight());
        }

        if (shader.gOutputSizeHandle != -1) {
            auto bounds = videoLayout.getRelativeForegroundBounds();
            float outputWidth = (bounds[2] - bounds[0]) * static_cast<float>(videoLayout.getScreenWidth());
            float outputHeight = (bounds[3] - bounds[1]) * static_cast<float>(videoLayout.getScreenHeight());
            glUniform2f(shader.gOutputSizeHandle, outputWidth, outputHeight);
        }

        if (shader.gScreenDensityHandle != -1) {
            glUniform1f(shader.gScreenDensityHandle, getScreenDensity());
        }

        glDrawArrays(GL_TRIANGLES, 0, 6);

        if (shader.gvPositionHandle != -1) {
            glDisableVertexAttribArray(shader.gvPositionHandle);
        }
        if (shader.gvCoordinateHandle != -1) {
            glDisableVertexAttribArray(shader.gvCoordinateHandle);
        }

        if (shader.gPreviousPassTextureHandle != -1 && passData.texture.has_value()) {
            glActiveTexture(GL_TEXTURE0 + 1);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        for (unsigned int textureUnit = 1;
             textureUnit < nextTextureUnit + auxiliaryTextures.size();
             ++textureUnit) {
            glActiveTexture(GL_TEXTURE0 + textureUnit);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);

        glUseProgram(0);
    }

    if (hasNewFrame) {
        captureCurrentFrameToHistory();
    }
}

float Video::getScreenDensity() {
    return std::min(videoLayout.getScreenWidth() / getTextureWidth(), videoLayout.getScreenHeight() / getTextureHeight());
}

float Video::getTextureWidth() {
    return renderer->lastFrameSize.first;
}

float Video::getTextureHeight() {
    return renderer->lastFrameSize.second;
}

void Video::onNewFrame(const void *data, unsigned width, unsigned height, size_t pitch) {
    if (data != nullptr) {
        renderer->onNewFrame(data, width, height, pitch);
        isDirty = true;
    }
}

void Video::updateScreenSize(unsigned width, unsigned height) {
    videoLayout.updateScreenSize(width, height);
}

void Video::updateViewportSize(Rect viewportRect) {
    videoLayout.updateViewportSize(viewportRect);
}

void Video::updateRendererSize(unsigned int width, unsigned int height) {
    LOGD("Updating renderer size: %d x %d", width, height);
    renderer->updateRenderedResolution(width, height);
}

void Video::updateRotation(float rotation) {
    videoLayout.updateRotation(rotation);
}

Video::Video(
    RenderingOptions renderingOptions,
    ShaderManager::Config shaderConfig,
    bool bottomLeftOrigin,
    float rotation,
    bool skipDuplicateFrames,
    bool immersiveModeEnabled,
    Rect viewportRect,
    ImmersiveMode::Config immersiveModeConfig
) :
    requestedShaderConfig(std::move(shaderConfig)),
    skipDuplicateFrames(skipDuplicateFrames),
    immersiveModeEnabled(immersiveModeEnabled),
    immersiveMode(immersiveModeConfig),
    videoLayout(bottomLeftOrigin, rotation, viewportRect) {

    printGLString("Version", GL_VERSION);
    printGLString("Vendor", GL_VENDOR);
    printGLString("Renderer", GL_RENDERER);
    printGLString("Extensions", GL_EXTENSIONS);
    initializeGLESLogCallbackIfNeeded();

    LOGI("Initializing graphics");

    glViewport(0, 0, videoLayout.getScreenWidth(), videoLayout.getScreenHeight());

    glUseProgram(0);

    initializeRenderer(renderingOptions);
}

Video::~Video() {
    releaseCustomTextures();
    for (auto& shader : shadersChain) {
        if (shader.gProgram != 0) {
            glDeleteProgram(shader.gProgram);
            shader.gProgram = 0;
        }
    }
    shadersChain.clear();
    renderer.reset();
}

void Video::updateShaderType(ShaderManager::Config shaderConfig) {
    requestedShaderConfig = std::move(shaderConfig);
}

void Video::initializeRenderer(RenderingOptions renderingOptions) {
    auto shaders = ShaderManager::getShader(requestedShaderConfig);

    if (renderingOptions.hardwareAccelerated) {
        renderer = std::make_unique<FramebufferRenderer>(
            renderingOptions.width,
            renderingOptions.height,
            renderingOptions.useDepth,
            renderingOptions.useStencil,
            std::move(shaders)
        );
    } else {
        if (renderingOptions.openglESVersion >= 3) {
            renderer = std::make_unique<ImageRendererES3>();
        } else {
            renderer = std::make_unique<ImageRendererES2>();
        }
    }

    renderer->setPixelFormat(renderingOptions.pixelFormat);
    updateProgram();
}

void Video::updateAspectRatio(float aspectRatio) {
    videoLayout.updateAspectRatio(aspectRatio);
}

} //namespace libretrodroid
