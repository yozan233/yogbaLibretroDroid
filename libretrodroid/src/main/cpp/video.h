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

#ifndef LIBRETRODROID_VIDEO_H
#define LIBRETRODROID_VIDEO_H

#include <GLES2/gl2.h>
#include <optional>
#include <array>
#include <memory>

#include "renderers/renderer.h"
#include "shadermanager.h"
#include "utils/rect.h"
#include "immersivemode.h"
#include "videolayout.h"

namespace libretrodroid {

class Video {
public:

    struct RenderingOptions {
        bool hardwareAccelerated = false;
        unsigned int width;
        unsigned int height;
        bool useDepth;
        bool useStencil;
        int openglESVersion;
        int pixelFormat;
    };

    struct ShaderChainEntry {
        GLint gProgram = 0;
        GLint gvPositionHandle = 0;
        GLint gvCoordinateHandle = 0;
        GLint gTextureHandle = 0;
        GLint gPreviousPassTextureHandle = 0;
        GLint gScreenDensityHandle = 0;
        GLint gTextureSizeHandle = 0;
        GLint gLegacyTextureSizeHandle = -1;
        GLint gInputSizeHandle = -1;
        GLint gOutputSizeHandle = -1;
        GLint gHistoryTexturesHandle = -1;
        GLint gAuxiliaryTexturesHandle = -1;
        bool textureCoordinateBias = false;
    };

    Video(
        RenderingOptions renderingOptions,
        ShaderManager::Config shaderConfig,
        bool bottomLeftOrigin,
        float rotation,
        bool skipDuplicateFrames,
        bool immersiveMode,
        Rect viewportRect,
        ImmersiveMode::Config immersiveModeConfig
    );
    ~Video();

    VideoLayout& getLayout() { return videoLayout; }

    void updateAspectRatio(float aspectRatio);
    void updateScreenSize(unsigned screenWidth, unsigned screenHeight);
    void updateViewportSize(Rect viewportRect);
    void updateRendererSize(unsigned width, unsigned height);
    void updateRotation(float rotation);
    void updateShaderType(ShaderManager::Config shaderConfig);

    void renderFrame();

    void onNewFrame(const void *data, unsigned width, unsigned height, size_t pitch);

    uintptr_t getCurrentFramebuffer() {
        return renderer->getFramebuffer();
    };

    bool rendersInVideoCallback() {
        return renderer->rendersInVideoCallback();
    }

private:
    void updateProgram();
    void releaseCustomTextures();
    void createAuxiliaryTextures(const ShaderManager::Chain& shaders, unsigned int maxCount);
    void ensureHistoryTextures(unsigned int count);
    void captureCurrentFrameToHistory();

    float getScreenDensity();
    float getTextureWidth();
    float getTextureHeight();

    void initializeRenderer(RenderingOptions renderingOptions);

private:
    ShaderManager::Config requestedShaderConfig = ShaderManager::Config {
        ShaderManager::Type::SHADER_DEFAULT
    };
    std::optional<ShaderManager::Config> loadedShaderType = std::nullopt;

    bool isDirty = false;
    bool skipDuplicateFrames = false;

    std::vector<ShaderChainEntry> shadersChain;
    ShaderManager::Chain activeShaders;

    std::vector<GLuint> historyTextures;
    std::vector<GLuint> auxiliaryTextures;
    GLuint historyReadFramebuffer = 0;
    GLuint historyCopyProgram = 0;
    GLint historyCopyPositionHandle = -1;
    GLint historyCopyCoordinateHandle = -1;
    GLint historyCopyTextureHandle = -1;
    size_t historyWriteIndex = 0;
    std::pair<int, int> historyTextureSize = {0, 0};
    unsigned int activeHistoryCount = 0;

    bool immersiveModeEnabled = false;
    ImmersiveMode immersiveMode;
    VideoLayout videoLayout;

    std::unique_ptr<Renderer> renderer;
};

}

#endif //LIBRETRODROID_VIDEO_H
