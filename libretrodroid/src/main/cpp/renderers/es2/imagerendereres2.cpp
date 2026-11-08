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

#include "imagerendereres2.h"
#include "../../libretro-common/include/libretro.h"

#include <vector>

namespace libretrodroid {

ImageRendererES2::ImageRendererES2() {
    glGenTextures(1, &currentTexture);
    glBindTexture(GL_TEXTURE_2D, currentTexture);
}

ImageRendererES2::~ImageRendererES2() {
    if (currentTexture != 0) {
        glDeleteTextures(1, &currentTexture);
        currentTexture = 0;
    }
}

void ImageRendererES2::onNewFrame(const void *data, unsigned width, unsigned height, size_t pitch) {
    glBindTexture(GL_TEXTURE_2D, currentTexture);

    glPixelStorei(GL_UNPACK_ALIGNMENT, bytesPerPixel);

    if (pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
        convertDataFromRGB8888(data, pitch * height);
    } else if (pixelFormat == RETRO_PIXEL_FORMAT_0RGB1555) {
        convertDataFrom0RGB1555(data, width, height, pitch);
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeatSourceTexture ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeatSourceTexture ? GL_REPEAT : GL_CLAMP_TO_EDGE);

    if (lastFrameSize.first != width || lastFrameSize.second != height || isDirty) {
        unsigned int textureWidth = repeatSourceTexture ? nextPowerOfTwo(width) : width;
        unsigned int textureHeight = repeatSourceTexture ? nextPowerOfTwo(height) : height;
        textureSize = std::make_pair(textureWidth, textureHeight);

        // CMOf uses a power-of-two backing texture on GLES2 and uploads the
        // real frame into its top-left sub-rectangle. Zero the unused area so
        // samples near the active image boundary are deterministic.
        std::vector<unsigned char> emptyTexture;
        const void* initialData = nullptr;
        if (textureWidth != width || textureHeight != height) {
            emptyTexture.resize(textureWidth * textureHeight * bytesPerPixel, 0);
            initialData = emptyTexture.data();
        }
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            glInternalFormat,
            textureWidth,
            textureHeight,
            0,
            glFormat,
            glType,
            initialData
        );
        isDirty = false;
    }

    // If the given texture has the correct size we just upload it.
    if (bytesPerPixel * width == pitch) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, glFormat, glType, data);
    } else {
        // Here we are forced to take the long and slow way to upload the padded texture.
        for (int i = 0; i < height; i++) {
            auto row = (char*) data + (pitch) * i;
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, i, width, 1, glFormat, glType, row);
        }
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    Renderer::onNewFrame(data, width, height, pitch);
}

void ImageRendererES2::convertDataFromRGB8888(const void *data, size_t size) {
    char* pixelData = (char*) data;

    for (int i = 0; i < size - 4; i += 4) {
        auto currentRed = pixelData[i + 0];
        auto currentBlue = pixelData[i + 2];
        pixelData[i + 0] = currentBlue;
        pixelData[i + 2] = currentRed;
    }
}

uintptr_t ImageRendererES2::getTexture() {
    return currentTexture;
}

uintptr_t ImageRendererES2::getFramebuffer() {
    return 0; // ImageRender does not really expose a framebuffer.
}

void ImageRendererES2::setPixelFormat(int pixelFormat) {
    this->pixelFormat = pixelFormat;

    switch (pixelFormat) {
        case RETRO_PIXEL_FORMAT_XRGB8888:
            this->glInternalFormat = GL_RGBA;
            this->glFormat = GL_RGBA;
            this->glType = GL_UNSIGNED_BYTE;
            this->bytesPerPixel = 4;
            break;

        default:
        case RETRO_PIXEL_FORMAT_0RGB1555:
        case RETRO_PIXEL_FORMAT_RGB565:
            this->glInternalFormat = GL_RGB;
            this->glFormat = GL_RGB;
            this->glType = GL_UNSIGNED_SHORT_5_6_5;
            this->bytesPerPixel = 2;
            break;
    }
}

void ImageRendererES2::convertDataFrom0RGB1555(const void *data, unsigned int width, unsigned int height, size_t pitch) const {
    auto castData = (uint16_t*) data;

    for (int i = 0; i < height * pitch / bytesPerPixel; ++i) {
         castData[i] = ((0x1Fu) & castData[i])
            | (((0x1Fu << 5) & castData[i]) << 1)
            | (((0x1Fu << 10) & castData[i]) << 1);
    }
}

void ImageRendererES2::updateRenderedResolution(unsigned int width, unsigned int height) {}

bool ImageRendererES2::rendersInVideoCallback() {
    return false;
}

void ImageRendererES2::setShaders(ShaderManager::Chain shaders) {
    this->linear = shaders.linearTexture;
    this->repeatSourceTexture = shaders.repeatSourceTexture;
    this->isDirty = true;
}

unsigned int ImageRendererES2::nextPowerOfTwo(unsigned int value) {
    unsigned int result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

// ES2 Renderer doesn't currently support multiple passes.
Renderer::PassData ImageRendererES2::getPassData(unsigned int layer) {
    return { };
}

} //namespace libretrodroid
