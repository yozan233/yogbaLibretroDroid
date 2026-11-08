/*
 *     Copyright (C) 2022  Filippo Scognamiglio
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

#ifndef LIBRETRODROID_SHADERMANAGER_H
#define LIBRETRODROID_SHADERMANAGER_H

#include <string>
#include <unordered_map>
#include <vector>

namespace libretrodroid {

class ShaderManager {
public:
    struct AuxiliaryTexture {
        unsigned int width;
        unsigned int height;
        std::vector<unsigned char> rgba8888;

        bool operator==(const AuxiliaryTexture &other) const {
            return width == other.width &&
                   height == other.height &&
                   rgba8888 == other.rgba8888;
        }
    };

    struct Pass {
        std::string vertex;
        std::string fragment;
        bool linear;
        float scale;
        // Nearest custom passes that render directly to the viewport need the
        // same texel-boundary bias used by the default fragment shader.
        bool textureCoordinateBias = false;

        bool operator==(const ShaderManager::Pass &other) const;
    };

    struct Chain {
        std::vector<Pass> passes;
        bool linearTexture = false;
        // Legacy XML shaders loaded by CMOf sample the source texture with
        // GL_REPEAT semantics. Keep this separate from intermediate passes,
        // which remain clamped just like CMOf's output framebuffer.
        bool repeatSourceTexture = false;
        unsigned int historyCount = 0;
        std::vector<AuxiliaryTexture> auxiliaryTextures;

        bool operator==(const ShaderManager::Chain &other) const;
        bool operator!=(const ShaderManager::Chain &other) const;
    };

    enum class Type {
        SHADER_DEFAULT = 0,
        SHADER_CRT = 1,
        SHADER_LCD = 2,
        SHADER_SHARP = 3,
        SHADER_UPSCALE_CUT = 4,
        SHADER_UPSCALE_CUT2 = 5,
        SHADER_UPSCALE_CUT3 = 6,
        SHADER_CUSTOM = 7,
    };

    struct Config {
        Type type;
        std::unordered_map<std::string, std::string> params;
        std::vector<AuxiliaryTexture> auxiliaryTextures;

        inline bool operator==(const Config& other) const {
            return type == other.type &&
                   params == other.params &&
                   auxiliaryTextures == other.auxiliaryTextures;
        }
    };

private:
    static const std::string defaultShaderVertex;

    static const std::string defaultShaderFragment;
    static const std::string sharpFragment;
    static const std::string crtShaderFragment;
    static const std::string lcdShaderFragment;
    static const std::string passOutputFragment;
    static const std::string customPassOutputFragment;

    static const std::unordered_map<std::string, std::string> cutUpscaleParams;
    static const std::string cutUpscaleVertex;
    static const std::string cutUpscaleFragment;

    static const std::unordered_map<std::string, std::string> cut2UpscaleParams;
    static const std::string cut2UpscalePass0Vertex;
    static const std::string cut2UpscalePass0Fragment;
    static const std::string cut2UpscalePass1Vertex;
    static const std::string cut2UpscalePass1Fragment;

    static const std::unordered_map<std::string, std::string> cut3UpscaleParams;
    static const std::string cut3UpscalePass0Vertex;
    static const std::string cut3UpscalePass0Fragment;
    static const std::string cut3UpscalePass1Vertex;
    static const std::string cut3UpscalePass1Fragment;
    static const std::string cut3UpscalePass2Vertex;
    static const std::string cut3UpscalePass2Fragment;

private:
    static std::string buildDefines(
        std::unordered_map<std::string, std::string> baseParams,
        std::unordered_map<std::string, std::string> customParams
    );

public:
    static Chain getShader(const Config& config);
};

}

#endif //LIBRETRODROID_SHADERMANAGER_H
